// A plain (non-variant) union overlaying a c2go_managed pointer alternative
// with a non-managed alternative cannot be safely scanned: the GC cannot tell
// which alternative is live and must treat the storage as opaque. Sema warns.
// The escape hatches are to mark the union c2go_variant (converting it to a
// scanned struct), annotate the non-managed field c2go_unmanaged, or split the
// union. A pure-managed or pure-unmanaged union does not warn.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -Wno-c2go-unmanaged-pointer -fsyntax-only -verify %s

struct __attribute__((c2go_managed)) Foo { int x; };

// Mixed: managed pointer + raw void* (unmanaged).
union Bad { // expected-warning{{union 'Bad' mixes c2go_managed and unmanaged}}
  struct Foo *m;
  void *u;
};

// Mixed: managed pointer + non-pointer scalar.
union BadIntPtr { // expected-warning{{union 'BadIntPtr' mixes c2go_managed and unmanaged}}
  struct Foo *m;
  int x;
};

// Pure unmanaged union: no warning.
union UnmanagedOK {
  int *u1;
  void *u2;
};

// Pure managed union: no warning.
union ManagedOK {
  struct Foo *m1;
  struct Foo *m2;
};
