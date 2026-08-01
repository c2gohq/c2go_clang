; LLVM's leading \01 mangling escape is not part of the logical C symbol.
; Final auditing must still recognize an unresolved escaped libc declaration
; and fail before SelectionDAG.
;
; RUN: not opt < %s -passes=c2go-libcall-routing -S -o /dev/null 2>&1 | FileCheck %s

target triple = "aarch64-unknown-linux-goabi"

declare ptr @"\01memchr"(ptr, i32, i64)

define ptr @use_memchr(ptr %p, i32 %c, i64 %n) {
  %r = call ptr @"\01memchr"(ptr %p, i32 %c, i64 %n)
  ret ptr %r
}

; CHECK: error: c2go libc call 'memchr' has no direct-GoABI0 c2go_linkname route

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
