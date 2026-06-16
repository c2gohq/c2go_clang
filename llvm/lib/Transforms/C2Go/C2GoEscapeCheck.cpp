//===- C2GoEscapeCheck.cpp - Dynamic stack->heap escape instrumentation ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See C2GoEscapeCheck.h for the design rationale (c2go-lto client B, dynamic
// half, #289).
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoEscapeCheck.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/C2Go/C2GoCommon.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

using namespace llvm;

#define DEBUG_TYPE "c2go-escape-check"

// Default OFF. Enabled via `-mllvm -c2go-escape-check` (clang) or
// `-c2go-escape-check` (opt). When off the pass is a no-op, so it never
// perturbs normal codegen.
static cl::opt<bool> EnableEscapeCheck(
    "c2go-escape-check", cl::Hidden, cl::init(false),
    cl::desc("c2go: instrument heap/global pointer stores with a runtime "
             "stack-address-escape check (debug, #289)."));

namespace {

// IR-level name of the runtime helper. Signature (in Go terms):
//   func EscapeCheck(v, dst unsafe.Pointer, site *byte)
// We emit the call under the Go *package* symbol name so the MCPlan9 emitter
// mangles it to `github·com∕c2go_project∕c2go_libc·EscapeCheck(SB)` and the Go
// linker resolves it to c2go-libc's `EscapeCheck` (exposed via //go:linkname).
// The helper reads [g.stack.lo, g.stack.hi) via X28 and reports when `v` is a
// stack address stored through a non-stack `dst`.
constexpr char kHelperName[] =
    "github.com/c2go_project/c2go_libc.EscapeCheck";

// True if `dst` is *provably* a stack slot, in which case a stack->stack store
// is not an escape and we skip instrumentation (also the common case, so this
// keeps the instrumented binary's overhead down).
bool isProvablyStack(const Value *Dst) {
  return isa<AllocaInst>(getUnderlyingObject(Dst));
}

// Declare (or reuse) the helper. The callee MUST be GoABI0: the Go linker
// generates an ABI0 entry that reads args from the stack, so a default-CC call
// site would pass them in registers and the helper would read garbage. Mirrors
// C2GoMemcpyTyping::getOrDeclareTypedMemmove.
Function *getOrDeclareHelper(Module &M) {
  if (auto *F = M.getFunction(kHelperName)) {
    if (F->getCallingConv() != CallingConv::GoABI0)
      F->setCallingConv(CallingConv::GoABI0);
    return F;
  }
  LLVMContext &Ctx = M.getContext();
  Type *VoidTy = Type::getVoidTy(Ctx);
  PointerType *PtrTy = PointerType::getUnqual(Ctx);
  FunctionType *FT =
      FunctionType::get(VoidTy, {PtrTy, PtrTy, PtrTy}, /*isVarArg=*/false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, kHelperName, &M);
  F->setCallingConv(CallingConv::GoABI0);
  return F;
}

// Build (and cache) a private constant C string naming the store site, used as
// the third argument so the helper can print where the escape happened.
Constant *getSiteString(Module &M, IRBuilder<> &B, const StoreInst *SI) {
  const Function *F = SI->getFunction();
  std::string S;
  raw_string_ostream OS(S);
  OS << F->getName();
  if (const DebugLoc &DL = SI->getDebugLoc()) {
    OS << " @ ";
    if (const auto *Scope = dyn_cast_or_null<DIScope>(DL.getScope()))
      OS << Scope->getFilename() << ":";
    OS << DL.getLine() << ":" << DL.getCol();
  } else {
    OS << " @ <no-dbg>";
  }
  return B.CreateGlobalString(OS.str(), "c2go.escape.site", /*AddressSpace=*/0,
                              &M);
}

} // namespace

PreservedAnalyses C2GoEscapeCheckPass::run(Module &M, ModuleAnalysisManager &) {
  if (!EnableEscapeCheck)
    return PreservedAnalyses::all();
  if (!M.getModuleFlag(llvm::c2go::kGoabiModuleFlag))
    return PreservedAnalyses::all();

  LLVMContext &Ctx = M.getContext();
  PointerType *I8PtrTy = PointerType::getUnqual(Ctx);
  // #459: lazy Helper materialization — getOrDeclareHelper mutates the IR
  // (Function::Create on first call, setCallingConv on a re-entrant call into
  // a pre-existing decl with a stale CC). Eagerly calling it before knowing
  // whether any Sites exist would mutate IR yet return PreservedAnalyses::all()
  // on a no-Sites/no-sweep run, violating the new-PM contract. Defer until the
  // first store that actually needs instrumentation.
  Function *Helper = nullptr;

  unsigned NumInstrumented = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    // Collect first; instrumenting mutates the instruction stream.
    SmallVector<StoreInst *, 32> Sites;
    for (Instruction &I : instructions(F)) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI)
        continue;
      Value *Val = SI->getValueOperand();
      if (!Val->getType()->isPointerTy())
        continue;
      // Only stores whose destination is NOT provably a stack slot can be a
      // stack->heap / stack->global escape.
      if (isProvablyStack(SI->getPointerOperand()))
        continue;
      Sites.push_back(SI);
    }

    for (StoreInst *SI : Sites) {
      if (!Helper)
        Helper = getOrDeclareHelper(M);
      IRBuilder<> B(SI->getNextNode());
      Value *Val = SI->getValueOperand();
      Value *Dst = SI->getPointerOperand();
      // The helper takes plain (addrspace 0) i8* arguments. Pointers in c2go
      // may be managed (addrspace 1); normalize to addrspace 0 for the call so
      // the helper only ever sees raw bit patterns to range-check.
      if (Val->getType() != I8PtrTy)
        Val = B.CreatePointerBitCastOrAddrSpaceCast(Val, I8PtrTy);
      if (Dst->getType() != I8PtrTy)
        Dst = B.CreatePointerBitCastOrAddrSpaceCast(Dst, I8PtrTy);
      Constant *Site = getSiteString(M, B, SI);
      // CallInst inherits the FunctionType but NOT the CC; set GoABI0 to match
      // the callee declaration (see getOrDeclareHelper).
      CallInst *CI = B.CreateCall(Helper, {Val, Dst, Site});
      CI->setCallingConv(CallingConv::GoABI0);
      ++NumInstrumented;
    }
  }

  // #429 (Wave CD #419 audit Finding #6): pass-end CC sweep — defends
  // against a pre-existing CallInst against `EscapeCheck` carrying a
  // default-C CC. Mirrors C2GoMemcpyTyping / C2GoWriteBarriers. The
  // name string is kept as a literal here (rather than pulled from the
  // anonymous-namespace `kHelperName`) so the sweep stays inside this
  // function scope without needing to widen namespace visibility.
  //
  // #453: OR the sweep's return into the pass-level Changed indicator so
  // a sweep-only rewrite (no new instrumented sites this run) still
  // surfaces as PreservedAnalyses::none().
  bool SweepChanged = llvm::c2go::enforceCallSiteCC(
      M.getFunction("github.com/c2go_project/c2go_libc.EscapeCheck"));

  errs() << "c2go-escape-check: instrumented " << NumInstrumented
         << " heap/global pointer store(s) in " << M.getName() << "\n";
  return (NumInstrumented || SweepChanged) ? PreservedAnalyses::none()
                                           : PreservedAnalyses::all();
}
