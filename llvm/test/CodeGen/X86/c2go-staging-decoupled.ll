; X86 c2go staged-meta producer: TEXT-meta staging is decoupled from the
; leaf-ABI CC flip.
;
; Pre-fix the producer required the c2goabiinternalcc CC before staging
; the Plan-9 TEXT meta. That CC gate was the root: functions the leaf-ABI
; pass declined to flip (e.g. because the second c2go.x86-leaf-abi module
; flag was OFF - the production gate-off path - or because the function
; failed an eligibility predicate) fell back to the streamer's argsize-
; less TEXT directive, so downstream Go-runtime probes crashed in
; copystack with no FUNCDATA $0.
;
; This LIT pins the new contract: the stager publishes
; TEXT ... $framesize-argsize whenever
;   (a) the module is c2go-mode (c2go.goabi flag ON),
;   (b) the target triple is X86,
;   (c) the function carries a c2go-argsize IR fn attribute
; - regardless of whether the leaf-ABI pass flipped the CC. Physical
; prologue emission stays CC-gated as a SysV-fallback safety belt; that
; fallback path is exercised elsewhere.
;
; The repro deliberately keeps the second leaf-abi flag OFF so the
; leaf-ABI pass leaves the CC at the default ccc - the staging path still
; has to fire.
;
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -output-asm-variant=2 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

; CC-not-flipped + c2go-argsize attribute present -> the stager stages
; baseline meta with FrameSize from MFI (0 here, a true leaf, emits as
; NOFRAME) and ArgSize from the attribute (16). NoSplit is false because
; the CC was not certified by the leaf-ABI pass - the absence of the
; NOSPLIT flag word in the directive is what proves staging came from the
; SysV-fallback branch (the CC-flipped path stamps NOSPLIT).
;
; The TU-local fallback shape is NOFRAME, $0 (argsize-less); staging
; upgrades this to NOFRAME, $0-16 - the trailing -16 is the load-bearing
; diff and is exactly what copystack's FUNCDATA $0 walker needs to size
; the args region.
;
; CHECK-LABEL: TEXT {{[^[:space:]]+}}staged_no_flip(SB), NOFRAME, $0-16
define i64 @staged_no_flip(i64 %a, i64 %b) #0 {
  %s = add i64 %a, %b
  ret i64 %s
}

; Same shape, but the c2go-argsize attribute is MISSING - this function
; is not a c2go-managed function (the producer never stamped it). The
; stager must be a strict no-op so the streamer falls back to its TU-local
; NOFRAME, $0 shape (the production default before any c2go staging
; fires). The argsize-less $0 (no trailing -M) is the byte-identity diff
; that distinguishes a stager no-op from a SysV-fallback publish.
;
; CHECK-LABEL: TEXT {{[^[:space:]]+}}not_a_c2go_fn(SB), NOFRAME, $0
; CHECK-NOT: TEXT {{[^[:space:]]+}}not_a_c2go_fn(SB), {{.*}}-{{[0-9]+}}
define i64 @not_a_c2go_fn(i64 %a, i64 %b) {
  %s = add i64 %a, %b
  ret i64 %s
}

attributes #0 = { "c2go-argsize"="16" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
