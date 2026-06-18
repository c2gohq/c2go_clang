; Plan 9 printer: AArch64 LDRQui/STRQui symbolic (:lo12:) coverage.
;
; Shape under test: MachineLICM hoists the ADRP of a loop-invariant global
; address out of the loop while the in-loop Q-reg LDR/STR keeps the :lo12:
; low-12 addend. The Plan 9 printer flushes the lone ADRP as
; MOVD $·sym(SB), Rn (full address, page+lo12 folded) and remembers Rn -> sym,
; but the loop-head label clears the register-state tracker, so the in-loop
; LDRQui/STRQui escapes both the tracked-ADRP path (tracker miss) and the
; symbolic-field path (Q-reg early-return). A fail-open streamer would swallow
; it as a comment - a silent miscompile where the 128-bit load/store vanishes.
;
; The Q-reg :lo12: branch in the symbolic-field path closes this: because every
; ADRP this printer emits lowers to the full-address MOVD, the base register
; holds the complete symbol address at the access, so the Q access prints as
; register-indirect FMOVQ. Go's assembler rejects the direct
; FMOVQ sym(SB), F0 spelling; the register-indirect form is the only legal
; 128-bit SIMD load/store spelling (same as the tracked-ADRP and
; pair-completion Q paths).
;
; The volatile accesses keep the Q load/store inside the loop (only the ADRP is
; loop-invariant), reproducing the hoisted-ADRP + label-boundary shape
; deterministically.
;
; RUN: llc < %s --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s --implicit-check-not=PLAN9-ERROR

target triple = "arm64-unknown-none-goabi"

@gvec = internal global <2 x i64> zeroinitializer, align 16

; CHECK-LABEL: TEXT ·qspin(SB)
; CHECK: MOVD $·gvec(SB), [[BASE:R[0-9]+]]
; CHECK: _LBB0_1:
; CHECK: FMOVQ ([[BASE]]), F{{[0-9]+}}
define internal void @qspin(ptr %out, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i1, %loop ]
  %v = load volatile <2 x i64>, ptr @gvec, align 16
  %slot = getelementptr <2 x i64>, ptr %out, i64 %i
  store <2 x i64> %v, ptr %slot, align 16
  %i1 = add i64 %i, 1
  %c = icmp ult i64 %i1, %n
  br i1 %c, label %loop, label %done
done:
  ret void
}

; Store dual: STRQui with a :lo12: displacement after the same tracker-clearing
; label boundary.
; CHECK-LABEL: TEXT ·qstorespin(SB)
; CHECK: MOVD $·gvec(SB), [[BASE2:R[0-9]+]]
; CHECK: _LBB1_1:
; CHECK: FMOVQ F{{[0-9]+}}, ([[BASE2]])
define internal void @qstorespin(ptr %src, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i1, %loop ]
  %slot = getelementptr <2 x i64>, ptr %src, i64 %i
  %v = load <2 x i64>, ptr %slot, align 16
  store volatile <2 x i64> %v, ptr @gvec, align 16
  %i1 = add i64 %i, 1
  %c = icmp ult i64 %i1, %n
  br i1 %c, label %loop, label %done
done:
  ret void
}

; Adjacent ADRP+STRQui pair (no label boundary - the pair-completion path, not
; the tracker-miss path above): the direct FMOVQ F0, ·gvec(SB) spelling is
; rejected by Go's assembler ("illegal combination"; both Q direct forms are
; illegal). Must print the register-indirect mirror of the LDRQui pair branch:
; full address into the pair's own base register, then FMOVQ through it.
; CHECK-LABEL: TEXT ·qstoreonce(SB)
; CHECK: MOVD $·gvec(SB), [[BASE3:R[0-9]+]]
; CHECK-NEXT: FMOVQ F{{[0-9]+}}, ([[BASE3]])
define internal void @qstoreonce(ptr %src) {
entry:
  %v = load <2 x i64>, ptr %src, align 16
  store volatile <2 x i64> %v, ptr @gvec, align 16
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
