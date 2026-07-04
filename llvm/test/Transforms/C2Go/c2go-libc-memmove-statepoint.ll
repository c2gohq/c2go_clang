; Helper-classification invariant for C2GoMemcpyTyping lowerings, end-to-end
; through RS4GC. Two buckets coexist in the same module and must travel through
; the GC pipeline differently:
;
;   (1) Typed-copy helpers (_c2go_typedmemmove / _c2go_typedMemmoveArray) carry
;       gc-leaf-function because their Go-side shims are //go:nosplit. RS4GC
;       must skip the statepoint wrap.
;
;   (2) libc.Memmove is NOT gc-leaf-function - it is a real non-leaf libc copy
;       path and RS4GC must statepoint-wrap it. AS1-to-AS0 boundary safety is
;       preserved by the c2go-gc strategy: RS4GC's base-pointer search walks
;       across the addrspacecast and still sees the AS1 root, so the
;       caller-side managed pointer is spilled and relocated correctly.
;
; Removing gc-leaf-function from the typed helpers - or adding it to
; libc.Memmove - flips the checks below. That is the mistake this test catches.
;
; RUN: opt < %s -passes='c2go-memcpy-typing,c2go-gc-setup,rewrite-statepoints-for-gc' -S | FileCheck %s

target triple = "arm64-unknown-none-goabi"

@c2go.typeinfo.N = external global i8

declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

; A managed-memory function with both flavors of copy:
;   - large untyped AS1 memcpy -> lowered to libc.Memmove (non-leaf, must be
;     wrapped in gc.statepoint by RS4GC).
;   - typed singleton AS1 memcpy (!c2go.elem.type + typeinfo) -> lowered to
;     _c2go_typedmemmove (gc-leaf, must NOT be wrapped).
;
; CHECK-LABEL: define void @both_paths{{.*}} gc "c2go-gc"
;
; libc.Memmove is the real non-leaf path: it must end up under a statepoint.
; CHECK: gc.statepoint{{.*}}@"github.com/c2gohq/c2go_libc.memmove"
;
; The typed helper is a leaf and must NOT be wrapped in a statepoint.
; CHECK-NOT: gc.statepoint{{.*}}@_c2go_typedmemmove
define void @both_paths(ptr addrspace(1) %dst, ptr addrspace(1) %src) {
  ; (1) untyped large AS1 memcpy -> libc.Memmove (statepoint-wrapped).
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dst, ptr addrspace(1) %src, i64 4096, i1 false)
  ; (2) typed singleton AS1 memcpy -> _c2go_typedmemmove (leaf, skip-wrap).
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dst, ptr addrspace(1) %src, i64 24, i1 false), !c2go.elem.type !1
  ret void
}

; The libc.Memmove declaration must NOT carry gc-leaf-function on its line - it
; is a real non-leaf libc copy path and RS4GC needs to wrap it. The pattern
; below asserts the declaration line does not mention the attribute.
; CHECK-NOT: declare {{.*}}@"github.com/c2gohq/c2go_libc.memmove"{{.*}}gc-leaf-function

; The typed helper declaration must still carry gc-leaf-function via its
; attribute group.
; CHECK: declare {{.*}}void @_c2go_typedmemmove(ptr, ptr, ptr) [[TYPED_ATTRS:#[0-9]+]]
; CHECK: attributes [[TYPED_ATTRS]] = {{.*}}"gc-leaf-function"{{.*}}

!1 = !{!"N", i64 1}
!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
