// c2go #650 MODEL CONSTRAINT: qsort cannot sort elements that carry managed
// pointers — its void* interface erases the element type, so the byte-wise
// moves are not GC-safe under concurrent marking (invisible/torn pointers, no
// write barriers). The visible call-site spelling is a hard error; bsearch
// only reads elements and stays allowed, as do scalar/unmanaged elements. A
// base laundered through an explicit void* cast escapes the check (UB,
// documented in c2go-libc source/qsort.c) — the same best-effort contract as
// the union-punning error vs memcpy.
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

typedef unsigned long size_t;
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
void *bsearch(const void *, const void *, size_t, size_t,
              int (*)(const void *, const void *));

#pragma c2go managed(6) push
struct Node {
  struct Node *next;
  long tag;
};
struct Rec { // transitively managed: holds a Node *
  struct Node *n;
  int k;
};
#pragma c2go pop

struct Plain { // scalar-only: fine to sort
  int a;
  long b;
};

// A comparator held in an fp VARIABLE keeps the test focused on the base
// argument (passing a named function directly trips unrelated c2go
// function-pointer CC diagnostics).
static int (*cmp)(const void *, const void *);

void f(struct Node *nodes[8], struct Rec recs[4], struct Plain plain[4],
       int ints[16]) {
  // Array of managed pointers: rejected.
  qsort(nodes, 8, sizeof nodes[0], cmp); // expected-error {{qsort cannot sort elements of type}}

  // Aggregate elements transitively holding a managed pointer: rejected.
  // (The lossy managed→void* argument warning is pre-existing and unrelated.)
  qsort(recs, 4, sizeof recs[0], cmp); // expected-error {{qsort cannot sort elements of type}} \
                                       // expected-warning {{drops its managed (AS1) write-barrier tracking}}

  // Scalar / unmanaged elements: allowed.
  qsort(ints, 16, sizeof ints[0], cmp);
  qsort(plain, 4, sizeof plain[0], cmp);

  // bsearch only reads — allowed even on managed elements.
  struct Node *key = 0;
  (void)bsearch(&key, nodes, 8, sizeof nodes[0], cmp);

  // Explicit void* laundering escapes the static check (documented UB).
  qsort((void *)recs, 4, sizeof recs[0], cmp);
}
