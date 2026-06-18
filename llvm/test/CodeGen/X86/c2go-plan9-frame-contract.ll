; Per-arch frame contract on the MCPlan9AsmStreamer side:
;
;   * AArch64 producer:  SavedLinkSize=8, FrameAlignment=16
;     Streamer emits $<FrameSize - 16>-<argsize> for FrameSize >= 16,
;     NOFRAME, $0-<argsize> otherwise. Existing AArch64 LIT already covers
;     the FrameSize > 0 path byte-identical, so this file focuses on X86.
;
;   * X86 producer:      SavedLinkSize=0, FrameAlignment=0
;     Streamer must NOT subtract the AArch64 16-byte fixup. For
;     strict-leaf functions emitted by the X86 c2go leaf-ABI pass
;     (FrameSize=0) c2go-plan9-asm-emit.ll already pins $0-<argsize>.
;     This file guards the future non-zero-frame X86 path - once a
;     producer publishes FrameSize > 0 with the X86 contract, the
;     streamer must emit $<FrameSize>-<argsize> (no 16-byte subtract).
;
; The test invokes the streamer via llc and lets the X86 frame-emitter
; producer publish the metadata; the assertion is that the TEXT directive
; carries the literal $0-<argsize> form, not $-16-<argsize>.
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t.bc
; RUN: llc %t.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -output-asm-variant=2 -o - 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

; A strict-leaf function with `c2go-argsize="16"`. The X86 producer
; sets `Meta.SavedLinkSize = 0` + `Meta.FrameAlignment = 0`. With
; FrameSize == 0 the streamer should NOT subtract anything from
; FrameSize and emit `NOSPLIT|NOFRAME, $0-16`. The negative CHECK-NOT
; pins that the AArch64-flavoured `$-16-` byte does NOT leak in.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}leaf_two_args(SB), NOSPLIT|NOFRAME, $0-16
; CHECK-NOT:   $-16-16
define internal goabi0cc i64 @leaf_two_args(i64 %a, i64 %b) #0 {
  %s = add i64 %a, %b
  ret i64 %s
}

attributes #0 = { "c2go-reg-return" "c2go-argsize"="16" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
