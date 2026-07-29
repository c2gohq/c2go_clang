; AArch64 writes to a W register clear the upper 32 bits of the corresponding
; X register. The Plan 9 MOVW register form sign-extends instead, so the
; ORRWrs WZR, Wm move alias must be printed as MOVWU.
;
; RUN: llc < %s --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

; CHECK-LABEL: TEXT ·zext_arg(SB)
; CHECK: MOVWU R0, R0
; CHECK-NOT: MOVW R0, R0
define i64 @zext_arg(i32 %x) {
entry:
  %z = zext i32 %x to i64
  ret i64 %z
}

; This is the union reconstruction shape used by optimized musl log10/log1p:
; the low word must be zero-extended before it is combined with the high word.
; CHECK-LABEL: TEXT ·join_words(SB)
; CHECK: MOVWU R0, [[LO:R[0-9]+]]
; CHECK-NOT: MOVW R0, [[LO]]
define i64 @join_words(i32 %lo, i32 %hi) {
entry:
  %lo64 = zext i32 %lo to i64
  %hi64 = zext i32 %hi to i64
  %shift = shl i64 %hi64, 32
  %result = or i64 %shift, %lo64
  ret i64 %result
}
