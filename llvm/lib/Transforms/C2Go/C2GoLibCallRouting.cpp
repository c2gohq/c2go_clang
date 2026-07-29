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
// optimisations and rewrites only declaration-only raw symbols. Definitions are
// deliberately left untouched: a user-provided function with a libc-like name
// is not an optimizer-synthesized external libcall.
//
// The pass must precede RewriteStatepointsForGC. Retrofitting GoABI0 after
// RS4GC would create a real Go call that has no statepoint wrapper.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoLibCallRouting.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

#include <map>
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

} // namespace

PreservedAnalyses C2GoLibCallRoutingPass::run(Module &M,
                                              ModuleAnalysisManager &) {
  if (!M.getModuleFlag(c2go::kGoabiModuleFlag))
    return PreservedAnalyses::all();

  RouteMap Routes;
  if (!readRoutes(M, Routes) || Routes.empty())
    return PreservedAnalyses::all();

  bool Changed = false;
  for (const auto &[CName, TargetName] : Routes) {
    Function *Raw = M.getFunction(CName);
    if (!Raw || Raw->use_empty())
      continue;

    // A differently-named definition is real program code, not a synthetic
    // external libcall. When CName == TargetName, the route only needs to
    // normalize that definition/declaration's CC and direct call sites.
    if (CName != TargetName && !Raw->isDeclaration())
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

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
