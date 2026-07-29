//===--- CGC2GoTypeInfo.cpp - c2go Go-runtime typeinfo emission -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go §A2: emit the Go runtime `internal/abi.Type` typeinfo global for every
// `c2go_struct`-attributed RecordDecl during clang CodeGen. This replaces
// the IR-pass-side bitmap synthesis that previously lived inside
// `C2GoMallocReplacementPass` for the common case, by computing the GC
// bitmap directly from the AST field hierarchy — which is the only place
// where per-field `c2go_managed` / `c2go_unmanaged` decisions are visible
// without losing accuracy across nested records, arrays, and unions.
//
// Two ownership policies, switched on the presence of `c2go_linkname` on
// the RecordDecl:
//
//   * C-owner — no `c2go_linkname`. The C side is authoritative; emit a
//     `linkonce_odr` definition of `@c2go.typeinfo.<RecordName>` populated
//     from the AST, plus the matching `@c2go.gcbitmap.<RecordName>` byte
//     array. linkonce_odr lets multiple translation units agree on a
//     single typeinfo without ODR diagnostics; comdat-by-name groups the
//     typeinfo+bitmap pair so they survive --gc-sections together.
//
//   * Go-owner — `c2go_linkname("pkg.X")`. The Go runtime side already
//     ships a `_type` for `pkg.X` (Go compiler emits it for any declared
//     type). Emit only an external declaration of `@"type:pkg.X"` (Go's
//     standard runtime symbol naming for type metadata). To let downstream
//     name-based lookups still find the typeinfo by the C-side struct name
//     we also emit a `@c2go.typeinfo.<RecordName>` external alias pointing
//     at the Go-side symbol.
//
// The C2GoMallocReplacement IR pass remains the path for anonymous structs
// (literal LLVM types with no AST RecordDecl reachable) and for older
// modules that bypass this entry-point; it checks for an existing
// `@c2go.typeinfo.<X>` global before emitting its own, so the two paths
// are idempotent.
//
//===----------------------------------------------------------------------===//

#include "CodeGenModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/C2GoUtil.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/Diagnostic.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Comdat.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

using namespace clang;
using namespace clang::CodeGen;

namespace {

// Mirror of `internal/abi.Kind` values used by Go's `_type.Kind_` byte.
// We only emit Struct here; nested fields don't carry their own typeinfo
// (Go's runtime only consults the top-level `_type` for typedmemmove).
enum class GoKind : uint8_t {
  Struct = 25,
};

// PtrBitmap — Go's pointer bitmap: one bit per pointer-sized word, LSB-first
// within each byte, set to 1 for words that hold a GC-managed pointer.
// PtrBytes (a.k.a. ptrdata in Go runtime) is the length of the prefix that
// can contain managed pointers — Go GC stops scanning past this.
struct PtrBitmap {
  llvm::SmallVector<uint8_t, 8> Bytes;
  uint64_t PtrBytes = 0;

  void setWord(uint64_t WordIndex, unsigned PtrSize) {
    unsigned ByteIdx = WordIndex / 8;
    unsigned BitIdx = WordIndex % 8;
    while (Bytes.size() <= ByteIdx)
      Bytes.push_back(0);
    Bytes[ByteIdx] |= (uint8_t)(1u << BitIdx);
    uint64_t WordEnd = (WordIndex + 1) * PtrSize;
    if (WordEnd > PtrBytes)
      PtrBytes = WordEnd;
  }

  bool empty() const { return Bytes.empty(); }
};

// Field-world resolution. Per the c2go attribute hierarchy: an explicit
// `c2go_managed` / `c2go_unmanaged` on the FieldDecl wins; otherwise we
// fall back to a per-type recursive walk (pointer→managed by default for
// c2go_struct contents, nested record→recurse, array→element repeat).
enum class FieldWorld { Default, Managed, Unmanaged };

static FieldWorld getFieldWorldExplicit(const FieldDecl *FD) {
  if (FD->hasAttr<C2GoManagedAttr>())
    return FieldWorld::Managed;
  if (FD->hasAttr<C2GoUnmanagedAttr>())
    return FieldWorld::Unmanaged;
  return FieldWorld::Default;
}

class TypeinfoEmitter {
public:
  TypeinfoEmitter(CodeGenModule &CGM, const RecordDecl *RD)
      : CGM(CGM), RD(RD), Ctx(CGM.getModule().getContext()),
        DL(CGM.getModule().getDataLayout()), ASTCtx(CGM.getContext()),
        PtrSize(DL.getPointerSize()) {}

