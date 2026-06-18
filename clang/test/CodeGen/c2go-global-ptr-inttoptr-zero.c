// The Go-owned-globals IR-null predicate is widened beyond
// Constant::isNullValue() so an `inttoptr (iN 0 to ptr ...)` ConstantExpr
// (produced when the address-space cast sits between the integer 0 and the
// pointer type, blocking the FE's null-pointer fold) is still classified as
// a null initializer for the single-pointer-word Go-owned cession path.
//
// Constant::isNullValue matches only ConstantPointerNull /
// ConstantAggregateZero / ConstantInt(0); a wrapping `inttoptr 0`
// ConstantExpr falls through. Without the widening, such globals would be
// excluded from !c2go.go_owned_globals, leak to the AsmPrinter fallback DATA
// path, and the C-side GLOBL storage would race with the Go-owned var.
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

// Positive A: implicit zero - produces `ptr addrspace(1) null`. Covered by
// Constant::isNullValue(); regression guard for the canonical case.
static struct N *gImplicitZero;

// Positive B: `(T *)0` folds to ConstantPointerNull at the FE. Same canonical
// case via a different syntactic surface.
static struct N *gExplicitNull = (struct N *)0;

// Negative A: `(T *)0x1000` produces a non-null
// `inttoptr (i64 4096 to ptr addrspace(1))` ConstantExpr. Must NOT be
// Go-owned - the C side keeps the explicit address in DATA and Go-side
// ownership would corrupt it.
static struct N *gNonZeroAddr = (struct N *)0x1000;

long ref_inttoptr(void) {
  return (long)gImplicitZero + (long)gExplicitNull + (long)gNonZeroAddr;
}

// --- IR shape pins ---------------------------------------------------------

// CHECK-DAG: @gImplicitZero = internal global ptr addrspace(1) null
// CHECK-DAG: @gExplicitNull = internal global ptr addrspace(1) null
// CHECK-DAG: @gNonZeroAddr = internal global ptr addrspace(1) inttoptr

// gcmask is emitted for every c2go-tracked pointer global regardless of
// whether storage is Go-owned.
// CHECK-DAG: @c2go.global.gcmask.gImplicitZero = {{.*}}c"\01"
// CHECK-DAG: @c2go.global.gcmask.gExplicitNull = {{.*}}c"\01"
// CHECK-DAG: @c2go.global.gcmask.gNonZeroAddr = {{.*}}c"\01"

// Go-owned list must include the null-initialized vars and exclude
// `gNonZeroAddr` (the inttoptr 0x1000 case).
// CHECK: !c2go.go_owned_globals = !{
// CHECK-DAG: !{!"gImplicitZero"}
// CHECK-DAG: !{!"gExplicitNull"}
// CHECK-NOT: !{!"gNonZeroAddr"}
