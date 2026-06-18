// Converting a c2go_managed pointer to a different managed pointer type whose
// GC pointer layout (gcdata) is not prefix-compatible is rejected: the GC
// scans the pointee with the destination type's bitmap, so a mismatched
// pointer layout scans the wrong words. Exemptions are a (managed) void*
// bridge and structural embedding where one type's scan-pointer layout is an
// initial prefix of the other. There is no plain explicit-cast escape; the
// only route is through void*. A "managed pointer" here is a pointer to a
// c2go_managed. Plain (non-managed) struct pointers are unaffected.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

// A: one managed pointer at offset 0; sizeof 16.
struct __attribute__((c2go_managed)) A {
  struct A *next; // managed ptr @0
  int x;
};
// B: managed pointers at offsets {0, 16}; sizeof 24. A's layout {0} is an
// initial prefix of B's {0,16}, and sizeof(A) <= sizeof(B).
struct __attribute__((c2go_managed)) B {
  struct B *next;  // managed ptr @0
  int y;
  struct B *other; // managed ptr @16
};
// C: scalar at 0, managed pointer at offset 8 -> layout {8}, NOT a prefix of
// A {0} or B {0,16}.
struct __attribute__((c2go_managed)) C {
  int z;
  struct C *p; // managed ptr @8
};

void f(struct A *a, struct B *b) {
  // A -> B: prefix-compatible -> OK.
  struct B *b2 = (struct B *)a;
  (void)b2;

  // B -> A: A {0} is a prefix of B {0,16} (prefix relation is symmetric in the
  // helper), and sizeof(A) <= sizeof(B) -> OK.
  struct A *a2 = (struct A *)b;
  (void)a2;

  // A -> C: {0} vs {8} not prefix -> error.
  struct C *c2 = (struct C *)a; // expected-error{{converting between c2go_managed pointer types with incompatible GC layouts}}
  (void)c2;

  // B -> C: {0,16} vs {8} not prefix -> error.
  struct C *c3 = (struct C *)b; // expected-error{{converting between c2go_managed pointer types with incompatible GC layouts}}
  (void)c3;

  // Same type -> OK.
  struct A *a3 = (struct A *)a;
  (void)a3;

  // Managed void* bridge -> OK (escape hatch).
  struct C *c4 = (struct C *)(__attribute__((c2go_managed)) void *)a;
  (void)c4;
}

// Plain (non-managed) struct pointers are unaffected by D4.
struct P1 { int *a; };
struct P2 { int *b; };
void plain(struct P1 *p) {
  struct P2 *q = (struct P2 *)p; // ok: neither is a managed pointer
  (void)q;
}
