//===--- CodeGenTypes.cpp - Type translation for LLVM CodeGen -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This is the code that handles AST -> LLVM type lowering.
//
//===----------------------------------------------------------------------===//

#include "CodeGenTypes.h"
#include "CGCXXABI.h"
#include "CGCall.h"
#include "CGDebugInfo.h"
#include "CGHLSLRuntime.h"
#include "CGOpenCLRuntime.h"
#include "CGRecordLayout.h"
#include "TargetInfo.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/C2GoUtil.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecordLayout.h"
#include "clang/CodeGen/CGFunctionInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"
#include <functional>

using namespace clang;
using namespace CodeGen;

// c2go (GC-S4): when enabled, a pointer whose pointee is a c2go_struct-managed
// record is lowered to addrspace(1), giving the Go-managed-heap-pointer
// discriminator that survives optimization (consumed by the "c2go-gc"
// GCStrategy + RewriteStatepointsForGC, see GC design). Default ON (2026-06-01);
// disable with -mllvm -c2go-managed-addrspace=0 for diagnostic regression only.
static llvm::cl::opt<bool> C2GoManagedAddrSpace(
    "c2go-managed-addrspace", llvm::cl::Hidden, llvm::cl::init(true),
    llvm::cl::desc("c2go: lower pointers to c2go_struct-managed records to "
                   "addrspace(1) (GC discriminator)"));

// True when PointeeTy's canonical type is a c2go_struct-managed record. Mirrors
// getC2GoManagedPointee in CGDecl.cpp (the GC-root-tracking criterion).
static bool isC2GoManagedRecordPointee(QualType PointeeTy) {
  if (PointeeTy.isNull())
    return false;
  const auto *RT = PointeeTy.getCanonicalType()->getAs<RecordType>();
  if (!RT)
    return false;
  const RecordDecl *RD = RT->getDecl();
  return RD && RD->hasAttr<C2GoStructAttr>();
}

CodeGenTypes::CodeGenTypes(CodeGenModule &cgm)
    : CGM(cgm), Context(cgm.getContext()), TheModule(cgm.getModule()),
      Target(cgm.getTarget()) {
  SkippedLayout = false;
  LongDoubleReferenced = false;
}

CodeGenTypes::~CodeGenTypes() {
  for (llvm::FoldingSet<CGFunctionInfo>::iterator
       I = FunctionInfos.begin(), E = FunctionInfos.end(); I != E; )
    delete &*I++;
}

CGCXXABI &CodeGenTypes::getCXXABI() const { return getCGM().getCXXABI(); }

const CodeGenOptions &CodeGenTypes::getCodeGenOpts() const {
  return CGM.getCodeGenOpts();
}

void CodeGenTypes::addRecordTypeName(const RecordDecl *RD,
                                     llvm::StructType *Ty,
                                     StringRef suffix) {
  SmallString<256> TypeName;
  llvm::raw_svector_ostream OS(TypeName);
  OS << RD->getKindName() << '.';

  // FIXME: We probably want to make more tweaks to the printing policy. For
  // example, we should probably enable PrintCanonicalTypes and
  // FullyQualifiedNames.
  PrintingPolicy Policy = RD->getASTContext().getPrintingPolicy();
  Policy.SuppressInlineNamespace =
      llvm::to_underlying(PrintingPolicy::SuppressInlineNamespaceMode::None);

  // Name the codegen type after the typedef name
  // if there is no tag type name available
  if (RD->getIdentifier()) {
    // FIXME: We should not have to check for a null decl context here.
    // Right now we do it because the implicit Obj-C decls don't have one.
    if (RD->getDeclContext())
      RD->printQualifiedName(OS, Policy);
    else
      RD->printName(OS, Policy);
  } else if (const TypedefNameDecl *TDD = RD->getTypedefNameForAnonDecl()) {
    // FIXME: We should not have to check for a null decl context here.
    // Right now we do it because the implicit Obj-C decls don't have one.
    if (TDD->getDeclContext())
      TDD->printQualifiedName(OS, Policy);
    else
      TDD->printName(OS);
  } else
    OS << "anon";

  if (!suffix.empty())
    OS << suffix;

  Ty->setName(OS.str());
}

/// ConvertTypeForMem - Convert type T into a llvm::Type.  This differs from
/// ConvertType in that it is used to convert to the memory representation for
/// a type.  For example, the scalar representation for _Bool is i1, but the
/// memory representation is usually i8 or i32, depending on the target.
///
/// We generally assume that the alloc size of this type under the LLVM
/// data layout is the same as the size of the AST type.  The alignment
/// does not have to match: Clang should always use explicit alignments
/// and packed structs as necessary to produce the layout it needs.
/// But the size does need to be exactly right or else things like struct
/// layout will break.
llvm::Type *CodeGenTypes::ConvertTypeForMem(QualType T) {
  if (T->isConstantMatrixType()) {
    const Type *Ty = Context.getCanonicalType(T).getTypePtr();
    const ConstantMatrixType *MT = cast<ConstantMatrixType>(Ty);
    llvm::Type *IRElemTy = ConvertType(MT->getElementType());
    if (Context.getLangOpts().HLSL && T->isConstantMatrixBoolType())
      IRElemTy = ConvertTypeForMem(Context.BoolTy);
    return llvm::ArrayType::get(IRElemTy, MT->getNumElementsFlattened());
  }

  llvm::Type *R = ConvertType(T);

  // Check for the boolean vector case.
  if (T->isExtVectorBoolType()) {
    auto *FixedVT = cast<llvm::FixedVectorType>(R);

    if (Context.getLangOpts().HLSL) {
      llvm::Type *IRElemTy = ConvertTypeForMem(Context.BoolTy);
      return llvm::FixedVectorType::get(IRElemTy, FixedVT->getNumElements());
    }

    // Pad to at least one byte.
    uint64_t BytePadded = std::max<uint64_t>(FixedVT->getNumElements(), 8);
    return llvm::IntegerType::get(FixedVT->getContext(), BytePadded);
  }

  // If T is _Bool or a _BitInt type, ConvertType will produce an IR type
  // with the exact semantic bit-width of the AST type; for example,
  // _BitInt(17) will turn into i17. In memory, however, we need to store
  // such values extended to their full storage size as decided by AST
  // layout; this is an ABI requirement. Ideally, we would always use an
  // integer type that's just the bit-size of the AST type; for example, if
  // sizeof(_BitInt(17)) == 4, _BitInt(17) would turn into i32. That is what's
  // returned by convertTypeForLoadStore. However, that type does not
  // always satisfy the size requirement on memory representation types
  // describe above. For example, a 32-bit platform might reasonably set
  // sizeof(_BitInt(65)) == 12, but i96 is likely to have to have an alloc size
  // of 16 bytes in the LLVM data layout. In these cases, we simply return
  // a byte array of the appropriate size.
  if (T->isBitIntType()) {
    if (typeRequiresSplitIntoByteArray(T, R))
      return llvm::ArrayType::get(CGM.Int8Ty,
                                  Context.getTypeSizeInChars(T).getQuantity());
    return llvm::IntegerType::get(getLLVMContext(),
                                  (unsigned)Context.getTypeSize(T));
  }

  if (R->isIntegerTy(1))
    return llvm::IntegerType::get(getLLVMContext(),
                                  (unsigned)Context.getTypeSize(T));

  // Else, don't map it.
  return R;
}

