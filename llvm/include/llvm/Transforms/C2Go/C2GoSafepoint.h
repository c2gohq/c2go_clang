//===- C2GoSafepoint.h - Insert llvm.experimental.stackmap at Go safepoints =//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go-safepoint LLVM pass.
//
// Go's garbage collector scans the stack at safepoints to find live managed
// pointers. Functions compiled in c2go-mode do not natively carry any
// stack-map information, so any managed pointer living on the C stack is
// invisible to the GC and at risk of being freed out from under us.
//
// This pass injects `@llvm.experimental.stackmap` intrinsic calls
// IMMEDIATELY BEFORE every call to a Go-runtime safepoint-bearing helper
// (mallocgc, typedmemmove, gcWriteBarrier, ...). The operands of each
// stackmap call are the function's `!c2go.ptr.managed`-tagged allocas (see
// §B2 in clang/lib/CodeGen/CGDecl.cpp), so the LLVM backend records the
// live spill-slot list for each safepoint PC into the
// `__llvm_stackmaps` section.
//
// A later post-process step (§B3.2) converts that section into Go-native
// `pcdata` / `funcdata` so the runtime GC stack walker can decode it.
// This pass is intentionally lightweight: it does NOT switch to
// gc.statepoint, does NOT change address-space attributes, and does NOT
// require a custom GCStrategy.
//
// Each stackmap intrinsic carries a module-monotonic i64 ID so the
// post-process tool can index it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOSAFEPOINT_H
#define LLVM_TRANSFORMS_C2GO_C2GOSAFEPOINT_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class C2GoSafepointPass : public PassInfoMixin<C2GoSafepointPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOSAFEPOINT_H
