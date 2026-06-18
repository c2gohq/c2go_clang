// Common-linkage tentative definitions of a single pointer-word
// managed-pointer global must flow into the Go-owned worklist the same way
// an explicit `static T *p = NULL;` does. With -fcommon, a file-scope
// `T *p;` synthesizes a zero initializer at the IR level (LLVM common
// linkage requires it); the IR-null predicate accepts that shape via
// isNullValue(). Pinned here so future LLVM refactors that decouple common
// linkage from the stored initializer keep flowing into
// !c2go.go_owned_globals.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fcommon -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

#pragma c2go managed(6) push
struct N { struct N *n; };
#pragma c2go pop

// Tentative def: no explicit init, no static, no other definition in the
// TU - -fcommon lowers this to common linkage with a zero initializer.
struct N *gTentativeCommon;

// Same shape via a one-pointer-word wrapper struct - exercises the
// common-linkage path through the aggregate predicate.
struct W { struct N *p; };
struct W gTentativeCommonAgg;

// Negative: a regular static with non-null init, to check the
// common-tentative path did not widen too far.
static struct N gPin;
static struct N *gNonNullStatic = &gPin;

long ref_common(void) {
  return (long)gTentativeCommon + (long)gTentativeCommonAgg.p +
         (long)gNonNullStatic;
}

// --- IR shape pins ---------------------------------------------------------

// Common-linkage tentative defs surface as `common global ... zeroinitializer`
// (the FE installs the zero init unconditionally).
// CHECK-DAG: @gTentativeCommon = common global ptr addrspace(1) null
// CHECK-DAG: @gTentativeCommonAgg = common global %struct.W zeroinitializer

// Both common-tentative single-ptr-word globals get gcmask + Go-owned.
// CHECK-DAG: @c2go.global.gcmask.gTentativeCommon = {{.*}}c"\01"
// CHECK-DAG: @c2go.global.gcmask.gTentativeCommonAgg = {{.*}}c"\01"

// Go-owned worklist contains the common-tentative entries, excludes the
// non-null static.
// CHECK: !c2go.go_owned_globals = !{
// CHECK-DAG: !{!"gTentativeCommon"}
// CHECK-DAG: !{!"gTentativeCommonAgg"}
// CHECK-NOT: !{!"gNonNullStatic"}
