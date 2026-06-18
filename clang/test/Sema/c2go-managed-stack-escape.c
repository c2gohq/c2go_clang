// Warn when storing &local (the address of an automatic or parameter in the
// current function) into a c2go-managed heap-rooted pointer field. The heap
// object outlives the stack frame, so the stored address dangles into
// reclaimed stack memory once the function returns and the GC trips on it the
// next time the heap object is scanned. Default-on warning
// (-Wc2go-managed-stack-escape).
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

struct __attribute__((c2go_managed)) Node {
  struct Node *parent; // managed (pointer to c2go_managed)
  void        *aux;    // plain pointer field on a managed record
};

struct Plain {
  int *p; // unmanaged record + unmanaged field: no diagnostic
};

void take(int);

// Positive: p->field = &local where p is a managed pointer (pointer to a
// c2go_managed) and field is a pointer field; the stored stack address
// outlives the call.
void positive_arrow(struct Node *n) {
  int  local_int = 0;
  // aux is a plain void* field on a managed record, hence heap-rooted.
  n->aux = &local_int; // expected-warning{{storing &local into a c2go-managed heap-rooted pointer field}}
  take(local_int);
}

// Positive: a parameter address is also a "local" (a parameter has local
// storage). (void *)&param with a C-style cast is the common shape.
void positive_param(struct Node *n, int param_int) {
  n->aux = (void *)&param_int; // expected-warning{{storing &local into a c2go-managed heap-rooted pointer field}}
}

// Positive: dot-access on a c2go_managed lvalue is also a heap-root; the field
// storage is GC-tracked regardless of the syntactic access form ((*n).aux is
// equivalent to n->aux).
void positive_dot(struct Node *n) {
  int x = 0;
  (*n).aux = &x; // expected-warning{{storing &local into a c2go-managed heap-rooted pointer field}}
}

// Negative: storing &local into a plain (unmanaged) struct's field; the holder
// is not heap-rooted under the GC, so no escape warning.
void negative_unmanaged(struct Plain *u) {
  int x = 0;
  u->p = &x; // no diagnostic
}

// Negative: storing &local into a stack local of struct type; the holder is
// itself a stack object, so this is intra-frame and not an escape.
void negative_local_holder(struct Node *parent) {
  struct Node local;
  local.parent = parent; // no diagnostic (parent is a managed pointer arg, not &local)
  (void)local;
}

// Negative: the RHS is not an &local; passing an already-managed pointer value
// is the legitimate use of the field.
void negative_non_addrof(struct Node *dst, struct Node *src) {
  dst->parent = src; // no diagnostic
}

// Negative: the field is explicitly c2go_unmanaged, so it is opted out of GC
// scanning and storing &local is the user's call.
struct __attribute__((c2go_managed)) Opt {
  void *__attribute__((c2go_unmanaged)) aux;
};
void negative_opt_out(struct Opt *o) {
  int x = 0;
  o->aux = &x; // no diagnostic
}
