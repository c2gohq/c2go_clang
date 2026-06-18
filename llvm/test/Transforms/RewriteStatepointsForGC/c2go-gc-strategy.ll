; The "c2go-gc" GCStrategy must be recognized by RewriteStatepointsForGC.
; Unlike statepoint-example (which tracks only addrspace(1)), c2go-gc tracks
; BOTH addrspace(0) and addrspace(1) pointers: the Go goroutine stack is
; movable, so every live pointer is a potential root that copystack must
; relocate (isGCManagedPointer returns nullopt -> value_or(true), so all
; pointers are tracked). A relocate preserves the address space of its input.
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -spp-rematerialization-threshold=0 -S | FileCheck %s

declare void @foo()

; A managed (addrspace(1)) pointer live across a call gets a statepoint +
; relocate; the relocate result stays in addrspace(1).
define ptr addrspace(1) @managed_live_across_call(ptr addrspace(1) %obj) gc "c2go-gc" {
; CHECK-LABEL: @managed_live_across_call
entry:
; CHECK: gc.statepoint
; CHECK: %obj.relocated = call coldcc ptr addrspace(1)
  call void @foo() [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret ptr addrspace(1) %obj
}

; A plain addrspace(0) C pointer is ALSO tracked (it may point into the
; movable stack), so it gets a relocate too - and the relocate stays in
; addrspace(0).
define ptr @unmanaged_relocated(ptr %p) gc "c2go-gc" {
; CHECK-LABEL: @unmanaged_relocated
entry:
; CHECK: gc.statepoint
; CHECK: %p.relocated = call coldcc ptr @llvm.experimental.gc.relocate.p0
  call void @foo() [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret ptr %p
}
