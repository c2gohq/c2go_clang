; X86 framed-function statepoint anchor rebase.
;
; Locks the bitmap-offset-from-anchor contract in X86MCInstLower: the Go
; locals bitmap is anchored at the REAL post-prologue hardware SP, which
; sits 8 bytes BELOW LLVM's own SP view on any framed TEXT because Go's
; obj6 injects a saved-BP word on top of the declared $framesize
; (ADJSP $(framesize+8) + MOVQ BP, framesize(SP) + LEAQ).
;
; Two body shapes, distinguished by X86FrameLowering::hasFP(MF):
;
;   * @framed_fp ("frame-pointer"="all" - the production darwin shape):
;     locals are RBP-addressed, slots pinned to BP = entrySP-8, so a
;     PEI-resolved Direct(RSP, k) statepoint location needs k+8 and the
;     getFrameIndexReference answer used by the alloca-field matcher is
;     RBP-relative (needs +FrameSize). Pre-fix the matcher demanded
;     FrameReg == RSP verbatim -> never matched on hasFP frames ->
;     all-zero FUNCDATA $1 -> copystack never relocated frame-resident
;     pointers.
;
;   * @framed_nofp (no attr, hasFP=false): locals are SP-addressed and
;     track the real hardware SP, so offsets pass through verbatim (the
;     pre-fix behavior, still correct for this shape).
;
; Layout check for @framed_fp (verified against the emitted body):
;   TEXT $40 -> Nbit = 40/8 = 5; obj6 ADJSP 48 -> realSP = S-48, BP = S-8.
;   %sl = struct.S at -24(BP)..0(BP):
;     p0 at -24(BP) = S-32 = realSP+16 -> bit 2
;     p2 at  -8(BP) = S-16 = realSP+32 -> bit 4
;   -> bitmap byte 0b10100 = $0x14.
; And for @framed_nofp:
;   TEXT $24 -> Nbit = 3; body stores at 0(SP)/16(SP) track realSP:
;     p0 -> bit 0, p2 -> bit 2 -> bitmap byte $0x05.
;
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   -mtriple=x86_64-unknown-linux-gnu \
; RUN:   | llc --output-asm-variant=2 -mtriple=x86_64-unknown-linux-gnu 2>&1 \
; RUN:   | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

%struct.S = type { ptr addrspace(1), i64, ptr addrspace(1) }

@sink = global i64 0

declare void @safepoint()

; CHECK-LABEL: TEXT ·framed_fp(SB), $40-16
; Body anchors (sanity - the bit math above depends on these):
; CHECK:       MOVQ DI, -24(BP)
; CHECK:       MOVQ SI, -8(BP)
; The per-PC map must be in effect AT the call:
; CHECK:       PCDATA $1, $1
; CHECK-NEXT:  CALL ·safepoint(SB)
define internal void @framed_fp(ptr addrspace(1) %a, ptr addrspace(1) %b) #0 gc "c2go-gc" {
  %sl = alloca %struct.S, align 8
  %p0 = getelementptr %struct.S, ptr %sl, i32 0, i32 0
  store ptr addrspace(1) %a, ptr %p0, align 8
  %p2 = getelementptr %struct.S, ptr %sl, i32 0, i32 2
  store ptr addrspace(1) %b, ptr %p2, align 8
  call void @safepoint() ["deopt"()]
  %la = load ptr addrspace(1), ptr %p0, align 8
  %v = ptrtoint ptr addrspace(1) %la to i64
  store volatile i64 %v, ptr @sink, align 8
  ret void
}

; FUNCDATA $1 for @framed_fp: n=2, nbit=5, entry map empty, call-site map
; bits {2,4} = $0x14 (not the pre-fix all-zero, not the unrebased $0x0a).
; CHECK:       FUNCDATA $1, gclocals·[[FP:[0-9a-f]+]](SB)
; CHECK-NEXT:  DATA gclocals·[[FP]]+0(SB)/4, $2
; CHECK-NEXT:  DATA gclocals·[[FP]]+4(SB)/4, $5
; CHECK-NEXT:  DATA gclocals·[[FP]]+8(SB)/1, $0x00
; CHECK-NEXT:  DATA gclocals·[[FP]]+9(SB)/1, $0x14

; CHECK-LABEL: TEXT ·framed_nofp(SB), $24-16
; CHECK:       MOVQ DI, 0(SP)
; CHECK:       MOVQ SI, 16(SP)
; CHECK:       PCDATA $1, $1
; CHECK-NEXT:  CALL ·safepoint(SB)
define internal void @framed_nofp(ptr addrspace(1) %a, ptr addrspace(1) %b) #1 gc "c2go-gc" {
  %sl = alloca %struct.S, align 8
  %p0 = getelementptr %struct.S, ptr %sl, i32 0, i32 0
  store ptr addrspace(1) %a, ptr %p0, align 8
  %p2 = getelementptr %struct.S, ptr %sl, i32 0, i32 2
  store ptr addrspace(1) %b, ptr %p2, align 8
  call void @safepoint() ["deopt"()]
  %la = load ptr addrspace(1), ptr %p0, align 8
  %v = ptrtoint ptr addrspace(1) %la to i64
  store volatile i64 %v, ptr @sink, align 8
  ret void
}

; FUNCDATA $1 for @framed_nofp: n=2, nbit=3, call-site map bits {0,2} = $0x05
; (verbatim SP offsets - no rebase for the SP-anchored body shape).
; CHECK:       FUNCDATA $1, gclocals·[[NOFP:[0-9a-f]+]](SB)
; CHECK-NEXT:  DATA gclocals·[[NOFP]]+0(SB)/4, $2
; CHECK-NEXT:  DATA gclocals·[[NOFP]]+4(SB)/4, $3
; CHECK-NEXT:  DATA gclocals·[[NOFP]]+8(SB)/1, $0x00
; CHECK-NEXT:  DATA gclocals·[[NOFP]]+9(SB)/1, $0x05

attributes #0 = { "c2go-argsize"="16" "frame-pointer"="all" noinline optnone }
attributes #1 = { "c2go-argsize"="16" noinline optnone }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
