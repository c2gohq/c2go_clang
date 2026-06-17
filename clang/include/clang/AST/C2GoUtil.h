//===--- C2GoUtil.h - c2go shared AST helpers -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go §A3 — shared helpers used by Sema and CodeGen to (a) decide whether an
// anonymous record is "interesting" to the Go side (contains a managed
// pointer transitively) and (b) synthesize a stable, location-keyed name for
// such an anonymous record so the typeinfo global, the `c2go.elem.type`
// metadata key, and the Go-binding emission all agree on the same string.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_C2GOUTIL_H
#define LLVM_CLANG_AST_C2GOUTIL_H

#include "clang/AST/Type.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdint>
#include <string>

namespace clang {

class ASTContext;
class FieldDecl;
class RecordDecl;

namespace c2go {

/// Returns true when \p Ty is a *managed* pointer in the c2go sense. The
/// question is asked BEFORE canonicalization — `getCanonicalType()` strips
/// the `AttributedType` sugar that an attr-only managed pointer
/// (`int * __attribute__((c2go_managed))`) relies on for round 21 / option A
/// — so callers must NOT canonicalize \p Ty first.
///
/// A pointer is managed iff any of these holds:
///   * its type is wrapped in an `AttributedType` whose attr-kind is
///     `attr::C2GoManaged` (option A, round 22 sugar); OR
///   * its pointee is a record (struct/union) carrying `C2GoStructAttr`
///     (signal (ii) — pointer-to-managed-struct propagates managedness).
///
/// All non-pointer types, null types, and pointer-to-non-record-non-attr
/// types return false. This is the single source of truth used by Sema
/// (`Sema::c2goTypeIsManagedPtr`), CGC2GoTypeInfo's bitmap walkers, and
/// the recordContainsManagedPointer transitivity check below.
bool isManagedPointerType(QualType Ty);

/// Returns true when \p RD contains a GC-managed pointer transitively (walking
/// nested records and arrays). The decision honors explicit
/// `c2go_managed`/`c2go_unmanaged` annotations on fields; in the absence of an
/// explicit marker a raw pointer field defaults to "managed" only when the
/// surrounding record itself is a c2go record (has C2GoStructAttr) — that
/// matches the existing pipeline default that pointer-typed fields inside a
/// c2go-tracked record are GC-traced. v15: a bare unannotated pointer in an
/// *unmarked* outer record defaults to UNMANAGED.
bool recordContainsManagedPointer(const RecordDecl *RD);

/// c2go §A4 — automatic scheme classification for a `union` record.
///
/// Unions cannot be described by a single static GC bitmap when different
/// alternatives put pointers and scalars at overlapping byte offsets — the
/// GC would either trace garbage (scalar misread as pointer) or miss real
/// references (pointer slot scanned as scalar). We pick between two
/// representation schemes at compile time based on a per-offset analysis of
/// the union's alternatives:
///
///   * Scheme1 ("opaque slab + single ptr slot"): all managed-pointer
///     alternatives sit at exactly one byte offset N, and no scalar
///     alternative overlaps that offset. The union is emitted as opaque
///     storage with a one-word GC bitmap bit set at offset N. The Go side
///     sees a typed pointer slot the GC can scan without ambiguity.
///
///   * Scheme2 ("type-punned pointer slot"): a scanned pointer coexists with
///     scalars at the same offset, or scanned pointers live at multiple
///     distinct offsets. The static-bitmap encoding cannot represent this.
///     There is no boxing / zero-bit fallback — the boxing scaffolding was
///     deleted. We report Scheme2 and the caller hard-errors (instructing
///     the user to give the pointer its own pure slot, change the punned
///     member to uintptr_t, or mark the union `__attribute__((c2go_variant))`).
///
///   * NotApplicable: \p UnionRD is not a union, or it contains no managed
///     pointer alternative (a pure-scalar union needs no special treatment;
///     the surrounding struct's bitmap simply has no bits in that span).
enum class C2GoUnionScheme { Scheme1, Scheme2, NotApplicable };

/// Result of classifyC2GoUnion. When \c Scheme is Scheme1 the
/// \c PointerOffsetBytes field reports the single byte offset at which all
/// managed pointer alternatives sit; the typeinfo emitter uses it to mark
/// one bit in the GC bitmap. For Scheme2 the \c BlockerFieldName /
/// \c BlockerReason describe one of the alternatives that prevented Scheme1
/// — surfaced in the diagnostic so users can refactor the offending field.
struct C2GoUnionClassification {
  C2GoUnionScheme Scheme = C2GoUnionScheme::NotApplicable;
  uint64_t PointerOffsetBytes = 0;
  std::string BlockerFieldName;
  std::string BlockerReason;
};

/// Walk \p UnionRD's alternatives (recursing into anonymous structs that
/// appear as direct alternatives) and decide between Scheme1, Scheme2, or
/// NotApplicable. The classification mirrors the rules in the doc-comment
/// on \c C2GoUnionScheme.
C2GoUnionClassification classifyC2GoUnion(const RecordDecl *UnionRD,
                                          const ASTContext &Ctx);

/// c2go §3.9 / task T3 — convert-to-struct layout for a union marked
/// `__attribute__((c2go_variant))`.
///
/// A `c2go_variant` union promises (the soundness contract) that it only ever
/// holds one alternative at a time and never type-puns a pointer byte with a
/// scalar byte. That lets us re-describe the union as a *struct* whose slots
/// are partitioned by GC class, so the Go GC can scan the pointer slots
/// precisely instead of force-scanning ambiguous overlapped bytes (which would
/// trip `invalidptr`).
///
/// Slot-allocation algorithm (T3b by-signature regions; see
/// C2GoUtil.cpp::computeC2GoVariantLayout for the full description):
///   * Each direct alternative is classified by its NATURAL layout into a
///     signature = (footprint, sorted set of pointer-word offsets). The walk
///     descends into nested structs and arrays (so a `struct{int tag; Foo*p;}`
///     alternative keeps `p` at its real offset and earns a precise scan word)
///     and treats function pointers as no-scan (§3.4).
///   * Alternatives with the same signature overlay into ONE shared region;
///     distinct signatures get distinct, non-overlapping regions concatenated
///     in first-appearance order. Pure-no-pointer alternatives all share the
///     empty-signature region (the largest one's footprint wins — the historic
///     "scalar/funcptr blob"). A flat bare-pointer alternative has signature
///     {ptr-word 0}, so all such members overlay into one pointer slot — the
///     historic flat behavior is the degenerate case of this generalization.
///   * Within a region we keep the alternative's natural layout: Ptr slots at
///     its pointer-word offsets, Scalar slots tiling the gaps. The slot list is
///     thus an ordered, gap-free cover of the converted struct.
///   * No byte is ever "pointer in one alternative, scalar in another":
///     distinct regions never overlap, and within a region a word that any
///     overlaid alternative scans is scanned for all — a disagreement (nested
///     union punning a scan pointer with a scalar) or a misaligned/VLA member
///     is UNREPRESENTABLE and sets \c Representable=false (the Sema fail-closed
///     guard then rejects the union).
struct C2GoVariantSlot {
  enum class Kind { Ptr, Scalar } Kind; // Ptr -> unsafe.Pointer; Scalar -> uint8[]
  uint64_t StructOffsetBytes;            // byte offset within the converted struct
  uint64_t SizeBytes;                    // slot size (PtrSize for Ptr, blob size for Scalar)
};

struct C2GoVariantLayout {
  bool Valid = false;          // false if \p UnionRD is not a variant union
  bool HasManagedPtr = false;  // true if any alternative is a managed pointer
  bool HasScanPtr = false;     // true if any scan-pointer (managed or unmanaged) slot exists
  // T3b soundness: true iff every alternative has an unambiguous, precisely
  // scannable layout. False when some alternative is unrepresentable — a nested
  // union that puns a scan pointer with a scalar at the same word, a
  // pointer at a non-pointer-aligned (e.g. packed) offset, or a flexible/VLA
  // member. The Sema fail-closed guard rejects such unions (a precise static
  // bitmap cannot describe them without dropping or over-scanning a pointer
  // word). \c BlockerFieldName names one offending member for the diagnostic.
  bool Representable = true;
  std::string BlockerFieldName;
  uint64_t SizeBytes = 0;      // total size of the converted struct
  uint64_t AlignBytes = 0;     // alignment of the converted struct
  llvm::SmallVector<C2GoVariantSlot, 4> Slots;

