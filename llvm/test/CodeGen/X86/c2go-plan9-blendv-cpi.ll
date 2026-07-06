; c2go #600: SSE4.1 implicit-XMM0 blend with a constant-pool memory operand.
;
; Once c2go long double == double, sqlite3AtoF's rounding compiles to SSE
; vector selects and ISel folds the constant vector into `BLENDVPDrm0
; LCPI, xmm` — a symbol-bearing MCInst the Plan-9 raw-byte fallback cannot
; encode (1 CPI fixup), so an unhandled opcode is a loud
; `report_fatal_error`, killing the amd64 SQLite gate. The printer's
; SSE table spells the implicit mask register explicitly, matching the Go
; assembler's syntax (`BLENDVPD X0, m/x, x`, cmd/asm testdata amd64enc.s),
; and the CPI symbol stays file-local (`<>`, #586).

; RUN: llc < %s --output-asm-variant=2 -mtriple=x86_64-unknown-linux-gnu \
; RUN:   -mattr=+sse4.1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

; CHECK-LABEL: TEXT ·blend(SB)
; CHECK: BLENDVPD X0, _LCPI0_0<>(SB), X{{[0-9]+}}
define double @blend(double %a, double %b, double %x, double %y) #0 {
entry:
  %v0 = insertelement <2 x double> poison, double %a, i64 0
  %v1 = insertelement <2 x double> %v0, double %b, i64 1
  %w0 = insertelement <2 x double> poison, double %x, i64 0
  %w1 = insertelement <2 x double> %w0, double %y, i64 1
  %c = fcmp olt <2 x double> %v1, zeroinitializer
  %r = select <2 x i1> %c, <2 x double> <double 1.0, double 2.0>, <2 x double> %w1
  %e0 = extractelement <2 x double> %r, i64 0
  %e1 = extractelement <2 x double> %r, i64 1
  %s = fadd double %e0, %e1
  ret double %s
}

attributes #0 = { "c2go-argsize"="32" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
