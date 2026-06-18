; X86 GC precision triplet (mirror of the AArch64 emitC2GoPrologue inline
; scan + the streamer's downstream OR/scrub / hex-decode of the
; c2go-argptrmask attr).
;
; Pinned behaviour (Plan-9 .s):
;
;   * FUNCDATA $0 (args pointer map): non-empty bytes come from the
;     c2go-argptrmask IR fn attr (hex-decoded into the staged
;     ArgPtrMaskBytes). The streamer replicates the bytes once per
;     locals-map entry. Test 1 sets the attr to "05" (bits 0 + 2 - two
;     pointer args, at word 0 and word 2) and confirms BOTH the entry-0
;     and entry-1 bytes come out as $0x05.
;
;   * FUNCDATA $1 (locals pointer map): the aggregate-field bits computed
;     over RSP frame-index allocas (pointer-field recursion) are OR'd
;     into every body locals bitmap (entry index >= 1). Test 2 wires a
;     c2go-gc statepoint + gc-live(%sl) so RS4GC keeps the alloca address
;     in the statepoint's gc-live set; LowerSTATEPOINT then expands the
;     Direct(SP, off) operand per-field and the streamer records the
;     per-PC bits plus the aggregate-mask contribution to entry 1.
;     We confirm:
;       - entry 0 (function-entry morestack PC) is the reserved empty
;         map ($0x00)
;       - entry 1 (post-statepoint PC) has the expected non-empty
;         word-1 + word-3 mask ($0x0a) - alloca at SP+8 with pointer
;         fields at offsets 0 and 16 -> words 1 and 3.
;
;   * The union-ambiguity mask is tested negatively (no
;     !c2go.union.ambig.words MD on any alloca -> vector stays empty, no
;     scrub byte emitted). Union-ambiguity X86 cases land in a separate
;     LIT (mirror of the AArch64 union-ambig tests).
;
; Build sequence: c2go-gc-setup + RewriteStatepointsForGC in opt, then
; llc emitting the Plan-9 assembly variant.

; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   -mtriple=x86_64-unknown-linux-gnu \
; RUN:   | llc --output-asm-variant=2 -mtriple=x86_64-unknown-linux-gnu \
; RUN:   | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

%struct.S = type { ptr addrspace(1), i64, ptr addrspace(1) }

declare void @safepoint()

; Two pointer-typed args at word 0 and word 2 - c2go-argptrmask = "05"
; (bits 0+2 set). The internal aggregate alloca holds two managed
; pointers (offsets 0 and 16 inside a 24-byte struct, FrameSize rounds
; up to 32) - words 1 and 3 in the locals region.
;
; CHECK-LABEL: TEXT {{[^[:space:]]+}}leaf_agg(SB)
; FUNCDATA $0 args mask: both entry-0 and entry-1 bytes carry the
; hex-decoded attr value 0x05 (replicated across all locals-map entries).
; CHECK:       FUNCDATA $0, gclocals·{{[0-9a-f]+}}(SB)
; CHECK-NEXT:  DATA gclocals·{{[0-9a-f]+}}+0(SB)/4, $2
; CHECK-NEXT:  DATA gclocals·{{[0-9a-f]+}}+4(SB)/4, $2
; CHECK-NEXT:  DATA gclocals·{{[0-9a-f]+}}+8(SB)/1, $0x05
; CHECK-NEXT:  DATA gclocals·{{[0-9a-f]+}}+9(SB)/1, $0x05
; FUNCDATA $1 locals mask: entry-0 is the reserved empty map; entry-1
; carries the aggregate-field bits OR'd with the per-PC
; stackmap-derived bits - both contribute the same word-1 + word-3
; pattern -> byte $0x0a.
; CHECK:       FUNCDATA $1, gclocals·{{[0-9a-f]+}}(SB)
; CHECK-NEXT:  DATA gclocals·{{[0-9a-f]+}}+0(SB)/4, $2
; CHECK-NEXT:  DATA gclocals·{{[0-9a-f]+}}+4(SB)/4, $4
; CHECK-NEXT:  DATA gclocals·{{[0-9a-f]+}}+8(SB)/1, $0x00
; CHECK-NEXT:  DATA gclocals·{{[0-9a-f]+}}+9(SB)/1, $0x0a
define internal c2goabiinternalcc ptr addrspace(1) @leaf_agg(ptr addrspace(1) %a, ptr addrspace(1) %b) #0 gc "c2go-gc" {
  %sl = alloca %struct.S, align 8
  %p0 = getelementptr %struct.S, ptr %sl, i32 0, i32 0
  store ptr addrspace(1) %a, ptr %p0, align 8
  %p2 = getelementptr %struct.S, ptr %sl, i32 0, i32 2
  store ptr addrspace(1) %b, ptr %p2, align 8
  call void @safepoint() ["deopt"()]
  %la = load ptr addrspace(1), ptr %p0, align 8
  ret ptr addrspace(1) %la
}

attributes #0 = { "c2go-reg-return" "c2go-argsize"="16" "c2go-argptrmask"="05" noinline optnone }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