bool CodeGenTypes::typeRequiresSplitIntoByteArray(QualType ASTTy,
                                                  llvm::Type *LLVMTy) {
  if (!LLVMTy)
    LLVMTy = ConvertType(ASTTy);

  CharUnits ASTSize = Context.getTypeSizeInChars(ASTTy);
  CharUnits LLVMSize =
      CharUnits::fromQuantity(getDataLayout().getTypeAllocSize(LLVMTy));
  return ASTSize != LLVMSize;
}

llvm::Type *CodeGenTypes::convertTypeForLoadStore(QualType T,
                                                  llvm::Type *LLVMTy) {
  if (!LLVMTy)
    LLVMTy = ConvertType(T);

  if (T->isBitIntType())
    return llvm::Type::getIntNTy(
        getLLVMContext(), Context.getTypeSizeInChars(T).getQuantity() * 8);

  if (LLVMTy->isIntegerTy(1))
    return llvm::IntegerType::get(getLLVMContext(),
                                  (unsigned)Context.getTypeSize(T));

  if (T->isConstantMatrixBoolType()) {
    // Matrices are loaded and stored atomically as vectors. Therefore we
    // construct a FixedVectorType here instead of returning
    // ConvertTypeForMem(T) which would return an ArrayType instead.
    const Type *Ty = Context.getCanonicalType(T).getTypePtr();
    const ConstantMatrixType *MT = cast<ConstantMatrixType>(Ty);
    llvm::Type *IRElemTy = ConvertTypeForMem(MT->getElementType());
    return llvm::FixedVectorType::get(IRElemTy, MT->getNumElementsFlattened());
  }

  if (T->isExtVectorBoolType())
    return ConvertTypeForMem(T);

  return LLVMTy;
}

/// isRecordLayoutComplete - Return true if the specified type is already
/// completely laid out.
bool CodeGenTypes::isRecordLayoutComplete(const Type *Ty) const {
  llvm::DenseMap<const Type*, llvm::StructType *>::const_iterator I =
  RecordDeclTypes.find(Ty);
  return I != RecordDeclTypes.end() && !I->second->isOpaque();
}

/// isFuncParamTypeConvertible - Return true if the specified type in a
/// function parameter or result position can be converted to an IR type at this
/// point. This boils down to being whether it is complete.
bool CodeGenTypes::isFuncParamTypeConvertible(QualType Ty) {
  // Some ABIs cannot have their member pointers represented in IR unless
  // certain circumstances have been reached.
  if (const auto *MPT = Ty->getAs<MemberPointerType>())
    return getCXXABI().isMemberPointerConvertible(MPT);

  // If this isn't a tagged type, we can convert it!
  const TagType *TT = Ty->getAs<TagType>();
  if (!TT) return true;

  // Incomplete types cannot be converted.
  return !TT->isIncompleteType();
}


/// Code to verify a given function type is complete, i.e. the return type
/// and all of the parameter types are complete.  Also check to see if we are in
/// a RS_StructPointer context, and if so whether any struct types have been
/// pended.  If so, we don't want to ask the ABI lowering code to handle a type
/// that cannot be converted to an IR type.
bool CodeGenTypes::isFuncTypeConvertible(const FunctionType *FT) {
  if (!isFuncParamTypeConvertible(FT->getReturnType()))
    return false;

  if (const FunctionProtoType *FPT = dyn_cast<FunctionProtoType>(FT))
    for (unsigned i = 0, e = FPT->getNumParams(); i != e; i++)
      if (!isFuncParamTypeConvertible(FPT->getParamType(i)))
        return false;

  return true;
}

/// UpdateCompletedType - When we find the full definition for a TagDecl,
/// replace the 'opaque' type we previously made for it if applicable.
void CodeGenTypes::UpdateCompletedType(const TagDecl *TD) {
  CanQualType T = CGM.getContext().getCanonicalTagType(TD);
  // If this is an enum being completed, then we flush all non-struct types from
  // the cache.  This allows function types and other things that may be derived
  // from the enum to be recomputed.
  if (const EnumDecl *ED = dyn_cast<EnumDecl>(TD)) {
    // Only flush the cache if we've actually already converted this type.
    if (TypeCache.count(T->getTypePtr())) {
      // Okay, we formed some types based on this.  We speculated that the enum
      // would be lowered to i32, so we only need to flush the cache if this
      // didn't happen.
      if (!ConvertType(ED->getIntegerType())->isIntegerTy(32))
        TypeCache.clear();
    }
    // If necessary, provide the full definition of a type only used with a
    // declaration so far.
    if (CGDebugInfo *DI = CGM.getModuleDebugInfo())
      DI->completeType(ED);
    return;
  }

  // If we completed a RecordDecl that we previously used and converted to an
  // anonymous type, then go ahead and complete it now.
  const RecordDecl *RD = cast<RecordDecl>(TD);
  if (RD->isDependentType()) return;

  // Only complete it if we converted it already.  If we haven't converted it
  // yet, we'll just do it lazily.
  if (RecordDeclTypes.count(T.getTypePtr()))
    ConvertRecordDeclType(RD);

  // If necessary, provide the full definition of a type only used with a
  // declaration so far.
  if (CGDebugInfo *DI = CGM.getModuleDebugInfo())
    DI->completeType(RD);
}

