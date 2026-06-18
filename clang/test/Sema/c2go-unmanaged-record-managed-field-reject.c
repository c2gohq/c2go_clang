// A record explicitly marked __attribute__((c2go_unmanaged)) must not contain
// a managed-pointer field, directly or through an embedded record. An
// unmanaged record gets no Go typeinfo or gcdata, so a managed pointer it
// holds would be invisible to the GC and could be reclaimed out from under it.
// Sema rejects it; the fix is to drop the c2go_unmanaged marker or annotate
// the offending field(s) c2go_unmanaged.
//
// A "managed-pointer field" is a pointer carrying c2go_managed (explicit or
// attribute sugar) or a pointer to a c2go_managed. A plain pointer field
// defaults to unmanaged and is not rejected.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -Wno-c2go-unmanaged-pointer -fsyntax-only -verify %s

struct __attribute__((c2go_managed)) M { int x; };

// Pointer to a c2go_managed is a managed field. Unmanaged record + managed
// field -> error.
struct __attribute__((c2go_unmanaged)) Bad1 { // expected-error{{record 'Bad1' is marked '__attribute__((c2go_unmanaged))' but contains a c2go_managed pointer field}}
  struct M *m;
};

// Explicit c2go_managed pointer field is a managed field -> error.
struct __attribute__((c2go_unmanaged)) Bad2 { // expected-error{{record 'Bad2' is marked '__attribute__((c2go_unmanaged))' but contains a c2go_managed pointer field}}
  int * __attribute__((c2go_managed)) p;
};

// Transitive: embedding an inferred-managed record by value -> error.
struct WithM { struct M *m; }; // inferred managed (has a managed-ptr field)
struct __attribute__((c2go_unmanaged)) Bad3 { // expected-error{{record 'Bad3' is marked '__attribute__((c2go_unmanaged))' but contains a c2go_managed pointer field}}
  struct WithM w;
};

// Plain (unannotated) pointers default to unmanaged -> no error.
struct __attribute__((c2go_unmanaged)) OK1 {
  int *a;
  void *b;
};

// A field explicitly re-annotated c2go_unmanaged neutralizes the managed
// pointer -> no error (the escape route named in the diagnostic).
struct __attribute__((c2go_unmanaged)) OK2 {
  struct M * __attribute__((c2go_unmanaged)) m;
};

// Pure scalar unmanaged record -> no error.
struct __attribute__((c2go_unmanaged)) OK3 {
  int x;
  double d;
};
