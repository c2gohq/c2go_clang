//===- CGC2GoManifestHelpers.cpp - c2go manifest helper impls -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CGC2GoManifestHelpers.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/C2GoUtil.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Type.h"
#include "clang/Basic/AddressSpaces.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

namespace clang {
namespace c2go {

bool isC2GoUnmanagedExternImport(const FunctionDecl *FD) {
  if (!FD || !FD->hasAttr<C2GoUnmanagedAttr>())
    return false;
  // c2go_extern is export-only; c2go_linkname owns its own bridge path.
  if (FD->hasAttr<C2GoExternAttr>() || FD->hasAttr<C2GoLinknameAttr>())
    return false;
  // An internal-linkage (static) function can never be an external import:
  // its definition lives in this TU by construction (#654; keep in sync with
  // SemaExpr's c2goAdjustImportFnPointee).
  if (!FD->isExternallyVisible())
    return false;
  // Declared-only: no body in this declaration and no definition anywhere in
  // the redecl chain. A c2go_unmanaged function names an external import, so it
  // must never be defined in c2go (Sema rejects a definition); this test keeps
  // the predicate precise for the codegen paths regardless.
  return !FD->doesThisDeclarationHaveABody() && !FD->isDefined();
}

bool isC2GoScalarRegReturnImport(const FunctionDecl *FD) {
  // A single scalar/pointer/float word returns/passes in a register, so the
  // synthesized .s dispatch wrapper for such a signature is reg-return-safe
  // (the internal ABIInternal-style register return, like any internal c2go
  // function). Record/complex/vector-by-value or variadic signatures keep the
  // ABI0 stack return — the wrapper marshals an sret buffer or bails to the
  // c2go-bind Go-dispatch path. Restricting to an all-scalar signature keeps
  // this predicate purely AST-based (no CGFunctionInfo arrangement), so Sema
  // (ActOnC2GoCallout) and CodeGen (shouldUseC2GoRegReturn + the wrapper) can
  // agree on a function's reg-return-ness with no early/late divergence.
  if (!isC2GoUnmanagedExternImport(FD))
    return false;
  const auto *FPT = FD->getType()->getAs<FunctionProtoType>();
  if (!FPT || FPT->isVariadic())
    return false;
  auto isScalarWord = [](QualType T) {
    return T->isVoidType() || T->isIntegralOrEnumerationType() ||
           T->isPointerType() || T->isRealFloatingType();
  };
  if (!isScalarWord(FPT->getReturnType()))
    return false;
  for (QualType P : FPT->getParamTypes())
    if (!isScalarWord(P))
      return false;
  return true;
}

std::string mapC2GoType(QualType QT, const ASTContext &Ctx, bool IsUnmanaged) {
  QT = QT.getCanonicalType();
  if (QT->isVoidType()) return "";
  if (QT->isPointerType()) {
    // Function pointers point at code, never data — they are always
    // no-scan and map to `uintptr` regardless of world (§3.4 / §9.2).
    if (QT->isFunctionPointerType()) return "uintptr";
    if (IsUnmanaged) {
      // 2026-06-16: unmanaged DATA pointers map to `unsafe.Pointer`
      // (force-scanned over-retain, no write barrier) — not `uintptr`.
      // Go has no "stack-scanned but heap-unscanned" pointer type, and
      // copystack must relocate every stack pointer (§9.2 note + §4.9 M5).
      return "unsafe.Pointer";
    }
    QualType Pointee = QT->getPointeeType();
    if (Pointee->isVoidType()) return "unsafe.Pointer";
    if (Pointee->isCharType()) return "*byte";
    return "*" + mapC2GoType(Pointee, Ctx);
  }
  if (const ConstantArrayType *CAT = Ctx.getAsConstantArrayType(QT)) {
    uint64_t N = CAT->getSize().getZExtValue();
    return "[" + std::to_string(N) + "]" +
           mapC2GoType(CAT->getElementType(), Ctx, IsUnmanaged);
  }
  if (QT->isBooleanType()) return "bool";
  if (QT->isCharType()) return "int8";
  if (const BuiltinType *BT = QT->getAs<BuiltinType>()) {
    switch (BT->getKind()) {
    case BuiltinType::SChar: return "int8";
    case BuiltinType::UChar: return "uint8";
    case BuiltinType::Short: return "int16";
    case BuiltinType::UShort: return "uint16";
    case BuiltinType::Int: return "int32";
    case BuiltinType::UInt: return "uint32";
    case BuiltinType::Long:
      // C `long` follows the target data model: it is 64 bits on LP64
      // targets and 32 bits on Windows LLP64.  The Go declaration must use
      // the same width as the ABI0 frame emitted from the C type; spelling it
      // as int64 unconditionally shifts every following slot on Windows.
      return Ctx.getTypeSize(QT) == 32 ? "int32" : "int64";
    case BuiltinType::LongLong: return "int64";
    case BuiltinType::ULong:
      return Ctx.getTypeSize(QT) == 32 ? "uint32" : "uint64";
    case BuiltinType::ULongLong: return "uint64";
    case BuiltinType::Float: return "float32";
    case BuiltinType::Double:
    case BuiltinType::LongDouble: return "float64";
    default: break;
    }
  }
  if (const RecordType *RT = QT->getAs<RecordType>()) {
    if (RecordDecl *RD = RT->getDecl()) {
      if (RD->hasAttr<C2GoStructAttr>()) {
        std::string Name = getStableRecordName(RD, Ctx);
        if (!Name.empty())
          return Name;
      }
      if (!RD->getNameAsString().empty())
        return RD->getNameAsString();
      // c2go §E: a by-value struct passed to / returned from an
      // unmanaged_extern is usually a `typedef struct { ... } T;` whose
      // RecordDecl is anonymous. Use the typedef name so the generated Go
      // wrapper signature carries a real, correctly-sized type (the manifest
      // emits its layout — see the RecordWorklist seeding in CodeGenAction).
      // Without this the struct would degrade to `uintptr` and the ABI0 frame
      // size would be wrong.
      if (const TypedefNameDecl *TD = RD->getTypedefNameForAnonDecl())
        if (!TD->getNameAsString().empty())
          return TD->getNameAsString();
    }
  }
  return "uintptr";
}

std::string buildC2GoGoSig(const FunctionDecl *FD, const ASTContext &Ctx) {
  bool RetUnmanaged = FD->hasAttr<C2GoUnmanagedAttr>();
  std::string Out = "func " + FD->getNameAsString() + "(";
  bool First = true;
  // Q4 fix: prefer canonical (first / prototype) decl's parameter names —
  // those are what the user wrote in the header for downstream consumers.
  // Fall back to the current decl's (definition's) param name only if the
  // canonical decl has no usable identifier at that slot (e.g. K&R-style
  // prototype with unnamed parameters).
  const FunctionDecl *NameSrc = FD->getCanonicalDecl();
  auto getParmName = [&](unsigned I) -> std::string {
    if (NameSrc && I < NameSrc->getNumParams())
      if (const ParmVarDecl *P = NameSrc->getParamDecl(I))
        if (P->getIdentifier())
          return P->getNameAsString();
    if (I < FD->getNumParams())
      if (const ParmVarDecl *P = FD->getParamDecl(I))
        if (P->getIdentifier())
          return P->getNameAsString();
    return "_";
  };
  unsigned Idx = 0;
  for (auto *PVD : FD->parameters()) {
    if (!First) Out += ", ";
    First = false;
    std::string ArgName = getParmName(Idx++);
    bool ParmUnmanaged = PVD->hasAttr<C2GoUnmanagedAttr>();
    Out += ArgName + " " + mapC2GoType(PVD->getType(), Ctx, ParmUnmanaged);
  }
  // c2go §2.3: a void** tagged-argument-pack variadic function carries a
  // synthetic trailing void** argptrs parameter in its lowered ABI. Expose it to
  // Go as an unsafe.Pointer so the binding stays callable — a Go caller builds
  // the void** array of pointers-to-varargs itself and passes &arr[0]. A true
  // unmanaged-extern variadic import keeps the platform va_list (no such param).
  if (Ctx.getLangOpts().C2GoMode && FD->isVariadic() &&
      !isC2GoUnmanagedExternImport(FD)) {
    if (!First)
      Out += ", ";
    First = false;
    Out += "argptrs unsafe.Pointer";
  }
  Out += ")";
  std::string Ret;
  // c2go_returntype(struct X): the boundary returns a Go multi-value tuple —
  // the record's fields map 1:1 to the called Go function's return values, each
  // its own ABI0 result slot (same stack layout as the by-value struct return).
  // Render `(t1, t2, ...)` so the emitted Go decl matches that multi-return
  // signature and needs no Go definition of the C struct. Sema
  // (handleC2GoReturnTypeAttr) has already verified every field is a valid ABI0
  // return slot. Without the attribute a record return renders as its mapped
  // type, unchanged.
  if (FD->hasAttr<C2GoReturnTypeAttr>())
    if (const auto *RT = FD->getReturnType()->getAs<RecordType>()) {
      std::string Tuple = "(";
      bool FirstF = true;
      for (const FieldDecl *Field : RT->getDecl()->fields()) {
        if (!FirstF)
          Tuple += ", ";
        FirstF = false;
        Tuple += mapC2GoType(Field->getType(), Ctx, RetUnmanaged);
      }
      Tuple += ")";
      Ret = Tuple;
    }
  if (Ret.empty())
    Ret = mapC2GoType(FD->getReturnType(), Ctx, RetUnmanaged);
  if (!Ret.empty()) Out += " " + Ret;
  return Out;
}

void emitC2GoStructMeta(llvm::Module &M, const RecordDecl *RD,
                        const ASTContext &Ctx) {
  if (!RD || !RD->isCompleteDefinition())
    return;
  if (!RD->hasAttr<C2GoStructAttr>())
    return;
  std::string Name = getStableRecordName(RD, Ctx);
  if (Name.empty())
    return;
  // Make sure the base `c2go.struct.<X>` marker NamedMD exists (downstream
  // consumers iterate Module::named_metadata() looking for that prefix).
  // CodeGenTypes::ConvertRecordDeclType emits this lazily, but only for
  // records actually used by-value in IR; we need it for all c2go_structs.
  std::string BaseName = (llvm::c2go::kStructMDPrefix + Name).str();
  M.getOrInsertNamedMetadata(BaseName);

  std::string MetaName = BaseName + ".meta";
  llvm::NamedMDNode *NMD = M.getOrInsertNamedMetadata(MetaName);
  if (NMD->getNumOperands() != 0)
    return; // idempotent: keep the first writer's view

  llvm::LLVMContext &LCtx = M.getContext();
  llvm::Type *I64 = llvm::Type::getInt64Ty(LCtx);

  llvm::StringRef ManagedStr =
      RD->hasAttr<C2GoUnmanagedAttr>() ? "unmanaged" : "managed";

  llvm::StringRef SchemeStr = "not_applicable";
  int64_t PtrOff = -1;
  if (RD->isUnion() && isC2GoVariantUnion(RD)) {
    // §3.9 / T3 — convert-to-struct; the precise layout is carried by the
    // .godef metadata, not a scheme/ptr-offset pair.
    SchemeStr = "variant";
  } else if (RD->isUnion()) {
    auto Class = classifyC2GoUnion(RD, Ctx);
    switch (Class.Scheme) {
    case C2GoUnionScheme::Scheme1:
      SchemeStr = "scheme1";
      PtrOff = (int64_t)Class.PointerOffsetBytes;
      break;
    case C2GoUnionScheme::PunHardError:
      SchemeStr = "pun_hard_error";
      break;
    case C2GoUnionScheme::NotApplicable:
      SchemeStr = "not_applicable";
      break;
    }
  }

  std::string Linkname;
  if (const auto *LN = RD->getAttr<C2GoLinknameAttr>())
    Linkname = LN->getName().str();

  llvm::Metadata *Ops[4] = {
      llvm::MDString::get(LCtx, ManagedStr),
      llvm::MDString::get(LCtx, SchemeStr),
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::getSigned(I64, PtrOff)),
      llvm::MDString::get(LCtx, Linkname),
  };
  NMD->addOperand(llvm::MDNode::get(LCtx, Ops));
  // 2026-06-16: type-punned unions (PunHardError, historically "scheme2")
  // hard-error in CodeGenAction (§3.9); the abandoned union→Go-`any` boxing
  // emitted per-alternative `union_alts` / `union_alts_go_type` MD here —
  // deleted, nothing to emit.
}

void emitC2GoFuncManifest(llvm::Module &M, llvm::StringRef CName,
                          const llvm::json::Object &Sym) {
  if (CName.empty())
    return;
  std::string MDName = (llvm::c2go::kFuncMDPrefix + CName).str();
  llvm::NamedMDNode *NMD = M.getOrInsertNamedMetadata(MDName);
  if (NMD->getNumOperands() != 0)
    return; // idempotent: keep the first writer's view

  llvm::LLVMContext &LCtx = M.getContext();
  llvm::Type *I1 = llvm::Type::getInt1Ty(LCtx);
  llvm::Type *I32 = llvm::Type::getInt32Ty(LCtx);

  auto getStr = [&](llvm::StringRef Key) -> llvm::Metadata * {
    if (auto S = Sym.getString(Key))
      return llvm::MDString::get(LCtx, *S);
    return llvm::MDString::get(LCtx, "");
  };
  auto getBool = [&](llvm::StringRef Key) -> llvm::Metadata * {
    bool V = Sym.getBoolean(Key).value_or(false);
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(I1, V ? 1 : 0));
  };
  auto getI32 = [&](llvm::StringRef Key) -> llvm::Metadata * {
    int64_t V = Sym.getInteger(Key).value_or(0);
    return llvm::ConstantAsMetadata::get(
        llvm::ConstantInt::get(I32, (uint64_t)V, /*isSigned=*/true));
  };

