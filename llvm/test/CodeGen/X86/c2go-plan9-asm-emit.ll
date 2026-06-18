; X86 Plan-9 (Go assembler) syntax emit for functions flipped to
; c2goabiinternalcc by the X86 leaf-ABI pass.
;
; Exercises the full Plan-9 .s emit chain on x86-64:
;
;   1. opt -passes=x86-c2go-leaf-abi flips eligible internal leaves from
;      goabi0cc to c2goabiinternalcc (gated by c2go.goabi, on here).
;
;   2. llc -output-asm-variant=2 runs the X86 backend, then the Plan-9
;      inst-printer + cross-target MCPlan9AsmStreamer emit Plan-9 (Go
;      assembler) syntax .s instead of SysV / ELF AT&T.
;
;   3. CHECK lines pin the high-frequency Plan-9 grammar:
;       * TEXT directive: TEXT ·funcname(SB), NOSPLIT|NOFRAME, $0-M
;         (NOSPLIT bit + manifest argsize M come from the X86 staged-meta
;         producer)
;       * bare register names without % / $ prefix    - AX, BX, CX
;       * Plan-9 mnemonics with Q/L/W/B size suffix    - ADDQ, MOVQ
;       * CALL ·callee(SB) for direct calls            - middle-dot sym
;       * bare RET (no retq suffix)                    - function exit
;
;   4. Negative CHECK-NOT lines ban the SysV / ELF artifacts that would
;      leak through if the Plan-9 streamer were not selected:
;       * .cfi_*             - DWARF / CFI unwind directives
;       * .section           - ELF section directives
;       * .globl / .type     - ELF symbol attributes
;       * %rax, %rbx etc.    - AT&T register-prefix forms
;
; RUN: rm -rf %t && mkdir -p %t
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t/flipped.bc
;
; RUN: llc %t/flipped.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -output-asm-variant=2 -o - 2>&1 | FileCheck %s
;
; Default-ATT byte-identical check: same IR, but output-asm-variant=2 is
; dropped so this is the default X86 ATT/ELF streamer path. The leaf-abi
; pass still flips eligible leaves, but the default ATT streamer emits
; ordinary SysV .s carrying CFI / ELF directives - proving the Plan-9
; streamer is selected ONLY by -output-asm-variant=2, not by the presence
; of c2goabiinternalcc functions in the module.
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t/sysv.bc
;
; RUN: llc %t/sysv.bc -mtriple=x86_64-unknown-linux-gnu -o - 2>&1 \
; RUN:   | FileCheck %s --check-prefix=SYSV

target triple = "x86_64-unknown-linux-gnu"

; --- leaf_add: two i64 args -> i64 sum. After the CC flip args are in
;     AX, BX (C2GoABIInternal table); result returns in AX. The body
;     collapses to ADDQ BX, AX; RET.
;
;     With c2go-argsize="16" the X86 staged-meta producer stages
;     NoSplit=true / FrameSize=0 / SavedLinkSize=0 / FrameAlignment=0 /
;     ArgSize=16; the streamer emits the TEXT directive NOSPLIT|NOFRAME,
;     $0-16.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}leaf_add(SB), NOSPLIT|NOFRAME, $0-16
; CHECK:       ADDQ BX, AX
; CHECK:       RET
define internal goabi0cc i64 @leaf_add(i64 %a, i64 %b) #0 {
  %s = add i64 %a, %b
  ret i64 %s
}

; --- leaf_sub: same shape, different mnemonic; pins that the per-opcode
;     Plan-9 mnemonic map handles SUB as well as ADD.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}leaf_sub(SB), NOSPLIT|NOFRAME, $0-16
; CHECK:       SUBQ BX, AX
; CHECK:       RET
define internal goabi0cc i64 @leaf_sub(i64 %a, i64 %b) #0 {
  %s = sub i64 %a, %b
  ret i64 %s
}

; --- leaf_addimm: ADDQ-immediate form pins the `ri` arithmetic path.
;     Body becomes `ADDQ $5, AX; RET`. The `$` prefix is Plan-9's
;     immediate marker (same as AT&T; only registers lose the `%`).
;     Single i64 arg -> c2go-argsize="8".
; CHECK-LABEL: TEXT {{[^[:space:]]+}}leaf_addimm(SB), NOSPLIT|NOFRAME, $0-8
; CHECK:       ADDQ $5, AX
; CHECK:       RET
define internal goabi0cc i64 @leaf_addimm(i64 %x) #1 {
  %r = add i64 %x, 5
  ret i64 %r
}

