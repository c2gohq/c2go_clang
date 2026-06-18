// A __c2go_typeinfo RTTI descriptor survives PCH deserialization. A second
// TU that #includes a PCH built from a header using __c2go_typeinfo(T) must
// still emit the descriptor definition (with initializer) in its own
// codegen, proving the C2GoTypeInfoAttr round-tripped through PCH rather
// than degrading into a bare extern reference. This complements the Modules
// and AST-merge deserialization tests.
//
// REQUIRES: aarch64-registered-target
//
// Build the PCH from the header.
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -emit-pch -o %t.pch %S/Inputs/c2go-typeinfo.h
//
// Consume the PCH from a second TU and check IR.
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -include-pch %t.pch -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

// prime392_pch is `static inline` in the PCH header, so its body - which
// references __c2go_typeinfo(struct Node392PCH) - is only codegen'd in TUs
// that call it. Calling it here forces typeinfo resolution to run on the
// PCH-deserialized record and implicit VarDecl.
void *alloc_node_via_pch(void) {
  return prime392_pch();
}

// Descriptor must be DEFINED in this TU's IR (initializer present), not a
// bare external - the proof the C2GoTypeInfoAttr survived PCH. Linkage is
// object-format dependent; we only assert "definition with initializer".
// CHECK: @c2go.typeinfo.Node392PCH = {{(linkonce_odr|weak_odr|internal)}} {{(hidden |unnamed_addr |constant|global| )*}}%c2go._gotype {

// The user-visible call site (inside the PCH-defined inline function, now
// codegen'd in this TU) loads the descriptor address by name.
// CHECK-LABEL: define {{.*}}@prime392_pch
// CHECK: call {{.*}}@gc_malloc({{.*}}@c2go.typeinfo.Node392PCH
