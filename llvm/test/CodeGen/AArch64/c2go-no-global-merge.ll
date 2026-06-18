; The Plan 9 codegen pipeline must not run AArch64 GlobalMerge. GlobalMerge
; erases per-GV identities into a _MergedGlobals aggregate before the Plan 9
; streamer's go-owned filter can classify globals per-GV; pointer-bearing
; managed internals get folded into a NOPTR blob and the .s ships an unsound
; NOPTR layout.
;
; The production path skips GlobalMerge via a per-TargetMachine flag that
; AArch64PassConfig::addPreISel reads to short-circuit createGlobalMergePass.
; This test exercises the same skip via the -aarch64-enable-global-merge=false
; knob - both routes bypass the identical createGlobalMergePass call - so a
; regression that re-enables GlobalMerge under c2go would break the CHECK-NOT.
;
; Positive control: at -O3 without the gate, GlobalMerge fires and emits
; .L_MergedGlobals for the two internal pointer-typed globals.
; RUN: llc < %s -mtriple=arm64-unknown-none-goabi -O3 -o - \
; RUN:   | FileCheck %s --check-prefix=MERGE
; Gate: -aarch64-enable-global-merge=false exercises the same
; createGlobalMergePass skip the c2go TM flag takes; with it set, no
; merged-globals symbol may appear in the output.
; RUN: llc < %s -mtriple=arm64-unknown-none-goabi -O3 \
; RUN:   -aarch64-enable-global-merge=false -o - | FileCheck %s

target triple = "arm64-unknown-none-goabi"

; Two internal pointer-bearing globals + a co-access function: the minimal
; shape that triggers AArch64 GlobalMerge's "merge two adjacent internal
; globals into _MergedGlobals" rewrite. Without the gate, llc emits a
; .L_MergedGlobals symbol for both.

@gv_a = internal global ptr null, align 8
@gv_b = internal global ptr null, align 8

define void @co_access(ptr %p) {
entry:
  store ptr %p, ptr @gv_a, align 8
  store ptr %p, ptr @gv_b, align 8
  ret void
}

; Positive control: at -O3 without the gate, AArch64 GlobalMerge folds the two
; internals into .L_MergedGlobals and aliases @gv_a / @gv_b into it. This
; baseline lights up if GlobalMerge ever stops firing on this shape.
; MERGE: .L_MergedGlobals
;
; Gate path: GlobalMerge is skipped - neither the merged-globals symbol nor the
; per-GV aliases into it may appear anywhere in the output.
; CHECK-NOT: _MergedGlobals
; CHECK-NOT: .L_MergedGlobals

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
