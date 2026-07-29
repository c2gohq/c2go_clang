; A declaration-only standard libc call cannot remain in a c2go module without
; a direct-GoABI0 route. A similarly shaped compiler-rt helper is not a libc
; function and remains valid on the target C ABI.
;
; RUN: not opt < %s -passes=c2go-libcall-routing -S -o /dev/null 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-goabi"

declare double @sin(double)
declare double @__powidf2(double, i32)

define double @bad(double %x) {
  %r = call double @sin(double %x)
  ret double %r
}

define double @compiler_rt_is_not_libc(double %x, i32 %n) {
  %r = call double @__powidf2(double %x, i32 %n)
  ret double %r
}

; CHECK: error: c2go libc call 'sin' has no direct-GoABI0 c2go_linkname route

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
