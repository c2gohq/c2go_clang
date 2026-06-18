; c2go-memcpy-typing must STRIP gc-leaf-function on reuse when the helper is a
; non-leaf libc copy (libc.Memmove or libc.Memset).
;
; If an upstream pass or hand-written IR pre-decorated libc.Memmove with
; gc-leaf-function, an additive-only reuse path would leak the stale attribute
; through -> RS4GC would treat the call as a GC-leaf and skip the statepoint
; wrap -> managed roots not relocated -> use-after-relocate. This test proves
; the reuse-path scrub.
;
; RUN: opt < %s -passes=c2go-memcpy-typing -S | FileCheck %s

; Pre-declare libc.Memmove WITH gc-leaf-function and default CC - the exact
; "pre-pollution" shape the strip path must scrub.
declare ptr @"github.com/c2go_project/c2go_libc.Memmove"(ptr, ptr, i64) #0

; An intrinsic memcpy with no !c2go.elem.type and a non-inlinable size forces
; rewrite into the non-leaf libc.Memmove fallback path, which drives the
; reuse-path scrub.
define void @drives_libc_path(ptr %dst, ptr %src) {
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 8192, i1 false)
  ret void
}

declare void @llvm.memcpy.p0.p0.i64(ptr nocapture writeonly, ptr nocapture readonly, i64, i1 immarg)

; Pre-existing gc-leaf-function must be STRIPPED on the declaration after the
; pass runs. CC must be GoABI0 (goabi0cc).
;
; CHECK:      declare {{.*}}goabi0cc{{.*}}ptr @"github.com/c2go_project/c2go_libc.Memmove"
; CHECK-NOT:    {{.*}}gc-leaf-function{{.*}}

; The original pre-pollution attribute group #0 may still appear in the
; module as an orphan, but the call site / function declaration must not
; carry it. The line-anchored CHECK-NOT on the libc.Memmove declaration
; above suffices.

attributes #0 = { "gc-leaf-function" }
