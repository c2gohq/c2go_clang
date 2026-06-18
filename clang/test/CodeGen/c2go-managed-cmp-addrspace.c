// A managed-record pointer is lowered to addrspace(1) at the type level, but
// the address of a managed-typed global (&gOther where gOther is struct N)
// comes out of the GV in its declared default address space - the LValue
// pointer is not re-cast to the C type's AS. The resulting `icmp ptr
// addrspace(1), ptr` would mismatch on operand types. EmitCompare inserts an
// addrspacecast on the AS0 side so the ICmp is well-formed and the comparison
// stays in AS1 (preserving the GC discriminator for the later
// RewriteStatepointsForGC carve-out).
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -mllvm -c2go-managed-addrspace -emit-llvm -o - %s | FileCheck %s
//
// Control group: with -c2go-managed-addrspace=0 the AS1 lowering is
// suppressed, every managed pointer stays in AS0, and the cross-AS compare
// path is moot - `icmp eq ptr` is well-formed on the AS0 operands alone and
// no addrspacecast is inserted. Pins the OFF-mode behavior so a later
// refactor cannot silently emit a redundant ascast.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -mllvm -c2go-managed-addrspace=0 -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=CHECK-OFF \
// RUN:     --implicit-check-not=addrspacecast
//
// --implicit-check-not=addrspacecast pins the OFF-mode no-addrspacecast
// invariant globally (not just inside each function scope), catching a stray
// addrspacecast emitted between two CHECK-OFF anchors.

struct __attribute__((c2go_managed)) N { struct N *next; int v; };

// A managed pointer-typed static global: storage word is `ptr addrspace(1)`,
// loaded into an AS1 SSA value at use sites.
// CHECK:     @gP = internal global ptr addrspace(1) null
// CHECK-OFF: @gP = internal global ptr null
static struct N *gP;

// A managed-record-typed (non-pointer) static global: the GV itself is in the
// default AS (no `addrspace(1)` on the global), so `&gOther` produces an AS0
// pointer at the use site.
// CHECK:     @gOther = internal global %struct.N zeroinitializer
// CHECK-OFF: @gOther = internal global %struct.N zeroinitializer
static struct N gOther;

// AS1 vs AS0: load of `gP` is AS1; `&gOther` is AS0. EmitCompare must insert
// an addrspacecast on the AS0 side so both ICmp operands carry AS1.
// CHECK-LABEL: define {{.*}}i32 @cmp_glob_glob
// CHECK: %[[L:[a-zA-Z0-9._]+]] = load ptr addrspace(1), ptr @gP
// CHECK: icmp eq ptr addrspace(1) %[[L]], addrspacecast (ptr @gOther to ptr addrspace(1))
//
// OFF mode: both operands stay in AS0, plain `icmp eq ptr` against @gOther.
// CHECK-OFF-LABEL: define {{.*}}i32 @cmp_glob_glob
// CHECK-OFF:     %[[LO:[a-zA-Z0-9._]+]] = load ptr, ptr @gP
// CHECK-OFF:     icmp eq ptr %[[LO]], @gOther
// CHECK-OFF-NOT: addrspacecast
int cmp_glob_glob(void) {
  return gP == &gOther;
}

// Symmetric LHS-AS0 / RHS-AS1 shape: &gOther on LHS, managed pointer load on
// RHS. The addrspacecast goes on the LHS (AS0) side, not the RHS, so the load
// stays a plain AS1 SSA value.
// CHECK-LABEL: define {{.*}}i32 @cmp_glob_glob_sym
// CHECK: %[[LS:[a-zA-Z0-9._]+]] = load ptr addrspace(1), ptr @gP
// CHECK: icmp eq ptr addrspace(1) addrspacecast (ptr @gOther to ptr addrspace(1)), %[[LS]]
//
// OFF mode: both operands stay in AS0, no ascast.
// CHECK-OFF-LABEL: define {{.*}}i32 @cmp_glob_glob_sym
// CHECK-OFF:     %[[LSO:[a-zA-Z0-9._]+]] = load ptr, ptr @gP
// CHECK-OFF:     icmp eq ptr @gOther, %[[LSO]]
// CHECK-OFF-NOT: addrspacecast
int cmp_glob_glob_sym(void) {
  return &gOther == gP;
}

// AS1-on-both-sides pointer/pointer compare (managed pointer load == managed
// pointer load). The compare path must NOT inject a redundant addrspacecast -
// both operands already share AS1 and the ICmp is well-formed without further
// AS-bridging.
// CHECK-LABEL: define {{.*}}i32 @cmp_glob_glob_as1
// CHECK: %[[A:[a-zA-Z0-9._]+]] = load ptr addrspace(1), ptr @gP
// CHECK: %[[B:[a-zA-Z0-9._]+]] = load ptr addrspace(1), ptr @gQ
// CHECK: icmp eq ptr addrspace(1) %[[A]], %[[B]]
// CHECK-NOT: addrspacecast
//
// OFF mode: AS0 on both sides, no ascast either.
// CHECK-OFF-LABEL: define {{.*}}i32 @cmp_glob_glob_as1
// CHECK-OFF:     %[[AO:[a-zA-Z0-9._]+]] = load ptr, ptr @gP
// CHECK-OFF:     %[[BO:[a-zA-Z0-9._]+]] = load ptr, ptr @gQ
// CHECK-OFF:     icmp eq ptr %[[AO]], %[[BO]]
// CHECK-OFF-NOT: addrspacecast
static struct N *gQ;
int cmp_glob_glob_as1(void) {
  return gP == gQ;
}

// Sanity: the AS1-on-both-sides case (null-literal RHS) is always well-formed
// because clang adopts the AS of the LHS for null-pointer-constants; pinned so
// the fix does not accidentally inject a redundant addrspacecast on the null
// side.
// CHECK-LABEL: define {{.*}}i32 @cmp_glob_null
// CHECK: %[[L2:[a-zA-Z0-9._]+]] = load ptr addrspace(1), ptr @gP
// CHECK: icmp eq ptr addrspace(1) %[[L2]], null
// CHECK-NOT: addrspacecast
//
// OFF mode: AS0 load + AS0 null, no ascast either.
// CHECK-OFF-LABEL: define {{.*}}i32 @cmp_glob_null
// CHECK-OFF:     %[[L2O:[a-zA-Z0-9._]+]] = load ptr, ptr @gP
// CHECK-OFF:     icmp eq ptr %[[L2O]], null
// CHECK-OFF-NOT: addrspacecast
int cmp_glob_null(void) {
  return gP == (struct N *)0;
}

// AS1 field load on LHS, AS0 `&gOther` on RHS - same AS-mismatch shape as
// cmp_glob_glob but with a managed-pointer field LValue on the load side.
// CHECK-LABEL: define {{.*}}i32 @cmp_field_glob
// CHECK: icmp eq ptr addrspace(1) %{{.*}}, addrspacecast (ptr @gOther to ptr addrspace(1))
//
// OFF mode: field load is AS0, no ascast needed.
// CHECK-OFF-LABEL: define {{.*}}i32 @cmp_field_glob
// CHECK-OFF:     icmp eq ptr %{{.*}}, @gOther
// CHECK-OFF-NOT: addrspacecast
int cmp_field_glob(struct N *p) {
  return p->next == &gOther;
}
