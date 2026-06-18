// The Go-owned-globals predicate must recognise a one-pointer-word
// ConstantStruct ({ ptr null }) initializer, not just the canonical
// zeroinitializer (ConstantAggregateZero). At -O0 the FE usually collapses
// { .p = 0 } into zeroinit, but mid-end IRMover paths and the c2go-lto
// round-trip can re-materialise the aggregate ConstantStruct form. The
// widened IR-null predicate accepts the one-operand-all-null aggregate shape
// so Go-owned cession stays stable across both representations.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

#pragma c2go managed(6) push
struct N { struct N *n; };
#pragma c2go pop

// A wrapper struct exactly one pointer-word wide. The eligibility predicate
// keys on size == PtrSize with bit 0 set, so this shape lands in the same
// worklist as a bare T * global.
struct W { struct N *p; };

// Positive A: implicit zero - FE emits zeroinitializer
// (ConstantAggregateZero). Canonical case; regression guard.
static struct W gAggZeroinit;

// Positive B: explicit { .p = 0 } - Go-owned regardless of whether the FE
// collapses to zeroinit or leaves a ConstantStruct of { null }. The widened
// predicate matches both.
static struct W gAggExplicitNull = { .p = 0 };

// Negative: explicit non-zero pointer in the aggregate - single-pointer-word
// shape but non-null init, must NOT be Go-owned (the C side keeps the DATA
// storage for &gTarget).
static struct N gTarget;
static struct W gAggNonZero = { .p = &gTarget };

long ref_aggregate(void) {
  return (long)gAggZeroinit.p + (long)gAggExplicitNull.p + (long)gAggNonZero.p;
}

// --- IR shape pins ---------------------------------------------------------

// Both null-init wrappers must keep their c2go gcmask byte.
// CHECK-DAG: @c2go.global.gcmask.gAggZeroinit = {{.*}}c"\01"
// CHECK-DAG: @c2go.global.gcmask.gAggExplicitNull = {{.*}}c"\01"
// CHECK-DAG: @c2go.global.gcmask.gAggNonZero = {{.*}}c"\01"

// Go-owned worklist must include the null-init aggregates and exclude the
// `{ &gTarget }` one.
// CHECK: !c2go.go_owned_globals = !{
// CHECK-DAG: !{!"gAggZeroinit"}
// CHECK-DAG: !{!"gAggExplicitNull"}
// CHECK-NOT: !{!"gAggNonZero"}
