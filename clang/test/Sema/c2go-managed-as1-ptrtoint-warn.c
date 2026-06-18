// Converting a c2go_managed pointer to an integer drops addrspace(1) GC
// tracking. This warns rather than errors: pure hash/printf uses are common
// and safe so long as the integer does not outlive a safepoint. Converting an
// unmanaged pointer to an integer is unaffected.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

struct __attribute__((c2go_managed)) Foo { int x; };
struct Foo *get(void);

unsigned long f(void) {
  return (unsigned long)get(); // expected-warning{{converting a c2go_managed pointer to integer drops AS1 GC provenance}}
}

unsigned long hash(void) {
  return (unsigned long)get() & 0xFF; // expected-warning{{converting a c2go_managed pointer}}
}

// Casting an unmanaged pointer to int is unaffected.
struct Bar { int x; };
struct Bar *get_unmanaged(void);
unsigned long g(void) {
  return (unsigned long)get_unmanaged(); // no warning
}
