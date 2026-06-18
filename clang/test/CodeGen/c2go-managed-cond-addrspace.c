// A `?:` (conditional operator) whose arms are pointer values in mismatched
// address spaces - one side a managed-pointer load (AS1) and the other a
// managed-record GV address (AS0) - must have an addrspacecast inserted so the
// select / PHI is well-formed and the result stays in AS1 (preserving the GC
// discriminator, the same way the compare path does). The conditional operator
// has two lowerings:
//   - a "cheap arms" path that lowers to `select`
//   - a "non-cheap arms" path that lowers to a PHI across the
//     cond.true / cond.false / cond.end BB graph
// In practice c2go-managed pointers come from mutable globals / loads / calls,
// so the cheap-arms check rejects them and every real-world case goes through
// the PHI path; this covers the PHI path across symmetric LHS/RHS placement,
// plus the AS1-on-both-sides sanity case that must NOT inject a redundant
// addrspacecast. The select-shape arm is guarded by an assertion that traps on
// any AS pair other than {AS0, AS1}, so a future cheap-arms managed-pointer
// shape cannot regress silently.
//
// Empirical select-shape coverage: a literal `(struct N *)0` arm paired with a
// managed-record GV address &gOther is evaluatable on both sides, so the cheap
// branch lowers the `?:` to a select rather than a PHI. The reachability is
// observed from the actual -emit-llvm output; if a future Sema/IRGen
// normalisation rewrites the null-arm shape onto the PHI path instead, the
// CHECK variant below still fires.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -mllvm -c2go-managed-addrspace -emit-llvm -o - %s | FileCheck %s
//
// Control group: with -c2go-managed-addrspace=0 the AS1 lowering is
// suppressed; every managed pointer stays in AS0, the AS-mismatch path is
// moot, and no addrspacecast may be emitted on the `?:` arms.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -mllvm -c2go-managed-addrspace=0 -emit-llvm -o - %s \
// RUN:   | FileCheck %s --check-prefix=CHECK-OFF

struct __attribute__((c2go_managed)) N { struct N *next; int v; };

// CHECK-DAG: @gP = internal global ptr addrspace(1) null
// CHECK-OFF-DAG: @gP = internal global ptr null
static struct N *gP;

// CHECK-DAG: @gQ = internal global ptr addrspace(1) null
// CHECK-OFF-DAG: @gQ = internal global ptr null
static struct N *gQ;

// CHECK-DAG: @gOther = internal global %struct.N zeroinitializer
// CHECK-OFF-DAG: @gOther = internal global %struct.N zeroinitializer
static struct N gOther;

// Forward decl for a non-evaluatable arm.
struct N *get_managed(void);

// PHI-shape `?:`, true-arm = managed-pointer load (AS1), false-arm = AS0 GV
// address &gOther. AS-unify so the PHI's two incoming pointer types match.
// The cast on &gOther constant-folds into the PHI incoming value as
// `addrspacecast (ptr @gOther to ptr addrspace(1))`.
// CHECK-LABEL: define {{.*}}ptr addrspace(1) @cond_glob_glob
// CHECK:        %[[L:[a-zA-Z0-9._]+]] = load ptr addrspace(1), ptr @gP
// CHECK:        phi ptr addrspace(1) [ %[[L]], %{{.*}} ], [ addrspacecast (ptr @gOther to ptr addrspace(1)), %{{.*}} ]
//
// OFF mode: both arms stay in AS0, no ascast.
// CHECK-OFF-LABEL: define {{.*}}ptr @cond_glob_glob
// CHECK-OFF:        %[[LO:[a-zA-Z0-9._]+]] = load ptr, ptr @gP
// CHECK-OFF:        phi ptr [ %[[LO]], %{{.*}} ], [ @gOther, %{{.*}} ]
// CHECK-OFF-NOT:    addrspacecast
struct N *cond_glob_glob(int c) {
  return c ? gP : &gOther;
}