  void emit() {
    // Only complete managed definitions get typeinfo. Forward decls,
    // C++ unions, anonymous nameless records — skip.
    if (!RD || !RD->isCompleteDefinition())
      return;
    // #272: accept any record the c2go design treats as a managed struct.
    // The pragma-region path (AddPragmaC2GoAttribute) promotes such records
    // to C2GoStructAttr, but a record written with a direct
    // `__attribute__((c2go_managed))` outside any `#pragma c2go push` region
    // — the documented `c2go_typeinfo(struct T)` usage in <c2go.h> — only
    // carries C2GoManagedAttr. Mirror the exact promotion signal used at
    // SemaDecl AddPragmaC2GoAttribute step 2 (explicit c2go_managed on the
    // record, or a transitively-managed pointer) so the builtin path emits a
    // real local descriptor instead of leaving the CreateRuntimeVariable i8
    // extern unresolved.
    if (!RD->hasAttr<C2GoStructAttr>() && !RD->hasAttr<C2GoManagedAttr>() &&
        !c2go::recordContainsManagedPointer(RD))
      return;

    // c2go §A3: anonymous c2go-tracked records (those carrying a managed
    // pointer transitively — Sema's AddPragmaC2GoAttribute already filtered
    // out the scalar-only anonymous tables) get a synthesized stable name
    // (`c2go.anon.<hash>` keyed on spelling location). That name is shared
    // with the `c2go.struct.*` named metadata, the `c2go.elem.type`
    // operand emitted by CGExprAgg/CGBuiltin, and the Go-binding sidecar,
    // so all four routes agree on a single identifier.
    std::string RecName = c2go::getStableRecordName(RD, ASTCtx);
    if (RecName.empty())
      return;
    std::string TypeinfoName = (llvm::c2go::kTypeinfoGVPrefix + RecName).str();

    llvm::Module &M = CGM.getModule();
    if (M.getNamedValue(TypeinfoName))
      return; // already emitted (idempotency)

    if (const auto *LN = RD->getAttr<C2GoLinknameAttr>()) {
      emitGoOwner(RecName, TypeinfoName, LN->getName());
    } else {
      emitCOwner(RecName, TypeinfoName);
    }

    // 2026-06-16: type-punned unions (PunHardError, historically "scheme2")
    // hard-error in CodeGenAction (§3.9); the abandoned union→Go-`any` box
    // scaffolding (union_alts MD + _c2go_union_box) is deleted, so there is
    // nothing extra to emit for unions here.
  }

private:
  CodeGenModule &CGM;
  const RecordDecl *RD;
  llvm::LLVMContext &Ctx;
  const llvm::DataLayout &DL;
  ASTContext &ASTCtx;
  unsigned PtrSize;

  // --- C-owner: linkonce_odr full definition ----------------------------

  // Build the Go `internal/abi.Type` struct type. Layout matches
  // `/usr/local/go/src/internal/abi/type.go` exactly so the typeinfo
  // global is binary-compatible with Go runtime's view.
  llvm::StructType *getGoTypeStruct() {
    if (llvm::StructType *Existing =
            llvm::StructType::getTypeByName(Ctx, "c2go._gotype"))
      return Existing;
    llvm::Type *IntPtr = DL.getIntPtrType(Ctx);
    llvm::Type *I8Ptr = llvm::PointerType::getUnqual(Ctx);
    llvm::Type *I8 = llvm::Type::getInt8Ty(Ctx);
    llvm::Type *I32 = llvm::Type::getInt32Ty(Ctx);
    return llvm::StructType::create(Ctx,
                                    {
                                        IntPtr, // Size_
                                        IntPtr, // PtrBytes
                                        I32,    // Hash
                                        I8,     // TFlag
                                        I8,     // Align_
                                        I8,     // FieldAlign_
                                        I8,     // Kind_
                                        I8Ptr,  // Equal
                                        I8Ptr,  // GCData
                                        I32,    // Str
                                        I32,    // PtrToThis
                                    },
                                    "c2go._gotype");
  }

  void emitCOwner(llvm::StringRef RecName, llvm::StringRef TypeinfoName) {
    PtrBitmap Bitmap;
    walkRecord(RD, /*Offset=*/0, /*ParentWorld=*/FieldWorld::Default, Bitmap);

    llvm::Module &M = CGM.getModule();

    // c2go §A2 fix #185: ODR-dedupe linkage strategy varies per object
    // format. ELF supports the canonical `linkonce_odr` + COMDAT group
    // pair; MachO does not honor arbitrary COMDAT groups but provides
    // equivalent semantics via `weak_def` linkage (linker coalesces by
    // symbol name, picking one definition). COFF has its own
    // `linkonce_odr` COMDAT mechanism. Everywhere else fall back to
    // `internal`: one copy per TU but functionally correct, since the
    // typeinfo is referenced only locally within each module.
    llvm::GlobalValue::LinkageTypes Linkage;
    bool UseComdat;
    switch (CGM.getTriple().getObjectFormat()) {
    case llvm::Triple::ELF:
    case llvm::Triple::COFF:
      Linkage = llvm::GlobalValue::LinkOnceODRLinkage;
      UseComdat = true;
      break;
    case llvm::Triple::MachO:
      Linkage = llvm::GlobalValue::WeakODRLinkage;
      UseComdat = false;
      break;
    default:
      Linkage = llvm::GlobalValue::InternalLinkage;
      UseComdat = false;
      break;
    }

    llvm::GlobalVariable *GCData = nullptr;
    if (!Bitmap.empty()) {
      std::string BitmapName = ("c2go.gcbitmap." + RecName.str());
      llvm::SmallVector<llvm::Constant *, 8> Bytes;
      for (uint8_t B : Bitmap.Bytes)
        Bytes.push_back(
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(Ctx), B));
      llvm::ArrayType *AT =
          llvm::ArrayType::get(llvm::Type::getInt8Ty(Ctx), Bytes.size());
      GCData = new llvm::GlobalVariable(
          M, AT, /*isConstant=*/true, Linkage,
          llvm::ConstantArray::get(AT, Bytes), BitmapName);
      if (UseComdat)
        GCData->setComdat(M.getOrInsertComdat(BitmapName));
      GCData->setVisibility(llvm::GlobalValue::HiddenVisibility);
      GCData->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    }