  // For each *field* of the union (by FieldDecl* identity is impractical here;
  // we key by the field's original union byte offset + a per-field probe), the
  // access redirector needs the converted-struct byte offset. FieldOffsets maps
  // the field declaration index (RD->fields() order) to its struct offset.
  llvm::SmallVector<uint64_t, 8> FieldStructOffsets;
};

/// Returns true when \p RD is a union carrying `C2GoVariantAttr`.
bool isC2GoVariantUnion(const RecordDecl *RD);

/// Returns true when \p RD is (directly or through nested anonymous structs)
/// declared lexically inside a `c2go_variant` union — i.e. it is one of the
/// union's converted-to-struct alternatives. Such a record's pointer fields are
/// scanned precisely by the variant convert-to-struct layout, so the
/// "pointer-to-c2go_struct inside a plain record" warning is a false positive
/// for them and must be suppressed. A genuinely top-level plain record (whose
/// DeclContext is the TU or a function) is never matched.
bool isInsideC2GoVariantUnion(const RecordDecl *RD);

/// Compute the convert-to-struct layout for a `c2go_variant` union. Returns a
/// layout with Valid=false when \p UnionRD is not a variant union.
C2GoVariantLayout computeC2GoVariantLayout(const RecordDecl *UnionRD,
                                           const ASTContext &Ctx);

/// Returns a usable name for \p RD that is stable across translation units:
///   1. \c RD->getName() when the record has an identifier.
///   2. The typedef name from `getTypedefNameForAnonDecl()` when the record is
///      a `typedef struct {...} T` style declaration.
///   3. Otherwise a synthesized `c2go.anon.<hash>` name derived from the
///      record's spelling location (filename + line + column). Same source
///      location across TUs produces the same hash, so linkonce_odr dedupes.
///
/// The returned name never contains slashes or shell-special characters; it
/// is safe to embed verbatim into LLVM global names.
std::string getStableRecordName(const RecordDecl *RD, const ASTContext &Ctx);

} // namespace c2go
} // namespace clang

#endif // LLVM_CLANG_AST_C2GOUTIL_H
