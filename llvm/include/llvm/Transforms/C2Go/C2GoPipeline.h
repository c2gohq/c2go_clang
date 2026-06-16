//===- C2GoPipeline.h - Shared c2go late-pipeline helpers -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared helpers that assemble the c2go "late" IR pass sequence so that
// both Layer 1 (BackendUtil OptimizerLastEP, inside clang -cc1) and
// Layer 3 (c2go-lto post-inliner) emit the same pass order without
// copy-pasting `MPM.addPass(...)` calls.
//
// The late pipeline is split into three groups so Layer 3 (c2go-lto post-
// inliner replay) can pick the subset that is *safe* to re-run on already
// post-RS4GC bitcode (round 24 / GPT round 5 refinement of #372):
//
//   * Poll group (Layer 1 ONLY; NOT replayable):
//       C2GoLoopPollPass           — inject cooperative `Gosched()` polls.
//     Gosched() is a real cooperative safepoint (callee may GC); the
//     statepoint that wraps it is laid down by RS4GC at Layer 1. Replaying
//     LoopPoll at Layer 3 (post-RS4GC) would emit a bare GoABI0 call
//     without a statepoint wrap — GC blind spot. See round 24.
//
//   * Leaf group (Layer 1 AS LATE HALF; Layer 3 replays):
//       C2GoMemcpyTypingPass       — type raw memcpy/memmove via metadata
//                                    (already a leaf via `gc-leaf-function`
//                                    on the runtime.typedmemmove shim).
//       C2GoWriteBarriersPass      — hybrid managed-store write barriers
//                                    (idempotent: `!c2go.wb.done` guard,
//                                    #371). The slow-path `_c2go_writePtr`
//                                    shim is declared `gc-leaf-function`
//                                    (#NOSPLIT internally), so RS4GC will
//                                    NOT wrap newly inlined calls — safe
//                                    to re-emit at Layer 3 without a
//                                    statepoint pass.
//
//   * GC group (Layer 1 ONLY; NOT replayable):
//       UseStatepoint == true :
//           C2GoGCSetupPass            — gc.statepoint setup
//           RewriteStatepointsForGC    — the upstream RS4GC pass
//           C2GoFoldAllocaRelocatesPass — fold relocate-of-alloca back
//                                          to alloca (#327)
//       UseStatepoint == false:
//           C2GoSafepointPass          — lightweight alloca-only path
//
// Layer 3 (c2go-lto) deliberately does NOT call addC2GoLatePollPasses or
// addC2GoLateGCPasses after the inliner: BackendUtil already ran RS4GC at
// OptimizerLastEP inside the per-TU clang -cc1, so the combined bitcode is
// already in post-RS4GC form. RewriteStatepointsForGC has a hard assert
// "repeat safepoint insertion is not supported" — re-running it would
// crash. Newly inlined GoABI0 calls (including write-barrier slow paths)
// are still required to be wrapped in statepoints; that wrap happened in
// BackendUtil before the calls were inlined here, so the statepoint
// structure around them is preserved by ordinary inlining. See #372.
//
// Round 23 regression (commit af15fda9) tried to move WriteBarriers OUT of
// the Layer 3 replay; -O2 SQLite then crashed in `adjustpointers` because
// the cross-TU inliner exposed stores in newly-merged functions that no
// pass typed as write-barrier slow paths. WriteBarriers MUST replay at
// Layer 3 to catch those — GPT round 5 concession. See round 24 prompt.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOPIPELINE_H
#define LLVM_TRANSFORMS_C2GO_C2GOPIPELINE_H

#include "llvm/IR/PassManager.h"

namespace llvm {

/// Add the c2go late pipeline's *poll* portion (LoopPoll) to \p MPM.
///
/// Layer 1 only: the injected `runtime.Gosched()` is a real cooperative
/// safepoint and Layer 1's RS4GC wraps it in a `gc.statepoint`. Replaying
/// at Layer 3 (post-RS4GC) would emit an un-wrapped GoABI0 call — GC
/// blind spot. See file header.
void addC2GoLatePollPasses(ModulePassManager &MPM);

/// Add the c2go late pipeline's *leaf* portion (MemcpyTyping +
/// WriteBarriers) to \p MPM.
///
/// Safe to call multiple times on the same module: every pass is
/// idempotent (MemcpyTyping rewrites already carry typed runtime calls;
/// WriteBarriers fast-path stores carry `!c2go.wb.done`). Both runtime
/// shims they emit are `gc-leaf-function` (typed memmove via attribute on
/// the runtime declaration; `_c2go_writePtr` via attribute set on the
/// declaration in C2GoWriteBarriersPass), so RS4GC will not wrap them —
/// safe to re-emit at Layer 3 without a follow-up statepoint pass.
///
/// Used by:
///   * Layer 1 (BackendUtil OptimizerLastEP), AFTER the GC pipeline as
///     the late half of the post-RS4GC sequence.
///   * Layer 3 (c2go-lto), once after the cross-TU inliner. See #372.
void addC2GoLateLeafPasses(ModulePassManager &MPM);

/// Add the c2go late pipeline's *GC* portion to \p MPM.
///
/// MUST NOT be called more than once on a given module: the underlying
/// RewriteStatepointsForGC pass asserts "repeat safepoint insertion is
/// not supported".
///
/// Used only by Layer 1 (BackendUtil OptimizerLastEP); c2go-lto skips
/// this half — see file header for rationale.
///
/// \p UseStatepoint selects between the SOUND statepoint pipeline
/// (true; #326 Stage III) and the lightweight alloca-only safepoint
/// pipeline (false; #307).
void addC2GoLateGCPasses(ModulePassManager &MPM, bool UseStatepoint);

} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOPIPELINE_H
