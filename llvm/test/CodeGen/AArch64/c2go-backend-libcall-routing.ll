; Backend-only libcalls do not exist as CallInsts for the IR routing pass to
; rewrite. SelectionDAG must consume the same route table for both their symbol
; and calling convention.
;
; REQUIRES: aarch64-registered-target
; RUN: llc -mtriple=aarch64-unknown-linux-goabi < %s | FileCheck %s

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

; A compiler-rt helper with no route must keep its ordinary target symbol and
; AAPCS calling convention. The route table is an allow-list, not a blanket
; "all backend helpers are GoABI0" switch.
define goabi0cc i128 @unmapped_div(i128 %a, i128 %b) {
  %r = sdiv i128 %a, %b
  ret i128 %r
}

; CHECK-LABEL: backend_sin:
; CHECK: str d0, [sp, #8]
; CHECK-NEXT: bl "example.com/lib.sin"
; CHECK-LABEL: backend_memcpy:
; CHECK: str x8, [sp, #24]
; CHECK-NEXT: stur q0, [sp, #8]
; CHECK-NEXT: bl "example.com/lib.memcpy"
; CHECK-LABEL: unmapped_div:
; CHECK: ldp x0, x1,
; CHECK: ldp x2, x3,
; CHECK: bl __divti3
; CHECK-NOT: bl sin
; CHECK-NOT: bl memcpy

!llvm.module.flags = !{!0}
!c2go.libcall.routes = !{!1, !2}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{!"sin", !"example.com/lib.sin"}
!2 = !{!"memcpy", !"example.com/lib.memcpy"}
