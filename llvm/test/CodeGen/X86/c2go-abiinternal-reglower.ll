; X86 register-passing lowering for c2goabiinternalcc, verifying the
; CC_X86_64_C2GoABIInternal_TD table. Under c2goabiinternalcc:
;   - integer args use the Go ABIInternal int reg list
;     AX, BX, CX, DI, SI, R8, R9, R10, R11
;     (RDX = closure ctxt and R15 = zero reg are skipped)
;   - FP/f64 args and results use XMM0..XMM14
;     (XMM15 = FP zero reg is skipped)
; Mirror of AArch64 c2go-statepoint-reg-return.ll on the X86 backend.
;
; leaf_add3 (3 i32) and leaf_ret_f64 (2 f64) overlap heavily with the
; leading SysV regs, so the stress functions exercise the reg-list tail,
; stack overflow, and the RDX skip:
;   - leaf_10ints  : 10 i32 pin all 9 int regs + the 10th at 8(%rsp)
;   - leaf_16floats: 16 f64 pin all 15 XMM (XMM0..14) + the 16th on the
;                    stack + CHECK-NOT XMM15
;   - leaf_mix6    : 6 i32 pin EAX..R8D + CHECK-NOT EDX (SysV 3rd-arg slot)
;
; The stress functions run at -O2 so spills do not reuse XMM15 / EDX as
; scratch and falsely trip the CHECK-NOT lines; leaf_add3 / leaf_ret_f64
; stay at -O0.
;
; REG path (primary): with c2go.x86-leaf-abi on, the leaf-abi pass flips
; eligible leaves and llc does ISel + register-passing emit directly.
;
; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -S \
; RUN:   | llc -mtriple=x86_64-unknown-linux-gnu -O0 \
; RUN:   | FileCheck %s --check-prefix=REG
;
; REG-STRESS path: same IR, llc at -O2 for stable stress-function output.
;
; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -S \
; RUN:   | llc -mtriple=x86_64-unknown-linux-gnu -O2 \
; RUN:   | FileCheck %s --check-prefix=REG-STRESS
;
; NOFLIP path: llc on the same IR without the leaf-abi pass (simulating
; the leaf-abi gate off, IR still goabi0cc) - confirms goabi0cc keeps its
; own stack-passing ABI and does not fall through to SysV.
;
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -O0 \
; RUN:   | FileCheck %s --check-prefix=NOFLIP

target triple = "x86_64-unknown-linux-gnu"

; --- 3-integer leaf: flipped to c2goabiinternalcc, args %a -> EAX,
;     %b -> EBX, %c -> ECX (first 3 of the int reg list, 32-bit view).
;     The i32 result is in EAX.
;
; REG-LABEL: leaf_add3:
; REG-DAG: addl %ebx, %eax
; REG-DAG: addl %ecx, %eax
; REG: retq
define internal goabi0cc i32 @leaf_add3(i32 %a, i32 %b, i32 %c) #0 {
  %s1 = add i32 %a, %b
  %s2 = add i32 %s1, %c
  ret i32 %s2
}

; --- f64 leaf: f64 args + result use the XMM list, XMM0 first. SysV
;     also passes in XMM0/XMM1, so the args alone do not distinguish SysV
;     from C2GoABIInternal (that distinction is leaf_add3's EAX/EBX/ECX
;     vs SysV's EDI/ESI/EDX). This only confirms f64 reg-passing works
;     and CC dispatch reached the C2GoABIInternal table.
;
; REG-LABEL: leaf_ret_f64:
; REG: addsd %xmm1, %xmm0
; REG: retq
define internal goabi0cc double @leaf_ret_f64(double %a, double %b) #0 {
  %s = fadd double %a, %b
  ret double %s
}

; --- stress #1: 10 i32 args pin all 9 int regs
;     EAX BX CX DI SI R8D R9D R10D R11D; the 10th spills to 8(%rsp).
;     Result in EAX. RDX (closure ctxt) must not appear.
;
;     The equivalent SysV signature puts arg 1 in EDI, arg 3 in EDX, and
;     args 7+ on the stack - a completely different reg shape - so this
;     is a precise probe for any regression back to the SysV table. At
;     -O2 every arg is added directly into %eax.
;
; REG-STRESS-LABEL: leaf_10ints:
; REG-STRESS-DAG: %ebx
; REG-STRESS-DAG: %ecx
; REG-STRESS-DAG: %edi
; REG-STRESS-DAG: {{%esi|%rsi}}
; REG-STRESS-DAG: %r8
; REG-STRESS-DAG: %r9
; REG-STRESS-DAG: %r10
; REG-STRESS-DAG: %r11
; REG-STRESS: addl 8(%rsp), %eax
; REG-STRESS: retq
; REG-STRESS-NOT: %edx
; REG-STRESS-NOT: %rdx
define internal goabi0cc i32 @leaf_10ints(i32 %a0, i32 %a1, i32 %a2, i32 %a3,
                                          i32 %a4, i32 %a5, i32 %a6, i32 %a7,
                                          i32 %a8, i32 %a9) #0 {
  %s0 = add i32 %a0, %a1
  %s1 = add i32 %s0, %a2
  %s2 = add i32 %s1, %a3
  %s3 = add i32 %s2, %a4
  %s4 = add i32 %s3, %a5
  %s5 = add i32 %s4, %a6
  %s6 = add i32 %s5, %a7
  %s7 = add i32 %s6, %a8
  %s8 = add i32 %s7, %a9
  ret i32 %s8
}

