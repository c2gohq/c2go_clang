; A GoABI0 call must not mark RBP call-clobbered.
;
; Failure chain if it does:
;   CSR_NoRegs mask (clobbers everything, RBP included)
;     -> setFPClobberedByCall(true)
;     -> PEI spillFPBP (an X86-only pass; AArch64 has no such mechanism,
;        so its empty-set CSR_AArch64_NoRegs mask is harmless - the
;        mechanical mirror breaks on X86)
;     -> checkInterferedAccess finds a frame-index access inside the
;        call-sequence range (at -O0 FastRA inserts the cross-call reload
;        between CALL and ADJCALLSTACKUP)
;     -> "error: Interference usage of base pointer/frame pointer."
;
; Production shape: hasFP=true (frame-pointer=all) + a typeinfo value
; live across the call + a GoABI0 stack-passing call, with
; hasBasePointer(MF)=false (all fixed-size 8-aligned slots, no
; VLA/realign/preallocated). So the failure is not confined to a
; hasBasePointer=true corner.
;
; Fix: CSR_NoRegs -> CSR_64_NoneRegs (= {RBP}). Under the Go amd64 ABI
; BP is callee-saved (obj6.go injects PUSHQ BP; MOVQ SP, BP for frames
; with framesize>0 / a call; NOFRAME/$0 leaves leave BP alone; the
; LLVM-compiled c2go body is reserved-pinned off RBP).
;
; RUN 1 (-O0, production FastRA shape): before the fix this run printed
; "Interference usage" to stderr and llc exited non-zero; after the fix
; it compiles cleanly.
;
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -O0 < %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ASM
;
; ASM-NOT: Interference usage
; ASM-LABEL: two_mallocs_fp:
; ASM: callq{{.*}}GCMalloc
; ASM: callq{{.*}}GCMalloc
; ASM: retq
; ASM-NOT: Interference usage
;
; RUN 2 (MIR): the GoABI0 callee's call-preserved mask is csr_64_noneregs
; (RBP preserved; RBX/R12-R15 still clobbered). A regression to
; csr_noregs (RBP clobbered -> spillFPBP) or a fall-through to csr_64
; both fail it.
;
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -O2 -stop-after=greedy < %s \
; RUN:   | FileCheck %s --check-prefix=MIR
;
; MIR-LABEL: name: two_mallocs_fp
; MIR: CALL64pcrel32 {{.*}}@GCMalloc
; MIR-SAME: csr_64_noneregs
; MIR-NOT: {{csr_64,}}
; MIR-NOT: csr_noregs

target triple = "x86_64-unknown-linux-gnu"

@ti = external global i8
@g1 = external global ptr
@g2 = external global ptr

declare goabi0cc ptr @GCMalloc(ptr, i64)

define goabi0cc void @two_mallocs_fp() #0 {
entry:
  %p1 = call goabi0cc ptr @GCMalloc(ptr @ti, i64 56)
  store ptr %p1, ptr @g1, align 8
  %p2 = call goabi0cc ptr @GCMalloc(ptr @ti, i64 56)
  store ptr %p2, ptr @g2, align 8
  ret void
}

attributes #0 = { "frame-pointer"="all" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
