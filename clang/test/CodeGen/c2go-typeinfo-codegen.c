// Positive CodeGen test for the `__c2go_typeinfo(T)` lowering path.
//
// `__c2go_typeinfo(struct T)` must produce a locally-defined
// `@c2go.typeinfo.<Name>` constant (with an initializer, not a bare extern
// reference), and the user-visible call site (`gc_malloc(__c2go_typeinfo(T),
// n)`) must load from that same symbol. Pins the happy-path IR shape so a
// refactor that drops the descriptor emission or the carrier attribute is
// caught here.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

typedef __SIZE_TYPE__ size_t;

struct __attribute__((c2go_managed)) Node392 {
  struct Node392 *next;
  long            tag;
};

extern void *gc_malloc(const void *type_info, size_t n);

void *alloc_node392(void) {
  return gc_malloc(__c2go_typeinfo(struct Node392), sizeof(struct Node392));
}

// The descriptor must be DEFINED in this TU (initializer present), not a bare
// `external` decl. The exact linkage is object-format dependent, so it is not
// pinned here; `linkonce_odr` / `weak_odr` / `internal` are all acceptable.
// What is pinned is "has an initializer" (i.e. `{ ... }` follows the type).
// CHECK: @c2go.typeinfo.Node392 = {{(linkonce_odr|weak_odr|internal)}} {{(hidden |unnamed_addr |constant|global| )*}}%c2go._gotype {

// And the user-visible call site must reference the same symbol by name -
// `gc_malloc`'s first argument is the address of `@c2go.typeinfo.Node392`.
// CHECK-LABEL: define {{.*}}@alloc_node392
// CHECK: call {{.*}}@gc_malloc({{.*}}@c2go.typeinfo.Node392
