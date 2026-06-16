//===- C2GoGCMaskUtils.h - Shared module_gcmask manifest helper *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #401(b) aux audit cleanup: shared helper that rebuilds the
// `module_gcmask.vars[]` manifest array from the `@c2go.global.gcmask.<var>`
// internal globals CodeGenModule::emitC2GoGlobalGCMask stamped into the
// module, plus the `!c2go.go_owned_globals` named-metadata (#389 / #387 §B4
// phase 6 sub-step 1). Used by both WF1 (clang/lib/CodeGen/CodeGenAction.cpp
// — `buildC2GoManifest`) and WF2 (llvm/tools/c2go-lto/c2go-lto.cpp — manifest
// rebuild after llvm-link); prior to #401(b) the two copies were character-
// for-character mirrors maintained by hand, with comments explicitly noting
// the duplication. Lifting to llvm/Transforms/C2Go is safe: clang/lib/CodeGen
// already links the LLVMC2Go component.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOGCMASKUTILS_H
#define LLVM_TRANSFORMS_C2GO_C2GOGCMASKUTILS_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/JSON.h"

#include <cstdint>

namespace llvm {
class Module;
class Type;

namespace c2go {

/// Rebuild the manifest's `module_gcmask.vars[]` array from the
/// `@c2go.global.gcmask.<var>` GVs in \p M, surfacing the `go_owned` bit for
/// any var named in the `!c2go.go_owned_globals` named-metadata. The result is
/// sorted by var name for deterministic manifest output.
json::Array collectGCMaskVarsFromModule(const Module &M);

/// c2go #432 — single shared aggregate pointer-field walker.
///
/// Recursively enumerate every pointer-typed field of \p Ty laid out at base
/// byte offset \p Base under \p DL, invoking \p Cb once per pointer field with
/// the field's absolute byte offset. Scalars contribute no callback. Used by:
///   - C2GoSafepoint::collectPointerFieldOffsets (append offset to vec)
///   - C2GoGCSetup::collectPtrFieldOffsets       (append offset to vec)
///   - AArch64 C2GoFrameEmitter::c2goMarkPtrFieldBits (OR bit into bitmap,
///     honouring a per-call SkipBytes set computed by the caller)
///
/// Each former local walker was a character-for-character copy of the others;
/// keeping them in lock-step by hand was a foot-gun (one site silently
/// diverging from another flips which pointer-field words get marked / null-
/// inited / scanned).
///
/// #455(a): the pointer-Type parameter that used to ride alongside Off was
/// unused at every callsite (all three drop it on the `Type *` placeholder),
/// so the callback signature is narrowed to `void(uint64_t)`. The walker
/// still uses `Ty->isPointerTy()` internally to terminate recursion; the
/// pointer Type itself is just never propagated outward.
inline void walkPointerFields(Type *Ty, uint64_t Base, const DataLayout &DL,
                              function_ref<void(uint64_t Off)> Cb) {
  if (Ty->isPointerTy()) {
    Cb(Base);
    return;
  }
  if (auto *ST = dyn_cast<StructType>(Ty)) {
    const StructLayout *SL = DL.getStructLayout(ST);
    for (unsigned I = 0, N = ST->getNumElements(); I < N; ++I)
      walkPointerFields(ST->getElementType(I),
                        Base + SL->getElementOffset(I), DL, Cb);
    return;
  }
  if (auto *AT = dyn_cast<ArrayType>(Ty)) {
    Type *ET = AT->getElementType();
    uint64_t ESz = DL.getTypeAllocSize(ET).getFixedValue();
    for (uint64_t I = 0, N = AT->getNumElements(); I < N; ++I)
      walkPointerFields(ET, Base + I * ESz, DL, Cb);
    return;
  }
  // Scalars (int/float/etc.) contribute no pointer word.
}

} // namespace c2go
} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOGCMASKUTILS_H
