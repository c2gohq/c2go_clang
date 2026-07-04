; c2go #586: clang emits anonymous string literals as private globals (`.str`,
; mangled to `.L.str` on the neutral-ELF triple c2go-lto retargets to). In Plan 9
; these MUST be file-local (`name<>(SB)`), not a plain global symbol: two
; separately compiled c2go packages would each define a global `_L_str` and the
; Go linker rejects the duplicate ("duplicated definition of symbol _L_str").
; The Plan-9 streamer scopes private data symbols with `<>` on BOTH the DATA /
; GLOBL definition and every (SB) reference, so each package's literal is
; distinct.
;
; RUN: llc %s -mtriple=arm64-unknown-none-goabi --output-asm-variant=2 -o - \
; RUN:   | FileCheck %s

@.str = private unnamed_addr constant [3 x i8] c"hi\00", align 1

; The load reference is file-local scoped ...
; CHECK-LABEL: TEXT {{.*}}firstchar(SB)
; CHECK: MOVBU _L_str<>(SB)
define goabi0cc i8 @firstchar() {
  %c = load i8, ptr @.str
  ret i8 %c
}

; ... and so is the definition. A bare global `_L_str(SB)` (no `<>`) is the #586
; bug and must never appear.
; CHECK: DATA _L_str<>+0(SB)
; CHECK: GLOBL _L_str<>(SB), RODATA, $3
; CHECK-NOT: GLOBL _L_str(SB)

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
