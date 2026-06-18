// A `c2go_variant` union whose alternative is a nested struct holding a managed
// data pointer is converted to a struct that keeps the alternative's natural
// layout and scans the pointer word precisely. The nested
// `struct{int tag; Foo* p;}` keeps `tag` at offset 0 (scalar) and `p` at offset
// 8 (a precisely-scanned `unsafe.Pointer` word); the `double` alternative goes
// into a no-scan blob in its own non-overlapping region. No byte is "pointer in
// one alternative, scalar in another", so the Go GC scans `s.p` precisely
// instead of force-scanning a punned scalar (which would trip `invalidptr`).
// Member access is redirected to the alternative's converted region; nested
// `.tag` / `.p` then fall out of the natural struct layout.
//
// Because the nested struct is a variant alternative, Sema suppresses the
// "not scanned by the Go GC" warning on `p` (it would be a false positive - the
// variant conversion is what scans it). -Werror with no escape-hatch flag
// proves the warning is gone.
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

// region A = struct{int tag; Foo* p;} (16 bytes @0; tag@0 scalar, p@8 SCAN)
// region B = double (8 bytes @16, no-scan blob). sizeof = 24.
union __attribute__((c2go_variant)) V {
  struct { int tag; struct Foo *p; } s;
  double d;
};

unsigned long size_of_v(void) { return sizeof(union V); }

struct __attribute__((c2go_managed)) Holder {
  struct Holder *self;   // word0 -> scan
  union V        v;      // word1 = s.tag (no-scan); word2 = s.p (SCAN); word3 = double blob
  long           after;  // word4 -> scalar (no-scan)
};

// The IR globals (gcbitmap / typeinfo) are emitted before the function bodies,
// so their CHECKs come first in FileCheck order even though declared later.
//
// Holder GC bitmap: word0 (self) + word2 (the variant's nested s.p) scan; the
// variant's tag word (word1), the double blob (word3) and `after` (word4) are
// NOT scanned. `\05` = 0b00000101. ptrdata = 24 (through word2). size = 40.
// CHECK: @c2go.gcbitmap.Holder = {{.*}}constant [1 x i8] c"\05"
// CHECK: @c2go.typeinfo.Holder = {{.*}}%c2go._gotype { i64 40, i64 24,

// The variant union is 24 bytes (16 nested-struct region + 8 scalar blob).
// CHECK-LABEL: @size_of_v
// CHECK: ret i64 24

extern void *gc_malloc(const void *ti, size_t n);
void *mk(void) { return gc_malloc(__c2go_typeinfo(struct Holder), sizeof(struct Holder)); }

// `u->s.tag`: `.s` is region A at offset 0 (no first-hop GEP); `.tag` is field 0
// of the nested struct (offset 0) -> a plain i32 load.
// CHECK-LABEL: @get_tag
// CHECK-NOT: getelementptr inbounds i8
// CHECK: load i32
int get_tag(union V *u) { return u->s.tag; }

// `u->s.p`: `.s` region A @0 (no first-hop GEP); `.p` is field 1 of the nested
// struct (offset 8) -> a managed (addrspace(1)) pointer load. The +8 comes from
// the nested struct's natural member GEP, not an i8 region redirect.
// CHECK-LABEL: @get_p
// CHECK-NOT: getelementptr inbounds i8
// CHECK: getelementptr inbounds nuw %struct.anon, ptr %{{[0-9]+}}, i32 0, i32 1
// CHECK: load ptr addrspace(1)
struct Foo *get_p(union V *u) { return u->s.p; }

// `u->d`: region B at offset 16 -> redirected by an i8 GEP of 16, then a double
// load.
// CHECK-LABEL: @get_d
// CHECK: getelementptr inbounds i8, ptr %{{[0-9]+}}, i64 16
// CHECK: load double
double get_d(union V *u) { return u->d; }

// Storing through `u->s.p` writes the managed pointer slot at region A + 8.
// CHECK-LABEL: @set_p
// CHECK: getelementptr inbounds nuw %struct.anon, ptr %{{[0-9]+}}, i32 0, i32 1
// CHECK: store ptr addrspace(1)
void set_p(union V *u, struct Foo *f) { u->s.p = f; }
