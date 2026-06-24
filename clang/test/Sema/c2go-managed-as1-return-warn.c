// Returning a c2go_managed pointer through an unmanaged return type drops the
// addrspace(1) discriminator, so Sema warns (it is allowed but lossy). Returning through a managed
// return type is fine, and an explicit (__attribute__((c2go_managed)) T *)
// cast is the escape hatch.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

struct __attribute__((c2go_managed)) N { int v; };

void *bad_ret(struct N *managed) {
  return managed; // expected-warning{{allowed but lossy}}
}

// Returning to a managed return type is fine.
struct N *good_ret(struct N *managed) { return managed; }

// Explicit escape-hatch cast suppresses the diagnostic.
void *ok_ret(struct N *managed) {
  return (__attribute__((c2go_managed)) void *)managed;
}