    llvm::StructType *GoType = getGoTypeStruct();
    const ASTRecordLayout &RL = ASTCtx.getASTRecordLayout(RD);
    uint64_t SizeBytes = RL.getSize().getQuantity();
    unsigned AlignBytes = RL.getAlignment().getQuantity();

    llvm::Type *IntPtrTy = DL.getIntPtrType(Ctx);
    llvm::Type *I8Ty = llvm::Type::getInt8Ty(Ctx);
    llvm::Type *I32Ty = llvm::Type::getInt32Ty(Ctx);
    llvm::Type *I8PtrTy = llvm::PointerType::getUnqual(Ctx);

    llvm::Constant *Init = llvm::ConstantStruct::get(
        GoType,
        {
            llvm::ConstantInt::get(IntPtrTy, SizeBytes),
            llvm::ConstantInt::get(IntPtrTy, Bitmap.PtrBytes),
            llvm::ConstantInt::get(I32Ty, computeStableHash(RecName, SizeBytes)),
            llvm::ConstantInt::get(I8Ty, 0), // TFlag
            llvm::ConstantInt::get(I8Ty, (uint8_t)AlignBytes),
            llvm::ConstantInt::get(I8Ty, (uint8_t)AlignBytes),
            llvm::ConstantInt::get(I8Ty, (uint8_t)GoKind::Struct),
            llvm::ConstantPointerNull::get(
                llvm::cast<llvm::PointerType>(I8PtrTy)), // Equal
            GCData ? llvm::cast<llvm::Constant>(GCData)
                   : llvm::ConstantPointerNull::get(
                         llvm::cast<llvm::PointerType>(I8PtrTy)),
            llvm::ConstantInt::get(I32Ty, 0), // Str
            llvm::ConstantInt::get(I32Ty, 0), // PtrToThis
        });

    auto *GV = new llvm::GlobalVariable(
        M, GoType, /*isConstant=*/true, Linkage, Init, TypeinfoName);
    GV->setAlignment(llvm::Align(PtrSize));
    if (UseComdat)
      GV->setComdat(M.getOrInsertComdat(TypeinfoName));
    GV->setVisibility(llvm::GlobalValue::HiddenVisibility);
    GV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  }

  // --- Go-owner: external decl to Go-side `type:pkg.X` ------------------

  void emitGoOwner(llvm::StringRef RecName, llvm::StringRef TypeinfoName,
                   llvm::StringRef Linkname) {
    llvm::Module &M = CGM.getModule();

    // External declaration of the Go-side `_type` symbol. We model it as
    // a c2go._gotype (the actual size is determined Go-side; this is
    // purely for IR-level reference). The symbol name is `type:<linkname>`
    // matching Go runtime's emission for a declared type — Go's compiler
    // emits `type:pkg.X` when it sees `type X struct { ... }`. The Plan 9
    // emitter rewrites the `type:` prefix to the middle-dot form
    // (`type·pkg·X`) so the Go-side linker resolves it directly.
    //
    // We deliberately do NOT emit a `@c2go.typeinfo.<RecName>` bridge
    // alias: GlobalAlias requires its aliasee to be a definition, and our
    // aliasee is an external declaration by construction. The future
    // c2gobind wiring + a tweak to C2GoMemcpyTyping is expected to look
    // up Go-owner typeinfo through the linkname manifest emitted by
    // CodeGenAction. Until then, Go-owner struct copies lower without a
    // `runtime.typedmemmove` redirect — safe for the typical case of
    // Go-side opaque handles (FILE, sqlite3) whose bodies are scalar
    // bytes and don't need a write barrier.
    std::string ExtSymName = ("type:" + Linkname.str());
    if (!M.getNamedGlobal(ExtSymName)) {
      auto *ExtSym = new llvm::GlobalVariable(
          M, getGoTypeStruct(), /*isConstant=*/true,
          llvm::GlobalValue::ExternalLinkage,
          /*Initializer=*/nullptr, ExtSymName);
      ExtSym->setAlignment(llvm::Align(PtrSize));
    }
    (void)RecName;
    (void)TypeinfoName;
  }

  // --- AST → bitmap walk ------------------------------------------------

