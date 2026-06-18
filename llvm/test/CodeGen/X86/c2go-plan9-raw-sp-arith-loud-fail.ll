; X86 raw SP-arithmetic loud-fail: positive production-path lock (no
; fatal expected).
;
; Why the fatal exists: Go's obj6 deltasp tracks only APUSH/APOP/AADJSP.
; A raw `SUBQ $imm,SP` / `ADDQ $imm,SP` falls into the default arm (silent
; auto-SPWRITE; the bad-SPWRITE fatal is gated on !IsAsm, and the
; assembler always sets IsAsm=true). obj6 also unconditionally injects
; AADJSP $localoffset in the prologue, so if LLVM leaks a raw SUBQ/ADDQ
; into the Plan-9 .s, SP moves twice at runtime - silent stack
; corruption, not a compile-time diagnostic.
;
; Real-path coverage in production (no fatal expected):
;   (a) The c2go frame emitter's prologue/epilogue SUBQ/ADDQ carry
;       FrameSetup/FrameDestroy flags and are suppressed at .s lowering.
;   (b) Forced reserved-call-frame + X86CallFrameOptimization bail make
;       the !reserveCallFrame branch in eliminateCallFramePseudoInstr
;       unreachable; the residual InternalAmt path stays 0 because
;       c2goabiinternalcc is not a callee-pop CC.
;   (c) DynAlloca / TCRETURN / IRET / WIN_ALLOCA / split-stack are gated
;       out by c2go leaf eligibility / mayTailCallThisCC.
;
; This LIT is the positive sanity lock: a strict-leaf goabi0cc function
; with a non-zero frame (locals + call-site outgoing args) must compile
; clean through the Plan-9 streamer without tripping the loud-fail. If a
; future pass starts emitting raw SUBQ/ADDQ against RSP without
; FrameSetup/FrameDestroy flags inside a c2go function, the fatal catches
; it at compile time instead of producing a silent runtime miscompile.
;
; This LIT is NOT a negative trigger of the fatal; it covers the
; production path on which no fatal is expected. The fatal here matches
; only direct ADD/SUB-imm-to-RSP forms; the LEA64r SP, [SP+imm] form
; selected when STI.useLeaForSP() is true is exercised by the companion
; .mir (c2go-plan9-raw-sp-arith-lea-loud-fail.mir).
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t.bc
; RUN: llc %t.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -output-asm-variant=2 -o - 2>&1 | FileCheck %s
; RUN: llc %t.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -output-asm-variant=2 -O2 -o - 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

; A strict-leaf goabi0cc function with a non-trivial frame. The X86
; producer's prologue/epilogue SUBQ/ADDQ get FrameSetup/FrameDestroy
; flags and are suppressed at .s lowering time; the loud-fail must not
; trip. The function emits a normal Plan-9 TEXT directive.
;
; CHECK-LABEL: TEXT {{[^[:space:]]+}}leaf_with_locals(SB),
; CHECK-NOT: LLVM ERROR: X86 c2go: raw SP arithmetic
define internal goabi0cc i64 @leaf_with_locals(i64 %a, i64 %b) #0 {
entry:
  %slot = alloca i64, align 8
  store i64 %a, ptr %slot, align 8
  %v = load i64, ptr %slot, align 8
  %s = add i64 %v, %b
  ret i64 %s
}

attributes #0 = { "c2go-reg-return" "c2go-argsize"="16" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