; --- stress #2: 16 f64 args pin all 15 XMM regs XMM0..XMM14; the 16th
;     spills to 8(%rsp). Result in XMM0. XMM15 (FP zero reg, Go-runtime
;     reserved) must not appear - the precise check at the reg-list tail.
;     At -O2 each XMM1..XMM14 is added directly into XMM0, the 16th via
;     addsd 8(%rsp), %xmm0.
;
; REG-STRESS-LABEL: leaf_16floats:
; REG-STRESS-DAG: addsd %xmm1, %xmm0
; REG-STRESS-DAG: addsd %xmm2, %xmm0
; REG-STRESS-DAG: addsd %xmm3, %xmm0
; REG-STRESS-DAG: addsd %xmm4, %xmm0
; REG-STRESS-DAG: addsd %xmm5, %xmm0
; REG-STRESS-DAG: addsd %xmm6, %xmm0
; REG-STRESS-DAG: addsd %xmm7, %xmm0
; REG-STRESS-DAG: addsd %xmm8, %xmm0
; REG-STRESS-DAG: addsd %xmm9, %xmm0
; REG-STRESS-DAG: addsd %xmm10, %xmm0
; REG-STRESS-DAG: addsd %xmm11, %xmm0
; REG-STRESS-DAG: addsd %xmm12, %xmm0
; REG-STRESS-DAG: addsd %xmm13, %xmm0
; REG-STRESS-DAG: addsd %xmm14, %xmm0
; REG-STRESS: addsd 8(%rsp), %xmm0
; REG-STRESS: retq
; REG-STRESS-NOT: %xmm15
define internal goabi0cc double @leaf_16floats(double %a0, double %a1, double %a2,
                                               double %a3, double %a4, double %a5,
                                               double %a6, double %a7, double %a8,
                                               double %a9, double %a10, double %a11,
                                               double %a12, double %a13, double %a14,
                                               double %a15) #0 {
  %s0 = fadd double %a0, %a1
  %s1 = fadd double %s0, %a2
  %s2 = fadd double %s1, %a3
  %s3 = fadd double %s2, %a4
  %s4 = fadd double %s3, %a5
  %s5 = fadd double %s4, %a6
  %s6 = fadd double %s5, %a7
  %s7 = fadd double %s6, %a8
  %s8 = fadd double %s7, %a9
  %s9 = fadd double %s8, %a10
  %s10 = fadd double %s9, %a11
  %s11 = fadd double %s10, %a12
  %s12 = fadd double %s11, %a13
  %s13 = fadd double %s12, %a14
  %s14 = fadd double %s13, %a15
  ret double %s14
}

; --- stress #3: 6 i32 args verify the RDX skip. The first 6 int regs
;     are EAX BX CX DI SI R8D. Under SysV (rdi rsi rdx rcx r8 r9) EDX is
;     the 3rd int-arg register, so "EDX never appears as an arg
;     accumulation target" is the precise Go-ABIInternal-vs-SysV check -
;     RDX is the reserved closure-ctxt slot.
;
; REG-STRESS-LABEL: leaf_mix6:
; REG-STRESS-DAG: addl %ebx,
; REG-STRESS-DAG: addl {{%edi|%ecx,}}
; REG-STRESS-DAG: %esi,
; REG-STRESS-DAG: %r8d,
; REG-STRESS: retq
; REG-STRESS-NOT: addl %edx,
define internal goabi0cc i32 @leaf_mix6(i32 %a, i32 %b, i32 %c, i32 %d,
                                        i32 %e, i32 %f) #0 {
  %s0 = add i32 %a, %b
  %s1 = add i32 %s0, %c
  %s2 = add i32 %s1, %d
  %s3 = add i32 %s2, %e
  %s4 = add i32 %s3, %f
  ret i32 %s4
}

; --- NOFLIP path: no pass, IR stays goabi0cc, llc emits the GoABI0 stack
;     calling convention (args at 8(%rsp), 12(%rsp), 16(%rsp)).
;     leaf_add3 must load its args from the stack, not from SysV regs
;     (EDI/ESI/EDX) nor from pre-colored C2GoABIInternal regs
;     (EAX/EBX/ECX). The load-bearing check is the presence of the
;     stack-load (stack-in) shape; the EDI/ESI/EDX-NOT lines guard against
;     a fall-through to SysV register passing.
;
; NOFLIP-LABEL: leaf_add3:
; NOFLIP-DAG: movl 8(%rsp), {{%e[a-z0-9]+}}
; NOFLIP-DAG: movl 12(%rsp), {{%e[a-z0-9]+}}
; NOFLIP-DAG: movl 16(%rsp), {{%e[a-z0-9]+}}
; NOFLIP-NOT: movl %edi, %
; NOFLIP-NOT: movl %esi, %
; NOFLIP-NOT: movl %edx, %
; NOFLIP-LABEL: leaf_ret_f64:

attributes #0 = { "c2go-reg-return" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
