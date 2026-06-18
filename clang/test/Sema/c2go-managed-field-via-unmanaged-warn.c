// Storing a pointer into a managed-pointer field through an unmanaged struct
// pointer is a warning, not an error. No write barrier is emitted on this
// path: the slot is reached through an unmanaged pointer, stays in addrspace(0),
// and the barrier pass skips it, so the barrier's precondition (the slot is
// GC-managed memory) cannot be established. The programmer assumes
// responsibility. The diagnostic is in the default-on warning group
// -Wc2go-managed-field-via-unmanaged. Storing through a managed pointer, or
// reading or writing a scalar field, is unaffected.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s
// Suppressing the group turns the (now non-fatal) diagnostic off entirely:
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -Wno-c2go-managed-field-via-unmanaged -fsyntax-only -verify=quiet %s

// quiet-no-diagnostics

struct __attribute__((c2go_managed)) Obj { int v; };

// A managed record with a managed-pointer field (pointer to a c2go_managed).
struct __attribute__((c2go_managed)) Holder {
  struct Obj *m;            // managed pointer field
  int scalar;
};

void take_unmanaged(struct Holder * __attribute__((c2go_unmanaged)) up,
                    struct Obj *o) {
  // Storing into the managed-ptr field through an unmanaged pointer: warning,
  // no barrier.
  up->m = o; // expected-warning{{storing into managed-pointer field 'm' through an unmanaged pointer}}

  // Writing a scalar field through the unmanaged pointer is unaffected.
  up->scalar = 3; // ok

  // Reading the managed field through the unmanaged pointer is fine.
  struct Obj *r = up->m; // ok
  (void)r;
}

void take_managed(struct Holder *mp, struct Obj *o) {
  // Through a managed pointer the barrier can be emitted: no diagnostic.
  mp->m = o; // ok
}
