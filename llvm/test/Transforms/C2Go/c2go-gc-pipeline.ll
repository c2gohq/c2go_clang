; The late GC pipeline composes correctly:
;   c2go-write-barriers         (managed heap store -> inline check + shim)
;   c2go-gc-setup               (tag AS1-using function with gc "c2go-gc")
;   rewrite-statepoints-for-gc  (spill the managed pointer across the call)
;
; The write-barrier shim _c2go_writePtr carries gc-leaf-function (its Go-side
; shim is //go:nosplit), so RS4GC must NOT wrap it. RS4GC still wraps real
; safepoint calls like @runtime_safepoint (non-leaf declaration), proving the
; pipeline still works for non-leaf paths.
;
; RUN: opt < %s -passes='c2go-write-barriers,c2go-gc-setup,rewrite-statepoints-for-gc' -S | FileCheck %s

%struct.N = type { ptr addrspace(1), i32 }

declare void @runtime_safepoint()

; %p is a managed heap node; store q into p->next (needs a barrier), then a
; call across which %p stays live (RS4GC must relocate it). gc-setup tags @f
; (it uses managed pointers); the barrier check, the bare leaf shim call
; (RS4GC skips the statepoint wrap because of gc-leaf-function), the statepoint
; around @runtime_safepoint, and the relocate are all present.
; CHECK-LABEL: define ptr addrspace(1) @f(ptr addrspace(1) %p, ptr addrspace(1) %q) gc "c2go-gc"
; CHECK: load i32, ptr @runtime.writeBarrier
; CHECK: call goabi0cc void @_c2go_writePtr(ptr addrspace(1) %{{.*}}, ptr addrspace(1) %q){{( \[ "deopt"\(\) \])?}}
; CHECK-NOT: gc.statepoint{{.*}}@_c2go_writePtr
; CHECK: gc.statepoint{{.*}}@runtime_safepoint
; CHECK: relocated = call {{.*}}ptr addrspace(1)
define ptr addrspace(1) @f(ptr addrspace(1) %p, ptr addrspace(1) %q) {
entry:
  %next = getelementptr inbounds %struct.N, ptr addrspace(1) %p, i32 0, i32 0
  store ptr addrspace(1) %q, ptr addrspace(1) %next, align 8
  call void @runtime_safepoint() [ "deopt"() ]
  ret ptr addrspace(1) %p
}

; A function with no managed pointers must NOT be tagged (avoid RS4GC churn).
; CHECK-LABEL: define void @no_managed()
; CHECK-NOT: gc "c2go-gc"
define void @no_managed() {
  call void @runtime_safepoint() [ "deopt"() ]
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
