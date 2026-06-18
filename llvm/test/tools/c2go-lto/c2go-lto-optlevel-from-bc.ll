; c2go-lto reads the c2go.opt-level / c2go.target-cpu / c2go.target-features
; module flags from the input bitcode and uses them to construct the Plan-9
; codegen TargetMachine. The startup check also refuses any bitcode compiled
; at -O < 2 or carrying an optnone function - both signal that the upstream
; -fc2go -emit-llvm pipeline did NOT run the late optimizer passes (RS4GC,
; GC setup, alloca folding) that c2go-lto's Plan-9 emit assumes have run.

; RUN: c2go-lto --c2go-emit-asm=%t.s %s 2>&1 | FileCheck %s --check-prefix=OK --allow-empty
; RUN: not c2go-lto %p/Inputs/optnone-bc.ll 2>&1 \
; RUN:   | FileCheck %s --check-prefix=REJECT_O0

; OK-NOT: error
; OK-NOT: requires -O2

; REJECT_O0: error
; REJECT_O0: c2go-lto requires -O2

target triple = "aarch64-unknown-linux"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128"

define void @ok_fn() {
entry:
  ret void
}

!llvm.module.flags = !{!0, !1, !2, !3}

!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.opt-level", i32 3}
!2 = !{i32 1, !"c2go.target-cpu", !"generic"}
!3 = !{i32 1, !"c2go.target-features", !"+neon"}