void CodeGenTypes::RefreshTypeCacheForClass(const CXXRecordDecl *RD) {
  CanQualType T = Context.getCanonicalTagType(RD);
  T = Context.getCanonicalType(T);

  const Type *Ty = T.getTypePtr();
  if (RecordsWithOpaqueMemberPointers.count(Ty)) {
    TypeCache.clear();
    RecordsWithOpaqueMemberPointers.clear();
  }
}

static llvm::Type *getTypeForFormat(llvm::LLVMContext &VMContext,
                                    const llvm::fltSemantics &format,
                                    bool UseNativeHalf = false) {
  if (&format == &llvm::APFloat::IEEEhalf()) {
    if (UseNativeHalf)
      return llvm::Type::getHalfTy(VMContext);
    else
      return llvm::Type::getInt16Ty(VMContext);
  }
  if (&format == &llvm::APFloat::BFloat())
    return llvm::Type::getBFloatTy(VMContext);
  if (&format == &llvm::APFloat::IEEEsingle())
    return llvm::Type::getFloatTy(VMContext);
  if (&format == &llvm::APFloat::IEEEdouble())
    return llvm::Type::getDoubleTy(VMContext);
  if (&format == &llvm::APFloat::IEEEquad())
    return llvm::Type::getFP128Ty(VMContext);
  if (&format == &llvm::APFloat::PPCDoubleDouble())
    return llvm::Type::getPPC_FP128Ty(VMContext);
  if (&format == &llvm::APFloat::x87DoubleExtended())
    return llvm::Type::getX86_FP80Ty(VMContext);
  llvm_unreachable("Unknown float format!");
}

llvm::Type *CodeGenTypes::ConvertFunctionTypeInternal(QualType QFT) {
  assert(QFT.isCanonical());
  const FunctionType *FT = cast<FunctionType>(QFT.getTypePtr());
  // First, check whether we can build the full function type.  If the
  // function type depends on an incomplete type (e.g. a struct or enum), we
  // cannot lower the function type.
  if (!isFuncTypeConvertible(FT)) {
    // This function's type depends on an incomplete tag type.

    // Force conversion of all the relevant record types, to make sure
    // we re-convert the FunctionType when appropriate.
    if (const auto *RD = FT->getReturnType()->getAsRecordDecl())
      ConvertRecordDeclType(RD);
    if (const FunctionProtoType *FPT = dyn_cast<FunctionProtoType>(FT))
      for (unsigned i = 0, e = FPT->getNumParams(); i != e; i++)
        if (const auto *RD = FPT->getParamType(i)->getAsRecordDecl())
          ConvertRecordDeclType(RD);

    SkippedLayout = true;

    // Return a placeholder type.
    return llvm::StructType::get(getLLVMContext());
  }

  // The function type can be built; call the appropriate routines to
  // build it.
  const CGFunctionInfo *FI;
  if (const FunctionProtoType *FPT = dyn_cast<FunctionProtoType>(FT)) {
    FI = &arrangeFreeFunctionType(
        CanQual<FunctionProtoType>::CreateUnsafe(QualType(FPT, 0)));
  } else {
    const FunctionNoProtoType *FNPT = cast<FunctionNoProtoType>(FT);
    FI = &arrangeFreeFunctionType(
        CanQual<FunctionNoProtoType>::CreateUnsafe(QualType(FNPT, 0)));
  }

  llvm::Type *ResultType = nullptr;
  // If there is something higher level prodding our CGFunctionInfo, then
  // don't recurse into it again.
  if (FunctionsBeingProcessed.count(FI)) {

    ResultType = llvm::StructType::get(getLLVMContext());
    SkippedLayout = true;
  } else {

    // Otherwise, we're good to go, go ahead and convert it.
    ResultType = GetFunctionType(*FI);
  }

  return ResultType;
}

