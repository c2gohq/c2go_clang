; Compiler-rt helpers synthesized by SelectionDAG are not libc routes and must
; retain the platform C ABI in a c2go module.
;
; REQUIRES: x86-registered-target
; RUN: llc -mtriple=x86_64-unknown-linux-goabi < %s | FileCheck %s

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

; A route is only relevant if codegen really needs a libcall. Operations that
; lower to hardware must not be forced through the Go implementation.
define goabi0cc double @hardware_sqrt(double %x) {
  %r = call double @llvm.sqrt.f64(double %x)
  ret double %r
}

; The GoABI0 caller loads stack arguments into the SysV registers required by
; compiler-rt, and receives the result from the SysV return register.
; CHECK-LABEL: unmapped_powi:
; CHECK: movsd {{[0-9]+}}(%rsp), %xmm0
; CHECK: movl {{[0-9]+}}(%rsp), %edi
; CHECK: callq __powidf2@PLT
; CHECK-LABEL: unmapped_div:
; CHECK: movq {{[0-9]+}}(%rsp), %rdi
; CHECK: callq __divti3@PLT
; CHECK-LABEL: hardware_sqrt:
; CHECK: sqrtsd
; CHECK-NOT: callq

!llvm.module.flags = !{!0}
!c2go.libcall.routes = !{!1}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{!"sqrt", !"example.com/lib.sqrt"}