  // Walk a RecordDecl's fields, filling Bitmap. ParentWorld is the world
  // inherited from the enclosing struct (in the field-world hierarchy);
  // explicit per-field attributes still override.
  void walkRecord(const RecordDecl *R, uint64_t Offset, FieldWorld ParentWorld,
                  PtrBitmap &Bitmap) {
    if (!R || !R->isCompleteDefinition())
      return;

    // Record-level world inheritance: explicit attribute on the struct
    // overrides any parent context; otherwise inherit ParentWorld.
    FieldWorld RecWorld = ParentWorld;
    if (R->hasAttr<C2GoManagedAttr>())
      RecWorld = FieldWorld::Managed;
    else if (R->hasAttr<C2GoUnmanagedAttr>())
      RecWorld = FieldWorld::Unmanaged;

    const ASTRecordLayout &RL = ASTCtx.getASTRecordLayout(R);

    // Unions: §A4 / §3.9 — classify from the per-alternative layout.
    // Scheme1 yields one bitmap bit at the unique scanned-pointer offset
    // inside the union. PunHardError type-puns a scanned pointer slot → a
    // static bitmap cannot encode it; since this struct gets a real
    // typeinfo (it is GC-tracked), force-scanning the punned slot would
    // feed Go's GC a non-pointer and trip `invalidptr`. That is a HARD
    // ERROR here too (the CodeGenAction worklist path catches unions that
    // reach the manifest; this catches those reached only via a parent
    // struct's typeinfo walk). NotApplicable: no scanned-pointer
    // alternative → nothing to add.
    if (R->isUnion()) {
      if (RecWorld == FieldWorld::Unmanaged)
        return;
      // §3.9 / T3 — a `c2go_variant` union is converted to a struct whose
      // pointer slots are GC-class-partitioned (no punned bytes). Set a scan
      // bit at each pointer slot's offset within the converted struct; the
      // scalar blob is no-scan. This bypasses the PunHardError below.
      if (c2go::isC2GoVariantUnion(R)) {
        auto VL = c2go::computeC2GoVariantLayout(R, ASTCtx);
        if (VL.Valid)
          for (const auto &Slot : VL.Slots)
            if (Slot.Kind == c2go::C2GoVariantSlot::Kind::Ptr)
              Bitmap.setWord((Offset + Slot.StructOffsetBytes) / PtrSize,
                             PtrSize);
        return;
      }
      auto Class = c2go::classifyC2GoUnion(R, ASTCtx);
      if (Class.Scheme == c2go::C2GoUnionScheme::Scheme1) {
        uint64_t BitOff = Offset + Class.PointerOffsetBytes;
        Bitmap.setWord(BitOff / PtrSize, PtrSize);
      } else if (Class.Scheme == c2go::C2GoUnionScheme::PunHardError) {
        DiagnosticsEngine &Diags = CGM.getDiags();
        Diags.Report(R->getLocation(),
                     Diags.getCustomDiagID(
                         DiagnosticsEngine::Error,
                         "c2go: union type-puns a scanned pointer slot "
                         "(%0, blocker=%1) inside a GC-tracked struct; a "
                         "static GC bitmap cannot encode it. Give the "
                         "pointer its own pure slot, change the punned "
                         "member to uintptr_t, or mark the union "
                         "__attribute__((c2go_variant))"))
            << Class.BlockerReason << Class.BlockerFieldName;
      }
      return;
    }

    unsigned FieldNo = 0;
    for (const FieldDecl *F : R->fields()) {
      uint64_t FieldOffBits = RL.getFieldOffset(FieldNo);
      uint64_t FieldOffBytes = Offset + (FieldOffBits / 8);
      FieldWorld FW = getFieldWorldExplicit(F);
      if (FW == FieldWorld::Default)
        FW = RecWorld;
      walkType(F->getType(), FieldOffBytes, FW, Bitmap);
      ++FieldNo;
    }
  }

  void walkType(QualType QT, uint64_t Offset, FieldWorld World,
                PtrBitmap &Bitmap) {
    // Probe the round 22 attr-only sugar BEFORE canonicalization — once
    // we canonicalize, `c2go::isManagedPointerType` can no longer see the
    // C2GoManaged AttributedType wrapper. The pointer-to-c2go_struct
    // signal (ii) survives canonicalization, so this is purely about the
    // attr-only case.
    const bool AttrOnlyManaged = c2go::isManagedPointerType(QT);
    QT = QT.getCanonicalType();

    if (QT->isPointerType()) {
      // 2026-06-16: BOTH managed AND unmanaged DATA pointers are scanned
      // (force-scan / over-retain — §3.1, §9.2 note). The only data
      // pointer the GC must NOT touch is a FUNCTION pointer (points at
      // code, never relocated — §3.4). So: scan every pointer word except
      // function pointers, regardless of FieldWorld.
      (void)AttrOnlyManaged;
      if (QT->isFunctionPointerType())
        return; // no-scan: function pointer / opaque code handle
      Bitmap.setWord(Offset / PtrSize, PtrSize);
      return;
    }

    if (const auto *AT = ASTCtx.getAsConstantArrayType(QT)) {
      uint64_t ElemSize =
          ASTCtx.getTypeSizeInChars(AT->getElementType()).getQuantity();
      uint64_t Count = AT->getSize().getZExtValue();
      for (uint64_t I = 0; I < Count; ++I)
        walkType(AT->getElementType(), Offset + I * ElemSize, World, Bitmap);
      return;
    }

    if (const auto *RT = QT->getAs<RecordType>()) {
      walkRecord(RT->getDecl()->getDefinition(), Offset, World, Bitmap);
      return;
    }

    // Integers, floats, enums, complex, vectors: no managed pointers.
  }

  // FNV-1a over (RecordName | size). Stable across TUs without requiring
  // full field walks — sufficient for `runtime._type.Hash` which is only
  // consulted by reflect, not by mallocgc/typedmemmove.
  uint32_t computeStableHash(llvm::StringRef Name, uint64_t SizeBytes) {
    uint32_t H = 0x811c9dc5u;
    auto mix8 = [&](uint8_t V) {
      H ^= V;
      H *= 0x01000193u;
    };
    for (char C : Name)
      mix8((uint8_t)C);
    uint64_t V = SizeBytes;
    for (unsigned I = 0; I < 8; ++I) {
      mix8((uint8_t)(V & 0xff));
      V >>= 8;
    }
    return H;
  }
};

} // end anonymous namespace

