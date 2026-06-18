// A `c2go_variant` union surfaces in the JSON GC manifest as its own type
// entry whose go_def is the converted struct (pointer slots as
// `unsafe.Pointer`, scalars/funcptrs as a `[N]uint8` blob) and whose
// union_scheme is "variant".
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fc2go-emit-manifest=%t.json -emit-llvm-bc -o %t.bc %s
// RUN: FileCheck %s --input-file=%t.json

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

typedef __SIZE_TYPE__ size_t;
struct __attribute__((c2go_managed)) Foo { struct Foo *next; };

union __attribute__((c2go_variant)) V {
  struct Foo *p;   // managed data ptr -> unsafe.Pointer slot
  double      d;   // scalar -> blob
  long        n;   // scalar -> blob
};

struct __attribute__((c2go_managed)) Holder {
  struct Holder *self;
  union V        v;
  long           after;
};

extern void *gc_malloc(const void *ti, size_t n);
void *mk(void) { return gc_malloc(__c2go_typeinfo(struct Holder), sizeof(struct Holder)); }

// The variant union V is its own manifest type entry (referenced by Holder's
// `v V`). Its go_def is the converted struct: a precisely-scanned pointer slot
// plus a no-scan scalar blob. Types are sorted by name and JSON keys are sorted
// alphabetically inside each entry, so within V the order is:
// go_def, linkage_owner, managed_record, name, union_scheme.
//
// CHECK: "go_def": "struct {{.*}}unsafe.Pointer{{.*}}uint8{{.*}}"
// CHECK: "name": "V"
// CHECK: "union_scheme": "variant"
