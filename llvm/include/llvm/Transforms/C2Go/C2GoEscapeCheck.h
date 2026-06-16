//===- C2GoEscapeCheck.h - Dynamic stack->heap escape instrumentation -*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go-lto client B, DYNAMIC half (#289). The static counterpart is
// C2GoEscapeAudit (whole-program may-point-to). This pass is the runtime
// ground-truth probe: it instruments every `store ptr %v, ptr %dst` whose
// destination is not provably a stack slot (alloca) with a call to the runtime
// helper `__c2go_escape_check(%v, %dst, <site-id>)`. At run time the helper
// reads the current goroutine stack bounds [g.stack.lo, g.stack.hi) (via X28)
// and reports the store as a stack->heap escape iff %v lies inside the stack
// range and %dst does not. This catches the c2go-specific bug class where a
// stack address survives in a heap/global object across a copystack (Option 3
// only relocates pointers found *on* the stack — see design.md §4.3.1(b)).
//
// Gated by the `-c2go-escape-check` LLVM cl::opt (default OFF); the pass is a
// no-op unless explicitly enabled, so it never affects normal codegen.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOESCAPECHECK_H
#define LLVM_TRANSFORMS_C2GO_C2GOESCAPECHECK_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class C2GoEscapeCheckPass : public PassInfoMixin<C2GoEscapeCheckPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOESCAPECHECK_H
