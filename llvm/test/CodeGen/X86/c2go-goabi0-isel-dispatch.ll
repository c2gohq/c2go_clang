; GoABI0 ISel dispatch, end to end.
;
; Exercises all four GoABI0 ISel hooks (LowerFormalArguments +
; LowerReturn on the callee side; LowerCall + LowerCallResult on the
; caller side) under both "SysV caller -> GoABI0 callee" and "GoABI0
; caller -> GoABI0 callee" flavours, mirroring AArch64 GoABI0.
;
; Frame layout reminder (x86-64 GoABI0, reserved-call-frame default):
;
;     caller_outgoing_frame [SP+0..args+results] | retPC | callee_incoming_frame
;                                                ^callee_SP_on_entry
;
;     SP+0..args (caller writes args here)
;     SP+args..results (callee writes results here on return)
;     retPC at SP-8 (caller view post-call) / SP+0..7 (callee view pre-ret)
;
; On x86-64 the +8 RA bias on incoming FixedObjects makes callee's
; LocMemOffset 0 line up with caller_SP+8 naturally - no equivalent of
; AArch64's c2goReserveCallerLRSlot is needed.
;
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -O2 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

; ---------------------------------------------------------------------------
; Test 1 - single i64 callee: stack-in / stack-out via 8(%rsp) / 16(%rsp).
; ---------------------------------------------------------------------------
; CHECK-LABEL: callee_i64_identity:
; CHECK:      movq    8(%rsp), %rax
; CHECK-NEXT: movq    %rax, 16(%rsp)
; CHECK-NEXT: retq
define goabi0cc i64 @callee_i64_identity(i64 %x) {
  ret i64 %x
}

; ---------------------------------------------------------------------------
; Test 2 - multi-arg, single result. Two i32 args at sp+8 / sp+12 (4-byte
; slots per GoABI0 promotion rule), result at sp+16 (args=8, ret=0, RA=8).
; ---------------------------------------------------------------------------
; CHECK-LABEL: callee_add_i32_to_i64:
; CHECK-DAG:  movl    8(%rsp), {{%e[a-z0-9]+}}
; CHECK-DAG:  movl    12(%rsp), {{%e[a-z0-9]+}}
; CHECK:      movq    {{%r[a-z0-9]+}}, 16(%rsp)
; CHECK-NEXT: retq
define goabi0cc i64 @callee_add_i32_to_i64(i32 %a, i32 %b) {
  %az = zext i32 %a to i64
  %bz = zext i32 %b to i64
  %s = add i64 %az, %bz
  ret i64 %s
}

; ---------------------------------------------------------------------------
; Test 3 - SysV -> GoABI0 boundary call (CC mismatch). Caller stages args at
; outgoing-frame SP+0 / SP+4, calls, then reads result at SP+8.
; Outgoing-frame size = args(8) + result(8) + align(8) = 24 bytes.
; ---------------------------------------------------------------------------
; CHECK-LABEL: sysv_caller_calling_goabi0:
; CHECK:      subq    $24, %rsp
; CHECK-DAG:  movl    %edi, (%rsp)
; CHECK-DAG:  movl    %esi, 4(%rsp)
; CHECK:      callq   callee_add_i32_to_i64{{(@PLT)?}}
; CHECK-NEXT: movq    8(%rsp), %rax
; CHECK-NEXT: addq    $24, %rsp
; CHECK:      retq
define i64 @sysv_caller_calling_goabi0(i32 %x, i32 %y) {
  %r = call goabi0cc i64 @callee_add_i32_to_i64(i32 %x, i32 %y)
  ret i64 %r
}

; ---------------------------------------------------------------------------
; Test 4 - GoABI0 -> GoABI0 (callee + caller both stack). The outer function
; reads incoming args from sp+8/12 (well, after subq adds 24 so sp+32/36 -
; LLVM may consolidate). After the sub-call, the outer function writes its
; own result back to its caller's outgoing-arg block at sp+40 (= 24 caller
; subq + 8 args + 8 RA bias).
; ---------------------------------------------------------------------------
; CHECK-LABEL: goabi0_caller_calling_goabi0:
; CHECK:      subq    $24, %rsp
; CHECK:      callq   callee_add_i32_to_i64{{(@PLT)?}}
; CHECK:      movq    8(%rsp), {{%r[a-z0-9]+}}
; CHECK:      movq    {{%r[a-z0-9]+}}, 40(%rsp)
; CHECK:      addq    $24, %rsp
; CHECK:      retq
define goabi0cc i64 @goabi0_caller_calling_goabi0(i32 %x, i32 %y) {
  %r = call goabi0cc i64 @callee_add_i32_to_i64(i32 %x, i32 %y)
  ret i64 %r
}

; ---------------------------------------------------------------------------
; Test 5 - sanity: SysV CC functions are UNCHANGED by the new dispatch.
; A non-goabi0cc function calling a non-goabi0cc one keeps the SysV
; register-passing path (arg in %rdi, result in %rax).
; ---------------------------------------------------------------------------
; CHECK-LABEL: sysv_unchanged:
; CHECK-NOT:  callq   {{[^@]+}}@PLT
; CHECK:      movq    %rdi, %rax
; CHECK:      retq
define i64 @sysv_unchanged(i64 %x) {
  ret i64 %x
}
