; c2go-lto's private pass pipeline declares several IPO passes off-limits
; (GlobalOpt / DAE / ArgPromotion / Internalize - they all break c2go
; invariants like the no-callee-saved-regs CC, FUNCDATA/PCDATA pairing,
; and RS4GC results). A pass-instrumentation callback report_fatal_error()s
; when a banned pass is about to run. The hidden --c2go-lto-test-banpass=
; flag injects a banned pass so the gate fires in-test instead of needing
; a full regression with a malformed call site.

; RUN: not c2go-lto --c2go-lto-test-banpass=GlobalOptPass %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=BAN_GLOBALOPT
; RUN: not c2go-lto --c2go-lto-test-banpass=DeadArgumentEliminationPass %s \
; RUN:   2>&1 | FileCheck %s --check-prefix=BAN_DAE
; RUN: not c2go-lto --c2go-lto-test-banpass=InternalizePass %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=BAN_INTERNALIZE

; BAN_GLOBALOPT: banned IPO pass 'GlobalOptPass'
; BAN_DAE: banned IPO pass 'DeadArgumentEliminationPass'
; BAN_INTERNALIZE: banned IPO pass 'InternalizePass'

target triple = "aarch64-unknown-linux"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128"

define void @noop() {
entry:
  ret void
}

!llvm.module.flags = !{!0, !1, !2, !3}

!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.opt-level", i32 3}
!2 = !{i32 1, !"c2go.target-cpu", !"generic"}
!3 = !{i32 1, !"c2go.target-features", !"+neon"}