void CodeGenModule::emitC2GoTypeinfo(const RecordDecl *RD) {
  if (!getLangOpts().C2GoMode)
    return;
  TypeinfoEmitter(*this, RD).emit();
}

void CodeGenModule::attachC2GoElemTypeMetadata(llvm::CallInst *Call,
                                               QualType DstPointerType,
                                               unsigned ByteLenArgIdx) {
  if (!getLangOpts().C2GoMode)
    return;
  if (!Call || DstPointerType.isNull() || !DstPointerType->isPointerType())
    return;
  ASTContext &ASTCtx = getContext();
  QualType Pointee = DstPointerType->getPointeeType().getCanonicalType();
  const RecordType *RT = Pointee->getAs<RecordType>();
  if (!RT)
    return;
  const RecordDecl *RD = RT->getDecl();
  if (!RD->hasAttr<C2GoStructAttr>())
    return;
  // c2go §A3: anonymous c2go records resolve to a synthesized stable name
  // (`c2go.anon.<hash>`) shared with the typeinfo global and the Go-binding
  // emission — never the raw (possibly empty) identifier, so the metadata
  // key always lands.
  std::string Key = c2go::getStableRecordName(RD, ASTCtx);
  if (Key.empty())
    return;
  uint64_t ElemSizeBytes = ASTCtx.getTypeSizeInChars(Pointee).getQuantity();
  if (ElemSizeBytes == 0)
    return;
  if (Call->arg_size() <= ByteLenArgIdx)
    return;
  auto *NConst =
      llvm::dyn_cast<llvm::ConstantInt>(Call->getArgOperand(ByteLenArgIdx));
  if (!NConst)
    return;
  uint64_t N = NConst->getZExtValue();
  if (N == 0 || (N % ElemSizeBytes) != 0)
    return;
  uint64_t ElemCount = N / ElemSizeBytes;
  llvm::LLVMContext &Ctx = Call->getContext();
  llvm::Metadata *Ops[] = {
      llvm::MDString::get(Ctx, Key),
      llvm::ConstantAsMetadata::get(
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(Ctx), ElemCount)),
  };
  Call->setMetadata(llvm::c2go::kElemTypeMD, llvm::MDNode::get(Ctx, Ops));
}

//===----------------------------------------------------------------------===//
// c2go §B4: per-global GC pointer-mask bitmap for file-scope globals
//===----------------------------------------------------------------------===//
//
// Go's GC discovers global roots from compiler-generated moduledata. For a
// zero-initialized C global containing managed pointers, c2go instead cedes the
// storage definition to a Go variable with the same pointer/scalar word layout.
// We emit the per-global bitmap under the predictable name
// `@c2go.global.gcmask.<varname>` so the manifest can carry that layout to
// c2go-bind. The bitmap is generation metadata and is never registered with the
// runtime itself.
//
// Bitmap encoding mirrors `runtime.bitvector`: one bit per pointer-sized
// word, LSB-first inside each byte. We reuse the §A2 PtrBitmap walker
// rooted at the variable's QualType so:
//   * arrays of c2go_struct propagate per-element bits;
//   * nested c2go_struct fields contribute their own bits at the right
//     byte offset;
//   * c2go_unmanaged / c2go_managed annotations are honored via the
//     same field-world hierarchy used for struct typeinfo.
//
// Scalars (int, char[], non-c2go struct of scalars, ...) produce an
// empty bitmap — in which case we emit nothing because no Go-owned pointer
// layout is needed.

namespace {

// Decide whether a top-level VarDecl's type can transitively contain a
// managed pointer worth a gcmask. Mirrors c2go::recordContainsManagedPointer
// but rooted at QualType (the variable's type, which may be an array of
// c2go_struct or a pointer to a c2go_struct itself — both deserve a bit).
static bool typeNeedsGCMask(QualType QT, const ASTContext &ASTCtx) {
  // Peel arrays at the as-written level first so the round 22 attr-only
  // managed sugar on an element type survives — `getCanonicalType()`
  // would otherwise strip the C2GoManaged AttributedType wrapper before
  // we can probe for it.
  while (const ArrayType *AT = QT->getAsArrayTypeUnsafe()) {
    QT = AT->getElementType();
    if (QT.isNull())
      return false;
  }
  if (QT->isPointerType()) {
    // Shared helper handles both signals: round 22 attr-only sugar
    // (`int * c2go_managed`) and pointer-to-c2go_struct (signal (ii)).
    // A plain `int *p` at file scope still falls through to false.
    return c2go::isManagedPointerType(QT);
  }
  // Canonicalize past the pointer/array check above so the record walk
  // sees through typedefs etc.
  QT = QT.getCanonicalType();
  if (const auto *RT = QT->getAs<RecordType>()) {
    if (const RecordDecl *RD = RT->getDecl()->getDefinition())
      return c2go::recordContainsManagedPointer(RD);
  }
  return false;
}

// Walk a VarDecl's type to fill PtrBitmap. Reuses the §A2 walker
// semantics so nested arrays/records contribute the right bits.
class GlobalBitmapWalker {
public:
  GlobalBitmapWalker(CodeGenModule &CGM)
      : CGM(CGM), ASTCtx(CGM.getContext()),
        PtrSize(CGM.getModule().getDataLayout().getPointerSize()) {}

