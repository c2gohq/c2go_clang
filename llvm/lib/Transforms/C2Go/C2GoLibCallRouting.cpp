//===- C2GoLibCallRouting.cpp - Route synthesized libc calls -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A source-level call through a c2go_linkname declaration is already emitted
// with the declaration's Go symbol name and GoABI0 calling convention. LLVM
// can, however, create a semantically equivalent call only after AST lowering:
// LoopIdiomRecognize emits `@strlen`, InstCombine may emit `@puts`, and similar
// library optimisations use TargetLibraryInfo's canonical C spelling. The AST
// declaration may never have been materialised in IR, so those new calls lose
// both pieces of c2go information.
//
// clang preserves the direct-GoABI0 subset of its c2go_linkname declarations
// in !c2go.libcall.routes. This pass consumes that table after all ordinary IR
// optimisations. It rewrites declaration-only raw symbols and materialises
// routed math intrinsics/frem as real calls. Definitions are deliberately left
// untouched: a user-provided function with a libc-like name is not an
// optimizer-synthesized external libcall.
//
// The pass must precede RewriteStatepointsForGC. Retrofitting GoABI0 during
// SelectionDAG lowering would create a real Go call that has no statepoint
// wrapper. The backend therefore treats a surviving routed libcall as an error.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoLibCallRouting.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

#include <map>
#include <optional>
#include <string>

using namespace llvm;

