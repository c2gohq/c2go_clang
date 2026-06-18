// A __c2go_typeinfo RTTI descriptor survives AST merge: importing managed
// records from two translation units via -ast-merge must re-point each
// C2GoTypeInfoAttr's RecordDecl into the host ASTContext, so the merged IR
// defines a typeinfo descriptor (with initializer) rather than emitting a
// bare extern reference. AST merge (libASTImporter) is c2go's cross-TU path
// to CodeGen; this complements the PCH and Modules deserialization tests.
//
// REQUIRES: aarch64-registered-target
//
// Pre-build two AST inputs, each with its own managed record and
// __c2go_typeinfo use.
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -emit-pch -o %t.1.ast %S/Inputs/typeinfo1.c
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -emit-pch -o %t.2.ast %S/Inputs/typeinfo2.c
//
// Merge both ASTs into this host TU and lower to IR. The host TU itself is
// empty except for the FileCheck directives; the merge is what drives
// typeinfo descriptor emission for the imported records.
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -ast-merge %t.1.ast -ast-merge %t.2.ast \
// RUN:   -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

// Both descriptors must be DEFINED (initializer present) in the merged IR
// after import, proving the imported C2GoTypeInfoAttr referenced a
// host-context RecordDecl. Linkage is object-format dependent, so accept
// linkonce_odr / weak_odr / internal; what we pin is "has an initializer"
// (i.e. `= { ... }` follows the type, not `external`).
// CHECK-DAG: @c2go.typeinfo.NodeA434 = {{(linkonce_odr|weak_odr|internal)}} {{(hidden |unnamed_addr |constant|global| )*}}%c2go._gotype {
// CHECK-DAG: @c2go.typeinfo.NodeB434 = {{(linkonce_odr|weak_odr|internal)}} {{(hidden |unnamed_addr |constant|global| )*}}%c2go._gotype {

// The user-visible call sites in the imported bodies must reference those
// same symbols by name.
// CHECK-DAG: call {{.*}}@gc_malloc({{.*}}@c2go.typeinfo.NodeA434
// CHECK-DAG: call {{.*}}@gc_malloc({{.*}}@c2go.typeinfo.NodeB434