  void walk(QualType QT, uint64_t Offset, FieldWorld World,
            PtrBitmap &Bitmap) {
    // Probe the round 22 attr-only sugar BEFORE canonicalization — see
    // walkType for the rationale.
    const bool AttrOnlyManaged = c2go::isManagedPointerType(QT);
    QT = QT.getCanonicalType();

    if (QT->isPointerType()) {
      // 2026-06-16: function pointers are always no-scan (§3.4), every
      // other pointer word is a candidate for force-scan (§3.1 / §9.2).
      if (QT->isFunctionPointerType())
        return;
      // Inside a c2go-tracked record the world propagation already decided
      // we should scan: BOTH Managed (default raw ptr) AND Unmanaged
      // (force-scan data pointer) fields are scanned. Only at top level
      // (World==Default) do we keep the conservative "pointee is c2go-
      // tracked" filter so a bare `int *g` global stays no-scan.
      if (World == FieldWorld::Managed || World == FieldWorld::Unmanaged) {
        Bitmap.setWord(Offset / PtrSize, PtrSize);
        return;
      }
      bool ManagedPtr = AttrOnlyManaged;
      if (!ManagedPtr) {
        QualType Pointee = QT->getPointeeType();
        if (!Pointee.isNull())
          if (const auto *RT = Pointee->getAs<RecordType>())
            if (const RecordDecl *RD = RT->getDecl()->getDefinition())
              ManagedPtr = RD->hasAttr<C2GoStructAttr>();
      }
      if (ManagedPtr)
        Bitmap.setWord(Offset / PtrSize, PtrSize);
      return;
    }

    if (const auto *AT = ASTCtx.getAsConstantArrayType(QT)) {
      uint64_t ElemSize =
          ASTCtx.getTypeSizeInChars(AT->getElementType()).getQuantity();
      uint64_t Count = AT->getSize().getZExtValue();
      for (uint64_t I = 0; I < Count; ++I)
        walk(AT->getElementType(), Offset + I * ElemSize, World, Bitmap);
      return;
    }

    if (const auto *RT = QT->getAs<RecordType>()) {
      const RecordDecl *RD = RT->getDecl()->getDefinition();
      if (!RD)
        return;
      walkRecord(RD, Offset, World, Bitmap);
      return;
    }
    // Scalars, complex, vectors: no managed pointers.
  }

private:
  void walkRecord(const RecordDecl *R, uint64_t Offset, FieldWorld ParentWorld,
                  PtrBitmap &Bitmap) {
    if (!R || !R->isCompleteDefinition())
      return;
    FieldWorld RecWorld = ParentWorld;
    if (R->hasAttr<C2GoManagedAttr>())
      RecWorld = FieldWorld::Managed;
    else if (R->hasAttr<C2GoUnmanagedAttr>())
      RecWorld = FieldWorld::Unmanaged;
    // A c2go_struct without an explicit managed/unmanaged annotation:
    // pointer fields default to managed. Mirror the §A2 walker behavior.
    if (RecWorld == FieldWorld::Default && R->hasAttr<C2GoStructAttr>())
      RecWorld = FieldWorld::Managed;

    if (R->isUnion()) {
      if (RecWorld == FieldWorld::Unmanaged)
        return;
      // §3.9 / T3 — a `c2go_variant` union is converted to a struct; scan its
      // partitioned pointer slots precisely (scalar blob is no-scan).
      if (c2go::isC2GoVariantUnion(R)) {
        auto VL = c2go::computeC2GoVariantLayout(R, ASTCtx);
        if (VL.Valid)
          for (const auto &Slot : VL.Slots)
            if (Slot.Kind == c2go::C2GoVariantSlot::Kind::Ptr)
              Bitmap.setWord((Offset + Slot.StructOffsetBytes) / PtrSize,
                             PtrSize);
        return;
      }
      auto Class = c2go::classifyC2GoUnion(R, ASTCtx);
      if (Class.Scheme == c2go::C2GoUnionScheme::Scheme1) {
        uint64_t BitOff = Offset + Class.PointerOffsetBytes;
        Bitmap.setWord(BitOff / PtrSize, PtrSize);
      }
      return;
    }

    const ASTRecordLayout &RL = ASTCtx.getASTRecordLayout(R);
    unsigned FieldNo = 0;
    for (const FieldDecl *F : R->fields()) {
      uint64_t FieldOffBits = RL.getFieldOffset(FieldNo);
      uint64_t FieldOffBytes = Offset + (FieldOffBits / 8);
      FieldWorld FW = getFieldWorldExplicit(F);
      if (FW == FieldWorld::Default)
        FW = RecWorld;
      walk(F->getType(), FieldOffBytes, FW, Bitmap);
      ++FieldNo;
    }
  }

  CodeGenModule &CGM;
  ASTContext &ASTCtx;
  unsigned PtrSize;
};

} // end anonymous namespace

