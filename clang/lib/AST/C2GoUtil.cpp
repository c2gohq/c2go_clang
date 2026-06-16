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
