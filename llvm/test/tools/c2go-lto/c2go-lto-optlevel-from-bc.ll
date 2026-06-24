; c2go-lto reads the c2go.opt-level / c2go.target-cpu / c2go.target-features
; module flags from the input bitcode and uses them to construct the Plan-9
; codegen TargetMachine. c2go-lto is a bitcode linker that PRESERVES the entry
; pipeline's optimization level: it accepts -O0 / optnone bitcode (running the
; inliner only at -O2+) instead of refusing it, so both an -O2 module and an
; optnone module link cleanly.

; RUN: c2go-lto --c2go-emit-asm=%t.s %s 2>&1 | FileCheck %s --check-prefix=OK --allow-empty
; RUN: c2go-lto %p/Inputs/optnone-bc.ll --c2go-emit-asm=%t.o0.s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ACCEPT_O0 --allow-empty

; OK-NOT: error
; OK-NOT: requires -O2

; ACCEPT_O0-NOT: error
; ACCEPT_O0-NOT: requires -O2

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