namespace {

using RouteMap = std::map<std::string, std::string>;

static bool readRoutes(Module &M, RouteMap &Routes) {
  const NamedMDNode *NMD = M.getNamedMetadata(c2go::kLibCallRoutesMDName);
  if (!NMD)
    return true;

  for (const MDNode *Entry : NMD->operands()) {
    if (!Entry || Entry->getNumOperands() != 2) {
      M.getContext().emitError(
          "malformed !c2go.libcall.routes entry: expected two strings");
      return false;
    }
    const auto *CName = dyn_cast_or_null<MDString>(Entry->getOperand(0).get());
    const auto *Target = dyn_cast_or_null<MDString>(Entry->getOperand(1).get());
    if (!CName || !Target || CName->getString().empty() ||
        Target->getString().empty()) {
      M.getContext().emitError(
          "malformed !c2go.libcall.routes entry: expected non-empty strings");
      return false;
    }

    auto [It, Inserted] =
        Routes.emplace(CName->getString().str(), Target->getString().str());
    if (!Inserted && It->second != Target->getString()) {
      M.getContext().emitError(
          "conflicting !c2go.libcall.routes entries for C function '" +
          CName->getString() + "'");
      return false;
    }
  }
  return true;
}

static bool ensureStringAttr(Function &F, StringRef Key, StringRef Value) {
  Attribute A = F.getFnAttribute(Key);
  if (A.isValid() && A.getValueAsString() == Value)
    return false;
  F.addFnAttr(Key, Value);
  return true;
}

static bool isRoutedDefinition(const Function &F, StringRef CName,
                               StringRef TargetName) {
  if (F.isDeclaration())
    return false;
  Attribute CNameAttr = F.getFnAttribute("c2go-c-name");
  Attribute LinkNameAttr = F.getFnAttribute("c2go-linkname");
  return CNameAttr.isValid() && LinkNameAttr.isValid() &&
         CNameAttr.getValueAsString() == CName &&
         LinkNameAttr.getValueAsString() == TargetName;
}

enum class RoutedIntrinsicKind { Direct, ModF, Frexp, SinCos };

struct RoutedIntrinsic {
  std::string CName;
  RoutedIntrinsicKind Kind;
};

// Constrained math intrinsics use the same C entry points as their ordinary
// counterparts, but append rounding/exception metadata operands. Keep the
// classifier below single-sourced by mapping their IDs back to the ordinary
// intrinsic IDs defined in ConstrainedOps.def.
static Intrinsic::ID getUnconstrainedIntrinsicID(Intrinsic::ID ID) {
  switch (ID) {
#define DAG_FUNCTION(NAME, NARG, ROUND_MODE, CONSTRAINED, DAG_NODE)            \
  case Intrinsic::CONSTRAINED:                                                 \
    return Intrinsic::NAME;
#define FUNCTION(NAME, NARG, ROUND_MODE, CONSTRAINED)                          \
  case Intrinsic::CONSTRAINED:                                                 \
    return Intrinsic::NAME;
#define INSTRUCTION(NAME, NARG, ROUND_MODE, CONSTRAINED)
#define DAG_INSTRUCTION(NAME, NARG, ROUND_MODE, CONSTRAINED, DAG_NODE)
#define CMP_INSTRUCTION(NAME, NARG, ROUND_MODE, CONSTRAINED, DAG_NODE)
#include "llvm/IR/ConstrainedOps.def"
  default:
    return ID;
  }
}

static std::optional<std::string> getScalarFPName(StringRef Base, Type *Ty) {
  std::string Name = Base.str();
  if (Ty->isFloatTy())
    Name += "f";
  else if (Ty->isDoubleTy())
    ;
  else if (Ty->isX86_FP80Ty() || Ty->isFP128Ty() || Ty->isPPC_FP128Ty())
    Name += "l";
  else
    return std::nullopt;
  return Name;
}

// Identify IR operations that SelectionDAG may otherwise turn into a libc or
// libm call. The always-hardware forms on c2go's supported targets (sqrt,
// fabs, copysign, min/max) are deliberately absent: keeping those as
// intrinsics preserves normal target lowering, while the backend guard below
// still rejects an unexpected late libcall.
static std::optional<RoutedIntrinsic>
classifyRoutedIntrinsic(const IntrinsicInst &II) {
  StringRef Base;
  RoutedIntrinsicKind Kind = RoutedIntrinsicKind::Direct;
  Intrinsic::ID ID = getUnconstrainedIntrinsicID(II.getIntrinsicID());
  switch (ID) {
  default:
    if (II.getIntrinsicID() == Intrinsic::experimental_constrained_frem) {
      Base = "fmod";
      break;
    }
    return std::nullopt;
  case Intrinsic::sin:
    Base = "sin";
    break;
  case Intrinsic::cos:
    Base = "cos";
    break;
  case Intrinsic::tan:
    Base = "tan";
    break;
  case Intrinsic::asin:
    Base = "asin";
    break;
  case Intrinsic::acos:
    Base = "acos";
    break;
  case Intrinsic::atan:
    Base = "atan";
    break;
  case Intrinsic::atan2:
    Base = "atan2";
    break;
  case Intrinsic::sinh:
    Base = "sinh";
    break;
  case Intrinsic::cosh:
    Base = "cosh";
    break;
  case Intrinsic::tanh:
    Base = "tanh";
    break;
  case Intrinsic::exp:
    Base = "exp";
    break;
  case Intrinsic::exp2:
    Base = "exp2";
    break;
  case Intrinsic::exp10:
    Base = "exp10";
    break;
  case Intrinsic::log:
    Base = "log";
    break;
  case Intrinsic::log2:
    Base = "log2";
    break;
  case Intrinsic::log10:
    Base = "log10";
    break;
  case Intrinsic::pow:
    Base = "pow";
    break;
  case Intrinsic::floor:
    Base = "floor";
    break;
  case Intrinsic::ceil:
    Base = "ceil";
    break;
  case Intrinsic::trunc:
    Base = "trunc";
    break;
  case Intrinsic::rint:
    Base = "rint";
    break;
  case Intrinsic::nearbyint:
    Base = "nearbyint";
    break;
  case Intrinsic::round:
    Base = "round";
    break;
  case Intrinsic::roundeven:
    Base = "roundeven";
    break;
  case Intrinsic::lround:
    Base = "lround";
    break;
  case Intrinsic::llround:
    Base = "llround";
    break;
  case Intrinsic::lrint:
    Base = "lrint";
    break;
  case Intrinsic::llrint:
    Base = "llrint";
    break;
  case Intrinsic::fma:
    Base = "fma";
    break;
  case Intrinsic::ldexp:
    Base = "ldexp";
    break;
  case Intrinsic::modf:
    Base = "modf";
    Kind = RoutedIntrinsicKind::ModF;
    break;
  case Intrinsic::frexp:
    Base = "frexp";
    Kind = RoutedIntrinsicKind::Frexp;
    break;
  case Intrinsic::sincos:
    Base = "sincos";
    Kind = RoutedIntrinsicKind::SinCos;
    break;
  }

  Type *FPTy = II.getArgOperand(0)->getType();
  Type *ScalarTy = FPTy->getScalarType();
  std::optional<std::string> Name = getScalarFPName(Base, ScalarTy);
  if (!Name)
    return std::nullopt;
  return RoutedIntrinsic{std::move(*Name), Kind};
}

static Function *getOrCreateRouteTarget(Module &M, StringRef CName,
                                        StringRef TargetName, FunctionType *FT,
                                        bool &Changed) {
  Function *Target = M.getFunction(TargetName);
  if (!Target) {
    if (M.getNamedValue(TargetName)) {
      M.getContext().emitError("cannot route synthesized c2go libcall '" +
                               CName + "' to '" + TargetName +
                               "': target name belongs to a non-function "
                               "global");
      return nullptr;
    }
    Target = Function::Create(FT, GlobalValue::ExternalLinkage, TargetName, &M);
    Changed = true;
  } else if (Target->getFunctionType() != FT) {
    M.getContext().emitError("cannot route synthesized c2go libcall '" + CName +
                             "' to '" + TargetName +
                             "': function types differ");
    return nullptr;
  }

  if (Target->getCallingConv() != CallingConv::GoABI0) {
    Target->setCallingConv(CallingConv::GoABI0);
    Changed = true;
  }
  Changed |= ensureStringAttr(*Target, "c2go-linkname", TargetName);
  if (!Target->hasFnAttribute("c2go-c-name"))
    Changed |= ensureStringAttr(*Target, "c2go-c-name", CName);
  return Target;
}

static AllocaInst *createEntryAlloca(Function &F, Type *Ty, StringRef Name) {
  IRBuilder<> B(&*F.getEntryBlock().getFirstInsertionPt());
  return B.CreateAlloca(Ty, nullptr, Name);
}

static void copyCallProperties(CallInst &To, const CallInst &From) {
  To.setCallingConv(CallingConv::GoABI0);
  To.setDebugLoc(From.getDebugLoc());
  if (isa<FPMathOperator>(&To) && isa<FPMathOperator>(&From))
    To.copyFastMathFlags(&From);
  if (isa<ConstrainedFPIntrinsic>(&From) || From.hasFnAttr(Attribute::StrictFP))
    To.addFnAttr(Attribute::StrictFP);
}

static bool lowerRoutedIntrinsic(CallInst &CI, const RoutedIntrinsic &Desc,
                                 StringRef TargetName, bool &Changed) {
  Module &M = *CI.getModule();
  Type *FPTy = CI.getArgOperand(0)->getType();
  if (FPTy->isVectorTy()) {
    M.getContext().emitError("cannot route vector c2go intrinsic '" +
                             CI.getCalledFunction()->getName() + "' through '" +
                             Desc.CName + "' before GC lowering");
    return false;
  }

  SmallVector<OperandBundleDef, 2> Bundles;
  CI.getOperandBundlesAsDefs(Bundles);
  IRBuilder<> B(&CI);
  FunctionType *FT = nullptr;
  SmallVector<Value *, 4> Args;
  Value *Replacement = nullptr;

  switch (Desc.Kind) {
  case RoutedIntrinsicKind::Direct: {
    Intrinsic::ID ID = getUnconstrainedIntrinsicID(CI.getIntrinsicID());
    if (ID == Intrinsic::ldexp &&
        !CI.getArgOperand(1)->getType()->isIntegerTy(32)) {
      M.getContext().emitError("cannot route c2go intrinsic '" +
                               CI.getCalledFunction()->getName() +
                               "': C ldexp requires an i32 exponent");
      return false;
    }
    SmallVector<Type *, 4> ParamTys;
    unsigned NumArgs = CI.arg_size();
    if (auto *CFPI = dyn_cast<ConstrainedFPIntrinsic>(&CI))
      NumArgs = CFPI->getNonMetadataArgCount();
    for (unsigned I = 0; I != NumArgs; ++I) {
      Value *Arg = CI.getArgOperand(I);
      Args.push_back(Arg);
      ParamTys.push_back(Arg->getType());
    }
    FT = FunctionType::get(CI.getType(), ParamTys, false);
    Function *Target =
        getOrCreateRouteTarget(M, Desc.CName, TargetName, FT, Changed);
    if (!Target)
      return false;
    CallInst *Call = B.CreateCall(Target, Args, Bundles, CI.getName());
    copyCallProperties(*Call, CI);
    Replacement = Call;
    break;
  }
  case RoutedIntrinsicKind::ModF: {
    AllocaInst *Integral =
        createEntryAlloca(*CI.getFunction(), FPTy, "c2go.modf.integral");
    FT = FunctionType::get(FPTy, {FPTy, Integral->getType()}, false);
    Function *Target =
        getOrCreateRouteTarget(M, Desc.CName, TargetName, FT, Changed);
    if (!Target)
      return false;
    CallInst *Fraction = B.CreateCall(Target, {CI.getArgOperand(0), Integral},
                                      Bundles, "c2go.modf.fraction");
    copyCallProperties(*Fraction, CI);
    Value *IntegralValue = B.CreateLoad(FPTy, Integral, "c2go.modf.integral.v");
    Replacement =
        B.CreateInsertValue(PoisonValue::get(CI.getType()), Fraction, 0);
    Replacement =
        B.CreateInsertValue(Replacement, IntegralValue, 1, CI.getName());
    break;
  }
  case RoutedIntrinsicKind::Frexp: {
    auto *ResultTy = dyn_cast<StructType>(CI.getType());
    if (!ResultTy || ResultTy->getNumElements() != 2 ||
        !ResultTy->getElementType(1)->isIntegerTy(32)) {
      M.getContext().emitError("cannot route c2go intrinsic '" +
                               CI.getCalledFunction()->getName() +
                               "': C frexp requires an i32 exponent result");
      return false;
    }
    Type *ExponentTy = ResultTy->getElementType(1);
    AllocaInst *Exponent =
        createEntryAlloca(*CI.getFunction(), ExponentTy, "c2go.frexp.exp");
    FT = FunctionType::get(FPTy, {FPTy, Exponent->getType()}, false);
    Function *Target =
        getOrCreateRouteTarget(M, Desc.CName, TargetName, FT, Changed);
    if (!Target)
      return false;
    CallInst *Fraction = B.CreateCall(Target, {CI.getArgOperand(0), Exponent},
                                      Bundles, "c2go.frexp.fraction");
    copyCallProperties(*Fraction, CI);
    Value *ExponentValue =
        B.CreateLoad(ExponentTy, Exponent, "c2go.frexp.exp.v");
    Replacement =
        B.CreateInsertValue(PoisonValue::get(CI.getType()), Fraction, 0);
    Replacement =
        B.CreateInsertValue(Replacement, ExponentValue, 1, CI.getName());
    break;
  }
  case RoutedIntrinsicKind::SinCos: {
    AllocaInst *Sin =
        createEntryAlloca(*CI.getFunction(), FPTy, "c2go.sincos.sin");
    AllocaInst *Cos =
        createEntryAlloca(*CI.getFunction(), FPTy, "c2go.sincos.cos");
    FT = FunctionType::get(Type::getVoidTy(M.getContext()),
                           {FPTy, Sin->getType(), Cos->getType()}, false);
    Function *Target =
        getOrCreateRouteTarget(M, Desc.CName, TargetName, FT, Changed);
    if (!Target)
      return false;
    CallInst *Call =
        B.CreateCall(Target, {CI.getArgOperand(0), Sin, Cos}, Bundles);
    copyCallProperties(*Call, CI);
    Value *SinValue = B.CreateLoad(FPTy, Sin, "c2go.sincos.sin.v");
    Value *CosValue = B.CreateLoad(FPTy, Cos, "c2go.sincos.cos.v");
    Replacement =
        B.CreateInsertValue(PoisonValue::get(CI.getType()), SinValue, 0);
    Replacement = B.CreateInsertValue(Replacement, CosValue, 1, CI.getName());
    break;
  }
  }

  CI.replaceAllUsesWith(Replacement);
  CI.eraseFromParent();
  Changed = true;
  return true;
}

static bool lowerRoutedFRem(BinaryOperator &FRem, StringRef CName,
                            StringRef TargetName, bool &Changed) {
  Module &M = *FRem.getModule();
  Type *FPTy = FRem.getType();
  if (FPTy->isVectorTy()) {
    M.getContext().emitError("cannot route vector c2go frem through '" + CName +
                             "' before GC lowering");
    return false;
  }
  FunctionType *FT = FunctionType::get(FPTy, {FPTy, FPTy}, false);
  Function *Target = getOrCreateRouteTarget(M, CName, TargetName, FT, Changed);
  if (!Target)
    return false;
  IRBuilder<> B(&FRem);
  CallInst *Call = B.CreateCall(
      Target, {FRem.getOperand(0), FRem.getOperand(1)}, FRem.getName());
  Call->setCallingConv(CallingConv::GoABI0);
  Call->setDebugLoc(FRem.getDebugLoc());
  Call->copyFastMathFlags(&FRem);
  FRem.replaceAllUsesWith(Call);
  FRem.eraseFromParent();
  Changed = true;
  return true;
}

} // namespace

