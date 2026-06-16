//===- C2GoFrameEmitter.h - c2go (Plan 9) frame emission helpers -*- C++ -*===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #238 (Phase 1): extract the AArch64 c2go (Plan 9 / Go ABI0) prologue
// and epilogue emission helpers out of AArch64FrameLowering.cpp into a
// dedicated TU under namespace llvm::c2go.
//
// Phase 1 is a pure refactor: the four free helpers (isC2GoMode,
// c2goMakesRealCall, c2GoFrameSize, c2goMarkPtrFieldBits) and the two
// emitters (emitC2GoPrologue, emitC2GoEpilogue) are moved here verbatim.
// AArch64FrameLowering::emitPrologue / emitEpilogue dispatch to the c2go
// helpers via this header.
//
// See .build_status/issue238_design_draft.md and docs/c2go_design.md §4.11
// for the full design (Phase 2/3 will introduce a cached C2GoFrameInfo on
// AArch64FunctionInfo and collapse the streamer side-channel; not landed in
// Phase 1).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_C2GOFRAMEEMITTER_H
#define LLVM_LIB_TARGET_AARCH64_C2GOFRAMEEMITTER_H

#include <cstdint>

namespace llvm {
class MachineBasicBlock;
class MachineFunction;

namespace c2go {

/// c2go #238 (Phase 2): pure-function summary of the c2go frame decisions
/// that emitC2GoPrologue and its consumers need. Cached on
/// AArch64FunctionInfo so the underlying scans (real-call walk, frame-size
/// arithmetic) run at most once per MF.
///
/// Phase 2 scope (minimal): captures the two fields that were previously
/// recomputed within emitC2GoPrologue itself — FrameSize (already cached on
/// AArch64FunctionInfo as a uint64_t since #283, kept in sync here) and
/// MakesRealCall (previously walked twice: once inside c2GoFrameSize() and
/// once for the NOSPLIT-eligibility check). The remaining decisions
/// (NOSPLIT flag, ArgSize, ArgPtrMask, LocalsAggMask, LocalsAmbigMask) are
/// follow-ups; they stay inline in emit() for now because their byte-output
/// is path-sensitive (e.g. AmbigMask depends on alloca iteration order from
/// MFI, which is only safe to read post-PEI). See
/// .build_status/issue238_design_draft.md §3 Phase 2.
struct C2GoFrameInfo {
  /// (Locals + 16) align 16; 0 = true leaf, prologue/epilogue both no-op.
  uint64_t FrameSize = 0;

  /// Has a real BL/BLR (excludes STACKMAP/PATCHPOINT — #306/#326). Folded
  /// into both the frame-size decision and the NOSPLIT eligibility check.
  bool MakesRealCall = false;
};

/// True when the function lives in a c2go-mode module (module flag
/// `c2go.goabi` present).
bool isC2GoMode(const MachineFunction &MF);

/// Compute the c2go frame summary for MF. Pure function — no side effects
/// on MF / MFI / streamer state; safe to call multiple times. Callers
/// normally go through AArch64FunctionInfo::getOrComputeC2GoFI() which
/// caches the result.
C2GoFrameInfo computeC2GoFrameInfo(const MachineFunction &MF);

/// Emit the Go ABI0 (Plan 9) prologue when MF is in c2go mode.
/// Returns true when the c2go path handled prologue emission (the caller
/// must skip the standard AArch64PrologueEmitter); false otherwise.
bool emitC2GoPrologue(MachineFunction &MF, MachineBasicBlock &MBB);

/// Mirror of emitC2GoPrologue for the epilogue. Returns true when the c2go
/// path handled epilogue emission.
bool emitC2GoEpilogue(MachineFunction &MF, MachineBasicBlock &MBB);

} // namespace c2go
} // namespace llvm

#endif // LLVM_LIB_TARGET_AARCH64_C2GOFRAMEEMITTER_H