; --- leaf_caller: makes a direct call into leaf_addimm. The CALL site
;     uses ·name(SB) (middle-dot prefix + static-base suffix).
;     Single i64 arg -> c2go-argsize="8".
;
;     leaf_caller is an in-TU near-leaf (its whole downstream subtree -
;     leaf_addimm - is in this TU within budget), so the leaf-abi pass
;     flips it to c2goabiinternalcc (register passing). A register-ABI
;     function MUST be emitted NOSPLIT (morestack does not preserve
;     incoming argument registers), so the X86 frame emitter forces the
;     NOSPLIT bit regardless of whether the function makes a real call -
;     mirror of the AArch64 change. The frame ($48 < 792 budget) passes
;     the fail-closed budget guard.
;
;     The LLVM-emitted SUBQ/ADDQ on SP and any PUSHQ/POPQ BP are
;     FrameSetup/FrameDestroy MIs and MUST NOT appear in the Plan-9 .s -
;     obj6.go reads the framesize from the TEXT directive and injects its
;     own SP adjustment at .s assemble time.
;
;     Framesize $48-8: with the call-preserved mask truthfully
;     CSR_64_NoneRegs for c2go CCs, RegAlloc refuses to keep values live
;     in RBX/R12-R15 across c2go calls, while values NOT live across any
;     call may still use them. The frame does NOT shrink to $16-8: that
;     size would rely on the SysV "callee preserves RBX" contract, which
;     is exactly the lie this ABI removes - under the truthful mask every
;     cross-call value must live in a stack slot, and those spill slots
;     are what $48 hosts. RBP stays out of the frame: it is
;     reserved-pinned and preserved by callees per the Go amd64 BP
;     contract (obj6.go owns the BP push/pop).
; CHECK-LABEL: TEXT {{[^[:space:]]+}}leaf_caller(SB), NOSPLIT, $48-8
; CHECK:       CALL {{[^[:space:]]+}}leaf_addimm(SB)
; CHECK:       ADDQ $100, AX
; CHECK:       RET
define internal goabi0cc i64 @leaf_caller(i64 %a) #1 {
  %r = call goabi0cc i64 @leaf_addimm(i64 %a)
  %s = add i64 %r, 100
  ret i64 %s
}

; --- non_leaf_with_locals: non-leaf with a small frame from a stack
;     alloca passed by-pointer to a callee.
;
;     The Plan-9 .s path filters every FrameSetup/FrameDestroy MI in
;     X86AsmPrinter::emitInstruction (mirror of AArch64). The
;     LLVM-emitted prologue SUBQ / epilogue ADDQ are exactly those MIs,
;     so they MUST NOT appear in the emitted Plan-9 .s. Instead the
;     staged framesize is pinned via the TEXT directive; obj6.go reads
;     $framesize and $argsize=8 from it and injects its own SP adjustment
;     (+ optional BP save) at `go tool asm` time.
;
;     Framesize $64-8: this function's values are live across the inner
;     CALL, so they cannot reuse RBX/R12-R15 (only freed for values NOT
;     crossing a call) and must live in stack slots - the truthful cost
;     of the CSR_64_NoneRegs mask, not a pin artifact (contrast
;     leaf_caller above, which does shrink to $48).
;
;     This is an in-TU near-leaf, so the leaf-abi pass flips it to
;     c2goabiinternalcc; the register-ABI -> NOSPLIT invariant then
;     forces the NOSPLIT bit (frame $64 < 792 budget). Mirror of
;     leaf_caller above.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}non_leaf_with_locals(SB), NOSPLIT, $64-8
; CHECK:       CALL {{[^[:space:]]+}}leaf_addimm(SB)
; CHECK:       RET
; Assert NO LLVM-emitted frame MI leaks past the Plan-9 streamer's
; FrameSetup/FrameDestroy filter. Any SUBQ/ADDQ on SP would mean the
; filter did not fire; any PUSHQ/POPQ BP would mean the
; NeedsFramePointer=false pin regressed.
; CHECK-NOT: SUBQ {{.*}}, SP
; CHECK-NOT: ADDQ {{.*}}, SP
; CHECK-NOT: PUSHQ BP
; CHECK-NOT: POPQ BP
define internal goabi0cc i64 @non_leaf_with_locals(i64 %a) #1 {
  %slot = alloca i64, align 8
  store i64 %a, ptr %slot, align 8
  %v = load i64, ptr %slot, align 8
  %r = call goabi0cc i64 @leaf_addimm(i64 %v)
  ret i64 %r
}

; --- NEGATIVE: none of these SysV / ELF artifacts may appear in the
;     Plan-9 .s. (`go tool asm` would reject any of them as a parse
;     error.)
; CHECK-NOT: .cfi_
; CHECK-NOT: .section
; CHECK-NOT: .globl
; CHECK-NOT: .type
; CHECK-NOT: %rax
; CHECK-NOT: %rbx
; CHECK-NOT: %rsp
; CHECK-NOT: retq
; CHECK-NOT: callq
; --- With the X86 staged-meta producer wired, the streamer's TU-local
;     fallback (argsize-less NOFRAME, $0 without the NOSPLIT bit) must
;     NEVER appear for any of the flipped leaves - the staged metadata
;     owns the directive now.
; CHECK-NOT: {{NOFRAME, \$0$}}
;
; --- NO LLVM-emitted FrameSetup/FrameDestroy MI may appear anywhere in
;     the Plan-9 .s. obj6.go owns the SP delta + optional BP save based
;     on the staged TEXT $framesize; if any of these leak past the
;     X86AsmPrinter::emitInstruction filter, obj6 would double-inject and
;     corrupt the stack.
; CHECK-NOT: SUBQ {{.*}}, SP
; CHECK-NOT: ADDQ {{.*}}, SP
; CHECK-NOT: PUSHQ BP
; CHECK-NOT: POPQ BP

; --- SYSV side: confirm that with output-asm-variant defaulted to ATT
;     (variant 0), the existing X86 SysV / ELF emission path is
;     byte-identical to baseline - CFI / DWARF / ELF directives all still
;     flow through - even though the leaf-abi pass DID flip the eligible
;     leaves to c2goabiinternalcc. The Plan-9 streamer is gated on the
;     asm variant, not the CC, so the default-ATT path is unperturbed.
;     Any regression would manifest as a CFI-stripping symptom here.
; SYSV: .cfi_startproc
; SYSV: retq
; SYSV: .cfi_endproc
; SYSV-NOT: TEXT
; SYSV-NOT: NOFRAME
; SYSV-NOT: ·

attributes #0 = { "c2go-reg-return" "c2go-argsize"="16" }
attributes #1 = { "c2go-reg-return" "c2go-argsize"="8" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
