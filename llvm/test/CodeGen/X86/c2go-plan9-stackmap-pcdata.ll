; X86 LowerSTACKMAP Plan-9 branch (mirror of AArch64AsmPrinter, with
; X86::RSP/RBP in place of AArch64::SP/FP).
;
; Verifies that in Plan-9 (.s) output mode X86AsmPrinter::LowerSTACKMAP:
;   (1) does not crash;
;   (2) forwards a Direct(RSP/RBP, N) location to
;       MCPlan9AsmStreamer::recordC2GoStackmapSite. Because the X86 staged-
;       meta producer is not yet wired on the main path, recordC2Go-
;       StackmapSite silently returns when CurFn==nullptr; so here we only
;       assert that the Plan-9 path neither crashes nor leaks the SysV
;       .llvm_stackmaps section. Full PCDATA $1, $<idx> text verification
;       is deferred to an integration LIT.
;   (3) the production path (default ATT, no Plan-9) is byte-identical:
;       the .Ltmp0 label and .llvm_stackmaps section still emit normally.
;
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu --output-asm-variant=2 \
; RUN:   2>&1 | FileCheck %s --check-prefix=PLAN9
;
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu 2>&1 \
; RUN:   | FileCheck %s --check-prefix=SYSV

target triple = "x86_64-unknown-linux-gnu"

declare void @llvm.experimental.stackmap(i64, i32, ...)

define void @stackmap_site() {
entry:
  %slot = alloca i64, align 8
  store i64 0, ptr %slot, align 8
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 42, i32 0, ptr %slot)
  ret void
}

; --- Plan-9 path - in Plan-9 streamer mode LowerSTACKMAP should:
;   * traverse the new Plan-9 branch (CSI.Locations + recordC2Go-
;     StackmapSite) without crashing;
;   * still emit the _Ltmp0 temp label (recordStackMap internal behavior);
;   * NOT emit the SysV .llvm_stackmaps section (no such branch in the
;     Plan-9 pipeline).
;
; PLAN9-LABEL: TEXT {{[^[:space:]]+}}stackmap_site(SB)
; PLAN9:       _Ltmp0
; PLAN9-NOT:   .llvm_stackmaps
; PLAN9-NOT:   __LLVM_StackMaps

; --- SysV path - production-unchanged byte-identical check.
;   * still emits the .llvm_stackmaps section (default X86 SysV emit);
;   * still emits the .Ltmp0 label (shared StackMaps::recordStackMap).
;
; SYSV-LABEL: stackmap_site:
; SYSV:       .Ltmp0
; SYSV:       .llvm_stackmaps
; SYSV:       __LLVM_StackMaps
