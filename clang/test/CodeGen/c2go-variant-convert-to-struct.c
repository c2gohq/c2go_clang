// A union marked `__attribute__((c2go_variant))` is converted to a struct
// whose slots are partitioned by GC class: data pointers (managed or
// unmanaged) go into precisely-scanned `unsafe.Pointer` slots; scalars and
// function pointers go into a single trailing no-scan `[N]uint8` blob. No byte
// is "pointer in one alternative, scalar in another", so the Go GC scans the
// pointer slot precisely instead of force-scanning a punned scalar (which
// would trip `invalidptr`). Member access is redirected to the converted-struct
// byte offset.
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

// Managed data ptr + scalars: punning at offset 0 would be a hard error for a
// plain union; c2go_variant converts it to ptr-slot@0 + scalar-blob@8.
union __attribute__((c2go_variant)) V {
  struct Foo *p;   // managed data ptr -> unsafe.Pointer slot @0 (scan)
  double      d;   // scalar -> blob @8 (no-scan)
  long        n;   // scalar -> blob @8 (no-scan)
};

// The variant union enlarges from 8 bytes (overlay) to 16 bytes (8 ptr slot +
// 8 scalar blob). sizeof reflects the converted-struct size. (The IR globals
// appear before the function bodies, so the bitmap/typeinfo CHECKs below come
// first in FileCheck order even though the source declares them later.)
unsigned long size_of_v(void) { return sizeof(union V); }

struct __attribute__((c2go_managed)) Holder {
  struct Holder *self;   // word0 -> scan
  union V        v;      // word1 = ptr slot (scan); word2 = blob (no-scan)
  long           after;  // word3 -> scalar (no-scan)
};

// Holder GC bitmap: word0 (self) + word1 (variant ptr slot) scan; the variant
// scalar blob (word2) and `after` (word3) are NOT scanned -> no punned byte.
// `\03` = 0b00000011. PtrBytes = 16, total size = 32.
// CHECK: @c2go.gcbitmap.Holder = {{.*}}constant [1 x i8] c"\03"
// CHECK: @c2go.typeinfo.Holder = {{.*}}%c2go._gotype { i64 32, i64 16,

// CHECK-LABEL: @size_of_v
// CHECK: ret i64 16

extern void *gc_malloc(const void *ti, size_t n);
void *mk(void) { return gc_malloc(__c2go_typeinfo(struct Holder), sizeof(struct Holder)); }

// Access redirection: `.p` is the pointer slot at offset 0 -> no GEP, the load
// is a managed (addrspace(1)) pointer.
// CHECK-LABEL: @get_p
// CHECK-NOT: getelementptr
// CHECK: load ptr addrspace(1)
struct Foo *get_p(union V *u) { return u->p; }

// `.d` is in the scalar blob at offset 8 -> redirected by an i8 GEP of 8.
// CHECK-LABEL: @get_d
// CHECK: getelementptr inbounds i8, ptr %{{[0-9]+}}, i64 8
// CHECK: load double
double get_d(union V *u) { return u->d; }

// `.n` shares the same blob slot (overlay of scalars) -> same i8 GEP of 8.
// CHECK-LABEL: @get_n
// CHECK: getelementptr inbounds i8, ptr %{{[0-9]+}}, i64 8
// CHECK: load i64
long get_n(union V *u) { return u->n; }

// Storing through `.d` also redirects to offset 8.
// CHECK-LABEL: @set_d
// CHECK: getelementptr inbounds i8, ptr %{{[0-9]+}}, i64 8
// CHECK: store double
void set_d(union V *u, double x) { u->d = x; }
