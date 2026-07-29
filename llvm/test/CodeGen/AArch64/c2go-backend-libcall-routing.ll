; Compiler-rt helpers synthesized by SelectionDAG are not libc routes and must
; retain AAPCS in a c2go module.
;
; REQUIRES: aarch64-registered-target
; RUN: llc -mtriple=aarch64-unknown-linux-goabi < %s | FileCheck %s

declare double @llvm.powi.f64.i32(double, i32)
declare double @llvm.sqrt.f64(double)

define goabi0cc double @unmapped_powi(double %x, i32 %n) {
  %r = call double @llvm.powi.f64.i32(double %x, i32 %n)
  ret double %r
}

define goabi0cc i128 @unmapped_div(i128 %a, i128 %b) {
  %r = sdiv i128 %a, %b
  ret i128 %r
}

define goabi0cc double @hardware_sqrt(double %x) {
  %r = call double @llvm.sqrt.f64(double %x)
  ret double %r
}

; CHECK-LABEL: unmapped_powi:
; CHECK: ldr d0,
; CHECK: ldr w0,
; CHECK: bl __powidf2
; CHECK-LABEL: unmapped_div:
; CHECK: ldp x0, x1,
; CHECK: bl __divti3
; CHECK-LABEL: hardware_sqrt:
; CHECK: fsqrt
; CHECK-NOT: bl

!llvm.module.flags = !{!0}
!c2go.libcall.routes = !{!1}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{!"sqrt", !"example.com/lib.sqrt"}
