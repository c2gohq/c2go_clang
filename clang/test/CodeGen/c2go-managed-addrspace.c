// Under -mllvm -c2go-managed-addrspace, a pointer to a c2go_managed record is
// lowered to addrspace(1) (the Go GC managed-heap discriminator): struct fields,
// parameters, locals, and globals all carry AS1. Crossing between a managed
// pointer and an unmanaged void* at a call boundary emits an addrspacecast
// (never an invalid same-AS bitcast), and the module verifies.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -mllvm -c2go-managed-addrspace -emit-llvm -o - %s | FileCheck %s
//
// The gate is default-on; pass =0 to force the unmanaged baseline.
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -mllvm -c2go-managed-addrspace=0 -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=OFF

struct __attribute__((c2go_managed)) N { struct N *next; int v; };

// CHECK: %struct.N = type { ptr addrspace(1), i32 }
// OFF:   %struct.N = type { ptr, i32 }

// A global managed-pointer null initializer is lowered in AS1.
// CHECK: @gnp = {{(dso_local )?}}global ptr addrspace(1) null
struct N *gnp = 0;

// CHECK-LABEL: define {{.*}}void @g(ptr addrspace(1) {{.*}}%d, ptr addrspace(1) {{.*}}%s)
// CHECK: store ptr addrspace(1) %{{.*}}, ptr addrspace(1) %next
void g(struct N *d, struct N *s) { d->next = s; }

// Passing a managed pointer to a void* parameter bridges AS1 -> default AS with
// an addrspacecast. The explicit (__attribute__((c2go_managed)) void *) cast is
// the user-acknowledged escape hatch, so Sema stays silent. The internal call
// uses the GoABI0 calling convention (printed goabi0cc, formerly numeric cc128).
// CHECK-LABEL: define {{.*}}i32 @use_voidstar(ptr addrspace(1) {{.*}}%p)
// CHECK: addrspacecast ptr addrspace(1) %{{.*}} to ptr
// CHECK: call {{(cc128 |goabi0cc )?}}i32 @munge(ptr
int use_voidstar(struct N *p) {
  extern int munge(void *);
  return munge((__attribute__((c2go_managed)) void *)p);
}

// The reverse cast (void* -> managed struct pointer) also needs an addrspacecast.
// CHECK-LABEL: define {{.*}}ptr addrspace(1) @from_voidstar(ptr {{.*}}%q)
// CHECK: addrspacecast ptr %{{.*}} to ptr addrspace(1)
struct N *from_voidstar(void *q) {
  return (struct N *)q;
}

// A null constant assigned to a managed pointer LValue stores a ptr addrspace(1)
// null, not a default-AS null cast at the use site.
// CHECK-LABEL: define {{.*}}void @null_assign(ptr addrspace(1) {{.*}}%h)
// CHECK: store ptr addrspace(1) null, ptr addrspace(1)
void null_assign(struct N *h) {
  h->next = 0;
}

// Comparing a managed pointer against NULL stays in AS1 on both sides.
// CHECK-LABEL: define {{.*}}i32 @is_null(ptr addrspace(1) {{.*}}%p)
// CHECK: icmp {{(eq|ne)}} ptr addrspace(1) %{{.*}}, null
int is_null(struct N *p) {
  return p == 0;
}
