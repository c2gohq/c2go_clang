// `c2go_managed` on a *data* pointer type sugars the pointer into target AS=1
// (managed-heap discriminator). This covers the attr-only path (no
// c2go_managed pointee), where a bare `int * __attribute__((c2go_managed))`
// would otherwise be indistinguishable from a plain `int *` at IR-gen time.
// The QualType is wrapped as AttributedType(C2GoManaged, AS1-pointer) so
// CodeGenTypes lowers it to `ptr addrspace(1)` and the existing AS1 path emits
// `ptrtoaddr` rather than `ptrtoint`.
//
// Also covers the file-scope global gcmask path: a record whose ONLY
// managedness signal is the attr-only sugar on one of its pointer fields. If
// the gcmask walker canonicalizes the QualType up front and loses the
// AttributedType wrapper, no gcmask byte is emitted; without gcmask the Go
// runtime treats the global as scalar bytes and the GC misses the managed
// pointer slot. Cases A5/A6 below.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -emit-llvm -Wno-c2go-managed-ptrtoint -o - %s | FileCheck %s

typedef unsigned long uintptr_t;
typedef unsigned uint32_t;

typedef int *__attribute__((c2go_managed)) ManagedIntPtr;

// Case A5: record with one attr-only managed pointer field at offset 0.
// Expect a per-global gcmask with bit 0 set (one 8-byte pointer slot at
// offset 0 -> byte 0 = 0x01). Declared up front so the global emits at the
// top of the IR (before any function body the later CHECKs scan).
// CHECK: @c2go.global.gcmask.a5_global = {{.*}}c"\01"
struct A5Record {
  int *__attribute__((c2go_managed)) p;
};
extern struct A5Record a5_global;
struct A5Record a5_global;

// Case A6: same shape but the managed pointer is the second field, after an
// 8-byte scalar hole - exercises the offset arithmetic on a non-zero pointer
// slot. The pointer at offset 8 lands in bit 1 of byte 0 of the mask ->
// byte 0 = 0x02.
// CHECK: @c2go.global.gcmask.a6_global = {{.*}}c"\02"
struct A6Record {
  unsigned long long pad;
  int *__attribute__((c2go_managed)) q;
};
extern struct A6Record a6_global;
struct A6Record a6_global;

// Case A1: typedef'd attr-only managed pointer -> uintptr_t. Must lower
// through AS(1) and use `ptrtoaddr`.
// CHECK-LABEL: define {{.*}}i64 @typedef_to_uintptr(ptr addrspace(1) {{.*}}%p)
// CHECK:   ptrtoaddr ptr addrspace(1) %{{.*}} to i64
// CHECK-NOT: ptrtoint ptr addrspace(1)
uintptr_t typedef_to_uintptr(ManagedIntPtr p) {
  return (uintptr_t)p;
}

// Case A2: inline attr-only managed pointer parameter -> uintptr_t. Same
// AS(1) + `ptrtoaddr` path, but without an intervening typedef.
// CHECK-LABEL: define {{.*}}i64 @inline_to_uintptr(ptr addrspace(1) {{.*}}%p)
// CHECK:   ptrtoaddr ptr addrspace(1) %{{.*}} to i64
// CHECK-NOT: ptrtoint ptr addrspace(1)
uintptr_t inline_to_uintptr(int *__attribute__((c2go_managed)) p) {
  return (uintptr_t)p;
}

// Case A3: attr-only managed pointer cast to a narrower integer (u32).
// `ptrtoaddr`'s dest type is fixed by the AS address width (i64 for AS1), so
// we emit `ptrtoaddr ... to i64` first and then truncate to i32.
// CHECK-LABEL: define {{.*}}i32 @typedef_to_u32(ptr addrspace(1) {{.*}}%p)
// CHECK:   %[[A:.*]] = ptrtoaddr ptr addrspace(1) %{{.*}} to i64
// CHECK:   trunc {{.*}}i64 %[[A]] to i32
// CHECK-NOT: ptrtoint
uint32_t typedef_to_u32(ManagedIntPtr p) {
  return (uint32_t)(uintptr_t)p;
}

// Case A4 (baseline): plain `int *p` without c2go_managed stays unmanaged
// (default AS) and keeps `ptrtoint`. Guards against the AS(1) sugar from
// leaking onto plain pointers.
// CHECK-LABEL: define {{.*}}i64 @baseline_to_uintptr(ptr {{.*}}%p)
// CHECK:   ptrtoint ptr %{{.*}} to i64
// CHECK-NOT: ptrtoaddr
uintptr_t baseline_to_uintptr(int *p) {
  return (uintptr_t)p;
}
