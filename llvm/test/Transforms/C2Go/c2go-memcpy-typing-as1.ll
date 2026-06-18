; AS1 (managed) to AS0 helper-param address-space adaptation. A C aggregate
; copy `*managed_struct = *other_managed_struct` lowers to
; @llvm.memcpy.p1.p1.i64 with both Dst/Src in addrspace(1). The helpers
; (_c2go_typedmemmove / _c2go_typedMemmoveArray / libc.Memmove) keep AS0
; signatures because libc.Memmove is genuinely unmanaged. C2GoMemcpyTyping must
; emit an addrspacecast at each call site rather than passing an AS1 arg into an
; AS0 parameter (the IR verifier rejects that).
;
; RUN: opt < %s -passes=c2go-memcpy-typing -S | FileCheck %s
;
; Soundness, two buckets:
;   (1) Typed helpers _c2go_typedmemmove / _c2go_typedMemmoveArray are
;       gc-leaf-function (RS4GC skip-wrap) and Go-side //go:nosplit (no
;       morestack / no stack relocation across the call), so the AS1-to-AS0
;       cast loses no load-bearing info.
;   (2) libc.Memmove is NOT gc-leaf-function - it is a real non-leaf libc copy
;       path and RS4GC statepoint-wraps it. The AS1-to-AS0 cast at the boundary
;       stays sound via the c2go-gc strategy (the base-pointer search walks
;       across the addrspacecast and tracks the AS1 root). See
;       c2go-libc-memmove-statepoint.ll for the pinned RS4GC invariant.

target triple = "arm64-unknown-none-goabi"

@c2go.typeinfo.N = external global i8
@c2go.typeinfo.A = external global i8

; ---- Typed singleton: AS1 Dst+Src, !c2go.elem.type, count==1 ----
; CHECK-LABEL: define void @copy_single_as1
; CHECK: %[[DC:.*]] = addrspacecast ptr addrspace(1) %dst to ptr
; CHECK: %[[SC:.*]] = addrspacecast ptr addrspace(1) %src to ptr
; CHECK: call goabi0cc void @_c2go_typedmemmove(ptr {{[^,]+}}, ptr %[[DC]], ptr %[[SC]])
; CHECK-NOT: llvm.memcpy
define void @copy_single_as1(ptr addrspace(1) %dst, ptr addrspace(1) %src) {
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dst, ptr addrspace(1) %src, i64 24, i1 false), !c2go.elem.type !1
  ret void
}

; ---- Typed array: AS1 Dst+Src, !c2go.elem.type, count>1 ----
; CHECK-LABEL: define void @copy_array_as1
; CHECK: %[[ADC:.*]] = addrspacecast ptr addrspace(1) %dst to ptr
; CHECK: %[[ASC:.*]] = addrspacecast ptr addrspace(1) %src to ptr
; CHECK: call goabi0cc void @_c2go_typedMemmoveArray(ptr {{[^,]+}}, ptr %[[ADC]], ptr %[[ASC]], i64 10)
; CHECK-NOT: llvm.memcpy
define void @copy_array_as1(ptr addrspace(1) %dst, ptr addrspace(1) %src) {
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dst, ptr addrspace(1) %src, i64 240, i1 false), !c2go.elem.type !2
  ret void
}

; ---- Untyped fallback: AS1 Dst+Src, no metadata, large N -> libc.Memmove ----
; CHECK-LABEL: define void @copy_untyped_as1
; CHECK: %[[LDC:.*]] = addrspacecast ptr addrspace(1) %dst to ptr
; CHECK: %[[LSC:.*]] = addrspacecast ptr addrspace(1) %src to ptr
; CHECK: call goabi0cc ptr @"github.com/c2go_project/c2go_libc.Memmove"(ptr %[[LDC]], ptr %[[LSC]], i64 4096)
; CHECK-NOT: llvm.memcpy
define void @copy_untyped_as1(ptr addrspace(1) %dst, ptr addrspace(1) %src) {
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dst, ptr addrspace(1) %src, i64 4096, i1 false)
  ret void
}

; ---- Sanity: AS0 path must NOT addrspacecast (no-op when types match) ----
; CHECK-LABEL: define void @copy_single_as0
; CHECK-NOT: addrspacecast
; CHECK: call goabi0cc void @_c2go_typedmemmove(ptr {{[^,]+}}, ptr %dst, ptr %src)
define void @copy_single_as0(ptr %dst, ptr %src) {
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 24, i1 false), !c2go.elem.type !1
  ret void
}

declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

!1 = !{!"N", i64 1}
!2 = !{!"A", i64 10}
!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
