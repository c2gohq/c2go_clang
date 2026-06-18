; X86 Plan-9 printer: an ELF small-PIC `MOV64rm @sym@GOTPCREL` two-step
; external-global load must have its first step rewritten into a Plan-9
; immediate LEA:
;
;     LEAQ ·sym(SB), Rd
;
; mirroring AArch64's `MOVD $·sym(SB), Rd` form. Without the rewrite the
; X86 .s prints `MOVQ ·sym(SB), Rd` (a value load in Plan-9 semantics);
; Go's assembler/linker has no GOTPCREL concept and would treat
; `runtime·writeBarrier(SB)` as a real 8-byte load of the i32 flag (with
; 4 bytes of trailing junk). The follow-up narrow-width load then
; dereferences a garbage pointer (a -O0 nil-deref at runtime.writeBarrier).
;
; Three external-global load shapes the c2go IR pipeline emits:
;
;   1. load i32, ptr @runtime.writeBarrier  - the wb.enabled flag check.
;   2. load i64, ptr @runtime.GOMAXPROCS    - different global, different
;      load width: proves the rewrite keys off the MOV64rm @GOTPCREL
;      shape (the first step is always 8B), not a symbol-name pattern.
;   3. load ptr, ptr @some_external_ptr     - a pointer-typed global;
;      the rewrite applies identically and the follow-up real load
;      MOVQ 0(Rd), Rt prints with the now-correct base.
;
; The rewrite must NOT fire on c2go.typeinfo.<X> / type:<pkg>.<X>: those
; have their own MOVQ ·_typeinfo_<X>(SB), Rd form (a typeinfo var's value
; IS the *_type pointer, so a single MOVQ load is correct). Pinned by the
; negative checks below (c2go.typeinfo.NodeX -> MOVQ form, not LEAQ).
;
; RUN: rm -rf %t && mkdir -p %t
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t/flipped.bc
;
; RUN: llc %t/flipped.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -output-asm-variant=2 -o - 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

@runtime.writeBarrier = external global i32
@runtime.GOMAXPROCS = external global i64
@some_external_ptr = external global ptr
@c2go.typeinfo.NodeX = external global i8

; CHECK-LABEL: TEXT {{[^[:space:]]+}}load_writeBarrier(SB)
; CHECK:       LEAQ runtime·writeBarrier(SB), {{[A-Z0-9]+}}
; CHECK:       MOVL 0({{[A-Z0-9]+}}),
; The buggy form would print MOVQ runtime·writeBarrier(SB), Rd (a load);
; the rewrite must replace it with LEAQ (an address-of).
; CHECK-NOT:   MOVQ runtime·writeBarrier(SB),
define internal goabi0cc i32 @load_writeBarrier() #0 {
  %v = load i32, ptr @runtime.writeBarrier
  ret i32 %v
}

; CHECK-LABEL: TEXT {{[^[:space:]]+}}load_GOMAXPROCS(SB)
; CHECK:       LEAQ runtime·GOMAXPROCS(SB), {{[A-Z0-9]+}}
; CHECK:       MOVQ 0({{[A-Z0-9]+}}),
; CHECK-NOT:   MOVQ runtime·GOMAXPROCS(SB), {{[A-Z0-9]+}}
; CHECK-NOT:   MOVQ runtime·GOMAXPROCS(SB),
define internal goabi0cc i64 @load_GOMAXPROCS() #0 {
  %v = load i64, ptr @runtime.GOMAXPROCS
  ret i64 %v
}

; A pointer-typed external global: the rewrite still applies because the
; ISel-chosen lowering is identical (8B GOTPCREL load as the first step,
; regardless of the IR load type).
; CHECK-LABEL: TEXT {{[^[:space:]]+}}load_external_ptr(SB)
; CHECK:       LEAQ ·some_external_ptr(SB), {{[A-Z0-9]+}}
; CHECK:       MOVQ 0({{[A-Z0-9]+}}),
define internal goabi0cc ptr @load_external_ptr() #0 {
  %v = load ptr, ptr @some_external_ptr
  ret ptr %v
}

; Negative case: the typeinfo rewrite must still fire on c2go.typeinfo.*
; (it runs before the GOTPCREL-to-LEA rewrite in the dispatch chain).
; This pins the dispatch ordering and proves the new rewrite did not
; accidentally hijack the typeinfo path.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}use_typeinfo(SB)
; CHECK:       MOVQ ·_typeinfo_NodeX(SB), {{[A-Z0-9]+}}
; CHECK-NOT:   LEAQ ·c2go·typeinfo·NodeX(SB),
; CHECK-NOT:   LEAQ c2go_typeinfo·NodeX(SB),
declare goabi0cc void @c2go_typedmemmove(ptr, ptr, ptr)
define internal goabi0cc void @use_typeinfo(ptr %dst, ptr %src) #0 {
  call goabi0cc void @c2go_typedmemmove(ptr @c2go.typeinfo.NodeX, ptr %dst, ptr %src)
  ret void
}

attributes #0 = { "c2go-reg-return" "c2go-argsize"="24" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
