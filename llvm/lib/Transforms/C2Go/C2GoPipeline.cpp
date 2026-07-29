//===- C2GoPipeline.cpp - Shared c2go late-pipeline helpers ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoPipeline.h"

#include "llvm/Transforms/C2Go/C2GoEscapeCheck.h"
#include "llvm/Transforms/C2Go/C2GoGCSetup.h"
#include "llvm/Transforms/C2Go/C2GoLibCallRouting.h"
#include "llvm/Transforms/C2Go/C2GoLoopPoll.h"
#include "llvm/Transforms/C2Go/C2GoMemcpyTyping.h"
#include "llvm/Transforms/C2Go/C2GoSafepoint.h"
#include "llvm/Transforms/C2Go/C2GoWriteBarriers.h"
#include "llvm/Transforms/Scalar/RewriteStatepointsForGC.h"

using namespace llvm;

void llvm::addC2GoLatePollPasses(ModulePassManager &MPM) {
  // Gosched() is a real cooperative safepoint. The owning layer must run this
  // before GCSetup/RS4GC so the new call receives a complete gc-live set.
  MPM.addPass(C2GoLoopPollPass());
}

void llvm::addC2GoLateLeafPasses(ModulePassManager &MPM) {
  // Both passes are leaf-safe and idempotent. They run AFTER the GC pass
  // pipeline because their emitted runtime calls carry `gc-leaf-function`.
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
  //      implementation; no follow-up statepoint pass is needed).
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

void llvm::addC2GoLatePasses(ModulePassManager &MPM, bool UseStatepoint) {
  // Single source of truth for clang's non-LTO path and c2go-lto's post-link,
  // post-inliner path. Calls introduced above GCSetup are included in liveness.
  MPM.addPass(C2GoMemcpyTypingPass());
  MPM.addPass(C2GoLibCallRoutingPass());
  addC2GoLatePollPasses(MPM);
  addC2GoLateGCPasses(MPM, UseStatepoint);
  addC2GoLateLeafPasses(MPM);
  MPM.addPass(C2GoEscapeCheckPass());
}
