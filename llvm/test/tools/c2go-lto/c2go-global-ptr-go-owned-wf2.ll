; WF2 (c2go-lto) handling of Go-owned globals.
;
; Storage ownership of single-pointer-word file-scope globals is ceded to
; the Go side: clang emits a !c2go.go_owned_globals named-metadata list,
; then the backend (a) writes go_owned: true into the manifest
; module_gcmask.vars[], and (b) feeds the same name set to
; MCPlan9AsmStreamer so its GLOBL trailer is suppressed (the Go-side
; bodyless `var X unsafe.Pointer` is the unique storage definition).
;
; This test wires both consumers on the c2go-lto side, reading the
; named-metadata directly from the .bc. It drives c2go-lto on a .ll that
; carries !c2go.go_owned_globals = !{!"gPtr"} plus the companion
; @c2go.global.gcmask.gPtr GV and checks:
;   asm-side:  GLOBL for gPtr    MUST be suppressed.
;              GLOBL for gScalar MUST stay (regression guard - only globals
;                                in the go-owned set are suppressed).
;   json-side: module_gcmask vars[name=gPtr] MUST carry go_owned: true.
;              Scalar globals have no gcmask GV and therefore no manifest
;              entry, so go_owned is irrelevant on that side.
;
; REQUIRES: aarch64-registered-target

; RUN: c2go-lto %s --c2go-emit-asm=%t.s --c2go-emit-manifest=%t.json
; RUN: FileCheck %s --check-prefix=ASM  --input-file=%t.s
; RUN: FileCheck %s --check-prefix=JSON --input-file=%t.json

target triple = "aarch64-unknown-unknown-elf"

; A single-pointer-word file-scope global. Internal linkage matches the
; real IR shape clang emits for `static T *gPtr;` (only single-ptr-word
; static C globals are tagged, and those land in IR as internal linkage).
; Without the suppression the streamer would emit a GLOBL line for gPtr;
; with it the streamer skips it. The eligibility guard requires internal
; linkage + pointer-sized; only the GlobalMerge keep-out narrows on that,
; the streamer-side suppression (the asserts below) is unaffected.
@gPtr = internal global ptr null, align 8

; A scalar word global. NOT in the go-owned set; its GLOBL must stay
; (this is the regression guard against over-suppression).
@gScalar = internal global i64 0, align 8

; The companion gcmask GV that clang would emit alongside @gPtr. The
; manifest vars[] builder picks it up via the c2go.global.gcmask. prefix
; and stamps go_owned: true because gPtr is in !c2go.go_owned_globals.
@c2go.global.gcmask.gPtr = internal constant [1 x i8] c"\01", align 1

; Keep the user-facing globals + the mask GV live through whatever
; mid-end DCE c2go-lto's lto pipeline runs. The mask GV is internal-
; linkage so it would otherwise be dropped before AsmPrinter sees it,
; which would in turn drop the annotated manifest entry.
@llvm.used = appending global [3 x ptr] [
  ptr @gPtr,
  ptr @gScalar,
  ptr @c2go.global.gcmask.gPtr
], section "llvm.metadata"

; Non-empty function so the module passes c2go-lto's normal validation.
define i64 @use() {
  %p = load ptr, ptr @gPtr, align 8
  %v = load i64, ptr @gScalar, align 8
  ret i64 %v
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.target_go_version", !"1.22-1.25"}
!2 = !{i32 1, !"c2go.pkgpath", !"wf2gop"}

; The go-owned set - drives both the streamer GLOBL suppression and the
; manifest go_owned field.
!c2go.go_owned_globals = !{!10}
!10 = !{!"gPtr"}

; --- ASM-side assertions ----------------------------------------------------
;
; gScalar's GLOBL must be present (regression guard - only go-owned
; globals are suppressed). gPtr's GLOBL must be absent.
;
; ASM:     GLOBL ·gScalar(SB), NOPTR, $8
; ASM-NOT: GLOBL ·gPtr(SB),

; --- JSON-side assertions ---------------------------------------------------
;
; module_gcmask vars[] is pretty-printed sorted by name; the pretty-printer
; also sorts keys alphabetically inside each entry. The single entry's
; go_owned: true bit is the field this test pins.
;
; JSON:      "module_gcmask":
; JSON:      "go_owned": true
; JSON-NEXT: "mask_hex": "01"
; JSON-NEXT: "name": "gPtr"
; JSON-NEXT: "ptr_bits": 1
