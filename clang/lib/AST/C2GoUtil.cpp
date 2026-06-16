//===--- C2GoUtil.cpp - c2go shared AST helpers ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/AST/C2GoUtil.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/Type.h"
#include "clang/Basic/AddressSpaces.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

namespace clang {
namespace c2go {

bool isManagedPointerType(QualType Ty) {
  // IMPORTANT: do NOT canonicalize. `getCanonicalType()` strips the
  // `AttributedType` sugar that option A (round 22) uses to express the
  // attr-only managed pointer `int * __attribute__((c2go_managed))`. The
  // sugar wraps the pointer type itself, not the pointee, and is the only
  // way to tell an attr-only managed `int *` from a plain `int *` after
  // type-construction.
  if (Ty.isNull() || !Ty->isPointerType())
    return false;
  // Signal (i): the pointer is wrapped in an AttributedType whose
  // attr-kind is c2go_managed (round 22 option A sugar). Sema's
  // HandleC2GoManagedPointerTypeAttr also stamps target AS=1 on the
  // pointee — round 24 multi-AS conflict check guards that side, but
  // the canonical decision here only depends on the attr presence.
  for (const AttributedType *AT = Ty->getAs<AttributedType>();
       AT; AT = AT->getModifiedType()->getAs<AttributedType>())
    if (AT->getAttrKind() == attr::C2GoManaged)
      return true;
  // Signal (ii): pointer-to-c2go_struct. The pointee record carries
  // C2GoStructAttr — the §3.5 D1/D2/D3 v15 default that managedness
  // propagates through "pointer to a managed struct".
  QualType Pointee = Ty->getPointeeType();
  if (Pointee.isNull())
    return false;
  if (const RecordType *RT = Pointee->getAs<RecordType>())
    if (const RecordDecl *RD = RT->getDecl())
      return RD->hasAttr<C2GoStructAttr>();
  return false;
}

namespace {

// Walk a QualType looking for a managed pointer. Recurses through array
// elements and nested records. `Visited` guards against cyclic records (rare
// but possible when a pointer-to-self walks back into the same RD; we stop
// at the *pointer* boundary because a pointer is the leaf question itself).
static bool typeContainsManagedPointer(QualType T,
                                       bool ParentIsC2Go,
                                       llvm::SmallPtrSetImpl<const RecordDecl *> &Visited) {
  if (T.isNull())
    return false;

  // Peel arrays — element type is what carries the GC payload.
  while (const ArrayType *AT = T->getAsArrayTypeUnsafe()) {
    T = AT->getElementType();
    if (T.isNull())
      return false;
  }

  // A pointer: the leaf question. With no explicit field attribute, the
  // pointer is "managed" iff it carries the round 22 attr-only sugar OR
  // its pointee is a c2go_struct (signal (ii)). The shared helper
  // checks both signals before canonicalization.
  if (T->isPointerType())
    return isManagedPointerType(T);

  // A nested record: recurse into its fields. Reuse the field-aware walk
  // because field-level attributes apply.
  if (const RecordType *RT = T->getAs<RecordType>()) {
    const RecordDecl *Inner = RT->getDecl();
    if (!Inner)
      return false;
    Inner = Inner->getDefinition();
    if (!Inner)
      return false;
    if (!Visited.insert(Inner).second)
      return false;

    // For a nested record, "ParentIsC2Go" propagates from the outer-most
    // record into the children — that mirrors auto-managed semantics.
    for (const FieldDecl *F : Inner->fields()) {
      if (F->hasAttr<C2GoUnmanagedAttr>())
        continue;
      if (F->getType()->isPointerType()) {
        if (F->hasAttr<C2GoManagedAttr>())
          return true;
        // Shared helper checks both the round 22 AttributedType sugar and
        // the pointer-to-c2go_struct signal (ii) — do NOT canonicalize.
        if (isManagedPointerType(F->getType()))
          return true;
        continue;
      }
      if (typeContainsManagedPointer(F->getType(),
                                     ParentIsC2Go ||
                                         Inner->hasAttr<C2GoStructAttr>(),
                                     Visited))
        return true;
    }
    return false;
  }

  return false;
}

} // namespace

bool recordContainsManagedPointer(const RecordDecl *RD) {
  if (!RD)
    return false;
  RD = RD->getDefinition();
  if (!RD)
    return false;

  llvm::SmallPtrSet<const RecordDecl *, 8> Visited;
  Visited.insert(RD);
  const bool OuterIsC2Go = RD->hasAttr<C2GoStructAttr>();
  for (const FieldDecl *F : RD->fields()) {
    // Explicit annotations win.
    if (F->hasAttr<C2GoUnmanagedAttr>())
      continue;
    if (F->hasAttr<C2GoManagedAttr>()) {
      if (F->getType()->isPointerType())
        return true;
      // managed on a non-pointer is meaningless for this question; fall
      // through to the recursive walk for nested records.
    }

    QualType T = F->getType();
    // Peel arrays so a `void *p[8]` counts as a pointer field. Peel only
    // the array layers; do NOT call getCanonicalType() — that would strip
    // the round 22 AttributedType sugar from a `T * c2go_managed` element.
    while (const ArrayType *AT = T->getAsArrayTypeUnsafe())
      T = AT->getElementType();

    if (T->isPointerType()) {
      // v15: a bare (unannotated) pointer field defaults to UNMANAGED. It
      // counts as a managed-ptr field only when it carries the round 22
      // attr-only sugar OR points to an already-managed struct (signal
      // (ii)). The Ptr-bit default is applied as an explicit
      // C2GoManagedAttr upstream (AddPragmaC2GoAttribute step 1), caught
      // by the C2GoManagedAttr check above.
      if (isManagedPointerType(T))
        return true;
      continue;
    }

    if (typeContainsManagedPointer(T, /*ParentIsC2Go=*/OuterIsC2Go, Visited))
      return true;
  }
  return false;
}

namespace {

// c2go §A4 helpers — classify a single union alternative.

// One observed alternative slot at byte offset `Offset` inside the union.
// `Kind` distinguishes the cases the classifier cares about:
//   * ScanPtr — a DATA pointer the GC must scan: managed pointer OR
//     unmanaged data pointer. 2026-06-16: both are force-scanned (§3.1 /
//     §9.2), so for union-slot purposes they are identical — each
//     contributes a scan bit at its offset and is "pure" only when no
//     scalar overlaps it.
//   * FuncPtr — a function pointer (points at code, never scanned, §3.4).
//     Benign for GC: no scan bit, does not block a scan-pointer at the
//     same offset.
//   * Scalar — anything else that occupies bytes (int, float, struct of
//     scalars, byte array, etc.). Blocks the precise-slot encoding when
//     it overlaps a scan-pointer offset.
enum class AltKind { ScanPtr, FuncPtr, Scalar };

struct UnionAlt {
  uint64_t OffsetBytes;
  uint64_t SizeBytes;
  AltKind Kind;
  std::string FieldName; // for diagnostics
};


// Collect alternatives for a record at base offset `BaseOff`. For unions
// this means every direct field (each at BaseOff + 0); for structs that
// appear as anonymous-struct alternatives this means each field at
// BaseOff + field-relative-offset. The parent's c2go-managed default
// (`ParentIsC2Go`) determines whether unannotated raw pointers count as
// managed.
static void collectAlternatives(const RecordDecl *RD, uint64_t BaseOff,
                                bool ParentIsC2Go, const ASTContext &Ctx,
                                llvm::SmallVectorImpl<UnionAlt> &Out) {
  if (!RD)
    return;
  RD = RD->getDefinition();
  if (!RD)
    return;
  const ASTRecordLayout &RL = Ctx.getASTRecordLayout(RD);
  unsigned FieldNo = 0;
  for (const FieldDecl *F : RD->fields()) {
    uint64_t FieldOffBits = RL.getFieldOffset(FieldNo);
    uint64_t FieldOff = BaseOff + (FieldOffBits / 8);
    ++FieldNo;
    // Keep the as-written field type so round 22 attr-only managed sugar
    // survives — `getCanonicalType()` would strip the AttributedType
    // wrapper. Use a separately-canonicalized view only for the byte size.
    QualType FT = F->getType();
    std::string FName = F->getNameAsString();
    if (FName.empty())
      FName = "<anonymous>";

    // Peel arrays for classification — `T arr[N]` of pointer counts as a
    // pointer slot at offset 0 of the array; later array slots also hold
    // pointers but they collectively occupy multiple words and we treat
    // that as "scalar with size" for scheme1 purposes (a single-bit
    // bitmap can't cover an array of pointers). Concretely: an array of
    // managed pointers in a union alternative falls through to Scalar
    // (blocks scheme1) which is the correct conservative answer — Scheme2
    // is needed to express that case.
    QualType Peeled = FT;
    bool IsArray = false;
    while (const ArrayType *AT = Peeled->getAsArrayTypeUnsafe()) {
      IsArray = true;
      Peeled = AT->getElementType();
    }

    if (!IsArray && Peeled->isPointerType()) {
      // 2026-06-16: function pointers are no-scan (§3.4); every other
      // pointer (managed OR unmanaged data pointer) is force-scanned
      // (§3.1 / §9.2) → ScanPtr. (void)fieldIsExplicitManaged: the
      // managed/unmanaged distinction no longer changes the GC slot
      // classification for unions — both scan.
      AltKind K =
          Peeled->isFunctionPointerType() ? AltKind::FuncPtr : AltKind::ScanPtr;
      (void)ParentIsC2Go;
      uint64_t Sz = Ctx.getTypeSizeInChars(FT).getQuantity();
      Out.push_back({FieldOff, Sz, K, std::move(FName)});
      continue;
    }

    if (const RecordType *RT = Peeled->getAs<RecordType>()) {
      const RecordDecl *Inner = RT->getDecl()->getDefinition();
      if (Inner && !Inner->isUnion() && !IsArray) {
        // Anonymous (or named) struct as a union alternative — descend
        // so each leaf field contributes its own alternative entry. The
        // anonymous-struct case is the common one: `union { struct {
        // int tag; Node *p; } tab; ... }` — without descent we would
        // treat `tab` as one big scalar and erase the pointer-at-offset
        // information that scheme1 needs.
        collectAlternatives(Inner, FieldOff, ParentIsC2Go, Ctx, Out);
        continue;
      }
      // Nested union or array-of-record: treat as a scalar blob covering
      // its whole footprint. A nested union with managed pointers is a
      // legitimate case that scheme1 cannot encode (would need a second
      // bit position), so falling through to Scalar correctly forces
      // scheme2.
    }

    uint64_t Sz = Ctx.getTypeSizeInChars(FT).getQuantity();
    Out.push_back({FieldOff, Sz, AltKind::Scalar, std::move(FName)});
  }
}

} // namespace

C2GoUnionClassification classifyC2GoUnion(const RecordDecl *UnionRD,
                                          const ASTContext &Ctx) {
  C2GoUnionClassification Result;
  if (!UnionRD)
    return Result;
  UnionRD = UnionRD->getDefinition();
  if (!UnionRD || !UnionRD->isUnion())
    return Result; // NotApplicable

  // Determine the c2go-managed default for this union's pointer fields.
  // A union that carries C2GoStructAttr (explicitly or via #pragma c2go
  // push) makes raw pointers managed by default; everywhere else, the
  // pointer-in-union is unmanaged unless individually annotated. This
  // mirrors the field-world resolution used by CGC2GoTypeInfo.
  const bool ParentIsC2Go = UnionRD->hasAttr<C2GoStructAttr>();

  llvm::SmallVector<UnionAlt, 16> Alts;
  collectAlternatives(UnionRD, /*BaseOff=*/0, ParentIsC2Go, Ctx, Alts);

  // Partition by AltKind and collect offsets. 2026-06-16: a scan-pointer
  // alternative (managed OR unmanaged data ptr) contributes a scan offset;
  // function pointers are benign (no scan, do not block the precise slot).
  llvm::SmallDenseSet<uint64_t, 4> ScanPtrOffsets;
  llvm::SmallVector<const UnionAlt *, 8> Scalars;
  for (const UnionAlt &A : Alts) {
    if (A.Kind == AltKind::ScanPtr)
      ScanPtrOffsets.insert(A.OffsetBytes);
    else if (A.Kind == AltKind::Scalar)
      Scalars.push_back(&A);
    // FuncPtr is benign — neither contributes a scan bit nor blocks the
    // precise-slot encoding (the alternative is opaque code-handle storage).
  }

  if (ScanPtrOffsets.empty()) {
    // No scan-pointer alternative → no GC bits needed. The union is a
    // plain opaque slab from the GC's point of view; classify as
    // NotApplicable so the caller doesn't bother emitting a single-bit
    // bitmap nor a punning diagnostic.
    return Result;
  }

  // Precise single-slot (Scheme1) requires (a) all scan-pointer
  // alternatives at the same offset and (b) no scalar alternative overlaps
  // that offset. Otherwise the union type-puns a pointer slot → hard error
  // (Scheme2 classification; the any-subtype box scaffolding is deleted).
  if (ScanPtrOffsets.size() != 1) {
    Result.Scheme = C2GoUnionScheme::Scheme2;
    // Pick any second offset as the "blocker" hint.
    uint64_t First = *ScanPtrOffsets.begin();
    for (const UnionAlt &A : Alts) {
      if (A.Kind == AltKind::ScanPtr && A.OffsetBytes != First) {
        Result.BlockerFieldName = A.FieldName;
        Result.BlockerReason =
            "scanned pointer at a different offset than other ptr "
            "alternatives";
        break;
      }
    }
    return Result;
  }

  uint64_t PtrOff = *ScanPtrOffsets.begin();
  const unsigned PtrSize = Ctx.getTargetInfo().getPointerWidth(LangAS::Default) / 8;
  uint64_t PtrEnd = PtrOff + PtrSize;
  for (const UnionAlt *S : Scalars) {
    uint64_t SBeg = S->OffsetBytes;
    uint64_t SEnd = SBeg + (S->SizeBytes ? S->SizeBytes : 1);
    // Overlap test: [SBeg, SEnd) intersects [PtrOff, PtrEnd).
    if (SBeg < PtrEnd && PtrOff < SEnd) {
      Result.Scheme = C2GoUnionScheme::Scheme2;
      Result.BlockerFieldName = S->FieldName;
      Result.BlockerReason =
          "scalar alternative overlaps the managed pointer slot";
      return Result;
    }
  }

  Result.Scheme = C2GoUnionScheme::Scheme1;
  Result.PointerOffsetBytes = PtrOff;
  return Result;
}

bool isC2GoVariantUnion(const RecordDecl *RD) {
  if (!RD)
    return false;
  const RecordDecl *Def = RD->getDefinition();
  if (!Def || !Def->isUnion())
    return false;
  return Def->hasAttr<C2GoVariantAttr>();
}

bool isInsideC2GoVariantUnion(const RecordDecl *RD) {
  // §3.9 / T3b — a `c2go_variant` union is re-described as a struct whose
  // pointer words are scanned PRECISELY (computeC2GoVariantLayout descends
  // into nested-struct alternatives and gives each scan pointer its own slot).
  // A nested anonymous struct that appears as an alternative therefore has its
  // managed pointer scanned, even though, analyzed in isolation as a plain
  // record, that pointer looks unscanned. Walk the lexical DeclContext chain:
  // a nested record alternative is lexically declared *inside* the variant
  // union (the union's DeclContext is its parent, possibly through further
  // anonymous-struct nesting). Stop climbing the moment we leave record scope
  // so a genuinely top-level plain record (whose DeclContext is the TU /
  // function) is never matched.
  if (!RD)
    return false;
  const DeclContext *DC = RD->getDeclContext();
  while (DC) {
    const auto *Parent = dyn_cast<RecordDecl>(DC);
    if (!Parent)
      return false; // left record scope: not inside any variant union
    if (isC2GoVariantUnion(Parent))
      return true;
    DC = Parent->getDeclContext();
  }
  return false;
}

namespace {

// c2go §3.9 / T3b — recursive GC-layout of one union alternative.
//
// A `c2go_variant` alternative no longer has to be a *bare* data pointer to
// earn precise scanning: T3b descends into nested structs and arrays, keeping
// the alternative's NATURAL layout, and records which pointer-sized words hold
// a scannable data pointer. The converted struct overlays alternatives with the
// same word-classification "signature" into one shared region and concatenates
// distinct signatures into non-overlapping regions, so the soundness invariant
// (no byte is "ptr in one alternative, scalar in another") is preserved.
//
// Per pointer-word state while walking an alternative's natural layout. A
// conflict (a nested *union* overlaying a scan pointer with a scalar at the
// same word — a genuinely ambiguous byte) is reported by classifyLayout
// returning false, not by a dedicated state; Unset is only the initial map
// value before a word is claimed.
enum class WordState { Unset, Ptr, Scalar };

// Result of walking one alternative.
struct AltLayout {
  uint64_t Footprint = 0; // sizeof the alternative (bytes)
  uint64_t Align = 1;     // alignof the alternative (bytes)
  // Pointer-word indices (relative to the alternative base, in units of
  // PtrSize) that hold a scannable data pointer. Sorted ascending.
  llvm::SmallVector<uint64_t, 4> PtrWords;
  bool HasManaged = false; // any scan word is a *managed* pointer
  bool Ok = true;          // false -> unrepresentable (ambiguous / misaligned)
};

// Recursively classify the natural layout of QualType T (placed at byte offset
// BaseOff within the alternative) into per-word states. PtrSize is the pointer
// width; OutHasManaged is set when any scanned word is a *managed* pointer.
// Returns false on an unrepresentable construct (ptr at a non-pointer-aligned
// offset, or a nested union that puns a scan pointer with a scalar).
static bool classifyLayout(QualType T, uint64_t BaseOff, unsigned PtrSize,
                           const ASTContext &Ctx,
                           llvm::SmallDenseMap<uint64_t, WordState, 8> &Words,
                           bool &OutHasManaged) {
  if (T.isNull())
    return true;

  // Arrays: classify the element type at each element offset.
  if (const ArrayType *AT = T->getAsArrayTypeUnsafe()) {
    if (const ConstantArrayType *CAT = Ctx.getAsConstantArrayType(T)) {
      QualType Elem = CAT->getElementType();
      uint64_t N = CAT->getSize().getZExtValue();
      uint64_t ElemSz = Ctx.getTypeSizeInChars(Elem).getQuantity();
      for (uint64_t i = 0; i < N; ++i)
        if (!classifyLayout(Elem, BaseOff + i * ElemSz, PtrSize, Ctx, Words,
                            OutHasManaged))
          return false;
      return true;
    }
    // VLA / incomplete array — unrepresentable as a fixed bitmap.
    (void)AT;
    return false;
  }

  // Pointer leaf.
  if (T->isPointerType()) {
    if (T->isFunctionPointerType())
      return true; // function pointers are no-scan (§3.4) -> Scalar by default
    // A scannable data pointer must land on a pointer-aligned word.
    if (BaseOff % PtrSize != 0)
      return false; // misaligned (e.g. packed) -> unrepresentable
    uint64_t W = BaseOff / PtrSize;
    auto It = Words.find(W);
    if (It != Words.end() && It->second == WordState::Scalar)
      return false; // a scalar already claimed this word -> conflict
    Words[W] = WordState::Ptr;
    if (isManagedPointerType(T))
      OutHasManaged = true;
    return true;
  }

  // Record: descend.
  if (const RecordType *RT = T->getAs<RecordType>()) {
    const RecordDecl *RD = RT->getDecl();
    RD = RD ? RD->getDefinition() : nullptr;
    if (!RD)
      return true; // incomplete -> treat as opaque scalar bytes
    const ASTRecordLayout &RL = Ctx.getASTRecordLayout(RD);
    if (RD->isUnion()) {
      // A nested union overlays every member at the same base. Each member's
      // word classification must AGREE with the others (and with anything
      // already claimed) or the byte is ambiguous -> fail-closed.
      for (const FieldDecl *F : RD->fields()) {
        llvm::SmallDenseMap<uint64_t, WordState, 8> Sub;
        bool SubManaged = false;
        if (!classifyLayout(F->getType(), BaseOff, PtrSize, Ctx, Sub,
                            SubManaged))
          return false;
        // Mark this member's footprint: ptr words from Sub, every other word in
        // the member's span as Scalar, then reconcile with Words.
        uint64_t MSz = Ctx.getTypeSizeInChars(F->getType()).getQuantity();
        for (uint64_t off = BaseOff; off < BaseOff + MSz; off += PtrSize) {
          uint64_t W = off / PtrSize;
          WordState WS = WordState::Scalar;
          auto SIt = Sub.find(W);
          if (SIt != Sub.end() && SIt->second == WordState::Ptr)
            WS = WordState::Ptr;
          auto It = Words.find(W);
          if (It == Words.end())
            Words[W] = WS;
          else if (It->second != WS)
            return false; // ptr in one alternative, scalar in another
        }
        OutHasManaged |= SubManaged;
      }
      return true;
    }
    // Struct: classify each field at its natural offset.
    unsigned FieldNo = 0;
    for (const FieldDecl *F : RD->fields()) {
      uint64_t FOff = BaseOff + RL.getFieldOffset(FieldNo) / 8;
      ++FieldNo;
      if (!classifyLayout(F->getType(), FOff, PtrSize, Ctx, Words,
                          OutHasManaged))
        return false;
    }
    return true;
  }

  // Plain scalar (int/float/_Bool/enum/...): occupies bytes but holds no
  // pointer. We do not need to record Scalar words for a top-level scalar
  // alternative — the only reason to track Scalar is to detect a conflict
  // against a Ptr at the same word, which only arises when a nested union
  // overlays. The union case above handles that explicitly.
  return true;
}

// Walk one direct union member into an AltLayout.
static AltLayout computeAltLayout(const FieldDecl *F, unsigned PtrSize,
                                  const ASTContext &Ctx) {
  AltLayout A;
  QualType FT = F->getType();
  A.Footprint = Ctx.getTypeSizeInChars(FT).getQuantity();
  A.Align = Ctx.getTypeAlignInChars(FT).getQuantity();
  llvm::SmallDenseMap<uint64_t, WordState, 8> Words;
  bool HasManaged = false;
  // An explicit c2go_managed on the field promotes a bare data pointer member.
  if (FT->isPointerType() && !FT->isFunctionPointerType() &&
      F->hasAttr<C2GoManagedAttr>())
    HasManaged = true;
  A.Ok = classifyLayout(FT, /*BaseOff=*/0, PtrSize, Ctx, Words, HasManaged);
  A.HasManaged = HasManaged;
  if (A.Ok) {
    for (const auto &KV : Words)
      if (KV.second == WordState::Ptr)
        A.PtrWords.push_back(KV.first);
    llvm::sort(A.PtrWords);
  }
  return A;
}

} // namespace

C2GoVariantLayout computeC2GoVariantLayout(const RecordDecl *UnionRD,
                                           const ASTContext &Ctx) {
  C2GoVariantLayout L;
  if (!isC2GoVariantUnion(UnionRD))
    return L; // Valid stays false
  const RecordDecl *RD = UnionRD->getDefinition();

  const unsigned PtrSize =
      Ctx.getTargetInfo().getPointerWidth(LangAS::Default) / 8;
  const unsigned PtrAlign = PtrSize;

  // ── Slot-allocation algorithm (T3b: by-signature region grouping) ─────────
  //
  // Each direct union member (alternative) is classified by its NATURAL layout
  // into a "signature" = (footprint, sorted set of pointer-word offsets). The
  // converted struct then places one *region* per distinct signature:
  //
  //   * Alternatives with the SAME signature overlay into ONE shared region of
  //     size = footprint, keeping the alternative's natural layout. (A flat
  //     bare-pointer alternative has signature {footprint=8, ptrwords={0}}; all
  //     such members share one ptr region. Pure-scalar / funcptr / pointer-free
  //     aggregate alternatives all have an empty ptr-word set; they group by
  //     footprint — and the largest such region subsumes the smaller ones via
  //     the max-footprint coalescing below, exactly the old flat blob.)
  //   * DISTINCT signatures get distinct, non-overlapping regions, concatenated
  //     in first-appearance order. Region bases are pointer-aligned so every
  //     in-region pointer word lands on a pointer-aligned struct offset.
  //
  // Within a region we emit Ptr slots at the alternative's pointer-word offsets
  // and Scalar slots tiling the gaps, so the slot list is an ordered, gap-free
  // cover of [0, SizeBytes) — the Go-type emitter and the gcdata bitmap walker
  // both consume that list verbatim. No byte is ever "ptr here, scalar there":
  // regions never overlap, and within a region a word that any overlaid
  // alternative scans is scanned for all (a disagreement is a hard error
  // detected in classifyLayout / the Sema fail-closed guard).
  //
  // NOTE: classifyLayout DOES consult getASTRecordLayout on NESTED records, but
  // never on RD (the variant union) itself — RecordLayoutBuilder calls into
  // here while building RD's layout (not yet cached). Direct members of a C
  // union all sit at byte offset 0, which we use without querying RD's layout.

  // Per-member analysis.
  llvm::SmallVector<AltLayout, 8> Alts;
  Alts.reserve(8);
  llvm::SmallVector<const FieldDecl *, 8> Fields;
  for (const FieldDecl *F : RD->fields()) {
    Alts.push_back(computeAltLayout(F, PtrSize, Ctx));
    Fields.push_back(F);
  }

  // T3b fail-closed: any unrepresentable alternative (ambiguous nested-union
  // pun, misaligned/packed pointer, or VLA member) makes a precise static
  // bitmap impossible. Flag it so the Sema guard rejects the union rather than
  // silently dropping a pointer word. We still complete the (best-effort)
  // layout so a defensive caller has well-formed sizes.
  for (unsigned i = 0; i < Alts.size(); ++i)
    if (!Alts[i].Ok) {
      L.Representable = false;
      if (L.BlockerFieldName.empty())
        L.BlockerFieldName = Fields[i]->getNameAsString();
    }

  // A region groups members sharing a (footprint, ptrwords) signature. The
  // pure-no-pointer members all share the empty-ptrwords signature but may have
  // different footprints; we coalesce them into the single largest no-scan blob
  // (matches the historic flat behavior and keeps the struct compact). Members
  // that DO have pointer words must match footprint *and* ptrword set exactly
  // to overlay (otherwise their ptr offsets would disagree).
  struct Region {
    uint64_t Footprint = 0;
    uint64_t Align = 1;
    llvm::SmallVector<uint64_t, 4> PtrWords; // empty => no-scan blob
    uint64_t BaseOff = 0;
  };
  llvm::SmallVector<Region, 8> Regions;
  llvm::SmallVector<int, 8> MemberRegion(Alts.size(), -1);

  auto sameSig = [](const Region &R, const AltLayout &A) {
    if (A.PtrWords.empty())
      return R.PtrWords.empty();
    if (R.PtrWords.size() != A.PtrWords.size())
      return false;
    for (unsigned i = 0; i < A.PtrWords.size(); ++i)
      if (R.PtrWords[i] != A.PtrWords[i])
        return false;
    // Identical pointer layout -> overlay regardless of footprint differences;
    // the region footprint grows to the max so every alternative fits.
    return true;
  };

  for (unsigned i = 0; i < Alts.size(); ++i) {
    const AltLayout &A = Alts[i];
    if (!A.PtrWords.empty()) {
      L.HasScanPtr = true;
      if (A.HasManaged)
        L.HasManagedPtr = true;
    }
    int Found = -1;
    for (unsigned r = 0; r < Regions.size(); ++r)
      if (sameSig(Regions[r], A)) {
        Found = (int)r;
        break;
      }
    if (Found < 0) {
      Region R;
      R.Footprint = A.Footprint;
      R.Align = A.Align;
      R.PtrWords = A.PtrWords;
      Regions.push_back(std::move(R));
      Found = (int)Regions.size() - 1;
    } else {
      Regions[Found].Footprint =
          std::max(Regions[Found].Footprint, A.Footprint);
      Regions[Found].Align = std::max(Regions[Found].Align, A.Align);
    }
    MemberRegion[i] = Found;
  }

  // Assign region base offsets (concatenate, pointer-aligned), then emit the
  // ordered, gap-free slot list region by region.
  uint64_t Cursor = 0;
  uint64_t StructAlign = 1;
  for (Region &R : Regions) {
    uint64_t RAlign = R.Align;
    if (!R.PtrWords.empty())
      RAlign = std::max<uint64_t>(RAlign, PtrAlign);
    Cursor = (Cursor + RAlign - 1) / RAlign * RAlign;
    R.BaseOff = Cursor;
    StructAlign = std::max(StructAlign, RAlign);

    // Tile the region: Ptr slots at the pointer-word offsets, Scalar slots in
    // the gaps. PtrWords is sorted ascending.
    uint64_t pos = 0; // byte offset within the region
    for (uint64_t W : R.PtrWords) {
      uint64_t PtrByte = W * PtrSize;
      if (PtrByte > pos)
        L.Slots.push_back({C2GoVariantSlot::Kind::Scalar, R.BaseOff + pos,
                           PtrByte - pos});
      L.Slots.push_back(
          {C2GoVariantSlot::Kind::Ptr, R.BaseOff + PtrByte, (uint64_t)PtrSize});
      pos = PtrByte + PtrSize;
    }
    if (R.Footprint > pos)
      L.Slots.push_back(
          {C2GoVariantSlot::Kind::Scalar, R.BaseOff + pos, R.Footprint - pos});

    Cursor = R.BaseOff + R.Footprint;
  }

  // pos(field): the first-hop redirection target is the member's region base.
  L.FieldStructOffsets.resize(Alts.size());
  for (unsigned i = 0; i < Alts.size(); ++i)
    L.FieldStructOffsets[i] = Regions[MemberRegion[i]].BaseOff;

  // Total size / alignment.
  uint64_t End = Cursor;
  if (StructAlign > 1)
    End = (End + StructAlign - 1) / StructAlign * StructAlign;
  // Defensive: a variant struct is at least one word so an empty/edge union
  // still has well-defined storage.
  if (End == 0)
    End = PtrSize;

  L.SizeBytes = End;
  L.AlignBytes = StructAlign;
  L.Valid = true;
  return L;
}

std::string getStableRecordName(const RecordDecl *RD, const ASTContext &Ctx) {
  if (!RD)
    return std::string();

  // 1. Named record — use the AST name verbatim.
  if (const IdentifierInfo *II = RD->getIdentifier())
    return II->getName().str();

  // 2. Anonymous record reachable via typedef.
  if (const TypedefNameDecl *TD = RD->getTypedefNameForAnonDecl())
    return TD->getNameAsString();

  // 3. Truly anonymous — synthesize from spelling location. We use
  // FNV-1a over "<presumed-filename>:<line>:<col>" so the result is
  // deterministic across processes and across TUs that include the same
  // header.
  const SourceManager &SM = Ctx.getSourceManager();
  SourceLocation Loc = RD->getLocation();
  PresumedLoc PL = SM.getPresumedLoc(Loc);

  std::string Key;
  llvm::raw_string_ostream OS(Key);
  if (PL.isValid())
    OS << PL.getFilename() << ':' << PL.getLine() << ':' << PL.getColumn();
  else
    OS << "<invalid>:" << Loc.getRawEncoding();
  OS.flush();

  // FNV-1a 64-bit
  uint64_t H = 0xcbf29ce484222325ULL;
  for (unsigned char C : Key) {
    H ^= (uint64_t)C;
    H *= 0x100000001b3ULL;
  }

  std::string Out;
  llvm::raw_string_ostream NameOS(Out);
  NameOS << "c2go.anon." << llvm::format_hex_no_prefix(H, 16);
  NameOS.flush();
  return Out;
}

} // namespace c2go
} // namespace clang