/// ConvertType - Convert the specified type to its LLVM form.
llvm::Type *CodeGenTypes::ConvertType(QualType T) {
  T = Context.getCanonicalType(T);

  const Type *Ty = T.getTypePtr();

  // For the device-side compilation, CUDA device builtin surface/texture types
  // may be represented in different types.
  if (Context.getLangOpts().CUDAIsDevice) {
    if (T->isCUDADeviceBuiltinSurfaceType()) {
      if (auto *Ty = CGM.getTargetCodeGenInfo()
                         .getCUDADeviceBuiltinSurfaceDeviceType())
        return Ty;
    } else if (T->isCUDADeviceBuiltinTextureType()) {
      if (auto *Ty = CGM.getTargetCodeGenInfo()
                         .getCUDADeviceBuiltinTextureDeviceType())
        return Ty;
    }
  }

  // RecordTypes are cached and processed specially.
  if (const auto *RT = dyn_cast<RecordType>(Ty))
    return ConvertRecordDeclType(RT->getDecl()->getDefinitionOrSelf());

  llvm::Type *CachedType = nullptr;
  auto TCI = TypeCache.find(Ty);
  if (TCI != TypeCache.end())
    CachedType = TCI->second;
    // With expensive checks, check that the type we compute matches the
    // cached type.
#ifndef EXPENSIVE_CHECKS
  if (CachedType)
    return CachedType;
#endif

  // If we don't have it in the cache, convert it now.
  llvm::Type *ResultType = nullptr;
  switch (Ty->getTypeClass()) {
  case Type::Record: // Handled above.
#define TYPE(Class, Base)
#define ABSTRACT_TYPE(Class, Base)
#define NON_CANONICAL_TYPE(Class, Base) case Type::Class:
#define DEPENDENT_TYPE(Class, Base) case Type::Class:
#define NON_CANONICAL_UNLESS_DEPENDENT_TYPE(Class, Base) case Type::Class:
#include "clang/AST/TypeNodes.inc"
    llvm_unreachable("Non-canonical or dependent types aren't possible.");

  case Type::Builtin: {
    switch (cast<BuiltinType>(Ty)->getKind()) {
    case BuiltinType::Void:
    case BuiltinType::ObjCId:
    case BuiltinType::ObjCClass:
    case BuiltinType::ObjCSel:
      // LLVM void type can only be used as the result of a function call.  Just
      // map to the same as char.
      ResultType = llvm::Type::getInt8Ty(getLLVMContext());
      break;

    case BuiltinType::Bool:
      // Note that we always return bool as i1 for use as a scalar type.
      ResultType = llvm::Type::getInt1Ty(getLLVMContext());
      break;

    case BuiltinType::Char_S:
    case BuiltinType::Char_U:
    case BuiltinType::SChar:
    case BuiltinType::UChar:
    case BuiltinType::Short:
    case BuiltinType::UShort:
    case BuiltinType::Int:
    case BuiltinType::UInt:
    case BuiltinType::Long:
    case BuiltinType::ULong:
    case BuiltinType::LongLong:
    case BuiltinType::ULongLong:
    case BuiltinType::WChar_S:
    case BuiltinType::WChar_U:
    case BuiltinType::Char8:
    case BuiltinType::Char16:
    case BuiltinType::Char32:
    case BuiltinType::ShortAccum:
    case BuiltinType::Accum:
    case BuiltinType::LongAccum:
    case BuiltinType::UShortAccum:
    case BuiltinType::UAccum:
    case BuiltinType::ULongAccum:
    case BuiltinType::ShortFract:
    case BuiltinType::Fract:
    case BuiltinType::LongFract:
    case BuiltinType::UShortFract:
    case BuiltinType::UFract:
    case BuiltinType::ULongFract:
    case BuiltinType::SatShortAccum:
    case BuiltinType::SatAccum:
    case BuiltinType::SatLongAccum:
    case BuiltinType::SatUShortAccum:
    case BuiltinType::SatUAccum:
    case BuiltinType::SatULongAccum:
    case BuiltinType::SatShortFract:
    case BuiltinType::SatFract:
    case BuiltinType::SatLongFract:
    case BuiltinType::SatUShortFract:
    case BuiltinType::SatUFract:
    case BuiltinType::SatULongFract:
      ResultType = llvm::IntegerType::get(getLLVMContext(),
                                 static_cast<unsigned>(Context.getTypeSize(T)));
      break;

    case BuiltinType::Float16:
      ResultType =
          getTypeForFormat(getLLVMContext(), Context.getFloatTypeSemantics(T),
                           /* UseNativeHalf = */ true);
      break;

    case BuiltinType::Half:
      // Half FP can either be storage-only (lowered to i16) or native.
      ResultType = getTypeForFormat(
          getLLVMContext(), Context.getFloatTypeSemantics(T),
          Context.getLangOpts().NativeHalfType ||
              !Context.getTargetInfo().useFP16ConversionIntrinsics());
      break;
    case BuiltinType::LongDouble:
      LongDoubleReferenced = true;
      [[fallthrough]];
    case BuiltinType::BFloat16:
    case BuiltinType::Float:
    case BuiltinType::Double:
    case BuiltinType::Float128:
    case BuiltinType::Ibm128:
      ResultType = getTypeForFormat(getLLVMContext(),
                                    Context.getFloatTypeSemantics(T),
                                    /* UseNativeHalf = */ false);
      break;

    case BuiltinType::NullPtr:
      // Model std::nullptr_t as i8*
      ResultType = llvm::PointerType::getUnqual(getLLVMContext());
      break;

    case BuiltinType::UInt128:
    case BuiltinType::Int128:
      ResultType = llvm::IntegerType::get(getLLVMContext(), 128);
      break;

#define IMAGE_TYPE(ImgType, Id, SingletonId, Access, Suffix) \
    case BuiltinType::Id:
#include "clang/Basic/OpenCLImageTypes.def"
#define EXT_OPAQUE_TYPE(ExtType, Id, Ext) \
    case BuiltinType::Id:
#include "clang/Basic/OpenCLExtensionTypes.def"
    case BuiltinType::OCLSampler:
    case BuiltinType::OCLEvent:
    case BuiltinType::OCLClkEvent:
    case BuiltinType::OCLQueue:
    case BuiltinType::OCLReserveID:
      ResultType = CGM.getOpenCLRuntime().convertOpenCLSpecificType(Ty);
      break;
#define SVE_VECTOR_TYPE(Name, MangledName, Id, SingletonId)                    \
  case BuiltinType::Id:
#define SVE_PREDICATE_TYPE(Name, MangledName, Id, SingletonId)                 \
  case BuiltinType::Id:
#include "clang/Basic/AArch64ACLETypes.def"
      {
        ASTContext::BuiltinVectorTypeInfo Info =
            Context.getBuiltinVectorTypeInfo(cast<BuiltinType>(Ty));
        // The `__mfp8` type maps to `<1 x i8>` which can't be used to build
        // a <N x i8> vector type, hence bypass the call to `ConvertType` for
        // the element type and create the vector type directly.
        auto *EltTy = Info.ElementType->isMFloat8Type()
                          ? llvm::Type::getInt8Ty(getLLVMContext())
                          : ConvertType(Info.ElementType);
        auto *VTy = llvm::VectorType::get(EltTy, Info.EC);
        switch (Info.NumVectors) {
        default:
          llvm_unreachable("Expected 1, 2, 3 or 4 vectors!");
        case 1:
          return VTy;
        case 2:
          return llvm::StructType::get(VTy, VTy);
        case 3:
          return llvm::StructType::get(VTy, VTy, VTy);
        case 4:
          return llvm::StructType::get(VTy, VTy, VTy, VTy);
        }
      }
    case BuiltinType::SveCount:
      return llvm::TargetExtType::get(getLLVMContext(), "aarch64.svcount");
    case BuiltinType::MFloat8:
      return llvm::VectorType::get(llvm::Type::getInt8Ty(getLLVMContext()), 1,
                                   false);
#define PPC_VECTOR_TYPE(Name, Id, Size) \
    case BuiltinType::Id: \
      ResultType = \
        llvm::FixedVectorType::get(ConvertType(Context.BoolTy), Size); \
      break;
#include "clang/Basic/PPCTypes.def"
#define RVV_TYPE(Name, Id, SingletonId) case BuiltinType::Id:
#include "clang/Basic/RISCVVTypes.def"
      {
        ASTContext::BuiltinVectorTypeInfo Info =
            Context.getBuiltinVectorTypeInfo(cast<BuiltinType>(Ty));
        if (Info.NumVectors != 1) {
          unsigned I8EltCount =
              Info.EC.getKnownMinValue() *
              ConvertType(Info.ElementType)->getScalarSizeInBits() / 8;
          return llvm::TargetExtType::get(
              getLLVMContext(), "riscv.vector.tuple",
              llvm::ScalableVectorType::get(
                  llvm::Type::getInt8Ty(getLLVMContext()), I8EltCount),
              Info.NumVectors);
        }
        return llvm::ScalableVectorType::get(ConvertType(Info.ElementType),
                                             Info.EC.getKnownMinValue());
      }
#define WASM_REF_TYPE(Name, MangledName, Id, SingletonId, AS)                  \
  case BuiltinType::Id: {                                                      \
    if (BuiltinType::Id == BuiltinType::WasmExternRef)                         \
      ResultType = CGM.getTargetCodeGenInfo().getWasmExternrefReferenceType(); \
    else                                                                       \
      llvm_unreachable("Unexpected wasm reference builtin type!");             \
  } break;
#include "clang/Basic/WebAssemblyReferenceTypes.def"
#define AMDGPU_OPAQUE_PTR_TYPE(Name, Id, SingletonId, Width, Align, AS)        \
  case BuiltinType::Id:                                                        \
    return llvm::PointerType::get(getLLVMContext(), AS);
#define AMDGPU_NAMED_BARRIER_TYPE(Name, Id, SingletonId, Width, Align, Scope)  \
  case BuiltinType::Id:                                                        \
    return llvm::TargetExtType::get(getLLVMContext(), "amdgcn.named.barrier",  \
                                    {}, {Scope});
#include "clang/Basic/AMDGPUTypes.def"
#define HLSL_INTANGIBLE_TYPE(Name, Id, SingletonId) case BuiltinType::Id:
#include "clang/Basic/HLSLIntangibleTypes.def"
      ResultType = CGM.getHLSLRuntime().convertHLSLSpecificType(Ty);
      break;
    case BuiltinType::Dependent:
#define BUILTIN_TYPE(Id, SingletonId)
#define PLACEHOLDER_TYPE(Id, SingletonId) \
    case BuiltinType::Id:
#include "clang/AST/BuiltinTypes.def"
      llvm_unreachable("Unexpected placeholder builtin type!");
    }
    break;
  }
  case Type::Auto:
  case Type::DeducedTemplateSpecialization:
    llvm_unreachable("Unexpected undeduced type!");
  case Type::Complex: {
    llvm::Type *EltTy = ConvertType(cast<ComplexType>(Ty)->getElementType());
    ResultType = llvm::StructType::get(EltTy, EltTy);
    break;
  }
  case Type::LValueReference:
  case Type::RValueReference: {
    const ReferenceType *RTy = cast<ReferenceType>(Ty);
    QualType ETy = RTy->getPointeeType();
    unsigned AS = getTargetAddressSpace(ETy);
    if (C2GoManagedAddrSpace && Context.getLangOpts().C2GoMode &&
        isC2GoManagedRecordPointee(ETy))
      AS = 1;
    ResultType = llvm::PointerType::get(getLLVMContext(), AS);
    break;
  }
  case Type::Pointer: {
    const PointerType *PTy = cast<PointerType>(Ty);
    QualType ETy = PTy->getPointeeType();
    unsigned AS = getTargetAddressSpace(ETy);
    if (C2GoManagedAddrSpace && Context.getLangOpts().C2GoMode &&
        isC2GoManagedRecordPointee(ETy))
      AS = 1;
    ResultType = llvm::PointerType::get(getLLVMContext(), AS);
    break;
  }

  case Type::VariableArray: {
    const VariableArrayType *A = cast<VariableArrayType>(Ty);
    assert(A->getIndexTypeCVRQualifiers() == 0 &&
           "FIXME: We only handle trivial array types so far!");
    // VLAs resolve to the innermost element type; this matches
    // the return of alloca, and there isn't any obviously better choice.
    ResultType = ConvertTypeForMem(A->getElementType());
    break;
  }
  case Type::IncompleteArray: {
    const IncompleteArrayType *A = cast<IncompleteArrayType>(Ty);
    assert(A->getIndexTypeCVRQualifiers() == 0 &&
           "FIXME: We only handle trivial array types so far!");
    // int X[] -> [0 x int], unless the element type is not sized.  If it is
    // unsized (e.g. an incomplete struct) just use [0 x i8].
    ResultType = ConvertTypeForMem(A->getElementType());
    if (!ResultType->isSized()) {
      SkippedLayout = true;
      ResultType = llvm::Type::getInt8Ty(getLLVMContext());
    }
    ResultType = llvm::ArrayType::get(ResultType, 0);
    break;
  }
  case Type::ArrayParameter:
  case Type::ConstantArray: {
    const ConstantArrayType *A = cast<ConstantArrayType>(Ty);
    llvm::Type *EltTy = ConvertTypeForMem(A->getElementType());

    // Lower arrays of undefined struct type to arrays of i8 just to have a
    // concrete type.
    if (!EltTy->isSized()) {
      SkippedLayout = true;
      EltTy = llvm::Type::getInt8Ty(getLLVMContext());
    }

    ResultType = llvm::ArrayType::get(EltTy, A->getZExtSize());
    break;
  }
  case Type::ExtVector:
  case Type::Vector: {
    const auto *VT = cast<VectorType>(Ty);
    // An ext_vector_type of Bool is really a vector of bits.
    llvm::Type *IRElemTy = VT->isPackedVectorBoolType(Context)
                               ? llvm::Type::getInt1Ty(getLLVMContext())
                           : VT->getElementType()->isMFloat8Type()
                               ? llvm::Type::getInt8Ty(getLLVMContext())
                               : ConvertType(VT->getElementType());
    ResultType = llvm::FixedVectorType::get(IRElemTy, VT->getNumElements());
    break;
  }
  case Type::ConstantMatrix: {
    const ConstantMatrixType *MT = cast<ConstantMatrixType>(Ty);
    ResultType =
        llvm::FixedVectorType::get(ConvertType(MT->getElementType()),
                                   MT->getNumRows() * MT->getNumColumns());
    break;
  }
  case Type::FunctionNoProto:
  case Type::FunctionProto:
    ResultType = ConvertFunctionTypeInternal(T);
    break;
  case Type::ObjCObject:
    ResultType = ConvertType(cast<ObjCObjectType>(Ty)->getBaseType());
    break;

  case Type::ObjCInterface: {
    // Objective-C interfaces are always opaque (outside of the
    // runtime, which can do whatever it likes); we never refine
    // these.
    llvm::Type *&T = InterfaceTypes[cast<ObjCInterfaceType>(Ty)];
    if (!T)
      T = llvm::StructType::create(getLLVMContext());
    ResultType = T;
    break;
  }

  case Type::ObjCObjectPointer:
    ResultType = llvm::PointerType::getUnqual(getLLVMContext());
    break;

  case Type::Enum: {
    const auto *ED = Ty->castAsEnumDecl();
    if (ED->isCompleteDefinition() || ED->isFixed())
      return ConvertType(ED->getIntegerType());
    // Return a placeholder 'i32' type.  This can be changed later when the
    // type is defined (see UpdateCompletedType), but is likely to be the
    // "right" answer.
    ResultType = llvm::Type::getInt32Ty(getLLVMContext());
    break;
  }

  case Type::BlockPointer: {
    // Block pointers lower to function type. For function type,
    // getTargetAddressSpace() returns default address space for
    // function pointer i.e. program address space. Therefore, for block
    // pointers, it is important to pass the pointee AST address space when
    // calling getTargetAddressSpace(), to ensure that we get the LLVM IR
    // address space for data pointers and not function pointers.
    const QualType FTy = cast<BlockPointerType>(Ty)->getPointeeType();
    unsigned AS = Context.getTargetAddressSpace(FTy.getAddressSpace());
    ResultType = llvm::PointerType::get(getLLVMContext(), AS);
    break;
  }

  case Type::MemberPointer: {
    auto *MPTy = cast<MemberPointerType>(Ty);
    if (!getCXXABI().isMemberPointerConvertible(MPTy)) {
      CanQualType T = CGM.getContext().getCanonicalTagType(
          MPTy->getMostRecentCXXRecordDecl());
      auto Insertion =
          RecordsWithOpaqueMemberPointers.try_emplace(T.getTypePtr());
      if (Insertion.second)
        Insertion.first->second = llvm::StructType::create(getLLVMContext());
      ResultType = Insertion.first->second;
    } else {
      ResultType = getCXXABI().ConvertMemberPointerType(MPTy);
    }
    break;
  }

  case Type::Atomic: {
    QualType valueType = cast<AtomicType>(Ty)->getValueType();
    ResultType = ConvertTypeForMem(valueType);

    // Pad out to the inflated size if necessary.
    uint64_t valueSize = Context.getTypeSize(valueType);
    uint64_t atomicSize = Context.getTypeSize(Ty);
    if (valueSize != atomicSize) {
      assert(valueSize < atomicSize);
      llvm::Type *elts[] = {
        ResultType,
        llvm::ArrayType::get(CGM.Int8Ty, (atomicSize - valueSize) / 8)
      };
      ResultType =
          llvm::StructType::get(getLLVMContext(), llvm::ArrayRef(elts));
    }
    break;
  }
  case Type::Pipe: {
    ResultType = CGM.getOpenCLRuntime().getPipeType(cast<PipeType>(Ty));
    break;
  }
  case Type::BitInt: {
    const auto &EIT = cast<BitIntType>(Ty);
    ResultType = llvm::Type::getIntNTy(getLLVMContext(), EIT->getNumBits());
    break;
  }
  case Type::HLSLAttributedResource:
  case Type::HLSLInlineSpirv:
    ResultType = CGM.getHLSLRuntime().convertHLSLSpecificType(Ty);
    break;
  }

  assert(ResultType && "Didn't convert a type?");
  assert((!CachedType || CachedType == ResultType) &&
         "Cached type doesn't match computed type");

  TypeCache[Ty] = ResultType;
  return ResultType;
}

