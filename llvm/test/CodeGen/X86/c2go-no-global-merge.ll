; X86 mirror of the AArch64 c2go-no-global-merge test.
;
; The DisableGlobalMerge knob exists in BackendConfig for X86 as a
; forward-compatible / defensive gate. X86PassConfig has no
; createGlobalMergePass call site today, so _MergedGlobals is never
; emitted regardless of the knob - this LIT pins that byte-identical
; vacuous-by-design baseline so a future X86 codegen change that
; introduces GlobalMerge into the pipeline is loud (the CHECK-NOT lines
; would start firing).
;
; Two RUN lines:
;   1. baseline at -O3 (no c2go gate, no knob) - must not emit
;      MergedGlobals.
;   2. c2go.goabi module flag present - must still not emit MergedGlobals,
;      so a future port of createGlobalMergePass into X86PassConfig has to
;      respect the DisableGlobalMerge knob (today the knob's X86 consumer
;      is vacuous; this pins that invariant).
;
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -O3 -o - \
; RUN:   | FileCheck %s --check-prefix=BASE
; RUN: llc < %s -mtriple=x86_64-unknown-none-goabi -O3 -o - \
; RUN:   | FileCheck %s --check-prefix=C2GO

target triple = "x86_64-unknown-linux-gnu"

; Two internal pointer-bearing globals + a co-access function - same minimal
; shape that triggers AArch64 GlobalMerge's "_MergedGlobals" rewrite. X86
; should leave them per-GV.

@gv_a = internal global ptr null, align 8
@gv_b = internal global ptr null, align 8

define void @co_access(ptr %p) {
entry:
  store ptr %p, ptr @gv_a, align 8
  store ptr %p, ptr @gv_b, align 8
  ret void
}

; Both routes must keep per-GV identities - no MergedGlobals coalescing
; symbol nor the per-GV alias into it. The LIT exists so any future
; regression that adds GlobalMerge to X86PassConfig without respecting
; the c2go gate lights up both run lines.

; BASE-NOT: _MergedGlobals
; BASE-NOT: .L_MergedGlobals
; C2GO-NOT: _MergedGlobals
; C2GO-NOT: .L_MergedGlobals

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