void CodeGenModule::emitC2GoGlobalGCMask(const VarDecl *D,
                                         llvm::GlobalVariable *GV) {
  if (!getLangOpts().C2GoMode)
    return;
  if (!D || !GV)
    return;
  // Only file-scope variables get a gcmask. Function-local statics also have
  // static storage, but their local naming/ownership bridge is not supported.
  if (!D->hasGlobalStorage() || D->isLocalVarDecl())
    return;
  // External declarations do not define storage here; TLS ownership is not
  // supported by the Go-owned global bridge.
  if (D->hasExternalStorage())
    return;
  if (D->getTLSKind() != VarDecl::TLS_None)
    return;

  QualType QT = D->getType();
  if (QT.isNull() || QT->isIncompleteType())
    return;

  ASTContext &ASTCtx = getContext();
  if (!typeNeedsGCMask(QT, ASTCtx))
    return;

  PtrBitmap Bitmap;
  GlobalBitmapWalker(*this).walk(QT, /*Offset=*/0,
                                 /*World=*/FieldWorld::Default, Bitmap);
  if (Bitmap.empty())
    return;

  llvm::Module &M = getModule();
  llvm::LLVMContext &Ctx = M.getContext();

  // Use the linker-visible name so the manifest and c2go-bind can match
  // `c2go.global.gcmask.<sym>` to the storage symbol. GV->getName() reflects
  // mangling / static-name mapping already applied by EmitGlobalVarDefinition.
  llvm::StringRef VarName = GV->getName();
  if (VarName.empty())
    return;
  std::string MaskName =
      (llvm::c2go::kGlobalGcmaskPrefix + VarName).str();
  if (M.getNamedValue(MaskName))
    return; // idempotency: already emitted for this variable

  llvm::SmallVector<llvm::Constant *, 8> Bytes;
  for (uint8_t B : Bitmap.Bytes)
    Bytes.push_back(llvm::ConstantInt::get(llvm::Type::getInt8Ty(Ctx), B));
  llvm::ArrayType *AT =
      llvm::ArrayType::get(llvm::Type::getInt8Ty(Ctx), Bytes.size());
  auto *MaskGV = new llvm::GlobalVariable(
      M, AT, /*isConstant=*/true, llvm::GlobalValue::InternalLinkage,
      llvm::ConstantArray::get(AT, Bytes), MaskName);
  MaskGV->setAlignment(llvm::Align(1));
  // c2go #407: deliberately do NOT set unnamed_addr on the gcmask GV.
  // ConstantMerge (-O2 mid-end) coalesces internal+unnamed_addr+constant
  // GVs with identical initializers, collapsing all single-pointer-word
  // masks ([1 x i8] c"\01") across TUs into one survivor and erasing
  // the rest. `collectGCMaskVarsFromModule` then walks the module's
  // globals by name prefix and the manifest is missing every coalesced
  // entry, so c2gobind never emits the corresponding Go-owned
  // `var X unsafe.Pointer` declaration and the linker fails with
  // "relocation target <pkg>.<var> not defined" against the C-side
  // `·X(SB)` reference (cat-merged multi-TU at -O2; surfaced in
  // c2go-stress SRCS=7 workflow w2zuppb52). The GV name IS load-bearing
  // (it encodes the C var name for manifest matching), so `unnamed_addr`
  // is not even semantically correct here.
  //
  // (NB: `@llvm.compiler.used` would normally exempt these from
  // ConstantMerge via FindUsedValues, but clang emits two arrays —
  // `@llvm.compiler.used` (c2go-extern KeepAlive) and
  // `@llvm.compiler.used.1` (emitLLVMUsed) — and FindUsedValues only
  // scans the exact name `llvm.compiler.used`, so the gcmask anchors
  // in `.1` are invisible to ConstantMerge.)

  // c2go #390: pin the gcmask GV through mid-end DCE. With internal +
  // constant linkage and only named-metadata references, -O2 GlobalDCE
  // would otherwise drop the GV before c2go-lto can scrape it for the
  // manifest. `@llvm.compiler.used` keeps the GV live through
  // compile-time optimization but lets the linker still drop it if no
  // post-link consumer references the symbol.
  addCompilerUsedGlobal(MaskGV);

  // Only zero-initialized C globals can cede storage to the Go side (#394).
  // For `static T *p = &gOther;` AsmPrinter's fallback DATA
  // path (AsmPrinter.cpp:924-936) would emit
  // `DATA ·p+0(SB)/8, $·gOther(SB)` + `GLOBL ·p(SB), NOPTR, $8`
  // alongside the c2gobind-generated `var p unsafe.Pointer`, producing
  // a link-time duplicate definition. Implicit zero, `= NULL`, `= 0`,
  // `(T *)0`, and folded equivalents all canonicalize to a null
  // Constant before reaching this point — IR predicate is exact.
  // (Direction A per .build_status/issue394_design.md §4.)
  //
  // c2go #423: widen the IR-null predicate beyond Constant::isNullValue()
  // so two cases observed in real-world c2go output also cede storage:
  //   (a) `inttoptr (iN 0 to ptr ...)` ConstantExpr — produced by some
  //       mid-end constant-folding paths for `(T *)0` when the AS-cast
  //       sits between the integer 0 and the pointer type. isNullValue()
  //       returns false on a ConstantExpr; recognise the wrapping
  //       IntToPtr whose operand is a zero ConstantInt as a null.
  //   (b) one-pointer-word ConstantStruct `{ ptr null }` initializer —
  //       a designated/aggregate init like `{ .p = (T *)0 }` that the FE
  //       did NOT collapse into zeroinitializer at -O0 leaves a
  //       ConstantStruct with a single ConstantPointerNull operand.
  //       isNullValue() returns false on ConstantStruct; recognise the
  //       single-operand all-null shape.
  //
  // c2go #424: common-linkage tentative defs (e.g. `int *p;` at file
  // scope with -fcommon) carry a synthesized zero initializer per LLVM
  // semantics; isNullValue() already covers them. Make the intent
  // explicit so future LLVM IR refactors that decouple common-linkage
  // from a stored initializer still flow into the Go-owned worklist:
  // hasCommonLinkage() with an absent or null init is the canonical
  // tentative-definition shape.
  auto isC2GoNullInit = [](llvm::GlobalVariable *G) -> bool {
    if (G->hasCommonLinkage() && !G->hasInitializer())
      return true; // #424 tentative def with no stored init
    if (!G->hasInitializer())
      return true;
    llvm::Constant *Init = G->getInitializer();
    if (Init->isNullValue())
      return true;
    // #423(a): inttoptr ConstantExpr whose integer operand is zero.
    if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(Init)) {
      if (CE->getOpcode() == llvm::Instruction::IntToPtr)
        if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CE->getOperand(0)))
          if (CI->isZero())
            return true;
    }
    // #423(b), generalized by #646 P2: a ConstantStruct/ConstantArray ALL of
    // whose operands are null (or the folded inttoptr-0 spelling of null) —
    // `{ NULL, NULL }` / `{ .p = (T *)0 }` aggregates the FE did not collapse
    // to zeroinitializer. (Constant::isNullValue does not recurse into
    // ConstantAggregate.)
    if (auto *CA = llvm::dyn_cast<llvm::ConstantAggregate>(Init)) {
      auto opIsNull = [](llvm::Constant *Op) {
        if (Op->isNullValue())
          return true;
        if (auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(Op))
          if (CE->getOpcode() == llvm::Instruction::IntToPtr)
            if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CE->getOperand(0)))
              if (CI->isZero())
                return true;
        return false;
      };
      bool AllNull = CA->getNumOperands() > 0;
      for (unsigned I = 0, E = CA->getNumOperands(); I < E; ++I)
        if (!opIsNull(CA->getOperand(I))) {
          AllNull = false;
          break;
        }
      if (AllNull)
        return true;
    }
    return false;
  };

  // #646 P2 (== the #388 follow-up): EVERY pointer-carrying file-scope global
  // cedes storage to the Go side — aggregates and multi-pointer layouts
  // included, not just the single-pointer-word case. c2gobind synthesizes a
  // Go var whose layout matches the mask ([N]unsafe.Pointer, or a mixed
  // unsafe.Pointer/uintptr struct), so moduledata.gcdata covers every pointer
  // slot and runtime.markroot scans them natively — a managed pointer parked
  // in such a global is a REAL root (previously the aggregate path had a
  // gcmask bitmap but no consumer: the object was collected out from under
  // the C global; proven by the #646 repro).
  //
  // Eligibility stays zero-init (#394's reasoning, aggregate-wide): the Go
  // var provides zero-value storage, so a DATA initializer cannot be
  // preserved. Managed pointer slots can only ever be statically null, so
  // real code is zero-init by construction; a global that mixes a managed
  // pointer with a NON-zero scalar initializer is a hard error — silently
  // keeping it C-owned would leave exactly the unrooted hole this closes.
  if (isC2GoNullInit(GV)) {
    llvm::NamedMDNode *NMD =
        M.getOrInsertNamedMetadata(llvm::c2go::kGoOwnedGlobalsMDName);
    NMD->addOperand(
        llvm::MDNode::get(Ctx, {llvm::MDString::get(Ctx, VarName)}));
    // Pin the DATA global itself (the mask GV is pinned above): mid-end
    // SROA/GlobalOpt would otherwise split or internalize the aggregate
    // (observed at -O2: `@g_slots` scalar-replaced into `@g_slots.0`),
    // detaching the ceded Go storage from the C reference sites and
    // invalidating the mask correspondence.
    addCompilerUsedGlobal(GV);
  } else {
    // Non-zero initializer: NOT ceded — the C side keeps storage and emits
    // the DATA initializer (#394 direction A; a static initializer can only
    // point at other C globals, which need no GC root). Residual hazard,
    // warned rather than rejected: if RUNTIME code later parks a gc_malloc'd
    // object in this global, that pointer has no root (the write barrier
    // covers the mark window, not persistence). Zero-initialize + assign at
    // startup to get Go-owned rooted storage.
    DiagnosticsEngine &DE = getDiags();
    unsigned ID = DE.getCustomDiagID(
        DiagnosticsEngine::Warning,
        "c2go: global %0 contains managed pointers but has a non-zero "
        "initializer, so its storage stays C-owned and is NOT scanned as a "
        "GC root; a heap object stored into it at runtime can be collected "
        "while still referenced. Zero-initialize it (assign at startup) to "
        "get Go-owned rooted storage");
    DE.Report(D->getLocation(), ID) << D->getName();
  }
}
