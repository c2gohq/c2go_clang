// Storing a c2go_managed pointer into an unmanaged location (e.g. void*) drops
// the addrspace(1) GC discriminator, so Sema rejects it. The escape hatch is an
// explicit (__attribute__((c2go_managed)) T *) cast. Plain assignment,
// declaration initialization, and aggregate (array element / struct field)
// initializers all flow through the same store check and must diagnose alike.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

struct __attribute__((c2go_managed)) N { int v; };

void f(struct N *managed) {
  // Implicit drop on assignment.
  void *p;
  p = managed; // expected-error{{drops the Go GC discriminator}}

  // Declaration initialization is also a store.
  void *q = managed; // expected-error{{drops the Go GC discriminator}}
  (void)q;

  // Explicit cast back to managed is the escape hatch (no diagnostic).
  void *ok = (__attribute__((c2go_managed)) void *)managed;
  (void)ok;

  // Aggregate initializers flow through the same store check.
  void *arr[1] = { managed }; // expected-error{{drops the Go GC discriminator}}
  (void)arr;

  struct U { void *p; };
  struct U u = { managed }; // expected-error{{drops the Go GC discriminator}}
  (void)u;
}
