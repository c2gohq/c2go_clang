; Verify that optimizer-synthesized raw libc declarations are redirected to
; the frontend-preserved c2go_linkname target and normalized to GoABI0.
;
; RUN: opt < %s -passes=c2go-libcall-routing -S | FileCheck %s

target triple = "aarch64-unknown-linux-goabi"

declare i64 @strlen(ptr)
declare i32 @puts(ptr) #0
declare i32 @"example.com/lib.puts"(ptr)
declare i32 @memcmp(ptr, ptr, i64)

define i64 @use_strlen(ptr %s) {
  %n = tail call i64 @strlen(ptr %s)
  ret i64 %n
}

define i32 @use_puts(ptr %s) {
  %r = call i32 @puts(ptr %s)
  ret i32 %r
}

; A real definition with a libc-like name must not be redirected.
define i32 @strcmp(ptr %a, ptr %b) {
  ret i32 0
}

define i32 @use_unmapped(ptr %a, ptr %b) {
  %r = call i32 @memcmp(ptr %a, ptr %b, i64 1)
  ret i32 %r
}

attributes #0 = { nounwind memory(argmem: read) }

; The declaration-only raw symbols disappear. The pre-existing target absorbs
; inferred raw-libcall attributes, while the newly named target retains its
; original function type and gains the c2go route attributes.
; CHECK: declare goabi0cc i64 @"example.com/lib.strlen"(ptr) #[[STR:[0-9]+]]
; CHECK: declare goabi0cc i32 @"example.com/lib.puts"(ptr) #[[PUTS:[0-9]+]]
; CHECK: declare i32 @memcmp(

; CHECK-LABEL: define i64 @use_strlen(
; CHECK: tail call goabi0cc i64 @"example.com/lib.strlen"(ptr %s)
; CHECK-LABEL: define i32 @use_puts(
; CHECK: call goabi0cc i32 @"example.com/lib.puts"(ptr %s)

; Definitions and names without a route remain byte-for-byte in their own
; symbol world.
; CHECK: define i32 @strcmp(
; CHECK: call i32 @memcmp(
; CHECK: attributes #[[STR]] = { "c2go-c-name"="strlen" "c2go-linkname"="example.com/lib.strlen" }
; CHECK: attributes #[[PUTS]] = {
; CHECK-SAME: nounwind
; CHECK-SAME: memory(argmem: read)
; CHECK-SAME: "c2go-c-name"="puts"
; CHECK-SAME: "c2go-linkname"="example.com/lib.puts"
; CHECK-NOT: !"c2go.cc.violations"

!llvm.module.flags = !{!0}
!c2go.libcall.routes = !{!1, !2, !3}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{!"strlen", !"example.com/lib.strlen"}
!2 = !{!"puts", !"example.com/lib.puts"}
!3 = !{!"strcmp", !"example.com/lib.strcmp"}
