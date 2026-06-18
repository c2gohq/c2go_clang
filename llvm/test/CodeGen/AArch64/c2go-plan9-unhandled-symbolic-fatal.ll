; Plan 9 streamer must fail CLOSED on AArch64 for an unhandled symbol-bearing
; instruction (matching the X86 policy in
; X86/c2go-plan9-unhandled-symbolic-fatal.ll).
;
; A fail-open AArch64 branch would emit a PLAN9-ERROR comment and continue;
; Go's assembler does not reject comments, so the instruction is silently
; swallowed and the load/store vanishes from the assembled function. With the
; LDRQui/STRQui :lo12: coverage gap closed (c2go-plan9-ldrq-symbolic.ll), any
; remaining symbol-bearing miss must abort the compile at build time.
;
; Trigger: an FP16 (half) load against a global lowers to LDRHui (H-register
; SIMD load) with a :lo12: displacement. The H-reg width is outside the Plan 9
; printer's LDR/STR coverage, so the streamer must report_fatal_error rather
; than ship a .s with the load missing.
;
; RUN: not --crash llc < %s --output-asm-variant=2 \
; RUN:     -mtriple=arm64-unknown-none-goabi -o /dev/null 2>&1 \
; RUN:   | FileCheck %s
;
; CHECK: LLVM ERROR: MCPlan9AsmStreamer: unhandled symbol-bearing instruction (opcode=LDRHui, 1 fixup(s))

target triple = "arm64-unknown-none-goabi"

@ghalf = internal global half 0xH0000, align 2

define internal void @huse(ptr %out) {
entry:
  %v = load volatile half, ptr @ghalf, align 2
  store half %v, ptr %out, align 2
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