// Symmetric: AS0 &gOther on the true arm, AS1 load on the false arm. AS-unify
// on the true arm so the PHI types match.
// CHECK-LABEL: define {{.*}}ptr addrspace(1) @cond_glob_glob_sym
// CHECK:        %[[LS:[a-zA-Z0-9._]+]] = load ptr addrspace(1), ptr @gP
// CHECK:        phi ptr addrspace(1) [ addrspacecast (ptr @gOther to ptr addrspace(1)), %{{.*}} ], [ %[[LS]], %{{.*}} ]
//
// OFF mode: both arms stay in AS0.
// CHECK-OFF-LABEL: define {{.*}}ptr @cond_glob_glob_sym
// CHECK-OFF:        %[[LSO:[a-zA-Z0-9._]+]] = load ptr, ptr @gP
// CHECK-OFF:        phi ptr [ @gOther, %{{.*}} ], [ %[[LSO]], %{{.*}} ]
// CHECK-OFF-NOT:    addrspacecast
struct N *cond_glob_glob_sym(int c) {
  return c ? &gOther : gP;
}

// AS1-on-both-sides: managed-pointer load on both arms. Must NOT insert a
// redundant addrspacecast - both arms already share AS1.
// CHECK-LABEL: define {{.*}}ptr addrspace(1) @cond_as1_as1
// CHECK:        %[[A1:[a-zA-Z0-9._]+]] = load ptr addrspace(1), ptr @gP
// CHECK:        %[[B1:[a-zA-Z0-9._]+]] = load ptr addrspace(1), ptr @gQ
// CHECK:        phi ptr addrspace(1) [ %[[A1]], %{{.*}} ], [ %[[B1]], %{{.*}} ]
// CHECK-NOT:    addrspacecast
//
// OFF mode: AS0 on both arms.
// CHECK-OFF-LABEL: define {{.*}}ptr @cond_as1_as1
// CHECK-OFF:        %[[A1O:[a-zA-Z0-9._]+]] = load ptr, ptr @gP
// CHECK-OFF:        %[[B1O:[a-zA-Z0-9._]+]] = load ptr, ptr @gQ
// CHECK-OFF:        phi ptr [ %[[A1O]], %{{.*}} ], [ %[[B1O]], %{{.*}} ]
// CHECK-OFF-NOT:    addrspacecast
struct N *cond_as1_as1(int c) {
  return c ? gP : gQ;
}

// PHI shape with a call on the true arm: managed-pointer-returning call (AS1)
// vs `&gOther` (AS0). Verifies the AS-unify works when the true-arm value is
// a `call` result rather than a load, exercising the same constant-fold of
// the cast on the AS0 arm.
// CHECK-LABEL: define {{.*}}ptr addrspace(1) @cond_phi_call_glob
// CHECK:        %[[CA:[a-zA-Z0-9._]+]] = call {{.*}}ptr addrspace(1) @get_managed
// CHECK:        phi ptr addrspace(1) [ %[[CA]], %{{.*}} ], [ addrspacecast (ptr @gOther to ptr addrspace(1)), %{{.*}} ]
//
// OFF mode: AS0 on both arms.
// CHECK-OFF-LABEL: define {{.*}}ptr @cond_phi_call_glob
// CHECK-OFF:        %[[CAO:[a-zA-Z0-9._]+]] = call {{.*}}ptr @get_managed
// CHECK-OFF:        phi ptr [ %[[CAO]], %{{.*}} ], [ @gOther, %{{.*}} ]
// CHECK-OFF-NOT:    addrspacecast
struct N *cond_phi_call_glob(int c) {
  return c ? get_managed() : &gOther;
}

// Select-shape `?:` with `(struct N *)0` true-arm and &gOther false-arm. Both
// arms are evaluatable so the cheap branch lowers to `select`. AS-unify wraps
// the AS0 &gOther in an addrspacecast so the select's operand types match; the
// AS1 null on the other arm prints as `ptr addrspace(1) null`.
// CHECK-LABEL: define {{.*}}ptr addrspace(1) @cond_select_null_glob
// CHECK:        select i1 {{.*}}, ptr addrspace(1) null, ptr addrspace(1) addrspacecast (ptr @gOther to ptr addrspace(1))
//
// OFF mode: both arms stay in AS0, no ascast on the select.
// CHECK-OFF-LABEL: define {{.*}}ptr @cond_select_null_glob
// CHECK-OFF:        select i1 {{.*}}, ptr null, ptr @gOther
// CHECK-OFF-NOT:    addrspacecast
struct N *cond_select_null_glob(int c) {
  return c ? (struct N *)0 : &gOther;
}
