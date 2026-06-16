//===- C2GoLoopPoll.h - Cooperative loop preemption poll ------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #252 — cooperative loop poll. c2go-managed functions carry the Go
// `FuncFlagAsm` contract, which disables async preemption. A loop body with no
// real call therefore offers no cooperative safepoint and stalls GC for as
// long as the loop runs (memory: project_cooperative_preempt_morestack
// measured 8.5 s for a pure-arithmetic NoCallLoop). This pass injects a
// periodic `call void @runtime.Gosched()` into qualifying loops; the call is
// non-leaf, so its morestack prologue gives the goroutine a cooperative
// preemption point every M iterations. See docs/c2go_design.md §4.10.6.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOLOOPPOLL_H
#define LLVM_TRANSFORMS_C2GO_C2GOLOOPPOLL_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class C2GoLoopPollPass : public PassInfoMixin<C2GoLoopPollPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOLOOPPOLL_H
