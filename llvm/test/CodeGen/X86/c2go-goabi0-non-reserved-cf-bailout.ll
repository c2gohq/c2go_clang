; GoABI0 ISel must loud-fail (not silently miscompile) when the function
; is not reserved-call-frame.
;
; The GoABI0 stack-only ISel paths (LowerCall / LowerCallResult /
; LowerReturn) assume the X86 reserved-call-frame model: SP does not move
; across calls, outgoing-arg block addresses are raw `sp + LocMemOffset`,
; and callee-side FixedObjects pick up only the +8 RA bias.
;
; X86FrameLowering::hasReservedCallFrame returns false when any of:
;   * MFI.hasVarSizedObjects()        (dynamic alloca)
;   * getHasPushSequences()           (push-based arg passing)
;   * hasPreallocatedCall()           (preallocated attr)
; In those cases the GoABI0 LocMemOffset arithmetic is silently wrong.
; All three ISel sites therefore report_fatal_error.
;
; This is a permanent fail-closed: no X86 c2go producer reaches the
; non-reserved-CF shape (c2go Sema rejects all dynamic stack allocation;
; push sequences and preallocated calls are off the c2go path; the
; AArch64-only variadic saved-LR-at-sp+0 producer does not exist on X86).
; This test reaches the shape only because hand-written IR bypasses Sema -
; exactly the case the guard exists for.
;
; Triggered via dynamic alloca on a goabi0cc function, the simplest of
; the three hasReservedCallFrame=false conditions.
;
; RUN: not --crash llc < %s -mtriple=x86_64-unknown-linux-gnu -O2 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

; goabi0cc callee with dynamic alloca -> hasVarSizedObjects() ->
; non-reserved-CF. LowerReturn must fatal-error rather than emit a
; wrongly-offset stack store.
; CHECK: LLVM ERROR: c2go #298 Wave AK: X86 GoABI0 LowerReturn
; CHECK-SAME: non-reserved-call-frame function
; CHECK-SAME: hasVarSizedObjects
; CHECK-SAME: permanently fail-closed (Wave AM.2)
define goabi0cc i64 @callee_with_dyn_alloca(i64 %n, i64 %x) {
  %p = alloca i8, i64 %n
  store i8 0, ptr %p
  ret i64 %x
}
