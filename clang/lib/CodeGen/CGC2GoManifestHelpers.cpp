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
    case BuiltinType::LongLong: return "int64";
    case BuiltinType::ULong:
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
    }
  }
  return "uintptr";
}

