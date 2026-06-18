// The documented idiom gc_malloc(__c2go_typeinfo(struct N), n) must NOT trip
// the c2go-managed-as1 diagnostics. The typeinfo expression is a stable
// per-type RTTI descriptor pointer (a *runtime._type), not a GC-tracked
// addrspace(1) reference, and the receiving const void* parameter is
// AS-neutral by design. Sema treats the typeinfo expression as AS-neutral at
// the source so the managed-pointer escape checks do not fire on it, on
// assignment, call, and pointer-to-integer paths alike.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

// expected-no-diagnostics

typedef __SIZE_TYPE__ size_t;

struct __attribute__((c2go_managed)) N { int v; };
struct __attribute__((c2go_managed)) A { int v; };

// gc_malloc declared as in <c2go.h>: AS-neutral const void* type_info.
extern void *gc_malloc(const void *type_info, size_t n);

// (a) the canonical idiom: bare __c2go_typeinfo passed straight in.
void *alloc_one(void) {
  return gc_malloc(__c2go_typeinfo(struct N), 16);
}

// (b) transitive: typeinfo stored into a local then forwarded, exercising the
// store/init and call paths without a per-use store-site carve-out.
void *alloc_via_local(void) {
  const void *ti = __c2go_typeinfo(struct N);
  return gc_malloc(ti, 16);
}

// (c) converting the typeinfo descriptor address to integer must not warn
// either, since the descriptor is AS-neutral.
unsigned long typeinfo_as_int(void) {
  return (unsigned long)__c2go_typeinfo(struct N);
}

// (d) conditional expression mixing two typeinfo values: the AS-neutrality is
// a property of the type, not of a bare __c2go_typeinfo CallExpr AST shape.
void *alloc_cond(int sel) {
  return gc_malloc(sel ? __c2go_typeinfo(struct N) : __c2go_typeinfo(struct A),
                   16);
}

// (e) gc_malloc(0, n): the noscan-blob form. The constant-0 null pointer
// constant is not a managed pointer and must keep compiling cleanly.
void *alloc_noscan(size_t n) {
  return gc_malloc(0, n);
}
