; A libc/libm call discovered only after the c2go GC boundary must fail closed:
; a route cannot safely be applied after RS4GC, and a raw standard C name is
; unresolved without a route.
;
; REQUIRES: x86-registered-target
; RUN: split-file %s %t
; RUN: not --crash llc -mtriple=x86_64-unknown-linux-goabi < %t/routed.ll 2>&1 | FileCheck %s --check-prefix=ROUTED
; RUN: not --crash llc -mtriple=x86_64-unknown-linux-goabi < %t/unrouted.ll 2>&1 | FileCheck %s --check-prefix=UNROUTED

; ROUTED: LLVM ERROR: c2go routed libcall 'sin' reached SelectionDAG after GC lowering
; UNROUTED: LLVM ERROR: c2go backend synthesized libc call 'sin' without a direct-GoABI0 c2go_linkname route

;--- routed.ll
declare double @llvm.sin.f64(double)

define goabi0cc double @routed(double %x) {
  %r = call double @llvm.sin.f64(double %x)
  ret double %r
}

!llvm.module.flags = !{!0}
!c2go.libcall.routes = !{!1}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{!"sin", !"example.com/lib.sin"}

;--- unrouted.ll
declare double @llvm.sin.f64(double)

define goabi0cc double @unrouted(double %x) {
  %r = call double @llvm.sin.f64(double %x)
  ret double %r
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
