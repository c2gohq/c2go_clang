; Optimizer-synthesized libc calls use the package-local suffix when the raw
; c2go_linkname target belongs to c2go.pkgpath. The raw target remains on the
; function attribute for manifest/audit identity. An unrelated definition with
; the same C spelling is not claimed unless it carries matching route attrs.
;
; RUN: opt < %s -passes=c2go-libcall-routing -S | FileCheck %s

target triple = "aarch64-unknown-linux-goabi"

declare i64 @strlen(ptr)
declare i32 @"\01memcmp"(ptr, ptr, i64)
declare void @"\01dispatch"()
declare double @llvm.sin.f64(double)

define goabi0cc void @dispatch() #0 {
  ret void
}

define goabi0cc double @sin(double %x) #1 {
  ret double %x
}

define i64 @use_strlen(ptr %s) {
  %n = call i64 @strlen(ptr %s)
  ret i64 %n
}

define i32 @use_memcmp(ptr %a, ptr %b, i64 %n) {
  %r = call i32 @"\01memcmp"(ptr %a, ptr %b, i64 %n)
  ret i32 %r
}

define void @use_dispatch() {
  call void @"\01dispatch"()
  ret void
}

define double @use_sin(double %x) {
  %r = call double @llvm.sin.f64(double %x)
  ret double %r
}

define i32 @strcmp(ptr %a, ptr %b) {
  ret i32 0
}

define i32 @use_strcmp(ptr %a, ptr %b) {
  %r = call i32 @strcmp(ptr %a, ptr %b)
  ret i32 %r
}

; CHECK: declare goabi0cc i64 @strlen(ptr) #[[STR:[0-9]+]]
; CHECK: declare goabi0cc i32 @memcmp(ptr, ptr, i64) #[[MEMCMP:[0-9]+]]
; CHECK: define goabi0cc void @dispatch() #[[DISPATCH:[0-9]+]]
; CHECK: define goabi0cc double @sin(double %x) #[[SIN:[0-9]+]]
; CHECK-LABEL: define i64 @use_strlen(
; CHECK: call goabi0cc i64 @strlen(ptr %s) #[[NOBUILTIN:[0-9]+]]
; CHECK-LABEL: define i32 @use_memcmp(
; CHECK: call goabi0cc i32 @memcmp(ptr %a, ptr %b, i64 %n) #[[NOBUILTIN]]
; CHECK-LABEL: define void @use_dispatch(
; CHECK: call goabi0cc void @dispatch() #[[NOBUILTIN]]
; CHECK-LABEL: define double @use_sin(
; CHECK: call goabi0cc double @sin(double %x) #[[NOBUILTIN]]
; CHECK: define i32 @strcmp(ptr %a, ptr %b)
; CHECK-LABEL: define i32 @use_strcmp(
; CHECK: call i32 @strcmp(ptr %a, ptr %b)
; CHECK: attributes #[[STR]] = { "c2go-c-name"="strlen" "c2go-linkname"="example.com/lib.strlen" "c2go-linkname-abi0" }
; CHECK: attributes #[[MEMCMP]] = { "c2go-c-name"="memcmp" "c2go-linkname"="example.com/lib.memcmp" "c2go-linkname-abi0" }
; CHECK: attributes #[[DISPATCH]] = { "c2go-boundary" "c2go-c-name"="dispatch" "c2go-linkname"="example.com/lib.dispatch" "c2go-linkname-abi0" }
; CHECK: attributes #[[SIN]] = { "c2go-boundary" "c2go-c-name"="sin" "c2go-linkname"="example.com/lib.sin" "c2go-linkname-abi0" }
; CHECK: attributes #[[NOBUILTIN]] = { nobuiltin }
; CHECK-NOT: @"example.com/lib.strlen"
; CHECK-NOT: @"\01memcmp"
; CHECK-NOT: @"\01dispatch"

attributes #0 = { "c2go-boundary" "c2go-c-name"="dispatch" }
attributes #1 = { "c2go-boundary" "c2go-c-name"="sin" }

!llvm.module.flags = !{!0, !1}
!c2go.libcall.routes = !{!2, !3, !4, !5, !6}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.pkgpath", !"example.com/lib"}
!2 = !{!"strlen", !"example.com/lib.strlen"}
!3 = !{!"strcmp", !"example.com/lib.strcmp"}
!4 = !{!"memcmp", !"example.com/lib.memcmp"}
!5 = !{!"dispatch", !"example.com/lib.dispatch"}
!6 = !{!"sin", !"example.com/lib.sin"}
