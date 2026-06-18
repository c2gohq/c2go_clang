; Lightweight stack-map path: a function whose every safepoint has an empty
; scalar live set but which owns an aggregate stack local with pointer fields
; must not degrade to a single all-zero FUNCDATA $1.
;
; If a site's raw (empty) bits are interned directly, it lands on the reserved
; EMPTY entry 0, and the flush-time aggregate-field OR only rewrites bitmaps at
; index >= 1, so the agg mask is dropped: copystack never relocates the
; aggregate's pointer fields and the program faults. The aggregate-field mask
; must be ORed into each site's bits before interning, forcing such sites onto
; a distinct entry >= 1. Entry 0 stays EMPTY (morestack-at-entry safety).
;
; Frame here: %s = alloca {ptr, i64} lands at SP+24, so the pointer field word
; is bit 3 of a 5-word locals bitmap (framesize $32, nbit=(40-8)/8).
;
; RUN: llc < %s --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

%struct.S = type { ptr, i64 }

declare void @ext(ptr)
declare void @llvm.experimental.stackmap(i64, i32, ...)

; CHECK-LABEL: TEXT ·agg_only(SB)
; The empty-live-set site must select a NON-zero bitmap (the agg mask),
; not the reserved EMPTY entry 0.
; CHECK:      PCDATA $1, $1
; CHECK:      CALL ·ext(SB)
; CHECK:      FUNCDATA $1, gclocals·[[LOC:[0-9a-f]+]](SB)
; Two bitmaps: entry 0 = EMPTY (morestack-at-entry), entry 1 = agg mask.
; CHECK:      DATA gclocals·[[LOC]]+0(SB)/4, $2
; CHECK-NEXT: DATA gclocals·[[LOC]]+4(SB)/4, $5
; CHECK-NEXT: DATA gclocals·[[LOC]]+8(SB)/1, $0x00
; CHECK-NEXT: DATA gclocals·[[LOC]]+9(SB)/1, $0x08
define void @agg_only(ptr %a) {
entry:
  %s = alloca %struct.S, align 8
  store ptr %a, ptr %s
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 1, i32 0)
  call void @ext(ptr %s)
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
