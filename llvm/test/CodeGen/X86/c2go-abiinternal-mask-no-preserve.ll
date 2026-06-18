; The call-preserved mask of a c2goabiinternalcc callee must be
; CSR_64_NoneRegs (= {RBP}), not the default SysV csr_64.
;
; c2goabiinternalcc uses RBX as an integer arg-reg. If a GoABI0 caller in
; c2go-mode calls a c2goabiinternalcc callee with the default SysV mask,
; RegAlloc believes RBX is preserved across the call while the callee
; overwrites it as an arg-reg - a silent miscompile. The mask flip must
; therefore also cover the C2GoABIInternal CC, not just GoABI0.
;
; The flip target is CSR_64_NoneRegs (the preserve_none set, {RBP}), not
; CSR_NoRegs. CSR_NoRegs marks RBP call-clobbered too, which sets
; FPClobberedByCall on every c2go call and makes PEI's spillFPBP /
; checkInterferedAccess loud-fail ("Interference usage of base
; pointer/frame pointer.") on production amd64. Under the Go amd64 ABI BP
; is callee-saved, so CSR_64_NoneRegs is the truthful set; RBX/R12-R15
; stay call-clobbered.
;
; -stop-after=greedy emits MIR and pins the csr_64_noneregs marker on
; CALL64pcrel32 directly, decoupled from reservedRegs / SaveList. The
; CALL's `implicit $rbx` is the correct expression of RBX as an arg-reg
; (the call site COPYs the actual arg into $rbx) and is unrelated to the
; mask flip; only csr_64_noneregs is pinned here, not implicit-use rbx.

target triple = "x86_64-unknown-linux-gnu"

@p1 = external global ptr
@p2 = external global ptr

declare c2goabiinternalcc i64 @leaf_internal_consumer(i64 %a, i64 %b)

; MIR shape: CALL64pcrel32 carries a csr_64_noneregs marker.
;   - A regression to the default SysV mask (csr_64, with RBX preserved)
;     is caught by the {{csr_64,}} NOT-pattern below; its trailing comma
;     avoids false-matching the csr_64_noneregs prefix.
;   - A regression to CSR_NoRegs is caught by the csr_noregs NOT-pattern
;     below (and RBP would again be call-clobbered -> spillFPBP).
;
; MIR-LABEL: name: caller_via_internal
; MIR: CALL64pcrel32 target-flags(x86-plt) @leaf_internal_consumer
; MIR-SAME: csr_64_noneregs
; MIR-NOT: {{csr_64,}}
; MIR-NOT: csr_noregs
; MIR: RET

define goabi0cc i64 @caller_via_internal() {
  %v1 = load i64, ptr @p1, align 8
  %v2 = load i64, ptr @p2, align 8
  %r1 = call c2goabiinternalcc i64 @leaf_internal_consumer(i64 %v1, i64 %v2)
  %t = add i64 %r1, %v1
  %u = add i64 %t, %v2
  ret i64 %u
}

; RUN: llc -mtriple=x86_64-unknown-linux-gnu -O2 -stop-after=greedy < %s \
; RUN:   | FileCheck %s --check-prefix=MIR

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
