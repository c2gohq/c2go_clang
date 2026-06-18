; X86 Plan-9 static-data symbol references must carry the same `·`
; (middle-dot) package prefix the definition side emits, or the Go linker
; sees two distinct symbols and fails with relocation-target-not-defined.
;
; Repro: a link failed on static globals all starting with a lowercase
; `l` (likeInfoNorm / leadName / ...). The pre-fix isLocalLabelName used
; the broad test `Name[0] == 'L' || Name[0] == 'l'`, so printPlan9MemRef
; classified these ordinary static C globals as assembler-private labels
; and rendered the code reference without the `·`, while the streamer's
; definition still emitted GLOBL ·likeInfoNorm(SB) - two distinct symbols.
;
; The definition side (MCPlan9AsmStreamer::symbolToPlan9) and goSymToPlan9
; both use the narrow test - only `L*`, `.L*`, `l_*`, `l<UPPER>*` are
; local. The fix narrows isLocalLabelName to the same predicate. AArch64
; is unaffected: its broad isLocalLabelName is branch-target-only; data
; refs always route through goSymToPlan9.
;
; CHECK pins (positive, reference and definition agree):
;   1. LEA64r address-of   -> LEAQ ·likeInfoNorm(SB), REG
;   2. MOV64rm value-load  -> MOVQ ·leadName(SB), REG
;   3. streamer definition -> GLOBL ·likeInfoNorm(SB) / GLOBL ·leadName(SB)
; BAN pins (negative, whole-module): the bare pre-fix spelling
; LEAQ likeInfoNorm / MOVQ leadName (no `·` between mnemonic and symbol)
; must not appear anywhere.
;
; `-relocation-model=pic` pins the SelectionDAG into RIP-relative
; LEA64r / MOV64rm forms.
;
; RUN: rm -rf %t && mkdir -p %t
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t/flipped.bc
;
; RUN: llc %t/flipped.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -relocation-model=pic -output-asm-variant=2 \
; RUN:     -o - 2>&1 | FileCheck %s
;
; RUN: llc %t/flipped.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -relocation-model=pic -output-asm-variant=2 \
; RUN:     -o - 2>&1 | FileCheck %s --check-prefix=BAN

target triple = "x86_64-unknown-linux-gnu"

; Lowercase-`l` static globals - the shape the broad predicate
; misclassified. `internal` (not `private`) keeps the source-level name.
@likeInfoNorm = internal constant [4 x i8] c"\03\02\01\00", align 1
@leadName = internal constant [5 x i8] c"lead\00", align 1

declare goabi0cc void @sink(ptr)

; LEA64r path: address-of flows through printPlan9MemRef with
; Disp.isExpr(). Reference must match the GLOBL definition spelling.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}useLikeInfo(SB)
; CHECK:       LEAQ ·likeInfoNorm(SB), {{[A-Z0-9]+}}
; CHECK:       CALL {{[^[:space:]]+}}sink(SB)
; CHECK:       RET
define internal goabi0cc void @useLikeInfo() #0 {
  call goabi0cc void @sink(ptr @likeInfoNorm)
  ret void
}

; MOV64rm path: volatile value-load keeps the symbolic disp (the DAG
; combiner cannot fold it), exercising the same local-label branch.
; CHECK-LABEL: TEXT {{[^[:space:]]+}}loadLead(SB)
; CHECK:       MOVQ ·leadName(SB), {{[A-Z0-9]+}}
; CHECK:       RET
define internal goabi0cc i64 @loadLead() #0 {
  %v = load volatile i64, ptr @leadName, align 1
  ret i64 %v
}

; Definition side (MCPlan9AsmStreamer): GLOBL spelling carries `·`.
; CHECK: GLOBL ·likeInfoNorm(SB), RODATA, $4
; CHECK: GLOBL ·leadName(SB), RODATA, $5

; --- Whole-module negative pins (separate FileCheck pass so the NOT
;     patterns scan the entire output, not just the tail after the last
;     CHECK). The buggy form is the mnemonic directly followed by the bare
;     symbol; the fixed form always has the two-byte `·` in between.
; BAN-NOT: LEAQ likeInfoNorm
; BAN-NOT: MOVQ leadName
; BAN-NOT: relocation target

attributes #0 = { "c2go-reg-return" "c2go-argsize"="0" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
