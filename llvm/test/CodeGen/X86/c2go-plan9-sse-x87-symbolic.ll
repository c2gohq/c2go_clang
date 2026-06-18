; X86 Plan-9 printer: translate the symbolic-displacement SSE
; packed/scalar, widening-integer-load and x87 memory forms that X86
; ISel folds RIP-relative constant-pool / global accesses into:
;
;   MOVDQArm / MOVDQUmr   - vector load/store     -> MOVO / MOVOU ·sym(SB)
;   MOVDI2PDIrm           - movd m32 -> xmm       -> MOVL ·sym(SB), Xn
;   MOVQI2PQIrm           - movq m64 -> xmm       -> MOVQ ·sym(SB), Xn
;   PADDDrm / PADDQrm     - packed RMW            -> PADDL/PADDQ ·sym(SB), Xn
;   ADDSDrm / UCOMISDrm   - scalar fold / compare -> ADDSD/UCOMISD ·sym(SB), Xn
;   MOVZX32rm8/16         - widening loads        -> MOVBLZX/MOVWLZX ·sym(SB), Rn
;   MOVSX64rm32           -                       -> MOVLQSX ·sym(SB), Rn
;   LD_F80m / MUL_F32m    - x87 long-double ops   -> FMOVX/FMULF ·sym(SB), F0
;
; AArch64 splits the same accesses into an already-covered ADRP+LDR pair,
; so only the X86 port had this gap. Every Plan-9 spelling was verified
; against `go tool asm` + `go tool objdump` round-trip (encodings match
; the original Intel forms: MOVO=movdqa, MOVL/MOVQ-to-X=movd/movq,
; PADDL=paddd, PUNPCKLLQ=punpckldq, FMOVX=fld tbyte, FMULF=fmul m32).
; Only symbolic (fixup-bearing) shapes are claimed; non-symbolic forms
; keep the raw-byte fallback byte-identical, and any remaining symbolic
; miss stays fail-closed in the streamer (see
; c2go-plan9-unhandled-symbolic-fatal.ll).
;
; `-relocation-model=pic` pins RIP-relative lowering (mirrors the darwin
; production triple; the absolute non-PIC form prints identically).
;
; RUN: rm -rf %t && mkdir -p %t
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t/flipped.bc
;
; RUN: llc %t/flipped.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -relocation-model=pic -output-asm-variant=2 -o - 2>&1 \
; RUN:   | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

@gVecA   = internal global <2 x i64> <i64 1, i64 2>, align 16
@gVecOut = internal global <2 x i64> zeroinitializer, align 8
@gV4     = internal global <4 x i32> <i32 1, i32 2, i32 3, i32 4>, align 16
@gV4Out  = internal global <4 x i32> zeroinitializer, align 8
@gQ      = internal global i64 5, align 8
@gD      = internal global double 1.5, align 8
@gB      = internal global i8 7, align 1
@gW      = internal global i16 9, align 2
@gL      = internal global i32 11, align 4
@gLD     = internal global x86_fp80 0xK3FFF8000000000000000, align 16
@gF      = internal global float 2.5, align 4

; MOVQI2PQIrm (movq zero-extending m64 -> xmm) + PADDQrm (packed RMW, mem
; source first) + MOVDQUmr (unaligned vector store, data reg first).
; CHECK-LABEL: TEXT ·sym_movq2x(SB)
; CHECK:       MOVQ ·gQ(SB), X{{[0-9]+}}
; CHECK:       PADDQ ·gVecA(SB), X{{[0-9]+}}
; CHECK:       MOVOU X{{[0-9]+}}, ·gVecOut(SB)
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc void @sym_movq2x() #0 {
  %q = load i64, ptr @gQ, align 8
  %v = insertelement <2 x i64> zeroinitializer, i64 %q, i64 0
  %a = load <2 x i64>, ptr @gVecA, align 16
  %s = add <2 x i64> %v, %a
  store <2 x i64> %s, ptr @gVecOut, align 8
  ret void
}

