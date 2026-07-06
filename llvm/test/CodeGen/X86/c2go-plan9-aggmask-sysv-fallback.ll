; c2go #602: X86 locals aggregate-field mask on the SysV-fallback stager
; path (CC NOT flipped — every ordinary c2go function).
;
; The mask used to be computed only on the emitX86C2GoPrologue path, which
; serves CC-flipped strict leaves (FrameSize==0 => empty masks), so the X86
; aggregate mask was EMPTY for every function that mattered: copystack never
; adjusted pointer fields inside stack aggregates (vsnprintf's stack FILE
; sink kept writing through the stale pre-move f->buf/f->cookie after the
; first mid-chain stack growth). Additionally, the mask scan's RSP-only
; FrameReg filter dropped every RBP-resolved object — on X86 that is every
; local once the function has a frame pointer (RBP == SP + FrameSize).
;
; Frame here ($24): outgoing arg word at SP+0, %struct.S at SP+8..23, so the
; pointer field is word 1 of a 3-word locals bitmap. Entry 0 stays EMPTY
; (morestack-at-entry safety); the stackmap site's entry 1 carries the
; aggregate-field OR (bit 1 -> $0x02).

; RUN: llc < %s --output-asm-variant=2 -mtriple=x86_64-unknown-linux-gnu \
; RUN:   | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

%struct.S = type { ptr, i64 }

declare void @ext(ptr)
declare void @llvm.experimental.stackmap(i64, i32, ...)

; CHECK-LABEL: TEXT ·agg_fallback(SB), $24-8
; CHECK:      PCDATA $1, $1
; CHECK:      CALL ·ext(SB)
; CHECK:      FUNCDATA $1, gclocals·[[LOC:[0-9a-f]+]](SB)
; CHECK:      DATA gclocals·[[LOC]]+0(SB)/4, $2
; CHECK-NEXT: DATA gclocals·[[LOC]]+4(SB)/4, $3
; CHECK-NEXT: DATA gclocals·[[LOC]]+8(SB)/1, $0x00
; CHECK-NEXT: DATA gclocals·[[LOC]]+9(SB)/1, $0x02
define void @agg_fallback(ptr %a) #0 {
entry:
  %s = alloca %struct.S, align 8
  store ptr %a, ptr %s
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 1, i32 0)
  call void @ext(ptr %s)
  ret void
}

attributes #0 = { "c2go-argsize"="8" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
