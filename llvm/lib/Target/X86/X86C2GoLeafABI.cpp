//===- X86C2GoLeafABI.cpp - c2go NOSPLIT leaf eligibility (X86) -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// X86 backend hook into the cross-target c2go NOSPLIT leaf eligibility
// analysis (Wave W Track A → Wave X Track A / #298 — real reg-pass landed).
// The reusable algorithm lives in
// `llvm/lib/Transforms/C2Go/C2GoLeafEligibility.cpp`; this TU only:
//   1. supplies the X86 target hooks (frame cushion, NOSPLIT budget),
//   2. registers the legacy ModulePass `x86-c2go-leaf-abi`,
//   3. provides the NewPM `X86C2GoLeafABIPass` entry point.
//
// The leaf CC flip is now POWERED ON for X86 (#298): it is gated only by
// `c2go.goabi`, exactly like the AArch64 backend. The former second gate
// `c2go.x86-leaf-abi` has been removed. The emergency off-switch is the
// shared `-mllvm -c2go-disable=leaf-abi` flag (isC2GoDisabled("leaf-abi"),
// checked inside the shared worker), mirroring AArch64.
//
// SAFETY / SCOPE — read before relying on this for production codegen:
//
//   The CC flip emitted here switches eligible callees + their direct call
//   sites to `CallingConv::C2GoABIInternal`. Wave X Track A landed the
//   matching x86-64 `CC_X86_64_C2GoABIInternal_TD` register-pass table in
//   X86CallingConv.{td,cpp} so the ISel + DAG layers are now end-to-end
//   sound for the supported scalar / aggregate type set. The earlier
//   "graceful loud" early-return (Wave W Track C A5) that refused to flip
//   when the flag was ON has been removed because the backend assert it
//   guarded against no longer exists.
//
//   The flip is now POWERED ON (default ON, gated only by `c2go.goabi`),
//   mirroring the AArch64 backend. The hard invariant the NOSPLIT decision
//   in X86C2GoFrameEmitter enforces is: a register-ABI (C2GoABIInternal)
//   function MUST be emitted NOSPLIT, because morestack does NOT preserve
//   incoming argument registers — a splittable register-ABI function would
//   lose its register args across copystack.
//
//   Production parity:
//     • `c2go.goabi` OFF → no new code path is reached;
//     • `c2go.goabi` ON → eligible in-TU near-leaves flip to
//       C2GoABIInternal (register passing) and are emitted NOSPLIT.
//   Emergency off-switch: `-mllvm -c2go-disable=leaf-abi`
//   (isC2GoDisabled("leaf-abi"), honored by the shared worker), exactly
//   as on AArch64.
//
//===----------------------------------------------------------------------===//

#include "X86C2GoLeafABI.h"
#include "X86.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/C2Go/C2GoLeafEligibility.h"

#define DEBUG_TYPE "x86-c2go-leaf-abi"

using namespace llvm;

namespace {

/// Build the X86-specific target hooks for the shared eligibility analysis.
/// amd64 / i386 are non-LR machines; the saved-RA + saved-BP linkage cushion
/// is 16B on amd64 (caller pushes 8B return addr, prologue pushes saved-BP
/// 8B). i386 also receives the same conservative 16B cushion — over-counting
/// is safe (only makes the analysis more conservative; linker's nosplit pass
/// is the hard backstop). NOSPLIT budget is computed from the target triple
/// via the shared `getNosplitBudgetForTriple` (which maps x86_64 -> "amd64",
/// x86 -> "386" and applies callSize 8 / 4 accordingly).
llvm::c2go::LeafEligibilityTargetHooks makeX86Hooks(const Module &M) {
  llvm::c2go::LeafEligibilityTargetHooks Hooks;
  Hooks.FrameOf = [](const Function &F) -> uint64_t {
    return llvm::c2go::estimateLeafFrameBytes(F, /*LinkageCushionBytes=*/16);
  };
  Hooks.Budget =
      llvm::c2go::getNosplitBudgetForTriple(M.getTargetTriple().str());
  return Hooks;
}

bool runX86C2GoLeafABIOnModule(Module &M) {
  // X86 is a no-op on non-X86 modules — defensive (this pass is only added
  // by X86PassConfig, but the legacy + NewPM entries are callable directly).
  Triple T(M.getTargetTriple());
  if (!T.isX86())
    return false;

  // #298: the leaf CC flip is POWERED ON for X86. The former second gate
  // `c2go.x86-leaf-abi` (Wave W/X contract-retention dead code) has been
  // removed — the shared worker is now gated only by `c2go.goabi`, exactly
  // like the AArch64 backend (which also passes ExtraGate=""). The
  // `CC_X86_64_C2GoABIInternal_TD` reg-pass table in X86CallingConv.{td,cpp}
  // lowers C2GoABIInternal soundly, and X86C2GoFrameEmitter forces NOSPLIT
  // on every flipped function (the register-ABI ⇒ NOSPLIT invariant).
  // Emergency off-switch: `-mllvm -c2go-disable=leaf-abi`, honored inside
  // the shared worker.
  auto Hooks = makeX86Hooks(M);
  return llvm::c2go::runC2GoLeafCCFlip(M, Hooks);
}

class X86C2GoLeafABI : public ModulePass {
public:
  static char ID;
  X86C2GoLeafABI() : ModulePass(ID) {
    initializeX86C2GoLeafABIPass(*PassRegistry::getPassRegistry());
  }
  StringRef getPassName() const override {
    return "X86 c2go leaf c2go-ABIInternal optimization";
  }
  bool runOnModule(Module &M) override {
    return runX86C2GoLeafABIOnModule(M);
  }
};

} // namespace

char X86C2GoLeafABI::ID = 0;

INITIALIZE_PASS(X86C2GoLeafABI, "x86-c2go-leaf-abi",
                "X86 c2go leaf c2go-ABIInternal optimization",
                false, false)

ModulePass *llvm::createX86C2GoLeafABIPass() { return new X86C2GoLeafABI(); }

PreservedAnalyses
llvm::X86C2GoLeafABIPass::run(Module &M, ModuleAnalysisManager &) {
  bool Changed = runX86C2GoLeafABIOnModule(M);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
