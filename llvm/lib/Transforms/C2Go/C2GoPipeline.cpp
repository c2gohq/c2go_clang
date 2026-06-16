//===- C2GoPipeline.cpp - Shared c2go late-pipeline helpers ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoPipeline.h"

#include "llvm/Transforms/C2Go/C2GoGCSetup.h"
#include "llvm/Transforms/C2Go/C2GoLoopPoll.h"
#include "llvm/Transforms/C2Go/C2GoMemcpyTyping.h"
#include "llvm/Transforms/C2Go/C2GoSafepoint.h"
#include "llvm/Transforms/C2Go/C2GoWriteBarriers.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"

using namespace llvm;

void llvm::addC2GoLatePollPasses(ModulePassManager &MPM) {
  // LoopPoll only. Gosched() is a real cooperative safepoint — Layer 1's
  // RS4GC must wrap it in a gc.statepoint; replaying at Layer 3 (post-
  // RS4GC) would emit an un-wrapped GoABI0 call. See header.
  MPM.addPass(C2GoLoopPollPass());
}

void llvm::addC2GoLateLeafPasses(ModulePassManager &MPM) {
  // Both passes are leaf-safe and idempotent. They run AFTER the GC pass
  // pipeline at Layer 1 (so their own emitted runtime calls do not need
  // statepoint wrapping — the called shims carry `gc-leaf-function`), and
  // they replay verbatim at Layer 3 after the cross-TU inliner (#372).
  //
  // Order rationale:
  //   1. MemcpyTyping first — locks in `!c2go.elem.type` routing so any
  //      surviving raw memcpy/memmove that the inliner exposed becomes a
  //      typed runtime call (which `_c2go_writePtr` slow paths can
  //      coexist with).
  //   2. WriteBarriers second — its fast-path is guarded by
  //      `!c2go.wb.done` (#371) so a Layer 3 re-run skips stores it
  //      already split, and its slow-path call uses `_c2go_writePtr`
  //      which is declared `gc-leaf-function` (so RS4GC at Layer 1
  //      explicitly does NOT wrap the call, matching its leaf C
  //      implementation; the Layer 3 re-emission therefore needs no
  //      follow-up statepoint pass).
  MPM.addPass(C2GoMemcpyTypingPass());
  MPM.addPass(C2GoWriteBarriersPass());
}

void llvm::addC2GoLateGCPasses(ModulePassManager &MPM, bool UseStatepoint) {
  if (UseStatepoint) {
    MPM.addPass(C2GoGCSetupPass());
    MPM.addPass(RewriteStatepointsForGC());
    // #327: fold relocate-of-alloca back to the alloca so live stack
    // aggregates surface as Direct stackmap locations at every safepoint
    // (per-PC aggregate-field expansion in LowerSTATEPOINT).
    MPM.addPass(C2GoFoldAllocaRelocatesPass());
  } else {
    MPM.addPass(C2GoSafepointPass());
  }
}
