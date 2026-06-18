; Both typed-memmove helpers emitted by c2go-memcpy-typing must carry the
; gc-leaf-function attribute. The attribute is backed by Go-side //go:nosplit
; on _c2go_typedmemmove (singleton) and _c2go_typedMemmoveArray. Dropping it
; here without also dropping the //go:nosplit annotation breaks the soundness
; contract RS4GC relies on to skip statepoint wrapping.
;
; RUN: opt < %s -passes=c2go-memcpy-typing -S | FileCheck %s

target triple = "arm64-unknown-none-goabi"

@c2go.typeinfo.S = external global i8

; Single-element typed memmove -> _c2go_typedmemmove (3-arg).
define void @copy_single(ptr %dst, ptr %src) {
  call void @llvm.memmove.p0.p0.i64(ptr %dst, ptr %src, i64 24, i1 false), !c2go.elem.type !1
  ret void
}

; Multi-element typed memmove -> _c2go_typedMemmoveArray (4-arg with count).
define void @copy_array(ptr %dst, ptr %src) {
  call void @llvm.memmove.p0.p0.i64(ptr %dst, ptr %src, i64 240, i1 false), !c2go.elem.type !2
  ret void
}

declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1)

; Both helpers must reference the SAME attribute group (LLVM dedupes identical
; groups), and that group must include gc-leaf-function.
; Singleton - gc-leaf-function attribute required.
; CHECK: declare {{.*}}void @_c2go_typedmemmove(ptr, ptr, ptr) [[ATTRS:#[0-9]+]]

; Array - same attribute group required.
; CHECK: declare {{.*}}void @_c2go_typedMemmoveArray(ptr, ptr, ptr, i64) [[ATTRS]]

; The shared attribute group must contain gc-leaf-function. A failure here
; means the attribute was dropped on the typed-memmove helpers without also
; dropping the //go:nosplit annotation on the Go side. The two sides must
; move together - never mismatch.
; CHECK: attributes [[ATTRS]] = {{.*}}"gc-leaf-function"{{.*}}

!1 = !{!"S", i64 1}
!2 = !{!"S", i64 10}
!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
