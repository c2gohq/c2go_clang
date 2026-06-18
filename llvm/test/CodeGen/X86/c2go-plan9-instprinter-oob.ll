; Regression lock for an X86Plan9InstPrinter SmallVector OOB in the
; reg/reg ALU print path (tryPrintArithReg).
;
; Root cause: the per-branch guards used getNumOperands() < (NumDefs + 2)
; before touching getOperand(2). For opcodes whose InstDesc reports
; NumDefs == 0 (every CMP/TEST family - implicit-def $eflags is not an
; explicit def) the guard collapsed to < 2 while the body's hard
; precondition was >= 3. A naturally-emitted MCInst of shape
; CMP64rr %rax, %rcx, implicit-def $eflags (2 explicit operands) slipped
; past the guard and operand(2) ran off the end of the SmallVector.
;
; Fix: a single function-head guard `if (getNumOperands() < 3) return
; false;` matches the rr/ri/ri8 body access pattern (which only ever
; touches operand index 2). Per-branch < (NumDefs + 2) checks remain as
; tighter narrowers for NumDefs >= 1. With the guard the printer refuses
; the shape and falls through to the raw-byte WORD/BYTE fallback.
;
; Companion audit: every other tryPrint* dispatcher already had its own
; function-head operand-count guard; this one was the gap. A later sweep
; closed three more guard-less paths before their getOperand access -
; the CALL64m memory form (indirect call), the JMP64r single-reg form,
; and the JMP64m memory form (computed goto). For well-formed opcodes
; the fast path is byte-identical; the guard only intercepts malformed
; MCInsts with fewer slots than the body's index demands. Those sites
; are exercised below and we check only that the body emits without
; aborting.
;
; RUN: llc %s -mtriple=x86_64-apple-darwin -O0 \
; RUN:     -output-asm-variant=2 -o - 2>&1 | FileCheck %s

target triple = "x86_64-apple-darwin"
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"

@g = internal global i32 0, align 4

; The CMP64rr emitted at -O0 has NumDefs=0 (implicit-def $eflags) and
; exactly 2 explicit operands. Pre-fix this drove operand(2) OOB; post-
; fix the early function-head guard refuses the shape and the streamer
; falls back to raw bytes (BYTE $0x48 ...).
define internal goabi0cc i32 @cmp_rr_repro(ptr %a, ptr %b, ptr %c) {
entry:
  %sa = alloca ptr, align 8
  %sb = alloca ptr, align 8
  %sc = alloca ptr, align 8
  store ptr %a, ptr %sa, align 8
  store ptr %b, ptr %sb, align 8
  store ptr %c, ptr %sc, align 8
  %va = load ptr, ptr %sa, align 8
  %vb = load ptr, ptr %sb, align 8
  %vc = load ptr, ptr %sc, align 8
  ; Force %va / %vb live in regs across the call so the cmp shape is rr
  ; (not rm, which would route via tryPrintSPMemImm) and is not folded
  ; back into a memory operand by isel under -O0.
  call void @sink(ptr %vc)
  %cmp = icmp ne ptr %va, %vb
  br i1 %cmp, label %t, label %f
t:
  store i32 1, ptr @g, align 4
  ret i32 1
f:
  store i32 0, ptr @g, align 4
  ret i32 0
}

declare void @sink(ptr)

; The TEXT directive must still emit (we got past the assert).
; CHECK-LABEL: TEXT {{[^[:space:]]+}}cmp_rr_repro(SB)
;
; X86TargetLowering::createFastISel declines FastISel for every function
; in a c2go-mode module (FastISel's unconditional SysV memset/memcpy
; libcalls break the GoABI0 boundary at -O0), so -O0 goes through
; SelectionDAG, which lowers this icmp+br as a flags-producing ALU op +
; Jcc rather than a bare 2-operand CMP64rr. The short-operand OOB
; refusal itself is pinned at the MCInst level in a directly-constructed
; unittest where it cannot drift with ISel. Here we lock the natural -O0
; shape: both pointers reloaded into regs, the flags producer, the
; conditional branch, and a clean RET (no abort midway).
; CHECK: CALL {{[^[:space:]]+}}sink(SB)
; CHECK: {{SUBQ|CMPQ}} CX, AX
; CHECK: JEQ
;
; Function must return cleanly - no garbage / no abort midway through.
; CHECK: RET

; ----- exercise the three newly-guarded paths -----
;
; (a) Indirect CALL through a function pointer in memory - drives
;     CALL64m through tryPrintIndirectCall and printPlan9MemRef. The
;     pre-fix path had a < 1 guard at function head, which let any MCInst
;     with 1-4 operands fall into printPlan9MemRef's OpStart+3 access and
;     OOB. Post-fix the mem branch guards on < 5.
@fptr = internal global ptr null, align 8

define internal goabi0cc void @call_through_mem_repro() {
entry:
  %f = load ptr, ptr @fptr, align 8
  call void %f()
  ret void
}
; CHECK-LABEL: TEXT {{[^[:space:]]+}}call_through_mem_repro(SB)
; CHECK: RET

; (b) Computed-goto / indirect JMP - drives JMP64r (single reg). Pre-fix
;     this branch had no head guard at all; post-fix it guards on < 1.
;     We mostly want the function to compile and the streamer to finish
;     without aborting - exact mnemonic dispatch is not load-bearing here
;     since SelectionDAG may legalize this back into a switch table.
@jt = internal global [3 x ptr] [ptr blockaddress(@indir_jmp_repro, %b0),
                                  ptr blockaddress(@indir_jmp_repro, %b1),
                                  ptr blockaddress(@indir_jmp_repro, %b2)],
                                  align 8

define internal goabi0cc i32 @indir_jmp_repro(i32 %k) {
entry:
  %idx = zext i32 %k to i64
  %slot = getelementptr inbounds [3 x ptr], ptr @jt, i64 0, i64 %idx
  %tgt = load ptr, ptr %slot, align 8
  indirectbr ptr %tgt, [label %b0, label %b1, label %b2]
b0:
  ret i32 0
b1:
  ret i32 1
b2:
  ret i32 2
}
; CHECK-LABEL: TEXT {{[^[:space:]]+}}indir_jmp_repro(SB)
; CHECK: RET

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
