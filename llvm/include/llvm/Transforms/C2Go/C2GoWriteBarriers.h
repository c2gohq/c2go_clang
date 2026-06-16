//===- C2GoWriteBarriers.h - Write-barrier insertion --*-C++-*-============//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go-write-barriers LLVM pass.
//
// For every `store ptr %new, ptr %slot` in c2go-mode IR where the
// store deposits a Go-heap pointer into Go-heap memory (i.e. a
// managed write), the pass rewrites the store to:
//
//   %enabled = load i32, ptr @runtime.writeBarrier
//   %cond    = icmp ne i32 %enabled, 0
//   br i1 %cond, label %wb_slow, label %wb_fast
// wb_fast:
//   store ptr %new, ptr %slot
//   br label %wb_done
// wb_slow:
//   call void @_c2go_writePtr(ptr %slot, ptr %new)   ; GoABI0 call
//   br label %wb_done
// wb_done:
//
// See docs/c2go_design.md §7.
//
// v0 implementation: rewrites every plain `store ptr addrspace(1) %v,
// ptr addrspace(1) %slot` whose destination is not a local alloca into
// the enabled-check + slow/fast diamond above. Volatile and atomic stores
// are left alone (the slow path is a GoABI0 call, so ordering is not
// preservable without a matching atomic shim). The fast-path store is
// tagged with `!c2go.wb.done` so the pass is idempotent across the two
// pipeline insertion points (BackendUtil clang pipeline + c2go-lto
// post-inliner re-run; see #371 / #372).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOWRITEBARRIERS_H
#define LLVM_TRANSFORMS_C2GO_C2GOWRITEBARRIERS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class C2GoWriteBarriersPass : public PassInfoMixin<C2GoWriteBarriersPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOWRITEBARRIERS_H
