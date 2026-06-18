// A union that type-puns a scanned pointer slot with a scalar at the same
// bytes is a hard error: the pointer slot is force-scanned, so a scalar alias
// would feed Go's GC a non-pointer (tripping `invalidptr`).
//
// REQUIRES: aarch64-registered-target
//
// RUN: not %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o /dev/null %s 2>&1 | FileCheck %s

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

typedef __SIZE_TYPE__ size_t;
struct Node { int x; };

// Punning union reached via a GC-tracked struct's typeinfo walk.
struct __attribute__((c2go_managed)) Host {
  struct Host *real;          // makes Host managed -> typeinfo emitted
  union {
    struct Node *p;           // scanned pointer at offset 0
    long         i;           // scalar overlap -> ERROR
  } u;
};

extern void *gc_malloc(const void *ti, size_t n);
void *mk(void) { return gc_malloc(__c2go_typeinfo(struct Host), sizeof(struct Host)); }

// CHECK: error: c2go: union type-puns a scanned pointer slot
// CHECK-SAME: blocker=i
