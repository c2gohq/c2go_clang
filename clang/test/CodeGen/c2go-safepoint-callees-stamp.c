// The clang frontend must stamp the canonical 8-entry safepoint-callee seed
// list into the .bc as `!c2go.safepoint.callees` named metadata. This pins
// both the entries and their canonical order, so a future edit that drops or
// renames one trips at build time rather than at runtime.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

// Body content is irrelevant - the assertion is on module-level named
// metadata.
void anchor(void) {}

// CHECK: !c2go.safepoint.callees = !{[[E0:![0-9]+]], [[E1:![0-9]+]], [[E2:![0-9]+]], [[E3:![0-9]+]], [[E4:![0-9]+]], [[E5:![0-9]+]], [[E6:![0-9]+]], [[E7:![0-9]+]]}
//
// Canonical order; producer, pass, and test share one source of truth.
// CHECK-DAG: [[E0]] = !{!"runtime.mallocgc"}
// CHECK-DAG: [[E1]] = !{!"runtime.typedmemmove"}
// CHECK-DAG: [[E2]] = !{!"_c2go_typedMemmoveArray"}
// CHECK-DAG: [[E3]] = !{!"runtime.gcWriteBarrier"}
// CHECK-DAG: [[E4]] = !{!"runtime.morestack"}
// CHECK-DAG: [[E5]] = !{!"runtime.newproc"}
// CHECK-DAG: [[E6]] = !{!"runtime.gopanic"}
// CHECK-DAG: [[E7]] = !{!"runtime.systemstack"}