PreservedAnalyses C2GoLibCallRoutingPass::run(Module &M,
                                              ModuleAnalysisManager &) {
  if (!M.getModuleFlag(c2go::kGoabiModuleFlag))
    return PreservedAnalyses::all();

  RouteMap Routes;
  if (!readRoutes(M, Routes))
    return PreservedAnalyses::all();

  bool Changed = false;

  // SelectionDAG-created Go calls are too late for RewriteStatepointsForGC.
  // Materialize the intrinsic forms that may need libm while this pass still
  // precedes the GC pipeline. Compiler-rt operations such as llvm.powi are
  // intentionally not classified and retain the target's ordinary ABI.
  SmallVector<Instruction *, 16> LateLibCalls;
  for (Function &F : M)
    if (!F.isDeclaration())
      for (Instruction &I : instructions(F))
        if (isa<IntrinsicInst>(I) || I.getOpcode() == Instruction::FRem)
          LateLibCalls.push_back(&I);

  for (Instruction *I : LateLibCalls) {
    if (auto *II = dyn_cast<IntrinsicInst>(I)) {
      std::optional<RoutedIntrinsic> Desc = classifyRoutedIntrinsic(*II);
      if (!Desc)
        continue;
      auto Route = Routes.find(Desc->CName);
      if (Route == Routes.end())
        continue;
      lowerRoutedIntrinsic(*cast<CallInst>(II), *Desc, Route->second, Changed);
      continue;
    }

    auto *FRem = cast<BinaryOperator>(I);
    Type *ScalarTy = FRem->getType()->getScalarType();
    std::optional<std::string> CName = getScalarFPName("fmod", ScalarTy);
    if (!CName)
      continue;
    auto Route = Routes.find(*CName);
    if (Route != Routes.end())
      lowerRoutedFRem(*FRem, *CName, Route->second, Changed);
  }

  for (const auto &[CName, TargetName] : Routes) {
    Function *Raw = M.getFunction(CName);
    if (!Raw || Raw->use_empty())
      continue;

    // A differently-named definition is normally real program code, not a
    // synthetic external libcall. Whole-package linking adds one important
    // case: the c2go definition itself can now satisfy the canonical name
    // synthesized by an earlier per-TU optimization (for example @strlen).
    // Its frontend attributes prove that it is the exact routed definition.
    bool IsLinkedRoutedDefinition =
        CName != TargetName && isRoutedDefinition(*Raw, CName, TargetName);
    if (CName != TargetName && !Raw->isDeclaration() &&
        !IsLinkedRoutedDefinition)
      continue;

    SmallVector<CallBase *, 8> Calls;
    bool HasNonCallUse = false;
    for (User *U : Raw->users()) {
      auto *CB = dyn_cast<CallBase>(U);
      if (!CB || CB->getCalledFunction() != Raw) {
        HasNonCallUse = true;
        break;
      }
      Calls.push_back(CB);
    }
    if (HasNonCallUse) {
      M.getContext().emitError("cannot route synthesized c2go libcall '" +
                               CName +
                               "': raw declaration has a non-direct-call use");
      continue;
    }

    // Normalize optimizer-synthesized calls to the linked c2go definition in
    // place. The definition already owns the exported Plan 9 symbol, so it
    // must not be renamed to the metadata route target.
    if (IsLinkedRoutedDefinition) {
      if (Raw->getCallingConv() != CallingConv::GoABI0) {
        Raw->setCallingConv(CallingConv::GoABI0);
        Changed = true;
      }
      for (CallBase *CB : Calls) {
        if (CB->getCallingConv() != CallingConv::GoABI0) {
          CB->setCallingConv(CallingConv::GoABI0);
          Changed = true;
        }
      }
      continue;
    }

    Function *Target = M.getFunction(TargetName);
    if (!Target) {
      if (GlobalValue *Collision = M.getNamedValue(TargetName)) {
        (void)Collision;
        M.getContext().emitError(
            "cannot route synthesized c2go libcall '" + CName + "' to '" +
            TargetName + "': target name belongs to a non-function global");
        continue;
      }
      Raw->setName(TargetName);
      Target = Raw;
      Changed = true;
    } else if (Target != Raw) {
      if (Target->getFunctionType() != Raw->getFunctionType()) {
        M.getContext().emitError("cannot route synthesized c2go libcall '" +
                                 CName + "' to '" + TargetName +
                                 "': function types differ");
        continue;
      }

      // Preserve attributes inferred for the canonical libc declaration. At
      // this late point they mostly document semantics for codegen, but losing
      // nounwind/memory effects would also make the merged declaration less
      // faithful when emitted as bitcode for WF2.
      AttrBuilder RawFnAttrs(M.getContext(), Raw->getAttributes().getFnAttrs());
      Target->addFnAttrs(RawFnAttrs);
    }

    if (Target->getCallingConv() != CallingConv::GoABI0) {
      Target->setCallingConv(CallingConv::GoABI0);
      Changed = true;
    }
    Changed |= ensureStringAttr(*Target, "c2go-linkname", TargetName);
    if (!Target->hasFnAttribute("c2go-c-name"))
      Changed |= ensureStringAttr(*Target, "c2go-c-name", CName);

    // This mismatch is expected at this normalization boundary, so update it
    // directly rather than using enforceCallSiteCC: that audit helper records
    // unexpected producer bugs in c2go.cc.violations and would make every
    // legitimate optimizer-synthesized libcall fail the WF2 ship gate.
    for (CallBase *CB : Calls) {
      if (Target != Raw)
        CB->setCalledFunction(Target);
      if (CB->getCallingConv() != CallingConv::GoABI0) {
        CB->setCallingConv(CallingConv::GoABI0);
        Changed = true;
      }
    }

    if (Target != Raw && Raw->use_empty()) {
      Raw->eraseFromParent();
      Changed = true;
    }
  }

  // Any remaining declaration-only standard libc symbol is unresolved in a
  // c2go link. Diagnose it here when possible; SelectionDAG performs the same
  // check for calls that only come into existence during instruction
  // selection.
  TargetLibraryInfoImpl TLII(Triple(M.getTargetTriple()));
  for (Function &F : M) {
    if (!F.isDeclaration() || F.use_empty())
      continue;
    LibFunc LF;
    if (!TLII.getLibFunc(F.getName(), LF))
      continue;
    M.getContext().emitError("c2go libc call '" + F.getName() +
                             "' has no direct-GoABI0 c2go_linkname route");
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
