// Passing a c2go_managed pointer to an unmanaged extern parameter (e.g. void*)
// drops the addrspace(1) GC discriminator, so Sema rejects it. The escape
// hatch is an explicit (__attribute__((c2go_managed)) T *) cast; passing to a
// managed parameter is fine.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

struct __attribute__((c2go_managed)) N { int v; };

extern void libc_free(void *p);
extern void libc_take_n(struct N *n); // managed param: OK

void caller(struct N *managed) {
  libc_free(managed); // expected-error{{drops the Go GC discriminator}}
  libc_take_n(managed); // OK (managed parameter)
  // Explicit cast back to managed is the in-source escape hatch.
  libc_free((__attribute__((c2go_managed)) void *)managed);
}