bool CodeGenModule::isPaddedAtomicType(QualType type) {
  return isPaddedAtomicType(type->castAs<AtomicType>());
}

bool CodeGenModule::isPaddedAtomicType(const AtomicType *type) {
  return Context.getTypeSize(type) != Context.getTypeSize(type->getValueType());
}

/// ConvertRecordDeclType - Lay out a tagged decl type like struct or union.
llvm::StructType *CodeGenTypes::ConvertRecordDeclType(const RecordDecl *RD) {
  // TagDecl's are not necessarily unique, instead use the (clang)
  // type connected to the decl.
  const Type *Key = Context.getCanonicalTagType(RD).getTypePtr();

  llvm::StructType *&Entry = RecordDeclTypes[Key];

  // If we don't have a StructType at all yet, create the forward declaration.
  if (!Entry) {
    Entry = llvm::StructType::create(getLLVMContext());
    addRecordTypeName(RD, Entry, "");
  }
  llvm::StructType *Ty = Entry;

  // If this is still a forward declaration, or the LLVM type is already
  // complete, there's nothing more to do.
  RD = RD->getDefinition();
  if (!RD || !RD->isCompleteDefinition() || !Ty->isOpaque())
    return Ty;

  // Force conversion of non-virtual base classes recursively.
  if (const CXXRecordDecl *CRD = dyn_cast<CXXRecordDecl>(RD)) {
    for (const auto &I : CRD->bases()) {
      if (I.isVirtual()) continue;
      ConvertRecordDeclType(I.getType()->castAsRecordDecl());
    }
  }

  // Layout fields.
  std::unique_ptr<CGRecordLayout> Layout = ComputeRecordLayout(RD, Ty);
  CGRecordLayouts[Key] = std::move(Layout);

  // Mark CGO struct types in the module so downstream passes can recognize
  // them. The named metadata node's presence is the signal; its operands
  // list the byte offsets of any cgo_noscan fields, in
  // (offset_in_bytes, size_in_bytes) pairs, so the pass can clear those
  // ranges from the GC bitmap.
  if (RD->hasAttr<C2GoStructAttr>()) {
    llvm::LLVMContext &Ctx = Ty->getContext();
    llvm::Module &M = CGM.getModule();
    // c2go §A3: derive the named-metadata key from the AST's stable name
    // (`c2go.anon.<hash>` for truly anonymous records). This matches the
    // typeinfo global `@c2go.typeinfo.<RecName>` produced by emitC2GoTypeinfo
    // and the `c2go.elem.type` metadata key emitted by CGExprAgg/CGBuiltin
    // — all three downstream consumers (LLVM passes, Go binding, GC bitmap)
    // now agree on a single name for an anonymous record. Fall back to the
    // legacy pointer-hex form only when no stable name can be produced
    // (defensive — would indicate an invalid RD).
    std::string StableName = c2go::getStableRecordName(RD, Context);
    std::string MetadataName =
        !StableName.empty()
            ? (llvm::c2go::kStructMDPrefix + StableName).str()
            : (llvm::c2go::kStructMDPrefix + "ptr_" +
               llvm::utohexstr(reinterpret_cast<uintptr_t>(Ty), true))
                  .str();
    llvm::NamedMDNode *NMD = M.getOrInsertNamedMetadata(MetadataName);
    if (NMD->getNumOperands() == 0) {
      llvm::SmallVector<llvm::Metadata *, 8> NoScanOps;
      const ASTRecordLayout &RL = Context.getASTRecordLayout(RD);
      llvm::Type *I64 = llvm::Type::getInt64Ty(Ctx);
      unsigned FieldNo = 0;
      for (FieldDecl *F : RD->fields()) {
        if (F->hasAttr<C2GoUnmanagedAttr>()) {
          uint64_t OffsetBits = RL.getFieldOffset(FieldNo);
          uint64_t OffsetBytes = OffsetBits / 8;
          uint64_t SizeBytes =
              Context.getTypeSizeInChars(F->getType()).getQuantity();
          NoScanOps.push_back(llvm::ConstantAsMetadata::get(
              llvm::ConstantInt::get(I64, OffsetBytes)));
          NoScanOps.push_back(llvm::ConstantAsMetadata::get(
              llvm::ConstantInt::get(I64, SizeBytes)));
        }
        ++FieldNo;
      }
      NMD->addOperand(llvm::MDNode::get(Ctx, NoScanOps));

      // c2go §D2 phase 2/3: also emit a sibling named metadata listing
      // every named field's (offset, fieldname). The AArch64 AsmPrinter
      // consults this in Plan 9 emission mode to rewrite LDR/STR
      // immediate offsets into symbolic `<Rec>_<Field>` references
      // (matched against the `go_asm.h` symbols Go emits for the
      // c2gobind-generated struct). Anonymous / unnamed fields are
      // skipped — the symbolic form requires a name.
      //
      // Phase 3 extension: for any field whose type is itself a
      // c2go-managed (or plain) record, recursively emit *flattened*
      // (offset, "outerField+InnerRec_innerField") entries so accesses
      // like `p->inner.y` (path-aware TBAA Offset=8, BaseType=Outer)
      // can still be rewritten symbolically. The Plan 9 assembler
      // accepts additive symbol expressions of the form
      // `Outer_inner+Inner_y(R0)` — the exact form Go's runtime uses
      // (`(g_sched+gobuf_sp)(R10)` etc.). At link time these resolve
      // via Go's asmhdr-emitted `Outer_inner` and `Inner_y` defines.
      if (!StableName.empty()) {
        std::string FieldsMetadataName =
            (llvm::c2go::kStructMDPrefix + StableName + ".fields").str();
        llvm::NamedMDNode *FieldsNMD =
            M.getOrInsertNamedMetadata(FieldsMetadataName);
        if (FieldsNMD->getNumOperands() == 0) {
          // Walk a (potentially nested) record, appending entries to
          // FieldsNMD. `Prefix` is the chain of named outer fields
          // joined with '.' visually but emitted as `outer+InnerRec_`
          // segments — empty at the top level. `BaseOff` accumulates
          // the absolute byte offset from the outermost record.
          std::function<void(const RecordDecl *, uint64_t,
                             llvm::StringRef)> emitFields =
              [&](const RecordDecl *CurRD, uint64_t BaseOff,
                  llvm::StringRef PathSym) {
            const ASTRecordLayout &CurRL =
                Context.getASTRecordLayout(CurRD);
            unsigned FNo = 0;
            for (FieldDecl *F : CurRD->fields()) {
              if (!F->getIdentifier()) { ++FNo; continue; }
              uint64_t OffBits = CurRL.getFieldOffset(FNo);
              uint64_t OffBytes = BaseOff + (OffBits / 8);
              // Build the symbol-name fragment used after the
              // outermost `<StableName>_` prefix the AsmPrinter
              // prepends. At depth 0 this is just the field name
              // (`"inner"` → `Outer_inner`); at deeper levels we
              // chain with the inner record's stable name:
              // `"inner+Inner_y"` → `Outer_inner+Inner_y`.
              std::string EntryName;
              if (PathSym.empty())
                EntryName = F->getName().str();
              else {
                EntryName = PathSym.str();
                EntryName += '_';
                EntryName += F->getName().str();
              }
              llvm::Metadata *Pair[2] = {
                  llvm::ConstantAsMetadata::get(
                      llvm::ConstantInt::get(I64, OffBytes)),
                  llvm::MDString::get(Ctx, EntryName),
              };
              FieldsNMD->addOperand(llvm::MDNode::get(Ctx, Pair));

              // Recurse into nested struct/union fields. Only when
              // the field type names a record with a stable AST
              // name (so Go's asmhdr will emit a matching
              // `<InnerRec>_<innerfield>` symbol for the offsets
              // we reference). Plain scalar / pointer / array
              // fields don't recurse here — arrays of records
              // would require per-element entries which the
              // current Plan 9 indexed-addressing rewrite doesn't
              // consume, so we leave them to phase 4.
              const Type *FTU = F->getType()
                                    .getCanonicalType()
                                    .getTypePtrOrNull();
              if (FTU) {
                if (const auto *InnerRT = FTU->getAs<RecordType>()) {
                  const RecordDecl *InnerRD =
                      InnerRT->getDecl()->getDefinition();
                  // Only recurse when the nested record is itself a
                  // `c2go_struct` — only then does c2gobind emit a
                  // matching Go-side type whose asmhdr will provide
                  // the `<InnerRec>_<field>` symbol our additive
                  // expression links against. Plain non-c2go inner
                  // records (e.g. system C types) would generate
                  // unresolved external references at link time, so
                  // we skip them and let the AsmPrinter fall back to
                  // the raw-byte offset for `outer->plain.field`.
                  if (InnerRD && !InnerRD->isUnion() &&
                      InnerRD->hasAttr<C2GoStructAttr>()) {
                    std::string InnerStable =
                        c2go::getStableRecordName(InnerRD, Context);
                    if (!InnerStable.empty()) {
                      // The Plan 9 form after the outermost
                      // `<StableName>_` prefix becomes
                      // `<currentEntry>+<InnerStable>_<innerfield>`.
                      std::string NextPath = EntryName;
                      NextPath += '+';
                      NextPath += InnerStable;
                      emitFields(InnerRD, OffBytes, NextPath);
                    }
                  }
                }
              }
              ++FNo;
            }
          };
          emitFields(RD, /*BaseOff=*/0, /*PathSym=*/llvm::StringRef());
          // Tag the named metadata with the record's stable name as the
          // last operand so consumers can disambiguate it from the
          // primary `c2go.struct.<X>` no-scan list (same prefix, harder
          // to filter at NamedMD iteration time). Stored as a single-
          // operand MDNode `!{ !"<StableName>" }` so the parser can
          // recognize it by shape.
          // (For now: not strictly required — consumers look up by
          // the exact metadata-name form. Keep this comment for the
          // future MDNode-walking variant.)
        }
      }

      // §A2: when the struct is Go-owner (carries `c2go_linkname`), record
      // the linkname as a second operand so the LLVM C2GoMallocReplacement
      // pass skips its default typeinfo emission for this struct — clang
      // has already emitted only the external `@"type:<linkname>"` decl
      // and any pass-side definition would conflict with the Go-side type.
      if (const auto *LN = RD->getAttr<C2GoLinknameAttr>()) {
        llvm::Metadata *LinknameOp =
            llvm::MDString::get(Ctx, LN->getName());
        NMD->addOperand(llvm::MDNode::get(Ctx, LinknameOp));
      }
    }

    // c2go §A2: emit the Go-runtime typeinfo global (and its gcbitmap)
    // straight from the AST. For C-owner structs this is a
    // `linkonce_odr` definition; for Go-owner structs (those carrying
    // `c2go_linkname`) it's just an external decl bridging to the
    // Go-side `type:pkg.X` symbol. The LLVM C2GoMallocReplacement pass
    // still runs but checks for an existing typeinfo global before
    // emitting its own — keeps the legacy literal/anonymous path alive
    // and avoids double-emit conflicts.
    CGM.emitC2GoTypeinfo(RD);
  }

  // If this struct blocked a FunctionType conversion, then recompute whatever
  // was derived from that.
  // FIXME: This is hugely overconservative.
  if (SkippedLayout)
    TypeCache.clear();

  return Ty;
}

