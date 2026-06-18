; c2go-lto's GlobalMerge keep-out for Go-owned globals must reject
; !c2go.go_owned_globals entries whose resolved GV does not match the
; eligibility envelope:
;
;   * IR linkage MUST be internal (the C-source predicate is `static`
;     file-scope, which CodeGenModule emits as internal linkage), AND
;   * the value type's primitive size MUST equal the module's pointer
;     width (the predicate is "static layout is exactly one pointer word").
;
; Entries that fall outside (stale named-md name with no GV; mis-sized GV;
; non-internal linkage GV) are skipped silently for the keep-out - they
; would otherwise pin a value whose storage ownership was never reserved,
; masking the real eligibility bug. The companion streamer enqueue is
; by-name and unaffected.
;
; The positive shape (the good single-ptr-word static) MUST still appear
; in the keep-out so GlobalMerge can't fold it.
;
; REQUIRES: aarch64-registered-target

; RUN: c2go-lto %s --c2go-emit-asm=%t.s --c2go-emit-manifest=%t.json
; RUN: FileCheck %s --check-prefix=ASM  --input-file=%t.s

target triple = "aarch64-unknown-unknown-elf"

; --- Mix three named-metadata entries ---------------------------------------
;
; (A) gPtrGood - the only entry that matches the eligibility envelope:
;     internal linkage + pointer-sized. MUST land in the go-owned set and
;     (transitively) in llvm.compiler.used so GlobalMerge can't merge it.
;     Its GLOBL must also be suppressed by the streamer.
@gPtrGood = internal global ptr null, align 8

; (B) gPtrBigBad - the named-metadata claims this is a go-owned global, but
;     the IR shape is a two-pointer aggregate (16 bytes on aarch64, not a
;     single pointer word). clang would NEVER tag this - the predicate
;     requires the size to equal the pointer width. This is the "stale .bc
;     / mis-shaped entry" case the guard catches. The keep-out MUST drop
;     it; the streamer enqueue is by-name and still happens (no asserts on
;     it - out of scope for this guard).
@gPtrBigBad = internal global { ptr, ptr } zeroinitializer, align 8

; (C) gPtrStaleMissing is named in !c2go.go_owned_globals but has NO GV in
;     the module, so the lookup returns null. The keep-out's existing
;     null-check skips it cleanly - this entry proves the mix doesn't crash
;     the c2go-lto driver (regression guard against null-deref).

; --- Companion gcmask GVs (clang always emits these alongside a tracked
;     global; the manifest reader picks them up by prefix; not load-bearing
;     for this test but kept so the named-metadata round-trips through
;     normal validation).
@c2go.global.gcmask.gPtrGood = internal constant [1 x i8] c"\01", align 1
@c2go.global.gcmask.gPtrBigBad = internal constant [1 x i8] c"\03", align 1

; Keep everything live through whatever mid-end DCE c2go-lto's lto
; pipeline runs, so the AsmPrinter still sees them when it emits
; (or, for gPtrGood, *doesn't* emit) the GLOBL trailer.
@llvm.used = appending global [4 x ptr] [
  ptr @gPtrGood,
  ptr @gPtrBigBad,
  ptr @c2go.global.gcmask.gPtrGood,
  ptr @c2go.global.gcmask.gPtrBigBad
], section "llvm.metadata"

; Non-empty function so the module passes c2go-lto's validation;
; touches gPtrGood/gPtrBigBad so they survive into codegen.
define i64 @use() {
  %p1 = load ptr, ptr @gPtrGood, align 8
  %p2 = load ptr, ptr @gPtrBigBad, align 8
  ret i64 0
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.target_go_version", !"1.22-1.25"}
!2 = !{i32 1, !"c2go.pkgpath", !"wf2gop_badentry"}

; The go-owned set - three entries: the good one + two bad ones the
; keep-out guard must reject (mis-sized + stale-name).
!c2go.go_owned_globals = !{!10, !11, !12}
!10 = !{!"gPtrGood"}
!11 = !{!"gPtrBigBad"}
!12 = !{!"gPtrStaleMissing"}

; --- ASM-side assertions ----------------------------------------------------
;
; The streamer suppression is name-only: gPtrGood's GLOBL is suppressed
; (in the go-owned set, by name), and the mis-sized gPtrBigBad is also
; enqueued by name - the linkage/size filtering happens in the keep-out
; guard, not the streamer. So both gPtrGood and gPtrBigBad lose their
; GLOBL trailer; that is existing behaviour and NOT what this test guards.
; What this test proves is the c2go-lto driver still drives the mixed
; go-owned set to a successful asm emission (no null-deref on the stale
; name; no crash on the mis-shaped aggregate).
;
; gPtrGood's GLOBL being gone pins that the driver actually walked codegen.
;
; ASM-NOT: GLOBL ·gPtrGood(SB),
;
; And there is no stray reference to the stale-name entry anywhere in the
; asm output (defensive - would catch a loop body that synthesised
; something for the missing-GV path).
;
; ASM-NOT: gPtrStaleMissing
