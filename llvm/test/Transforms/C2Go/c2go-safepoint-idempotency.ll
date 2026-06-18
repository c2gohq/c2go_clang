; c2go-safepoint is idempotent across re-runs.
;
; The pass instruments every potential-morestack call with an
; llvm.experimental.stackmap intrinsic immediately before the call. It runs
; twice in c2go mode (pipeline start, then optimizer last). On the second run
; the pre-existing stackmap from run 1 must be detected so we do not stack two
; stackmaps in front of the same call.
;
; Detection is not a pure adjacency check: the preceding stackmap must also
; carry an ID strictly less than the run-start watermark - only IDs emitted by
; a prior invocation satisfy that, because every freshly-emitted ID is
; >= watermark.
;
; This test feeds a module that already carries a watermark = 100 and a
; pre-existing stackmap with ID = 42 in front of a runtime.mallocgc call,
; mimicking run-1 output. Re-running the pass must NOT add a second stackmap.
;
; RUN: opt < %s -passes=c2go-safepoint -S | FileCheck %s

declare ptr @runtime.mallocgc(i64, ptr, i1)
declare void @llvm.experimental.stackmap(i64 immarg, i32 immarg, ...)
declare void @llvm.lifetime.start.p0(i64 immarg, ptr nocapture)
declare void @llvm.lifetime.end.p0(i64 immarg, ptr nocapture)
declare void @llvm.experimental.noalias.scope.decl(metadata)

; A function with a managed (!c2go.ptr.managed) alloca + a mallocgc call.
; A prior run already emitted a stackmap with ID 42 (< watermark 100). The
; second run must keep the original stackmap and emit NO additional one.
;
; The pass's zero-init path can still emit an entry `store ptr null, ...`
; tagged with !c2go.zeroinit on the slot - that is independent of stackmap
; idempotency. The invariant under test is that exactly ONE stackmap survives
; in front of the mallocgc call.
;
; CHECK-LABEL: define ptr @f()
; CHECK:       %slot = alloca ptr
; CHECK:       call void (i64, i32, ...) @llvm.experimental.stackmap(i64 42, i32 0,
; CHECK-NEXT:  %p = call ptr @runtime.mallocgc
; CHECK-NOT:   @llvm.experimental.stackmap
define ptr @f() {
entry:
  %slot = alloca ptr, !c2go.ptr.managed !1
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 42, i32 0, ptr %slot)
  %p = call ptr @runtime.mallocgc(i64 16, ptr null, i1 false)
  store ptr %p, ptr %slot
  ret ptr %p
}

; Negative control: a human-authored stackmap whose ID (9999) is >= the
; run-start watermark (100) is NOT recognised as a prior-run emission, so
; the pass adds its own stackmap and the resulting IR carries TWO stackmaps
; back-to-back in front of the call. This proves the ID-watermark check is
; doing real work; without it, any preceding stackmap would suppress emission
; and there would be exactly one stackmap, identical to @f.
;
; CHECK-LABEL: define ptr @g()
; CHECK:       call void (i64, i32, ...) @llvm.experimental.stackmap(i64 9999,
; CHECK-NEXT:  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 100,
; CHECK-NEXT:  %p = call ptr @runtime.mallocgc
define ptr @g() {
entry:
  %slot = alloca ptr, !c2go.ptr.managed !1
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 9999, i32 0, ptr %slot)
  %p = call ptr @runtime.mallocgc(i64 16, ptr null, i1 false)
  store ptr %p, ptr %slot
  ret ptr %p
}

; Watermark radius: the inliner / SROA / lifetime-marker passes can splice a
; llvm.lifetime.start (or any debug intrinsic) between a prior-run stackmap and
; the call it anchors. Pure adjacency would then miss the prior emission and
; stack a second stackmap at run 2. The detection walk must reverse-skip
; lifetime / debug / pseudo intrinsics and recognise the stackmap with ID 42
; (< watermark 100) as ours, so EXACTLY ONE stackmap survives in front of
; runtime.mallocgc.
;
; CHECK-LABEL: define ptr @h()
; CHECK:       call void (i64, i32, ...) @llvm.experimental.stackmap(i64 42, i32 0,
; CHECK-NEXT:  call void @llvm.lifetime.start
; CHECK-NEXT:  %p = call ptr @runtime.mallocgc
; CHECK-NOT:   @llvm.experimental.stackmap
define ptr @h() {
entry:
  %slot = alloca ptr, !c2go.ptr.managed !1
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 42, i32 0, ptr %slot)
  call void @llvm.lifetime.start.p0(i64 8, ptr %slot)
  %p = call ptr @runtime.mallocgc(i64 16, ptr null, i1 false)
  store ptr %p, ptr %slot
  ret ptr %p
}

; Watermark radius: the LLVM inliner emits
; llvm.experimental.noalias.scope.decl at the inlined call's insertion point.
; The intrinsic carries only metadata (no runtime semantics, no PC) and may sit
; between a prior-run stackmap and its anchor call. The watermark walk must
; reverse-skip it the same way it skips lifetime.start / dbg intrinsics,
; otherwise the second run double-stamps the call site. EXACTLY ONE stackmap
; must survive in front of the runtime.mallocgc call.
;
; CHECK-LABEL: define ptr @i()
; CHECK:       call void (i64, i32, ...) @llvm.experimental.stackmap(i64 42, i32 0,
; CHECK-NEXT:  call void @llvm.experimental.noalias.scope.decl
; CHECK-NEXT:  %p = call ptr @runtime.mallocgc
; CHECK-NOT:   @llvm.experimental.stackmap
define ptr @i() {
entry:
  %slot = alloca ptr, !c2go.ptr.managed !1
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 42, i32 0, ptr %slot)
  call void @llvm.experimental.noalias.scope.decl(metadata !3)
  %p = call ptr @runtime.mallocgc(i64 16, ptr null, i1 false)
  store ptr %p, ptr %slot
  ret ptr %p
}

!llvm.module.flags = !{!0, !2}
!0 = !{i32 2, !"c2go.goabi", i32 1}
; Watermark from a prior pass invocation: 100. Stackmap ID 42 < 100 so the
; pass classifies it as "ours" and skips re-emission.
!2 = !{i32 4, !"c2go.safepoint.next.id", i64 100}
!1 = !{}
!3 = !{!4}
!4 = distinct !{!4, !5, !"i: scope"}
!5 = distinct !{!5, !"i"}
