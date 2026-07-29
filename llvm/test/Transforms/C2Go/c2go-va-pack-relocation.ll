; Vararg packs contain stack-interior pointers: argptrs[i] points at the
; corresponding c2go.va.slot in the caller frame. The statepoint pipeline must
; zero-initialize every pointer-bearing pack field and fold alloca relocates so
; the target can expose those fields in the Go locals bitmap at a growing call.
;
; RUN: opt < %s \
; RUN:   -passes='c2go-gc-setup,rewrite-statepoints-for-gc,c2go-fold-alloca-relocates' \
; RUN:   -S | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @consume(ptr)

; CHECK-LABEL: define void @va_pack(
; CHECK: entry:
; CHECK: %pack = alloca [2 x ptr], align 8, !c2go.ptr.managed ![[MANAGED:[0-9]+]], !c2go.va.pack ![[PACK:[0-9]+]]
; CHECK-NEXT: store ptr null, ptr %pack, align 8
; CHECK-NEXT: %[[SECOND:[^ ]+]] = getelementptr inbounds i8, ptr %pack, i64 8
; CHECK-NEXT: store ptr null, ptr %[[SECOND]], align 8
; CHECK: gc.statepoint{{.*}}@consume
; CHECK-NOT: call {{.*}}@llvm.experimental.gc.relocate
; CHECK: ret void
define void @va_pack(ptr %p) #0 gc "c2go-gc" {
entry:
  %pack = alloca [2 x ptr], align 8, !c2go.ptr.managed !1, !c2go.va.pack !2
  %slot = alloca ptr, align 8, !c2go.va.pack !2
  store ptr %p, ptr %slot, align 8
  %elt0 = getelementptr inbounds [2 x ptr], ptr %pack, i64 0, i64 0
  %elt1 = getelementptr inbounds [2 x ptr], ptr %pack, i64 0, i64 1
  store ptr %slot, ptr %elt0, align 8
  store ptr null, ptr %elt1, align 8
  call void @consume(ptr %pack) [ "deopt"() ]
  ret void
}

attributes #0 = { noinline optnone }

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{!"c2go.va"}
!2 = !{}
