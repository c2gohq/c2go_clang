; c2go #654c: under the statepoint GC path, an aggregate whose ADDRESS escapes
; into memory must keep the static aggregate-field mask in FUNCDATA $1.
;
; The per-PC LowerSTATEPOINT expansion covers an aggregate only while its base
; SSA value is threaded through a gc-live set; once the address is stored out
; as a value (Lua: funcstate.ls = &lexstate), later reads arrive through
; pointer chains SSA liveness cannot see, the base drops out of every gc-live
; set, and copystack leaves the escaped interior pointer aimed at the dead
; pre-copy stack segment (probe5: lexstate.dyd). The backend must OR the
; captured aggregate's pointer-field bits into every body bitmap.
;
; %obj (a managed addrspace(1) pointer) is live across the safepoint so the
; site owns a REAL body bitmap (index >= 1) — in the full c2go-lto pipeline
; C2GoSafepoint's empty-operand stackmaps guarantee that; in this llc-only
; reduction the live relocate stands in for it. The CAPTURED %esc must
; contribute its pointer-field bit to that body bitmap even though %esc has no
; SSA use after the call (reachable only through %holder's field in memory).
;
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

%struct.S = type { ptr, i64 }

declare void @ext()

; Frame ($48): %obj's relocate spill sits at SP+16 (bit 2), %esc at SP+40 with
; its pointer field at offset 0 (bit 5), %holder's field at SP+24 (bit 3).
; Body bitmap = 0x24: the relocate spill AND the captured %esc's field. Bit 3
; stays clear — %holder never escapes and its base SSA value is dead, so its
; field can never be read again (nobody holds %holder's address).
;
; CHECK-LABEL: TEXT ·escaped_agg(SB)
; CHECK:      PCDATA $1, $1
; CHECK-NEXT: CALL ·ext(SB)
; CHECK:      FUNCDATA $1, gclocals·[[LOC:[0-9a-f]+]](SB)
; CHECK:      DATA gclocals·[[LOC]]+0(SB)/4, $2
; CHECK-NEXT: DATA gclocals·[[LOC]]+4(SB)/4, $7
; CHECK-NEXT: DATA gclocals·[[LOC]]+8(SB)/1, $0x00
; CHECK-NEXT: DATA gclocals·[[LOC]]+9(SB)/1, $0x24
define ptr addrspace(1) @escaped_agg(ptr addrspace(1) %obj, ptr %heap) gc "c2go-gc" {
entry:
  %esc = alloca %struct.S, align 8
  %holder = alloca %struct.S, align 8
  store ptr %heap, ptr %esc
  ; the ADDRESS of %esc escapes into %holder's pointer field
  store ptr %esc, ptr %holder
  call void @ext() [ "deopt"() ]
  ; %esc has no direct SSA use after the call: reachable only through memory
  ret ptr addrspace(1) %obj
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