  // c_entry is stored as a presence-only boolean in the JSON (writer
  // sets `Sym["c_entry"] = true` only for `main`). entry_sig is a
  // string ("void" | "argc_argv" | "argc_argv_envp" | "unknown"),
  // emitted only when c_entry is true; absent otherwise.
  llvm::Metadata *Ops[14] = {
      /* 0 name           */ getStr("name"),
      /* 1 go_sig         */ getStr("go_sig"),
      /* 2 kind           */ getStr("kind"),
      /* 3 managed        */ getBool("managed"),
      /* 4 go_name        */ getStr("go_name"),
      /* 5 abi            */ getStr("abi"),
      /* 6 asm_symbol     */ getStr("asm_symbol"),
      /* 7 argsize        */ getI32("argsize"),
      /* 8 needs_linkname */ getBool("needs_linkname"),
      /* 9 is_variadic    */ getBool("is_variadic"),
      /* 10 has_float     */ getBool("has_float"),
      /* 11 has_aggregate */ getBool("has_aggregate"),
      /* 12 c_entry       */ getBool("c_entry"),
      /* 13 entry_sig     */ getStr("entry_sig"),
  };
  NMD->addOperand(llvm::MDNode::get(LCtx, Ops));
}

void emitC2GoStructGoDef(llvm::Module &M, const RecordDecl *RD,
                         const ASTContext &Ctx, llvm::StringRef GoDef) {
  if (!RD || GoDef.empty())
    return;
  std::string Name = getStableRecordName(RD, Ctx);
  if (Name.empty())
    return;
  std::string MDName =
      (llvm::c2go::kStructMDPrefix + Name + ".godef").str();
  llvm::NamedMDNode *NMD = M.getOrInsertNamedMetadata(MDName);
  if (NMD->getNumOperands() != 0)
    return;
  llvm::LLVMContext &LCtx = M.getContext();
  llvm::Metadata *Op = llvm::MDString::get(LCtx, GoDef);
  NMD->addOperand(llvm::MDNode::get(LCtx, Op));
}

uint64_t computeC2GoArgSize(const FunctionDecl *FD, const ASTContext &Ctx) {
  const uint64_t RegSize =
      Ctx.getTargetInfo().getPointerWidth(LangAS::Default) / 8;
  auto alignUp = [](uint64_t Off, uint64_t Align) {
    if (Align == 0) Align = 1;
    return (Off + Align - 1) & ~(Align - 1);
  };
  auto Place = [&](QualType QT, uint64_t &Cur) {
    QT = QT.getCanonicalType();
    if (QT->isVoidType()) return;
    uint64_t Sz = Ctx.getTypeSizeInChars(QT).getQuantity();
    uint64_t Al = Ctx.getTypeAlignInChars(QT).getQuantity();
    Cur = alignUp(Cur, Al);
    Cur += Sz;
  };
  uint64_t Total = 0;
  for (auto *PVD : FD->parameters())
    Place(PVD->getType(), Total);
  Total = alignUp(Total, RegSize);
  Place(FD->getReturnType(), Total);
  Total = alignUp(Total, RegSize);
  return Total;
}

} // namespace c2go
} // namespace clang
