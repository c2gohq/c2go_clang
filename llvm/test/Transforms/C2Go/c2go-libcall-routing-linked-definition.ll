; After whole-package bitcode linking, a c2go libc definition can satisfy the
; canonical symbol that an optimizer synthesized in another translation unit.
; The matching c2go-c-name/c2go-linkname attributes distinguish that definition
; from unrelated user code and let the pass repair the synthesized call's ABI.
;
; RUN: opt < %s -passes=c2go-libcall-routing -S | FileCheck %s

target triple = "aarch64-unknown-linux-goabi"

define goabi0cc i64 @strlen(ptr %s) #0 {
  ret i64 0
}

define i64 @use_linked_strlen(ptr %s) {
  %n = call i64 @strlen(ptr %s)
  ret i64 %n
}

; A libc-like definition without matching c2go route attributes remains
; ordinary program code, even when a route with the same C name exists.
define i32 @strcmp(ptr %a, ptr %b) {
  ret i32 0
}

define i32 @use_local_strcmp(ptr %a, ptr %b) {
  %r = call i32 @strcmp(ptr %a, ptr %b)
  ret i32 %r
}

attributes #0 = { "c2go-c-name"="strlen" "c2go-linkname"="example.com/lib.strlen" }

; CHECK: define goabi0cc i64 @strlen(ptr %s)
; CHECK-LABEL: define i64 @use_linked_strlen(
; CHECK: call goabi0cc i64 @strlen(ptr %s)
; CHECK: define i32 @strcmp(ptr %a, ptr %b)
; CHECK-LABEL: define i32 @use_local_strcmp(
; CHECK: call i32 @strcmp(ptr %a, ptr %b)

!llvm.module.flags = !{!0}
!c2go.libcall.routes = !{!1, !2}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{!"strlen", !"example.com/lib.strlen"}
!2 = !{!"strcmp", !"example.com/lib.strcmp"}
