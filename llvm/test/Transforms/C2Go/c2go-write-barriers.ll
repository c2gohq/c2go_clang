; c2go-write-barriers inserts a Go hybrid write barrier for a store of a
; managed (addrspace(1)) pointer into managed heap memory, but NOT for a store
; into a current-frame local root slot (an alloca).
;
; RUN: opt < %s -passes=c2go-write-barriers -S | FileCheck %s

%struct.N = type { ptr addrspace(1), i32 }

; Store into p->next: dst underlying object is the parameter %p (heap), so this
; needs a barrier - inline writeBarrier.enabled check + slow-path shim call.
; CHECK-LABEL: define void @store_to_heap(ptr addrspace(1) %p, ptr addrspace(1) %q)
; CHECK: %wb.enabled = load i32, ptr @runtime.writeBarrier
; CHECK: icmp ne i32 %wb.enabled, 0
; CHECK: call {{.*}}void @_c2go_writePtr(ptr addrspace(1) %next, ptr addrspace(1) %q)
; CHECK: store ptr addrspace(1) %q, ptr addrspace(1) %next
define void @store_to_heap(ptr addrspace(1) %p, ptr addrspace(1) %q) gc "c2go-gc" {
entry:
  %next = getelementptr inbounds %struct.N, ptr addrspace(1) %p, i32 0, i32 0
  store ptr addrspace(1) %q, ptr addrspace(1) %next, align 8
  ret void
}

; Store into a local root slot (alloca): no barrier - the GC scans the slot via
; the stack map. The store must remain a plain store with no writeBarrier load.
; CHECK-LABEL: define void @store_to_local_root(ptr addrspace(1) %q)
; CHECK-NOT: @runtime.writeBarrier
; CHECK-NOT: @_c2go_writePtr
; CHECK: store ptr addrspace(1) %q, ptr %root
define void @store_to_local_root(ptr addrspace(1) %q) gc "c2go-gc" {
entry:
  %root = alloca ptr addrspace(1), align 8
  store ptr addrspace(1) %q, ptr %root, align 8
  ret void
}

; #646 hole-2: a store of a managed pointer into a GLOBAL slot (an AS0 address
; in the data segment — including the Go-owned ceded globals) IS barriered:
; globals are GC roots re-scanned only at mark start, so a store during
; concurrent mark must shade the value exactly like a heap write. The AS0 slot
; is addrspacecast for the AS1-typed shim call; the fast-path store stays AS0.
; CHECK-LABEL: define void @store_to_global(ptr addrspace(1) %q)
; CHECK: load i32, ptr @runtime.writeBarrier
; CHECK: call {{.*}}void @_c2go_writePtr(ptr addrspace(1) addrspacecast (ptr @g_single to ptr addrspace(1)), ptr addrspace(1) %q)
; CHECK: store ptr addrspace(1) %q, ptr @g_single
@g_single = internal global ptr addrspace(1) null, align 8
define void @store_to_global(ptr addrspace(1) %q) gc "c2go-gc" {
entry:
  store ptr addrspace(1) %q, ptr @g_single, align 8
  ret void
}

; The c2go-libc barrier shim, declared with GoABI0 + addrspace(1) params so
; RewriteStatepointsForGC keeps the managed operands live across the call.
; CHECK: declare {{.*}}void @_c2go_writePtr(ptr addrspace(1), ptr addrspace(1)) [[WP_ATTRS:#[0-9]+]]

; The shim must carry gc-leaf-function (backed by Go-side //go:nosplit). A
; failure here means the attribute was dropped on the shim without also
; dropping the //go:nosplit annotation on the Go side. Keep both or remove
; both - never mismatch.
; CHECK: attributes [[WP_ATTRS]] = {{.*}}"gc-leaf-function"{{.*}}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
