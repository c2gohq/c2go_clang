// Function-pointer alternatives in a `c2go_variant` union are no-scan (they
// point at code, never at the heap), so they go into the scalar blob, not into
// a precisely-scanned pointer slot. Only DATA pointers get a scan slot.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -Werror -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

typedef __SIZE_TYPE__ size_t;
struct __attribute__((c2go_managed)) Foo { struct Foo *next; };
typedef void (*fn_t)(void);

union __attribute__((c2go_variant)) W {
  struct Foo *p;   // DATA ptr -> scan slot @0
  fn_t        f;   // FUNCTION ptr -> no-scan blob @8
  long        n;   // scalar -> blob @8
};

struct __attribute__((c2go_managed)) H2 {
  struct H2 *self;   // word0 -> scan
  union W    w;      // word1 = data-ptr slot (scan); word2 = blob (no-scan)
};

// Only word0 (self) + word1 (W data-ptr slot) scan; the funcptr/scalar blob
// (word2) is NOT scanned -> `\03`. Size = 24 (self 8 + W 16).
// CHECK: @c2go.gcbitmap.H2 = {{.*}}constant [1 x i8] c"\03"
// CHECK: @c2go.typeinfo.H2 = {{.*}}%c2go._gotype { i64 24, i64 16,

extern void *gc_malloc(const void *ti, size_t n);
void *mk2(void) { return gc_malloc(__c2go_typeinfo(struct H2), sizeof(struct H2)); }

// The funcptr member redirects to the no-scan blob at offset 8 (not a scan
// slot at offset 0).
// CHECK-LABEL: @get_f
// CHECK: getelementptr inbounds i8, ptr %{{[0-9]+}}, i64 8
void *get_f(union W *u) { return (void *)u->f; }

// The data pointer is still the scan slot at offset 0 (no GEP).
// CHECK-LABEL: @get_dp
// CHECK-NOT: getelementptr
// CHECK: load ptr addrspace(1)
struct Foo *get_dp(union W *u) { return u->p; }
