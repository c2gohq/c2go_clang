; X86 Plan-9 printer: a mem-ref symbolic displacement must emit
; sym[+N](SB) for both Mach-O / ELF local labels (string literals, CPI
; blocks, LBB/.L block labels) and global externs.
;
; Repro: emitting Plan-9 asm for any TU that returns or LEAs a
; `const char *` literal produced lines such as
;
;   LEAQ _L_str_0, DI   ; go tool asm: illegal addressing mode
;   MOVQ _L_str_2, AX   ; same: bare sym, no SB-relative suffix
;
; The pre-fix printPlan9MemRef had a separate isLocalLabelName branch that
; emitted only the sanitized identifier and returned before appending any
; +N / (SB) suffix. The AArch64 mirror does not branch on isLocalLabel -
; it always emits goSymToPlan9(Sym) + offset suffix + "(SB)". The fix
; brings X86 into cross-arch symmetry: local labels share the same
; sym[+N](SB) grammar (go tool asm accepts the form for any label in
; scope), so the branch sanitizes the spelling but keeps (SB).
;
; `-relocation-model=pic` pins the SelectionDAG into RIP-relative LEA64r /
; MOV64rm forms (without it the linux-gnu small-code-model path folds a
; global address into a 32-bit immediate MOV32ri, a separate uncovered
; opcode out of scope here). Darwin emits the same LEA64r / MOV64rm
; sequences by default; the MCInst shape into the InstPrinter is identical
; across triples.
;
; CHECK pins:
;   1. use_str0      - LEA from .str.0 (address-of): LEAQ _L_str_0(SB), DI.
;                      The (SB) is the critical anti-regression marker.
;   2. use_str2_load - MOV64rm value-load from .str.2:
;                      MOVQ _L_str_2(SB), AX. Pins that the same (SB)
;                      suffix applies to value-loads, not just LEAs.
;
; The negative pins ban the pre-fix buggy form: a bare _L_str_<n> (no
; (SB)) on a LEAQ / MOVQ line must not appear - that is the exact spelling
; the Go assembler rejected.
;
; RUN: rm -rf %t && mkdir -p %t
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t/flipped.bc
;
; RUN: llc %t/flipped.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -relocation-model=pic -output-asm-variant=2 \
; RUN:     -o - 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

@.str.0 = private unnamed_addr constant [6 x i8] c"hello\00", align 1
@.str.2 = private unnamed_addr constant [4 x i8] c"abc\00", align 1

declare goabi0cc void @sink(ptr)

; LEA64r path: address-of a string literal flows through printPlan9MemRef
; with Disp.isExpr() and isLocalLabelName(Sym) == true. Pre-fix emitted
; LEAQ _L_str_0, DI (no SB); the fix emits LEAQ _L_str_0(SB), DI.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}use_str0(SB)
; CHECK:       LEAQ _L_str_0(SB), {{[A-Z]+}}
; CHECK:       CALL {{[^[:space:]]+}}sink(SB)
; CHECK:       RET
define internal goabi0cc void @use_str0() #0 {
  call goabi0cc void @sink(ptr @.str.0)
  ret void
}

; MOV64rm path: value-load through a constant pointer to the literal. The
; volatile qualifier prevents the DAG combiner from folding the load into
; a constant immediate; the resulting MOV64rm carries a symbolic disp
; that flows through the same printPlan9MemRef local-label branch.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}use_str2_load(SB)
; CHECK:       MOVQ _L_str_2(SB), {{[A-Z]+}}
; CHECK:       RET
define internal goabi0cc i64 @use_str2_load() #0 {
  %v = load volatile i64, ptr @.str.2, align 1
  ret i64 %v
}

; --- Negative pins: the pre-fix buggy form is exactly a LEAQ/MOVQ line
;     whose source operand is a bare _L_str_<n> token with no (SB) suffix
;     (the symbol is followed only by ", REG\n"). go tool asm rejected
;     this with "illegal addressing mode". Both opcode spellings are
;     banned across the whole module.
; CHECK-NOT: {{LEAQ _L_str_[0-9]+, [A-Z]+$}}
; CHECK-NOT: {{MOVQ _L_str_[0-9]+, [A-Z]+$}}
; CHECK-NOT: addressing mode

attributes #0 = { "c2go-reg-return" "c2go-argsize"="0" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
