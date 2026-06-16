//===- C2GoGCSetup.h - Attach the c2go-gc GC strategy ----------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go-gc-setup: set `gc "c2go-gc"` on every function definition that
// references *any* pointer-typed value — argument, instruction result, or
// instruction operand — so RewriteStatepointsForGC will process it (RS4GC
// skips functions without an RS4GC GCStrategy). The widened predicate (any
// pointer, not just managed addrspace(1)) is required by the Go movable-stack
// model: copystack relocates the goroutine's whole stack, so every pointer
// living on it is a potential root and must be visible at safepoints. The
// called-operand of a call/invoke is excluded — it is a callee address, not a
// relocatable data pointer. Functions with no pointers at all are left
// untouched to avoid needless statepoint rewriting. Runs after
// C2GoWriteBarriers and immediately before RS4GC.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOGCSETUP_H
#define LLVM_TRANSFORMS_C2GO_C2GOGCSETUP_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class C2GoGCSetupPass : public PassInfoMixin<C2GoGCSetupPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

// c2go #327: fold `gc.relocate` of an alloca-derived pointer back to its
// original value. Runs AFTER RewriteStatepointsForGC. RS4GC relocates every
// live pointer, including pointers whose base is a stack alloca (the c2go-gc
// strategy makes an alloca its own base — see findBaseDefiningValue). For an
// alloca-derived pointer the relocate is the IDENTITY: the alloca address is
// `SP + const`, it never moves within the IR, and after a copystack the
// runtime relocates the alloca's stored pointer FIELDS via the locals bitmap
// (not the SSA value). By replacing the relocate result with the original
// alloca-derived pointer, the value rematerializes as a frame-index (`ADD $off,
// SP`) at every safepoint instead of being spilled as an opaque pointer — so
// LowerSTATEPOINT sees it as a Direct(SP, off) location and can expand the
// aggregate's pointer fields into the per-PC locals bitmap (the sound,
// liveness-driven replacement for the static all-PCs aggregate-field mask).
class C2GoFoldAllocaRelocatesPass
    : public PassInfoMixin<C2GoFoldAllocaRelocatesPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOGCSETUP_H
