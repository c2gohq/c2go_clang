// A managed AS(1) pointer cast to integer drops GC provenance. It is lowered
// via the `ptrtoaddr` opcode (a non-capturing address extract) instead of
// `ptrtoint`. The unmanaged (default-AS) pointer-to-integer path keeps
// `ptrtoint` unchanged.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -emit-llvm -Wno-c2go-managed-ptrtoint -o - %s | FileCheck %s

struct __attribute__((c2go_managed)) N { int v; };

typedef unsigned long uintptr_t;

// Case 1: implicit managed ptr -> uintptr_t cast (pointer to a c2go_managed
// pointee). The AS1 source must be lowered via `ptrtoaddr`.
// CHECK-LABEL: define {{.*}}i64 @to_uintptr_managed(ptr addrspace(1) {{.*}}%p)
// CHECK:   ptrtoaddr ptr addrspace(1) %{{.*}} to i64
// CHECK-NOT: ptrtoint ptr addrspace(1)
uintptr_t to_uintptr_managed(struct N *p) {
  return (uintptr_t)p;
}

// Case 2: baseline - an unmanaged (default-AS) pointer keeps `ptrtoint`.
// CHECK-LABEL: define {{.*}}i64 @to_uintptr_unmanaged(ptr {{.*}}%p)
// CHECK:   ptrtoint ptr %{{.*}} to i64
// CHECK-NOT: ptrtoaddr
uintptr_t to_uintptr_unmanaged(int *p) {
  return (uintptr_t)p;
}

// Case 3: managed ptr cast to a narrower integer (uint32_t). `ptrtoaddr`'s
// destination type is fixed by the AS address width (i64 for AS1), so we
// emit `ptrtoaddr ... to i64` first and then truncate to i32.
// CHECK-LABEL: define {{.*}}i32 @to_u32_managed(ptr addrspace(1) {{.*}}%p)
// CHECK:   %[[A:.*]] = ptrtoaddr ptr addrspace(1) %{{.*}} to i64
// CHECK:   trunc {{.*}}i64 %[[A]] to i32
// CHECK-NOT: ptrtoint
unsigned to_u32_managed(struct N *p) {
  return (unsigned)(uintptr_t)p;
}

// Case 4: another struct pointer through a C-style `unsigned long` cast,
// exercises the same CK_PointerToIntegral path with a different surface
// syntax. Must use `ptrtoaddr`.
// CHECK-LABEL: define {{.*}}i64 @to_ulong_managed(ptr addrspace(1) {{.*}}%p)
// CHECK:   ptrtoaddr ptr addrspace(1) %{{.*}} to i64
// CHECK-NOT: ptrtoint ptr addrspace(1)
unsigned long to_ulong_managed(struct N *p) {
  return (unsigned long)p;
}
