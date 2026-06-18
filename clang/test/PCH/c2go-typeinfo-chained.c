// A __c2go_typeinfo RTTI descriptor survives chained-PCH deserialization.
// Layer 1 (base) holds only the managed record Node434Chain; layer 2
// (deriv), built on top of layer 1, adds a `static inline` helper whose
// body uses __c2go_typeinfo(struct Node434Chain). The implicit VarDecl is
// born in the deriv layer but references a record from the base layer, so
// both the cross-layer RecordDecl and the C2GoTypeInfoAttr must round-trip
// two ASTReader stages. The consumer TU loads layer 2 (transitively layer
// 1) and calls the helper, which must emit a defined descriptor (with
// initializer) rather than a bare external reference.
//
// REQUIRES: aarch64-registered-target
//
// Build layer 1: base PCH with just the managed record.
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -emit-pch -o %t.1.pch %S/Inputs/c2go-typeinfo-base.h
//
// Build layer 2: deriv PCH chained on top of layer 1, adding the static
// inline helper that uses __c2go_typeinfo.
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -emit-pch -o %t.2.pch -include-pch %t.1.pch \
// RUN:   %S/Inputs/c2go-typeinfo-deriv.h
//
// Consume the chained PCH and lower to IR.
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -include-pch %t.2.pch -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

// Calling the deriv-layer helper from this TU forces codegen of the inline
// body here, exercising the full chained-PCH deserialization on both the
// base-layer RecordDecl and the deriv-layer implicit VarDecl.
void *alloc_node_via_chain(void) {
  return prime434_chain();
}

// The descriptor must be DEFINED in this TU's IR (initializer present), not
// a bare `external` reference - the proof that both the C2GoTypeInfoAttr and
// its referenced RecordDecl survived the chained PCH round-trip. Linkage is
// object-format dependent; the lock is "has an initializer".
// CHECK: @c2go.typeinfo.Node434Chain = {{(linkonce_odr|weak_odr|internal)}} {{(hidden |unnamed_addr |constant|global| )*}}%c2go._gotype {

// The user-visible call site (inside the inline body, now codegen'd here)
// references the descriptor by name.
// CHECK-LABEL: define {{.*}}@prime434_chain
// CHECK: call {{.*}}@gc_malloc({{.*}}@c2go.typeinfo.Node434Chain
