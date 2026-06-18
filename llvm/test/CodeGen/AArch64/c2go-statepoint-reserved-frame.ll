; After RewriteStatepointsForGC, the gc.statepoint intrinsic is vararg, but it
; must not trip c2go's AAPCS-variadic non-reserved-call-frame carve-out (the
; AArch64 pre-scan skips intrinsics). With a reserved call frame the
; statepoint's spilled GC pointers are recorded at frame-relative offsets
; (SPAdj==0), so the Go locals bitmap actually marks them - guards against the
; empty-bitmap (missed GC root / use-after-free) bug.
;
; RUN: opt < %s -passes='c2go-write-barriers,c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

%struct.N = type { ptr addrspace(1), i32 }
declare void @runtime_safepoint()

; No per-call SUB/ADD SP around the statepoint calls (reserved frame). Also
; lock the declared $framesize exactly: declared 48-0, with FrameAlignment=16
; and SavedLinkSize=8 on AArch64 the physical FrameSize is 48+16=64 and the
; locals-bitmap Nbit is (64-8)/8 = 7.
; CHECK: TEXT ·f(SB), $48-0
; CHECK-NOT: SUB{{.*}}SP
; The locals bitmap at the barrier statepoint must be non-empty (the live
; managed pointers are captured, not dropped past Nbit). Byte +8 is the
; reserved EMPTY entry-0 (used at the entry morestack), so the live-pointer
; bits land in a body entry at byte +9 (or later).
;
; Pin the exact Nbit value at the +4 slot of the gclocals header
; (DATA <sym>+4/4, $Nbit). The Nbit formula is (FrameSize - SavedLinkSize) /
; PtrSize with the SavedLinkSize fallback 8 on AArch64; without an exact pin a
; one-byte drift (e.g. SavedLinkSize wrongly defaulting to 0 or 16) could slip
; past the non-zero-byte CHECK below.
; CHECK: FUNCDATA $1, gclocals·
; CHECK: DATA gclocals·{{[0-9a-f]+}}+4(SB)/4, $7
; CHECK: DATA gclocals·{{[0-9a-f]+}}+9(SB)/1, $0x{{0*[1-9a-f][0-9a-f]*}}
define ptr addrspace(1) @f(ptr addrspace(1) %p, ptr addrspace(1) %q) {
entry:
  %next = getelementptr inbounds %struct.N, ptr addrspace(1) %p, i32 0, i32 0
  store ptr addrspace(1) %q, ptr addrspace(1) %next, align 8
  call void @runtime_safepoint() [ "deopt"() ]
  ret ptr addrspace(1) %p
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
