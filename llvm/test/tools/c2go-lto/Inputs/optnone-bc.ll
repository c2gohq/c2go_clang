; Input: a c2go bitcode compiled at -O0 (c2go.opt-level=0). c2go-lto must
; refuse this with "c2go-lto requires -O2" on startup.

target triple = "aarch64-unknown-linux"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128"

define void @o0_fn() #0 {
entry:
  ret void
}

attributes #0 = { noinline optnone }

!llvm.module.flags = !{!0, !1, !2, !3}

!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.opt-level", i32 0}
!2 = !{i32 1, !"c2go.target-cpu", !"generic"}
!3 = !{i32 1, !"c2go.target-features", !"+neon"}
