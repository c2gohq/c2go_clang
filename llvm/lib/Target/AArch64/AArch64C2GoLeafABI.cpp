//===- AArch64C2GoLeafABI.cpp - c2go NOSPLIT leaf eligibility ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AArch64-specific glue around the shared c2go NOSPLIT leaf eligibility
// analysis (Wave W Track A / #298). The full analysis + CC-flip worker lives
// in `llvm/lib/Transforms/C2Go/C2GoLeafEligibility.cpp`; this TU only:
//   1. supplies the AArch64 target hooks (frame cushion, NOSPLIT budget),
//   2. registers the legacy ModulePass `aarch64-c2go-leaf-abi`,
//   3. provides the NewPM `AArch64C2GoLeafABIPass` entry point.
//
// SAFETY / SCOPE (read before enabling):
//
//   The private register convention is ONLY safe when the eligible function is
//   emitted as a Go-assembler NOSPLIT TEXT. NOSPLIT is the enabler: a splittable
//   function gets an entry stackguard/morestack detour injected by `go tool
//   asm`, and morestack does NOT preserve R0-R15/F0-F15 (proven by abitest T-fi
//   / §二 of ABI_TEST_FINDINGS_2026-05-25.md). The matching NOSPLIT emission
//   lives in MCPlan9AsmStreamer (the .s TEXT directive), which is OUTSIDE the
//   AArch64 target. The leaf C2GoABIInternal optimization is mature
//   (#283 default-on, #297 NOSPLIT backstop); operators can disable via
//   `-mllvm -c2go-disable=leaf-abi` if a regression is reported.
//
// When enabled, the pass switches each eligible function AND every direct call
// site targeting it to C2GoABIInternal in lockstep, so the callee's
// LowerFormalArguments and the caller's LowerCall both select
// CC_AArch64_C2GoABIInternal (register passing). Boundary symbols, externally-
// visible functions, address-taken functions, and any function whose downstream
// subtree is not statically analyzable in-TU are left on GoABI0.
//
//===----------------------------------------------------------------------===//

#include "AArch64C2GoLeafABI.h"
#include "AArch64.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/C2Go/C2GoLeafEligibility.h"

#define DEBUG_TYPE "aarch64-c2go-leaf-abi"

using namespace llvm;

namespace {

/// Build the AArch64-specific target hooks for the shared eligibility
/// analysis. arm64 reserves saved-LR + saved-FP (16B linkage cushion) and
/// is an LR machine (callSize = 0 in `getNosplitBudget`).
llvm::c2go::LeafEligibilityTargetHooks makeAArch64Hooks(const Module &M) {
  llvm::c2go::LeafEligibilityTargetHooks Hooks;
  Hooks.FrameOf = [](const Function &F) -> uint64_t {
    return llvm::c2go::estimateLeafFrameBytes(F, /*LinkageCushionBytes=*/16);
  };
  Hooks.Budget =
      llvm::c2go::getNosplitBudgetForTriple(M.getTargetTriple().str());
  return Hooks;
}

// Shared worker for both legacy ModulePass and NewPM PassInfoMixin entry
// points (#379). Returns true iff the module was modified.
bool runC2GoLeafABIOnModule(Module &M) {
  auto Hooks = makeAArch64Hooks(M);
  return llvm::c2go::runC2GoLeafCCFlip(M, Hooks);
}

class AArch64C2GoLeafABI : public ModulePass {
public:
  static char ID;
  AArch64C2GoLeafABI() : ModulePass(ID) {
    initializeAArch64C2GoLeafABIPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override {
    return "AArch64 c2go leaf c2go-ABIInternal optimization";
  }

  bool runOnModule(Module &M) override { return runC2GoLeafABIOnModule(M); }
};

} // namespace

char AArch64C2GoLeafABI::ID = 0;

INITIALIZE_PASS(AArch64C2GoLeafABI, "aarch64-c2go-leaf-abi",
                "AArch64 c2go leaf c2go-ABIInternal optimization", false, false)

ModulePass *llvm::createAArch64C2GoLeafABIPass() {
  return new AArch64C2GoLeafABI();
}

PreservedAnalyses
llvm::AArch64C2GoLeafABIPass::run(Module &M, ModuleAnalysisManager &) {
  bool Changed = runC2GoLeafABIOnModule(M);
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
