; Go-owned globals: DATA-path suppression backstop.
;
; Single-pointer-word file-scope globals are ceded to the Go side, which owns
; the storage (var X unsafe.Pointer). The Plan 9 streamer must not also emit
; a definition for them or it collides with the Go-side storage. The common
; case is a zero-init global that routes through emitCommonSymbol/emitZerofill
; where the suppression already lives; this guard covers the DATA-label path
; (emitC2GoDataLabel), reached when a global has a non-null initializer.
;
; The module hand-crafts a global with a non-null initializer (forcing the
; DATA-label path) that is also listed in !c2go.go_owned_globals (the streamer's
; source of truth). With the backstop, the streamer suppresses both the DATA
; directives and the GLOBL trailer for that global; without it, a duplicate
; DATA/GLOBL pair would ship in the .s. A control global not in the go-owned
; set keeps the same non-null-init shape and guards against over-suppressing
; all DATA-path globals.
;
; REQUIRES: aarch64-registered-target

; RUN: c2go-lto %s --c2go-emit-asm=%t.s --c2go-emit-manifest=%t.json
; RUN: FileCheck %s --check-prefix=ASM --input-file=%t.s

target triple = "aarch64-unknown-unknown-elf"

; Target globals for the pointer initializers - these need a definite address
; so the initializers below survive mid-end folding.
@gTargetA = internal global i64 0, align 8
@gTargetB = internal global i64 0, align 8

; Go-owned pointer global with a non-null initializer, forcing AsmPrinter onto
; the DATA-label path rather than emitCommonSymbol/emitZerofill. The streamer
; must skip both the DATA lines and the GLOBL trailer because the name appears
; in !c2go.go_owned_globals below; the Go side owns the storage.
@gOwnedPtr = global ptr @gTargetA, align 8

; Control: pointer global NOT in the go-owned set. Same non-null-init shape,
; but it must keep its DATA + GLOBL emission - the check only fires for names
; in the suppression set.
@gNonOwnedPtr = global ptr @gTargetB, align 8

; Companion gcmask GV (matches the type-info convention). Not required for the
; backstop, but keeps the module shape consistent with the other go-owned test.
@c2go.global.gcmask.gOwnedPtr = internal constant [1 x i8] c"\01", align 1

; Keep everything live through the mid-end pipeline (internal mask GV would
; otherwise be DCE'd).
@llvm.used = appending global [5 x ptr] [
  ptr @gOwnedPtr,
  ptr @gNonOwnedPtr,
  ptr @gTargetA,
  ptr @gTargetB,
  ptr @c2go.global.gcmask.gOwnedPtr
], section "llvm.metadata"

; Non-empty function so c2go-lto module validation passes.
define i64 @use() {
  %p = load ptr, ptr @gOwnedPtr, align 8
  %v = load i64, ptr %p, align 8
  ret i64 %v
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.target_go_version", !"1.22-1.25"}
!2 = !{i32 1, !"c2go.pkgpath", !"backstop"}

; Tag gOwnedPtr as Go-owned despite its non-null initializer - this stands in
; for a mid-end fold or widened predicate that lands a Go-owned global on the
; DATA path, which the backstop must catch.
!c2go.go_owned_globals = !{!10}
!10 = !{!"gOwnedPtr"}

; ASM-side assertions.
;
; Go-owned global: the backstop fires inside emitC2GoDataLabel, so neither DATA
; nor GLOBL lines for the symbol may appear.
;
; ASM-NOT: DATA ·gOwnedPtr+
; ASM-NOT: GLOBL ·gOwnedPtr(SB)
;
; Control: the non-Go-owned global keeps its DATA + GLOBL emission, guarding
; against over-suppressing all DATA-path globals.
;
; ASM-DAG: DATA ·gNonOwnedPtr+0(SB)/8, $·gTargetB(SB)
; ASM-DAG: GLOBL ·gNonOwnedPtr(SB), {{.*}}$8