/// getCGRecordLayout - Return record layout info for the given record decl.
const CGRecordLayout &
CodeGenTypes::getCGRecordLayout(const RecordDecl *RD) {
  const Type *Key = Context.getCanonicalTagType(RD).getTypePtr();

  auto I = CGRecordLayouts.find(Key);
  if (I != CGRecordLayouts.end())
    return *I->second;
  // Compute the type information.
  ConvertRecordDeclType(RD);

  // Now try again.
  I = CGRecordLayouts.find(Key);

  assert(I != CGRecordLayouts.end() &&
         "Unable to find record layout information for type");
  return *I->second;
}

bool CodeGenTypes::isPointerZeroInitializable(QualType T) {
  assert((T->isAnyPointerType() || T->isBlockPointerType() ||
          T->isNullPtrType()) &&
         "Invalid type");
  return isZeroInitializable(T);
}

bool CodeGenTypes::isZeroInitializable(QualType T) {
  if (T->getAs<PointerType>() || T->isNullPtrType())
    return Context.getTargetNullPointerValue(T) == 0;

  if (const auto *AT = Context.getAsArrayType(T)) {
    if (isa<IncompleteArrayType>(AT))
      return true;
    if (const auto *CAT = dyn_cast<ConstantArrayType>(AT))
      if (Context.getConstantArrayElementCount(CAT) == 0)
        return true;
    T = Context.getBaseElementType(T);
  }

  // Records are non-zero-initializable if they contain any
  // non-zero-initializable subobjects.
  if (const auto *RD = T->getAsRecordDecl())
    return isZeroInitializable(RD);

  // We have to ask the ABI about member pointers.
  if (const MemberPointerType *MPT = T->getAs<MemberPointerType>())
    return getCXXABI().isZeroInitializable(MPT);

  // HLSL Inline SPIR-V types are non-zero-initializable.
  if (T->getAs<HLSLInlineSpirvType>())
    return false;

  // Everything else is okay.
  return true;
}

bool CodeGenTypes::isZeroInitializable(const RecordDecl *RD) {
  return getCGRecordLayout(RD).isZeroInitializable();
}

unsigned CodeGenTypes::getTargetAddressSpace(QualType T) const {
  // Return the address space for the type. If the type is a
  // function type without an address space qualifier, the
  // program address space is used. Otherwise, the target picks
  // the best address space based on the type information
  return T->isFunctionType() && !T.hasAddressSpace()
             ? getDataLayout().getProgramAddressSpace()
             : getContext().getTargetAddressSpace(T.getAddressSpace());
}
