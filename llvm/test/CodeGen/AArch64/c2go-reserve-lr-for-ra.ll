; c2go emits Go assembler functions. Go's arm64 assembler decides whether a
; function is a leaf by looking for CALL instructions and only saves LR for
; non-leaf functions. Consequently, LLVM must not allocate LR as a scratch
; register even in an LLVM leaf: doing so makes RET branch to scratch data.
;
; Keep enough integer values live at once that an unreserved LR is selected by
; register allocation. The c2go module flag must instead force a spill and keep
; R30 absent from the generated Plan 9 body.
;
; RUN: llc < %s --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

@values = global [30 x i64] zeroinitializer

; CHECK-LABEL: TEXT ·keep_live(SB)
; CHECK-NOT: R30
; CHECK: RET
define void @keep_live() {
entry:
  %values = load volatile [30 x i64], ptr @values
  store volatile [30 x i64] %values, ptr @values
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
