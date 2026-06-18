; X86 Plan-9 printer: a symbolic LEA64r / MOV64rm whose displacement
; references an @c2go.typeinfo.<X> (or @"type:<pkg>.<X>") global must be
; rewritten into the Plan-9 form
;
;     MOVQ ·_typeinfo_<X>(SB), <Rd>
;
; mirroring the AArch64 ADRP+ADD typeinfo rewrite. Without the rewrite the
; .s references the mangled raw-name form c2go_typeinfo·<X>(SB) produced
; by goSymToPlan9, but c2gobind only ever emits the indirection var
; ·_typeinfo_<X>(SB), so the Go linker fails with relocation-target-not-
; defined.
;
; Pins:
;
;   1. c2go.typeinfo.Node         -> MOVQ ·_typeinfo_Node(SB), <Rd>
;      (C-owner case; the descriptor is emitted locally).
;   2. c2go.typeinfo.c2go.anon.6d -> MOVQ ·_typeinfo_c2go_anon_6d(SB), <Rd>
;      (anon-record sanitization: c2gobind flattens `.` -> `_` because Go
;      identifiers cannot contain `.`).
;   3. type:pkg.AsFrame           -> MOVQ ·_typeinfo_AsFrame(SB), <Rd>
;      (Go-owner / c2go_linkname case; tail after the last `.` is the bare
;      type name).
;   4. c2go.typeinfo.Outer.Inner  -> MOVQ ·_typeinfo_Outer.Inner(SB), <Rd>
;      (non-anon C-owner whose tail still carries `.`: flatten must NOT
;      trigger here - only c2go.*-prefixed remainders are flattened). This
;      pins the cross-arch symmetry invariant: if the X86 path regressed
;      to an unconditional flatten, the Go linker would resolve a
;      different symbol than what c2gobind exported. The AArch64 source
;      enforces the same invariant by guarding the flatten on the c2go.
;      prefix; this LIT mirrors that guarantee on the X86 side.
;
; All forms must rewrite to a Plan-9 MOVQ (the typeinfo var value, i.e.
; the runtime._type pointer) regardless of whether codegen chose LEA64r
; or MOV64rm. The rewrite also bans the raw c2go_typeinfo· / type·
; AT&T-mangled spellings from ever leaking through (those round-trip into
; link errors).
;
; RUN: rm -rf %t && mkdir -p %t
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t/flipped.bc
;
; RUN: llc %t/flipped.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -output-asm-variant=2 -o - 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

@c2go.typeinfo.Node = external global i8
@c2go.typeinfo.c2go.anon.6d = external global i8
@"type:pkg.AsFrame" = external global i8
@"c2go.typeinfo.Outer.Inner" = external global i8

declare goabi0cc void @c2go_typedmemmove(ptr, ptr, ptr)

; CHECK-LABEL: TEXT {{[^[:space:]]+}}use_c_owner(SB)
; CHECK:       MOVQ ·_typeinfo_Node(SB), {{[A-Z0-9]+}}
; CHECK-NOT:   c2go_typeinfo
; CHECK-NOT:   c2go.typeinfo
define internal goabi0cc void @use_c_owner(ptr %dst, ptr %src) #0 {
  call goabi0cc void @c2go_typedmemmove(ptr @c2go.typeinfo.Node, ptr %dst, ptr %src)
  ret void
}

; CHECK-LABEL: TEXT {{[^[:space:]]+}}use_anon_record(SB)
; CHECK:       MOVQ ·_typeinfo_c2go_anon_6d(SB), {{[A-Z0-9]+}}
; CHECK-NOT:   c2go_typeinfo
; CHECK-NOT:   c2go.typeinfo
define internal goabi0cc void @use_anon_record(ptr %dst, ptr %src) #0 {
  call goabi0cc void @c2go_typedmemmove(ptr @c2go.typeinfo.c2go.anon.6d, ptr %dst, ptr %src)
  ret void
}

; CHECK-LABEL: TEXT {{[^[:space:]]+}}use_go_owner(SB)
; CHECK:       MOVQ ·_typeinfo_AsFrame(SB), {{[A-Z0-9]+}}
; CHECK-NOT:   type·pkg
; CHECK-NOT:   type:pkg
define internal goabi0cc void @use_go_owner(ptr %dst, ptr %src) #0 {
  call goabi0cc void @c2go_typedmemmove(ptr @"type:pkg.AsFrame", ptr %dst, ptr %src)
  ret void
}

; Non-anon C-owner with a `.` in the tail (e.g. nested type spelling).
; This must preserve the dot - only c2go.*-prefixed remainders are
; flattened. Mirrors AArch64's conditional-on-c2go. check.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}use_dotted_c_owner(SB)
; CHECK:       MOVQ ·_typeinfo_Outer.Inner(SB), {{[A-Z0-9]+}}
; CHECK-NOT:   _typeinfo_Outer_Inner
; CHECK-NOT:   c2go_typeinfo
; CHECK-NOT:   c2go.typeinfo
define internal goabi0cc void @use_dotted_c_owner(ptr %dst, ptr %src) #0 {
  call goabi0cc void @c2go_typedmemmove(ptr @"c2go.typeinfo.Outer.Inner", ptr %dst, ptr %src)
  ret void
}

attributes #0 = { "c2go-reg-return" "c2go-argsize"="24" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
