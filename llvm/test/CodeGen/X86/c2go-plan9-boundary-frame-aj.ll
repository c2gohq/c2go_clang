; Plan-9 .s emit byte-correctness for X86 boundary functions: the
; WORD->LONG raw-byte fallback, the boundary framesize gate, and the
; framesize double-count.
;
; Three regressions are pinned here:
;
;   WORD->LONG: the raw-byte fallback in MCPlan9AsmStreamer used to emit
;   WORD $0xXXXXXXXX for every 4-byte chunk of un-translatable
;   instructions. AArch64 Go asm treats AWORD as 4 bytes, but X86 Go asm
;   treats AWORD as 2 bytes - so the X86 path silently truncated every
;   raw instruction to its lower 2 bytes. The fix dispatches on triple:
;   AArch64 keeps WORD, X86 uses LONG (4 bytes).
;
;   Boundary framesize gate: boundary functions (c2go-boundary attr;
;   c2go_extern at the C source) used to fall through the X86 frame-meta
;   stager gate (which only accepted c2go-argsize). The Plan-9 streamer
;   then published the manifest-side framesize=0 / argsize=N, but the
;   LLVM SysV body still contained MOVQ <arg>, N(%rsp) outgoing-args
;   writes that assume the SP delta is in place (the matching FrameSetup
;   MIs are suppressed in Plan-9 mode). With $framesize=0 obj6.go inserts
;   no frame either, and the outgoing-arg writes land on the caller's
;   retPC slot -> "unexpected return pc" crash. The fix extends the
;   stager gate to accept c2go-boundary and forwards c2go-boundary-argsize.
;
;   Framesize double-count: an earlier draft of the SysV-fallback
;   FrameSize computation read getStackSize() + getMaxCallFrameSize().
;   The stager runs after PrologEpilogInserter has already folded
;   MaxCallFrameSize into the StackSize and alignTo'd the sum, so adding
;   it again double-counted and broke the 16-byte alignment invariant
;   (framesize mod 16 == 4); obj6.go's alignment check rejected the
;   frame, and even when it slipped through the GC stackmap was off by 4
;   bytes from the actual SP delta - surfacing later as a nil-deref in a
;   write barrier / type-pointers walk. Fix: read getStackSize() alone.
;
; RUN: llc %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -output-asm-variant=2 -o - 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

declare void @sink7(i64, i64, i64, i64, i64, i64, i64)

; A boundary function with 7 i64 args (one more than the SysV register
; file holds -> MUST spill via outgoing-args stack reservation) AND a
; local alloca (forces locals into MFI.getStackSize()) AND a real call
; site (so MaxCallFrameSize > 0 too). Together these force PEI to:
;   * fold MaxCallFrameSize into StackSize, then
;   * alignTo(16) the sum.
; After PEI runs, `MFI.getStackSize()` is already that 16-aligned final
; autosize. The stager must:
;   (a) NOT bail at the `c2go-argsize` gate - it should accept
;       `c2go-boundary` instead.
;   (b) Publish FrameSize = MFI.getStackSize() (NOT plus
;       getMaxCallFrameSize again) so the resulting framesize is a
;       multiple of 16 (the 16B X86 SysV stack alignment invariant).
;   (c) Forward ArgSize from `c2go-boundary-argsize` (8 here).
;
; Positive lock: the TEXT directive must carry the literal $24-8
; framesize. The X86 SysV call-time ABI requires 16B RSP alignment at
; call sites; entry RSP = caller_RSP - 8 (the retPC slot), so the
; autosize in `TEXT name(SB), $framesize-N` must satisfy
; (8 + framesize) mod 16 == 0, i.e. framesize mod 16 == 8. The buggy
; draft computed getStackSize() + getMaxCallFrameSize(), producing
; values satisfying mod 16 == 4 (the double-count signature). Here
; getStackSize() (locals + CSR + folded MaxCallFrameSize + alignTo) is
; 24, so we expect exactly $24-8. Locking the literal value rather than
; a regex makes any regression that bumps the computation (re-adds
; MaxCallFrameSize, or forgets the alignTo) fail loudly.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}aj_boundary_7args(SB), $24-8
;
; Negative locks: must NOT be the $0-N framing (boundary gate
; fall-through), must NOT have raw `WORD $0x...` four-byte chunks
; (WORD->LONG fix), and must NOT carry raw SUBQ $.., %rsp / PUSHQ %rbp /
; POPQ %rbp in the Plan-9 body - the Plan-9 streamer suppresses those
; FrameSetup MIs and obj6.go re-injects them from the $framesize
; directive instead.
; CHECK-NOT: NOFRAME, $0-8
; CHECK-NOT: WORD $0x{{[0-9a-f]+}}
; CHECK-NOT: SUBQ ${{[0-9]+}}, %rsp
; CHECK-NOT: PUSHQ %rbp
; CHECK-NOT: POPQ %rbp
define void @aj_boundary_7args(i64 %a, i64 %b, i64 %c, i64 %d,
                                i64 %e, i64 %f, i64 %g) #0 {
  %slot = alloca i64
  store i64 %a, ptr %slot
  call void @sink7(i64 %a, i64 %b, i64 %c, i64 %d,
                   i64 %e, i64 %f, i64 %g)
  ret void
}

attributes #0 = { noinline nounwind "c2go-boundary" "c2go-boundary-argsize"="8" "c2go-argptrmask"="00" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
