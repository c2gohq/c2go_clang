; c2go #600: amd64 stack-alignment ship-gate. Go's amd64 stack is only
; 8-byte aligned, but IR known-bits folds trust an alloca's align attribute:
; a claimed 16 licenses rewrites like `buf+9` -> `buf|9` that are silently
; wrong when the frame lands at 8 mod 16 (fmt_fp's printf %e dropped its
; exponent exactly this way). clang -fc2go no longer emits >8-aligned
; allocas on x86-64, so any survivor reaching c2go-lto (explicit alignas /
; __int128 / vector local, or a pass raising an alloca's alignment) cannot
; be lowered soundly on the Go stack — c2go-lto must refuse to emit.

; RUN: not c2go-lto --c2go-escape-nonfatal %s 2>&1 | FileCheck %s
; RUN: sed 's/align 16/align 8/' %s > %t.ok.ll
; RUN: c2go-lto --c2go-escape-nonfatal %t.ok.ll

; CHECK: error: function 'over' has an alloca 'buf' requiring 16-byte stack alignment, but the Go amd64 stack guarantees only 8

target triple = "x86_64-unknown-linux-gnu"

define void @over(ptr %o) {
entry:
  %buf = alloca [22 x i8], align 16
  store ptr %buf, ptr %o
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
