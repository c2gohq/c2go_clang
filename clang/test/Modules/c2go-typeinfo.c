// A __c2go_typeinfo RTTI descriptor survives module deserialization. A
// module-defined `static inline` helper uses __c2go_typeinfo(T); when the
// consumer TU calls it, codegen of the inline body re-runs typeinfo
// resolution on the deserialized record, which must yield a defined
// descriptor (with initializer) rather than a bare external reference.
// Module deserialization is a separate channel from -ast-merge; this
// complements the PCH and AST-merge tests.
//
// REQUIRES: aarch64-registered-target
//
// RUN: rm -rf %t
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fmodules -fimplicit-module-maps -fmodules-cache-path=%t \
// RUN:   -fmodule-map-file=%S/Inputs/c2go-typeinfo.modulemap \
// RUN:   -I %S/Inputs -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

#include "c2go-typeinfo-mod.h"

// Calling the module-provided `static inline` helper from this TU forces
// codegen of its body here, re-running typeinfo resolution on the
// deserialized Node434Mod record.
void *alloc_node_via_mod(void) {
  return prime434_mod();
}

// The descriptor must be DEFINED in this TU's IR (initializer present), not
// a bare `external` reference - the proof that the C2GoTypeInfoAttr survived
// module deserialization. Linkage is object-format dependent; the lock is
// "has an initializer".
// CHECK: @c2go.typeinfo.Node434Mod = {{(linkonce_odr|weak_odr|internal)}} {{(hidden |unnamed_addr |constant|global| )*}}%c2go._gotype {

// The user-visible call site (inside the inline body, now codegen'd here)
// references the descriptor by name.
// CHECK-LABEL: define {{.*}}@prime434_mod
// CHECK: call {{.*}}@gc_malloc({{.*}}@c2go.typeinfo.Node434Mod
