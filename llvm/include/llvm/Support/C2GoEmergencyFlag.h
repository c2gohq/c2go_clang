//===- C2GoEmergencyFlag.h - c2go production-path emergency off -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #377: single `-mllvm -c2go-disable=<csv>` total switch that replaces
// the per-subsystem `cl::opt` toggles for mature default-ON production paths.
//
// Recognized tokens (case-sensitive):
//   - "statepoint-gc"      : clang BackendUtil RS4GC + GC strategy
//   - "ptrslot-liveness"   : AArch64 M5 per-PC pointer-slot liveness
//   - "spill-tags"         : AArch64 spill-tag pointer-map marking
//   - "leaf-abi"           : AArch64 leaf C2GoABIInternal optimization
//
// All four subsystems default ON. Production correctness depends on them; the
// emergency flag exists only so an operator can disable a single subsystem
// without a clang rebuild if a regression is reported in the field.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_C2GOEMERGENCYFLAG_H
#define LLVM_SUPPORT_C2GOEMERGENCYFLAG_H

#include "llvm/ADT/StringRef.h"

namespace llvm {
namespace c2go {

/// Returns true iff `Subsystem` appears in the `-c2go-disable=<csv>` list.
/// Subsystem names are matched verbatim against trimmed comma-separated
/// tokens. Empty list (the default) always returns false.
bool isC2GoDisabled(StringRef Subsystem);

} // namespace c2go
} // namespace llvm

#endif // LLVM_SUPPORT_C2GOEMERGENCYFLAG_H
