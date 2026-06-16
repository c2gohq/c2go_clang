//===- CGC2GoManifestHelpers.h - c2go manifest helpers shared by CodeGen --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go WF2 (#319): shared helpers that translate a clang AST signature /
// QualType into the Go-side spellings the manifest needs. Historically these
// lived as `static` helpers inside CodeGenAction.cpp's anonymous namespace,
// only callable from `buildC2GoManifest`. WF2 needs the same strings (go_sig,
// argsize, go_type) to be encoded as function/global IR attributes so that
// c2go-lto can rebuild the manifest from combined bitcode without re-reading
// the AST. To do so without duplicating the logic we expose the same helpers
// in a CodeGen-private header.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_CODEGEN_CGC2GOMANIFESTHELPERS_H
#define LLVM_CLANG_LIB_CODEGEN_CGC2GOMANIFESTHELPERS_H

#include "clang/AST/Type.h"
#include "llvm/Support/JSON.h"
#include <cstdint>
#include <string>

namespace llvm {
class Module;
} // namespace llvm

namespace clang {

class ASTContext;
class FunctionDecl;
class RecordDecl;
class VarDecl;

namespace c2go {

/// Convert a clang QualType to a Go-side type expression. Minimal v0 mapper —
/// handles primitive ints, pointers, named records, and constant-size arrays;
/// unhandled cases fall back to `uintptr`.
///
/// \p IsUnmanaged overrides pointer mapping: pointer-shaped types become
/// `uintptr` so the Go GC won't trace them. Use this when the original decl
/// carries c2go_unmanaged, or sits inside an unmanaged context.
std::string mapC2GoType(QualType QT, const ASTContext &Ctx,
                        bool IsUnmanaged = false);

/// Build a Go-style "func Name(arg, ...) ret" signature string for \p FD.
/// Honors c2go_unmanaged distinctly on the function (return) versus on each
/// parameter (per-parameter world, v14 spec).
std::string buildC2GoGoSig(const FunctionDecl *FD, const ASTContext &Ctx);

/// Compute Go ABI0 frame size for \p FD's signature. Mirrors Go's
/// cmd/compile TFUNCARGS / ABIAnalyzeTypes: places args type-aligned, rounds
/// the arg area up to RegSize, places the result, rounds total up to
/// RegSize. v0 only handles scalars and pointers (no aggregate-by-value
/// alignment recursion).
uint64_t computeC2GoArgSize(const FunctionDecl *FD, const ASTContext &Ctx);

/// WF2 (#319 C4a): emit the `c2go.struct.<RecName>.meta` named MD describing
/// a c2go_struct/c2go_managed record so c2go-lto can rebuild the manifest
/// `types[]` entry from combined bitcode. Idempotent — re-emission for the
/// same record is a no-op. \p M is the destination module; \p RD must be a
/// complete c2go-tracked record definition.
///
/// Operand layout (one MDNode, 4 operands):
///   0: !"managed" | !"unmanaged"
///   1: !"not_applicable" | !"scheme1" | !"scheme2"  (union scheme)
///   2: i64 union_ptr_offset (scheme1 only; -1 otherwise)
///   3: !"<linkname>" if c2go_linkname is set; empty MDString otherwise.
void emitC2GoStructMeta(llvm::Module &M, const RecordDecl *RD,
                        const ASTContext &Ctx);

/// WF2 (#319 C4a): emit the `c2go.struct.<RecName>.godef` named MD carrying
/// the serialized Go struct definition text computed by buildC2GoManifest.
/// Separated from emitC2GoStructMeta because the GoDef text is only available
/// after the manifest walk (which also runs diagnostics + nested anonymous
/// record promotion), while the lightweight 4-tuple in `.meta` is needed
/// before manifest emission. Idempotent.
void emitC2GoStructGoDef(llvm::Module &M, const RecordDecl *RD,
                         const ASTContext &Ctx, llvm::StringRef GoDef);

/// WF2 (#367 Bug A): emit the `c2go.func.<CName>` named MD carrying all
/// 8 manifest-grade boundary fields the AST writer stamps into the JSON
/// symbols[] entry. Lets c2go-lto rebuild the manifest from combined
/// bitcode byte-identically to the AST manifest path, without falling
/// back on per-field IR function attributes (which only cover three
/// fields: c-name, go-sig, unmanaged-return).
///
/// Operand layout (one MDNode, 14 operands, fixed order):
///   0:  name           (MDString)
///   1:  go_sig         (MDString)
///   2:  kind           (MDString: "func" | "unmanaged_extern")
///   3:  managed        (i1 ConstantAsMetadata)
///   4:  go_name        (MDString)
///   5:  abi            (MDString: "abi0")
///   6:  asm_symbol     (MDString)
///   7:  argsize        (i32 ConstantAsMetadata)
///   8:  needs_linkname (i1 ConstantAsMetadata)
///   9:  is_variadic    (i1 ConstantAsMetadata)
///   10: has_float      (i1 ConstantAsMetadata)
///   11: has_aggregate  (i1 ConstantAsMetadata)
///   12: c_entry        (i1 ConstantAsMetadata; true only for `main`)
///   13: entry_sig      (MDString; "" when c_entry=false)
///
/// Idempotent — re-emission for the same name is a no-op. Missing
/// strings emit empty MDString; missing bool flags emit i1 false.
void emitC2GoFuncManifest(llvm::Module &M, llvm::StringRef CName,
                          const llvm::json::Object &Sym);

} // namespace c2go
} // namespace clang

#endif // LLVM_CLANG_LIB_CODEGEN_CGC2GOMANIFESTHELPERS_H
