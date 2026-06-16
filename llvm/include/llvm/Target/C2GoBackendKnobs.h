//===- C2GoBackendKnobs.h - c2go per-TM AArch64 backend flags ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #375 slice 1: thin shim that lets clang/c2go-lto (which cannot include
// backend-private `AArch64TargetMachine.h`) toggle the two AArch64-specific
// Plan-9 codegen knobs that used to live on the base `llvm::TargetMachine`:
//
//   * ForceBlockAddressJumpTable — AArch64TargetLowering::getJumpTableEncoding
//     returns EK_BlockAddress, and AArch64PassConfig::addPostBBSections skips
//     AArch64CompressJumpTables. Plan 9 .s DATA cannot represent label-diff
//     encodings. (#120)
//
//   * DisableRegisterCoalescing — AArch64PassConfig ctor calls
//     disablePass(&RegisterCoalescerID). The coalescer is unsound against
//     c2go's CSR_AArch64_NoRegs + go-asm-owned prologue frame contract; it
//     miscompiles sqlite3Parser. Root cause tracked in #310.
//
// Both knobs are AArch64-only and default false. The implementation lives in
// `AArch64TargetMachine.cpp` (which can legitimately include the private
// AArch64TargetMachine header) and uses `dyn_cast<AArch64TargetMachine>`, so
// passing a non-AArch64 TM is a benign no-op.
//
// Style mirrors `llvm/Support/C2GoEmergencyFlag.{h,cpp}`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TARGET_C2GOBACKENDKNOBS_H
#define LLVM_TARGET_C2GOBACKENDKNOBS_H

#include "llvm/TargetParser/Triple.h"

namespace llvm {

class TargetMachine;

namespace c2go {

/// Set the AArch64 `ForceBlockAddressJumpTable` knob on `TM`. No-op if `TM`
/// is not an `AArch64TargetMachine` (or subclass).
void setForceBlockAddressJumpTable(TargetMachine *TM, bool Enable);

/// Set the AArch64 `DisableRegisterCoalescing` knob on `TM`. No-op if `TM`
/// is not an `AArch64TargetMachine` (or subclass).
void setDisableRegisterCoalescing(TargetMachine *TM, bool Enable);

/// Set the AArch64 `DisableGlobalMerge` knob on `TM`. No-op if `TM` is not
/// an `AArch64TargetMachine` (or subclass). GlobalMerge erases per-GV
/// identities into a `_MergedGlobals` aggregate before MCPlan9AsmStreamer
/// can apply its go-owned classification, producing unsound NOPTR blobs
/// for pointer-bearing managed globals (#397).
void setDisableGlobalMerge(TargetMachine *TM, bool Enable);

/// c2go #435 — POD that bundles every c2go-specific backend knob a
/// downstream tool (clang/BackendUtil + c2go-lto) flips on its Plan-9
/// codegen TargetMachine. The three booleans match the existing
/// `set*` shims one-for-one; future ports add a new backend can grow
/// this struct without changing every call site.
struct BackendConfig {
  bool ForceBlockAddressJumpTable = false;
  bool DisableRegisterCoalescing = false;
  bool DisableGlobalMerge = false;
};

/// Backend-specific applier signature. The backend's own
/// LLVMInitialize<Target>Target() registers one of these per ArchType it
/// owns (e.g. aarch64 registers `applyAArch64C2GoConfig`).
using ApplyC2GoConfigFn = void (*)(TargetMachine *TM,
                                   const BackendConfig &Cfg);

/// Register an applier `Fn` for `Arch`. Multiple ArchType keys may share
/// one Fn (AArch64 registers all of aarch64 / aarch64_be / aarch64_32).
/// Multiple registrations for the same key OVERWRITE — `LLVMInitialize*`
/// calls are idempotent, so re-running them in unit tests should not
/// keep stale appliers around.
void registerC2GoBackendConfigHook(Triple::ArchType Arch,
                                   ApplyC2GoConfigFn Fn);

/// Dispatch to the applier registered for `TM`'s triple arch. No-op when
/// no applier is registered (call sites are allowed to pass non-c2go
/// TargetMachines without guarding the arch themselves).
void applyC2GoBackendConfig(TargetMachine *TM, const BackendConfig &Cfg);

} // namespace c2go
} // namespace llvm

#endif // LLVM_TARGET_C2GOBACKENDKNOBS_H
