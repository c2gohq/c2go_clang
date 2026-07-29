; X86 backend-only libcalls must consume the same route table for both their
; symbol and GoABI0 calling convention. Unmapped compiler-rt helpers remain on
; the platform C ABI.
;
; REQUIRES: x86-registered-target
; RUN: llc -mtriple=x86_64-unknown-linux-goabi < %s | FileCheck %s

declare double @llvm.sin.f64(double)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1 immarg)

define goabi0cc double @backend_sin(double %x) {
  %r = call double @llvm.sin.f64(double %x)
  ret double %r
}

define goabi0cc void @backend_memcpy(ptr %dst, ptr %src, i64 %n) {
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 %n, i1 false)
  ret void
}

define goabi0cc i128 @unmapped_div(i128 %a, i128 %b) {
  %r = sdiv i128 %a, %b
  ret i128 %r
}

; A routed GoABI0 call moves the incoming stack argument to an outgoing stack
; slot instead of leaving it in an XMM argument register.
; CHECK-LABEL: backend_sin:
; CHECK: movsd {{[0-9]+}}(%rsp), %xmm0
; CHECK: movsd %xmm0, (%rsp)
; CHECK: callq "example.com/lib.sin"@PLT

; CHECK-LABEL: backend_memcpy:
; CHECK-COUNT-3: pushq
; CHECK: callq "example.com/lib.memcpy"@PLT

; CHECK-LABEL: unmapped_div:
; CHECK: movq {{[0-9]+}}(%rsp), %rdi
; CHECK: movq {{[0-9]+}}(%rsp), %rsi
; CHECK: movq {{[0-9]+}}(%rsp), %rdx
; CHECK: movq {{[0-9]+}}(%rsp), %rcx
; CHECK: callq __divti3@PLT
; CHECK-NOT: callq sin
; CHECK-NOT: callq memcpy

!llvm.module.flags = !{!0}
!c2go.libcall.routes = !{!1, !2}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{!"sin", !"example.com/lib.sin"}
!2 = !{!"memcpy", !"example.com/lib.memcpy"}
