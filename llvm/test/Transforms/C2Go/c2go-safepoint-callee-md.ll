; The safepoint-callee whitelist is module-driven.
;
; The whitelist of Go-runtime safepoint-bearing callees lives in the c2go
; named metadata c2go.safepoint.callees. The pass unions the built-in defaults
; with whatever the module declares, so a downstream tool (c2go-lto, opt-driven
; test, future helpers) can extend coverage without recompiling.
;
; This test exercises the metadata path with a callee - my.custom.safepoint -
; that is NOT in the built-in set. The pass must instrument the call to it
; because the module-supplied list extends the whitelist. A second callee -
; not_a_safepoint - is neither built-in nor module-listed and must not be
; instrumented, proving the extension is bounded (the pass does not just
; instrument every call).
;
; RUN: opt < %s -passes=c2go-safepoint -S | FileCheck %s

declare void @my.custom.safepoint()
declare void @not_a_safepoint()
declare void @use_addr(ptr)

; The managed alloca is a non-pointer / non-aggregate type so the entry-block
; classification routes the function through the safepoint-call path that
; consults the named-metadata-extended whitelist. The per-call-site full-
; coverage pipeline targets a different design space and would otherwise mask
; the whitelist behaviour asserted here.
;
; CHECK-LABEL: define void @f()
; CHECK:       call void (i64, i32, ...) @llvm.experimental.stackmap{{.*}}ptr %m
; CHECK-NEXT:  call void @my.custom.safepoint()
; CHECK-NOT:   call void (i64, i32, ...) @llvm.experimental.stackmap
; CHECK:       call void @use_addr(ptr %m)
; CHECK-NOT:   call void (i64, i32, ...) @llvm.experimental.stackmap
; CHECK:       call void @not_a_safepoint()
; CHECK-NOT:   call void (i64, i32, ...) @llvm.experimental.stackmap
define void @f() {
entry:
  %m = alloca i64, !c2go.ptr.managed !1
  call void @my.custom.safepoint()
  call void @use_addr(ptr %m)
  call void @not_a_safepoint()
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{}

; The named metadata extends the built-in callee whitelist by one entry.
; Each operand is an !{!"symbol"} MDNode. Built-in names (runtime.mallocgc,
; runtime.morestack, ...) need not be repeated here - the pass unions both
; sets at run start.
!c2go.safepoint.callees = !{!10}
!10 = !{!"my.custom.safepoint"}
