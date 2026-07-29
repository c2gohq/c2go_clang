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
// The clang-driver LTO route has one strict phase boundary: per-TU clang runs
// the ordinary optimizer but defers this complete late sequence; c2go-lto
// links and inlines the pre-link IR, then runs the sequence exactly once on
// the combined module. The non-LTO clang path runs it once at OptimizerLast.
//
// This ordering is required for GC correctness. Running RS4GC per TU and then
// inlining can introduce an ordinary safepoint call into a function whose
// pointer liveness was already lowered. SelectionDAG then sees only the empty
// deopt bundle and emits a statepoint with no gc-live operands.
//
// The sequence is split into three reusable groups:
//
//   * Poll group:
//       C2GoLoopPollPass           — inject cooperative `Gosched()` polls.
//     Gosched() is a real cooperative safepoint, so this precedes GCSetup.
//
//   * Leaf group:
//       C2GoMemcpyTypingPass       — type raw memcpy/memmove via metadata
//                                    (already a leaf via `gc-leaf-function`
//                                    on the runtime.typedmemmove shim).
//       C2GoWriteBarriersPass      — hybrid managed-store write barriers
//                                    (idempotent: `!c2go.wb.done` guard,
//                                    #371). The slow-path `_c2go_writePtr`
//                                    shim is declared `gc-leaf-function`
//                                    (#NOSPLIT internally), so no follow-up
//                                    statepoint pass is required.
//
//   * GC group:
//       UseStatepoint == true :
//           C2GoGCSetupPass            — gc.statepoint setup
//           RewriteStatepointsForGC    — the upstream RS4GC pass
//           C2GoFoldAllocaRelocatesPass — fold relocate-of-alloca back
//                                          to alloca (#327)
//       UseStatepoint == false:
//           C2GoSafepointPass          — lightweight alloca-only path
// The full helper also includes the pre-GC MemcpyTyping + LibCallRouting and
// the final EscapeCheck. It must not run twice on the same statepoint module.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOPIPELINE_H
#define LLVM_TRANSFORMS_C2GO_C2GOPIPELINE_H

#include "llvm/IR/PassManager.h"

namespace llvm {

/// Add the c2go late pipeline's *poll* portion (LoopPoll) to \p MPM.
///
/// The injected `runtime.Gosched()` is a real cooperative safepoint, so this
/// group must precede the GC group in whichever layer owns late lowering.
void addC2GoLatePollPasses(ModulePassManager &MPM);

/// Add the c2go late pipeline's *leaf* portion (MemcpyTyping +
/// WriteBarriers) to \p MPM.
///
/// MemcpyTyping and WriteBarriers are idempotent. Both runtime shims they emit
/// are `gc-leaf-function`, so this group intentionally follows the GC group.
void addC2GoLateLeafPasses(ModulePassManager &MPM);

/// Add the c2go late pipeline's *GC* portion to \p MPM.
///
/// MUST NOT be called more than once on a given module: the underlying
/// RewriteStatepointsForGC pass asserts "repeat safepoint insertion is
/// not supported".
///
/// \p UseStatepoint selects between the SOUND statepoint pipeline
/// (true; #326 Stage III) and the lightweight alloca-only safepoint
/// pipeline (false; #307).
void addC2GoLateGCPasses(ModulePassManager &MPM, bool UseStatepoint);

/// Add the complete late sequence:
///   MemcpyTyping -> LibCallRouting -> LoopPoll -> GC -> leaf passes ->
///   EscapeCheck.
///
/// The non-LTO clang path runs it at OptimizerLast. The clang-driver LTO path
/// defers it to c2go-lto after whole-module inlining.
void addC2GoLatePasses(ModulePassManager &MPM, bool UseStatepoint);

} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOPIPELINE_H
