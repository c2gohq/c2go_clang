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
