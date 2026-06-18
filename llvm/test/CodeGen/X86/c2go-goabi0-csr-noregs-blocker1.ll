; The call-preserved mask for a c2go-mode GoABI0 callee is
; CSR_64_NoneRegs.
;
; Two things are verified:
;   1. In c2go-mode the GoABI0 callPreservedMask is CSR_64_NoneRegs
;      (= {RBP}): RBX/R12-R15 are caller-clobbered across the call, so
;      RegAlloc cannot keep typeinfo-like values live in them across a
;      call. The flip target is CSR_64_NoneRegs rather than CSR_NoRegs;
;      the only difference is RBP - CSR_NoRegs marks RBP call-clobbered
;      too, tripping PEI's spillFPBP loud-fail, whereas under the Go
;      amd64 ABI BP is callee-saved. For this shape (typeinfo live across
;      GCMalloc) the two sets are equivalent for RBX/R12-R15.
;   2. RBP is RA-unallocatable inside a c2go GoABI0 body (reserved-pin;
;      the "callee preserves RBP" promise holds for the LLVM-compiled
;      body too), pinned by the %rbp NOT-pattern below. RBX/R12-R15 need
;      no reserved-pin: once the mask is truthful, RegAlloc itself will
;      not use them across c2go calls, and every value here is live
;      across a call.
;
; The SYSV run renames the c2go.goabi flag (sed -> c2go.off): the module
; is no longer in c2go-mode, the default CSR_64 mask applies, and RBX
; returns to callee-saved scratch (prologue pushq %rbx). This byte-pins
; that the c2go-mode gate does not leak into non-c2go modules.

target triple = "x86_64-unknown-linux-gnu"

@type1 = external global ptr
@type2 = external global ptr
@type3 = external global ptr

declare goabi0cc ptr @GCMalloc(ptr, i64)
declare goabi0cc void @sink(ptr)

; --- C2GO path: GoABI0 callee with the c2go.goabi flag - RBX/R12-R15
;     vanish from codegen; values live across the calls are spilled to /
;     reloaded from stack slots ((%rsp)).

; C2GO-LABEL: three_mallocs:
; C2GO-NOT: %rbx
; C2GO-NOT: %r12
; C2GO-NOT: %r13
; C2GO-NOT: %r14
; C2GO-NOT: %r15
; C2GO-NOT: %rbp
; C2GO: callq{{.*}}GCMalloc
; C2GO: callq{{.*}}GCMalloc
; C2GO: callq{{.*}}GCMalloc
; C2GO: retq

define goabi0cc void @three_mallocs() {
  %t1 = load ptr, ptr @type1, align 8
  %p1 = call goabi0cc ptr @GCMalloc(ptr %t1, i64 16)
  %t2 = load ptr, ptr @type2, align 8
  %p2 = call goabi0cc ptr @GCMalloc(ptr %t2, i64 32)
  %t3 = load ptr, ptr @type3, align 8
  %p3 = call goabi0cc ptr @GCMalloc(ptr %t3, i64 48)
  call goabi0cc void @sink(ptr %p1)
  call goabi0cc void @sink(ptr %p2)
  call goabi0cc void @sink(ptr %p3)
  ret void
}

; RUN: llc -mtriple=x86_64-unknown-linux-gnu -O2 < %s | FileCheck %s --check-prefix=C2GO

; --- SYSV path: same IR, the c2go.goabi flag renamed away by sed -> not
;     c2go-mode -> default CSR_64, RBX back to callee-saved (prologue
;     pushq %rbx + reuse across calls). If the c2go-mode gate leaked into
;     a non-c2go module, pushq %rbx would vanish and this run would fail.
;
; SYSV-LABEL: three_mallocs:
; SYSV: pushq %rbx
; SYSV: callq{{.*}}GCMalloc
; SYSV: popq %rbx
;
; RUN: sed 's/c2go.goabi/c2go.off/' %s | llc -mtriple=x86_64-unknown-linux-gnu -O2 \
; RUN:   | FileCheck %s --check-prefix=SYSV

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