; MOVDI2PDIrm (movd m32 -> xmm) + PADDDrm (Plan-9 spells paddd PADDL).
; CHECK-LABEL: TEXT ·sym_movd2x(SB)
; CHECK:       MOVL ·gL(SB), X{{[0-9]+}}
; CHECK:       PADDL ·gV4(SB), X{{[0-9]+}}
; CHECK:       MOVOU X{{[0-9]+}}, ·gV4Out(SB)
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc void @sym_movd2x() #0 {
  %i = load i32, ptr @gL, align 4
  %v = insertelement <4 x i32> zeroinitializer, i32 %i, i64 0
  %a = load <4 x i32>, ptr @gV4, align 16
  %s = add <4 x i32> %v, %a
  store <4 x i32> %s, ptr @gV4Out, align 8
  ret void
}

; MOVDQArm - aligned vector load that ISel does not fold (two reg uses).
; CHECK-LABEL: TEXT ·sym_vecload(SB)
; CHECK:       MOVO ·gVecA(SB), X{{[0-9]+}}
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc void @sym_vecload() #0 {
  %a = load <2 x i64>, ptr @gVecA, align 16
  %s = add <2 x i64> %a, %a
  store <2 x i64> %s, ptr @gVecOut, align 8
  ret void
}

; ADDSDrm - scalar-FP RMW fold (mem source first, Xd second).
; CHECK-LABEL: TEXT ·sym_scalar(SB)
; CHECK:       ADDSD ·gD(SB), X{{[0-9]+}}
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc double @sym_scalar(double %x) #0 {
  %g = load double, ptr @gD, align 8
  %s = fadd double %x, %g
  ret double %s
}

; UCOMISDrm - compare against a global. The reg operand carries the Intel
; reg-field subject in both syntaxes (UCOMISD mem, X0 encodes
; ucomisd %xmm0, mem), so the following SETcc/Jcc predicate matches the
; original encoding.
; CHECK-LABEL: TEXT ·sym_cmp(SB)
; CHECK:       UCOMISD ·gD(SB), X{{[0-9]+}}
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc i32 @sym_cmp(double %x) #0 {
  %g = load double, ptr @gD, align 8
  %c = fcmp ogt double %x, %g
  %r = zext i1 %c to i32
  ret i32 %r
}

; MOVZX32rm8 / MOVZX32rm16 - widening byte/word loads from globals.
; CHECK-LABEL: TEXT ·sym_widen(SB)
; CHECK-DAG:   MOVBLZX ·gB(SB), {{[A-Z0-9]+}}
; CHECK-DAG:   MOVWLZX ·gW(SB), {{[A-Z0-9]+}}
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc i32 @sym_widen() #0 {
  %b = load i8, ptr @gB, align 1
  %zb = zext i8 %b to i32
  %w = load i16, ptr @gW, align 2
  %zw = zext i16 %w to i32
  %r = add i32 %zb, %zw
  ret i32 %r
}

; MOVSX64rm32 - sign-extending dword load.
; CHECK-LABEL: TEXT ·sym_sext(SB)
; CHECK:       MOVLQSX ·gL(SB), {{[A-Z0-9]+}}
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc i64 @sym_sext() #0 {
  %l = load i32, ptr @gL, align 4
  %s = sext i32 %l to i64
  ret i64 %s
}

; LD_F80m (fld tbyte -> FMOVX, pushes onto the x87 stack) + MUL_F32m
; (fmul m32 -> FMULF, folded fpext float load) - long-double arithmetic.
; The fptrunc spill/reload through the local frame is non-symbolic and
; stays on the raw-byte path.
; CHECK-LABEL: TEXT ·sym_x87(SB)
; CHECK:       FMOVX ·gLD(SB), F0
; CHECK:       FMULF ·gF(SB), F0
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc double @sym_x87() #0 {
  %a = load x86_fp80, ptr @gLD, align 16
  %f = load float, ptr @gF, align 4
  %fe = fpext float %f to x86_fp80
  %m = fmul x86_fp80 %a, %fe
  %d = fptrunc x86_fp80 %m to double
  ret double %d
}

attributes #0 = { "c2go-reg-return" "c2go-argsize"="24" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
