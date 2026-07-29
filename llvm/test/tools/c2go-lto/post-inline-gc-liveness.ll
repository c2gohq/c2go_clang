; Driver-routed c2go bitcode defers GC lowering until c2go-lto has completed
; whole-module inlining. A call exposed by that inlining must become a real
; statepoint with the variadic pointer pack in its gc-live set; lowering each
; TU before the inliner would leave this as a deopt-only call with no roots.
;
; REQUIRES: aarch64-registered-target
;
; RUN: c2go-lto %s --c2go-escape-nonfatal --output-bc=%t.bc
; RUN: llvm-dis %t.bc -o - | FileCheck %s

target triple = "aarch64-unknown-none-elf"

declare goabi0cc void @sink(ptr)

define internal goabi0cc void @format_helper(ptr %value) alwaysinline {
entry:
  %slot = alloca ptr, align 8, !c2go.ptr.managed !4
  %c2go.va.argptrs = alloca [2 x ptr], align 8, !c2go.ptr.managed !4, !c2go.va.pack !4
  store ptr %value, ptr %slot, align 8
  %arg0 = getelementptr inbounds [2 x ptr], ptr %c2go.va.argptrs, i64 0, i64 0
  store ptr %slot, ptr %arg0, align 8
  %sentinel = getelementptr inbounds [2 x ptr], ptr %c2go.va.argptrs, i64 0, i64 1
  store ptr null, ptr %sentinel, align 8
  call goabi0cc void @sink(ptr %c2go.va.argptrs)
  ret void
}

define goabi0cc void @caller(ptr %value) {
entry:
  call goabi0cc void @format_helper(ptr %value)
  ret void
}

; CHECK-NOT: define internal goabi0cc void @format_helper
; CHECK-LABEL: define goabi0cc void @caller(ptr %value) gc "c2go-gc"
; CHECK: %c2go.va.argptrs{{.*}} = alloca [2 x ptr]
; CHECK: @llvm.experimental.gc.statepoint{{.*}}elementtype(void (ptr)) @sink
; CHECK-SAME: [ "deopt"(), "gc-live"({{.*}}ptr %c2go.va.argptrs{{.*}}) ]
; CHECK: !{i32 1, !"c2go.lto.prelink", i32 0}

!llvm.module.flags = !{!0, !1, !2, !3}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.opt-level", i32 2}
!2 = !{i32 1, !"c2go.lto.prelink", i32 1}
!3 = !{i32 1, !"c2go.target_go_version", !"1.22-1.26"}
!4 = !{}
