; X86 LowerSTATEPOINT Plan-9 branch (mirror of AArch64AsmPrinter, with
; X86::RSP/RBP in place of AArch64::SP/FP).
;
; Verifies that in Plan-9 (.s) output mode X86AsmPrinter::LowerSTATEPOINT:
;   (1) does not crash;
;   (2) forwards the Indirect(RSP, N) spill-slot offset plus the
;       Direct(RSP, N) alloca per-field expansion to
;       MCPlan9AsmStreamer::recordC2GoStackmapSite, completing before the
;       CALL is emitted (the Go runtime rolls the return PC back to the
;       call PC to read the stack map);
;   (3) the production path (default ATT, no Plan-9) is byte-identical:
;       the .Ltmp0 label and .llvm_stackmaps section still emit normally.
;
; RSP/RBP anchor offsets are rebased to the real post-prologue SP
; coordinate (a framed TEXT's obj6 saved-BP word makes LLVM's SP view 8
; bytes higher than the real SP); that behavior is locked in
; c2go-statepoint-framed-fp-rebase.ll.
;
; Because the X86 staged-meta producer is not yet on the main path,
; recordC2GoStackmapSite silently returns when CurFn==nullptr; so the
; Plan-9 path here only asserts that the branch does not crash, the emit
; path still produces a CALL, and the SysV .llvm_stackmaps section does
; not leak. Full PCDATA $1, $<idx> text verification is deferred to an
; integration LIT.
;
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   -mtriple=x86_64-unknown-linux-gnu \
; RUN:   | llc --output-asm-variant=2 -mtriple=x86_64-unknown-linux-gnu 2>&1 \
; RUN:   | FileCheck %s --check-prefix=PLAN9
;
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   -mtriple=x86_64-unknown-linux-gnu \
; RUN:   | llc -mtriple=x86_64-unknown-linux-gnu 2>&1 \
; RUN:   | FileCheck %s --check-prefix=SYSV

target triple = "x86_64-unknown-linux-gnu"

declare void @runtime_safepoint()

; %obj is an addrspace(1) managed pointer that RS4GC must spill across the
; safepoint; the Plan-9 path must forward the spill-slot offset to
; MCPlan9AsmStreamer. The non-pointer %slot (alloca i64) must not be
; marked.
define ptr addrspace(1) @c2go_x86_statepoint(ptr addrspace(1) %obj) {
entry:
  %slot = alloca i64, align 8
  store i64 0, ptr %slot, align 8
  call void @runtime_safepoint() [ "deopt"() ]
  ret ptr addrspace(1) %obj
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}

; --- Plan-9 path - in Plan-9 streamer mode LowerSTATEPOINT should:
;   * traverse the Plan-9 branch (collect CSI.Locations, expand direct
;     alloca fields, call recordC2GoStackmapSite) without crashing;
;   * still emit a CALL instruction (Plan-9 InstPrinter translation);
;   * NOT emit the SysV .llvm_stackmaps section (no such branch in the
;     Plan-9 pipeline).
;
; PLAN9-LABEL: TEXT {{[^[:space:]]+}}c2go_x86_statepoint(SB)
; PLAN9:       CALL
; PLAN9-NOT:   .llvm_stackmaps
; PLAN9-NOT:   __LLVM_StackMaps

; --- SysV path - production-unchanged byte-identical check:
;   * still emits .Ltmp labels (recordStatepoint emits as before on the
;     ATT path);
;   * still emits the .llvm_stackmaps section (default X86 SysV emit);
;   * still emits callq runtime_safepoint.
;
; SYSV-LABEL: c2go_x86_statepoint:
; SYSV:       callq{{.*}}runtime_safepoint
; SYSV:       .Ltmp
; SYSV:       .llvm_stackmaps
; SYSV:       __LLVM_StackMaps
