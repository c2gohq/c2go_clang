// A managed union (marked c2go_managed, or stamped by managed push/pop
// propagation) is represented as a c2go_variant struct - the invariant is that
// only c2go_variant coexists with managed semantics. So a managed punning
// union converts (case c, layout grows) instead of raising a hard error on
// type-punning a scanned pointer slot, and a managed shared-pointer-slot union
// converts layout-preserving (case b). The pointer word is precisely scanned
// and member access is redirected to the converted-struct offset. None of the
// union-overlay / ptr-in-plain warnings fire (-Werror below).
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -Werror -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif
struct __attribute__((c2go_managed)) Foo { struct Foo *next; };

// (case c) punning: a managed pointer and scalars overlay byte 0. Marked
// c2go_managed (NOT c2go_variant) -> converts to a variant struct: ptr slot @0
// + scalar blob @8 -> sizeof == 16 (a plain union would be 8 and hard-error).
union __attribute__((c2go_managed)) PunU {
  struct Foo *p;   // managed data ptr -> precisely-scanned slot @0
  double      d;   // scalar -> no-scan blob @8
  long        n;   // scalar -> no-scan blob @8
};
// CHECK-LABEL: @pun_size
// CHECK: ret i64 16
unsigned long pun_size(void) { return sizeof(union PunU); }

// (case b) shared pointer slot: two managed pointers at the same offset share
// one signature -> overlay into ONE region -> sizeof unchanged (8).
union __attribute__((c2go_managed)) ShU {
  struct Foo *p;   // managed ptr @0
  struct Foo *q;   // managed ptr @0 (same slot)
};
// CHECK-LABEL: @sh_size
// CHECK: ret i64 8
unsigned long sh_size(void) { return sizeof(union ShU); }

// Pointer member of the converted punning union loads a managed (addrspace(1))
// pointer from slot @0 (no GEP); the scalar member is redirected to blob @8.
// CHECK-LABEL: @get_p
// CHECK: load ptr addrspace(1)
struct Foo *get_p(union PunU *u) { return u->p; }
// CHECK-LABEL: @get_d
// CHECK: getelementptr inbounds i8, ptr addrspace(1) %{{[0-9]+}}, i64 8
// CHECK: load double
double get_d(union PunU *u) { return u->d; }
