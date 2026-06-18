; X86 mirror of the AArch64 stkobj statepoint LIT. A managed-record alloca
; (an LLVM struct type whose typeinfo path already emitted
; c2go.gcbitmap.<RecName>) that becomes a Direct(SP, off) gc-live operand
; of a statepoint must drive a FUNCDATA $2, gcstkobj·<hash> stack-objects
; table, with the corresponding rodata body (N x 16-byte entries:
; frameOffset / size / ptrBytes / SymPtrOff-to-gcdata).
;
; Mirror notes (vs the AArch64 source LIT):
;   * X86 amd64 has SavedLinkSize=0 - the CALL-pushed return PC sits ABOVE
;     $framesize, not inside it. The frameOffset formula collapses to
;     SpOff - FrameSize (no -8 LR bump). With SpOff=alloca offset and
;     FrameSize=funcspdelta the locals branch still yields a negative.
;   * Target triple = x86_64-unknown-none-goabi (Plan-9 streamer + X86
;     c2go module-flag path).
;   * The llc gate flag is -x86-c2go-funcdata2 (mirror of AArch64's
;     -c2go-funcdata2).
;   * Plan-9 emit needs --output-asm-variant=2 to pick X86Plan9InstPrinter.
;
; Field-stability vs the AArch64 source:
;   * Hash suffix gcstkobj·<hex> is content-hashed by the streamer -
;     pinned with a wildcard.
;   * size, ptrBytes, gcdata sym name are byte-identical to AArch64 (same
;     struct layout, same bitmap symbol).
;   * frameOffset numeric value differs from AArch64 by exactly the -8 LR
;     bump (AArch64 emits -16 for SpOff=16/FrameSize=32; X86 emits -16 too
;     for SpOff=0/FrameSize=16 - the alloca is the lone local and rounds
;     to 16-aligned, with frameOffset = 0 - 16 = -16). The observed value
;     is guarded below.
;
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   -mtriple=x86_64-unknown-none-goabi \
; RUN:   | llc --output-asm-variant=2 -mtriple=x86_64-unknown-none-goabi \
; RUN:        -x86-c2go-funcdata2 \
; RUN:   | FileCheck %s

target triple = "x86_64-unknown-none-goabi"

; A c2go_struct-shaped record: one managed-pointer field + one scalar.
; LLVM struct name struct.S -> bitmap key S after the struct. prefix is
; stripped (mirror of the AArch64 stkobj-entry decode).
%struct.S = type { ptr addrspace(1), i64 }

; ELF C-owner case: linkonce_odr / hidden / unnamed_addr.
@c2go.gcbitmap.S = linkonce_odr hidden unnamed_addr constant [1 x i8] c"\01"

declare void @runtime_safepoint()

; CHECK-LABEL: TEXT {{[^[:space:]]+}}c2go_stkobj_demo_x86(SB)
; CHECK:       FUNCDATA $2, gcstkobj·{{[0-9a-f]+}}(SB)
; CHECK:       DATA gcstkobj·{{[0-9a-f]+}}+0(SB)/8, $1
; CHECK:       DATA gcstkobj·{{[0-9a-f]+}}+12(SB)/4, $16
; CHECK:       DATA gcstkobj·{{[0-9a-f]+}}+16(SB)/4, $8
; CHECK:       DATA gcstkobj·{{[0-9a-f]+}}+20(SB)/4, $c2go.gcbitmap.S(SB)
; CHECK:       GLOBL gcstkobj·{{[0-9a-f]+}}(SB), DUPOK|RODATA, $24
define internal c2goabiinternalcc void @c2go_stkobj_demo_x86() #0 gc "c2go-gc" {
entry:
  ; Named alloca of the managed record. Under the c2go-gc strategy
  ; findBaseDefiningValue treats the alloca as a base-defining value, so
  ; its address enters the statepoint's gc-live set -> Direct(SP, off)
  ; location at lowering, where the stkobj harvest picks it up.
  %local = alloca %struct.S, align 8
  ; The non-intrinsic call is the safepoint RS4GC wraps.
  call void @runtime_safepoint() [ "deopt"() ]
  ; A real use of the alloca AFTER the call keeps `%local` live across
  ; it (so RS4GC actually puts it in the gc-live bundle).
  %p = getelementptr inbounds %struct.S, ptr %local, i32 0, i32 0
  store ptr addrspace(1) null, ptr %p, align 8
  ret void
}

; c2goabiinternalcc trigger attributes - the X86 stager keys off these.
;   * c2go-reg-return is a leaf-ABI eligibility marker (no behavioral side
;     effect when the function returns void).
;   * c2go-argsize="0" keeps the publish-time loud-fail in
;     X86AsmPrinter::emitFunctionEntryLabel happy (zero-arg callee).
attributes #0 = { "c2go-reg-return" "c2go-argsize"="0" }

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
