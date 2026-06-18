; GoABI0 reg-return fallback (X86 mirror of the AArch64 RetCC_AArch64_AAPCS
; bypass).
;
; An internal goabi0cc function carrying the c2go-reg-return fn-attr (set
; by clang on internal/indirect calls) returns via the SysV register file
; (RAX / XMM0 / ...) instead of writing its result to the caller's
; outgoing-arg block. It continues to use GoABI0 for incoming args
; (stack-only); only the result path is bypassed, via SysV RetCC_X86_64_C.
;
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -O2 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

; ---------------------------------------------------------------------------
; Test 1 - reg-return callee: incoming i64 from sp+8 (GoABI0 stack arg)
; but result returned in %rax (SysV reg, NOT stored to sp+16). The
; absence of any `, 16(%rsp)` store is the load-bearing diff vs the
; GoABI0 stack-result path.
; ---------------------------------------------------------------------------
; CHECK-LABEL: reg_return_i64_identity:
; CHECK:      movq    8(%rsp), %rax
; CHECK-NOT:  movq    {{%r[a-z0-9]+}}, 16(%rsp)
; CHECK:      retq
define goabi0cc i64 @reg_return_i64_identity(i64 %x) #0 {
  ret i64 %x
}

; ---------------------------------------------------------------------------
; Test 2 - reg-return f64: incoming f64 from sp+8, result in %xmm0 (SysV
; FP reg), NOT stored to sp+16.
; ---------------------------------------------------------------------------
; CHECK-LABEL: reg_return_f64_identity:
; CHECK:      movsd   8(%rsp), %xmm0
; CHECK-NOT:  movsd   %xmm0, 16(%rsp)
; CHECK:      retq
define goabi0cc double @reg_return_f64_identity(double %x) #0 {
  ret double %x
}

; ---------------------------------------------------------------------------
; Test 3 - reg-return caller -> callee. The call site carries the
; c2go-reg-return attr, so the caller reads the result from %rax
; directly, NOT from the outgoing-arg block at sp+8. The outgoing call
; frame is therefore args-only (8 rounded to 8 = 8), not args+result
; (16). LLVM may rewrite the 8-byte subq+addq as the equivalent
; pushq/popq pair on x86-64 - either form signals "args only, no result
; slot reservation".
; ---------------------------------------------------------------------------
; CHECK-LABEL: reg_return_caller:
; CHECK:      {{(subq[[:space:]]+\$8,[[:space:]]+%rsp|pushq[[:space:]]+%[a-z0-9]+)}}
; CHECK:      movq    {{%r[a-z0-9]+}}, (%rsp)
; CHECK:      callq   reg_return_i64_identity{{(@PLT)?}}
; CHECK-NOT:  movq    8(%rsp), %rax
; CHECK:      {{(addq[[:space:]]+\$8,[[:space:]]+%rsp|popq[[:space:]]+%[a-z0-9]+)}}
; CHECK:      retq
define i64 @reg_return_caller(i64 %x) {
  %r = call goabi0cc i64 @reg_return_i64_identity(i64 %x) #1
  ret i64 %r
}

; ---------------------------------------------------------------------------
; Test 4 - stack-result defense: a callee WITHOUT c2go-reg-return still
; goes through the GoABI0 stack-result contract (writes result to sp+16).
; The reg-return bypass must not regress this path; it is locked here
; alongside the bypass tests so the split is visible in one file.
; ---------------------------------------------------------------------------
; CHECK-LABEL: boundary_stack_i64_identity:
; CHECK:      movq    8(%rsp), %rax
; CHECK-NEXT: movq    %rax, 16(%rsp)
; CHECK-NEXT: retq
define goabi0cc i64 @boundary_stack_i64_identity(i64 %x) {
  ret i64 %x
}

; ---------------------------------------------------------------------------
; Test 5 - boundary stack-result caller: a CALL to a boundary GoABI0
; callee (no c2go-reg-return at the call site) must reserve result-slot
; space in the outgoing call frame (args=8 + result=8 + align=8 = 24
; bytes) and load the result from sp+8. Pins the baseline GoABI0
; stack-result contract and proves the reg-return bypass did NOT regress
; boundary calls into the args-only path.
; ---------------------------------------------------------------------------
; CHECK-LABEL: boundary_stack_caller_baseline:
; CHECK:      subq    $24, %rsp
; CHECK:      movq    {{%r[a-z0-9]+}}, (%rsp)
; CHECK:      callq   boundary_stack_i64_identity{{(@PLT)?}}
; CHECK:      movq    8(%rsp), %rax
; CHECK:      addq    $24, %rsp
; CHECK:      retq
define i64 @boundary_stack_caller_baseline(i64 %x) {
  %r = call goabi0cc i64 @boundary_stack_i64_identity(i64 %x)
  ret i64 %r
}

attributes #0 = { "c2go-reg-return" }
attributes #1 = { "c2go-reg-return" }
