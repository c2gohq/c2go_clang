; c2go-libc's compiler-intercepted memory operations have no source-level
; c2go_linkname attribute. MemcpyTyping must still preserve the raw linkname
; route while resolving the physical symbol against c2go.pkgpath. Local names
; are marked nobuiltin so SelectionDAG cannot reinterpret the GoABI0 call as a
; platform-C libcall after GC lowering.
;
; RUN: opt < %s -passes='c2go-memcpy-typing,c2go-libcall-routing' -S | FileCheck %s

target triple = "aarch64-unknown-linux-goabi"

declare ptr @memcpy(ptr, ptr, i64)

define ptr @copy(ptr %dst, ptr %src, i64 %n) {
  %r = call ptr @memcpy(ptr %dst, ptr %src, i64 %n)
  ret ptr %r
}

; CHECK: declare goabi0cc ptr @memcpy(ptr, ptr, i64) #[[MEMCPY:[0-9]+]]
; CHECK-LABEL: define ptr @copy(
; CHECK: call goabi0cc ptr @memcpy(ptr %dst, ptr %src, i64 %n) #[[NOBUILTIN:[0-9]+]]
; CHECK: attributes #[[MEMCPY]] = { "c2go-c-name"="memcpy" "c2go-linkname"="github.com/c2gohq/c2go_libc.memcpy" "c2go-linkname-abi0" }
; CHECK: attributes #[[NOBUILTIN]] = { nobuiltin }
; CHECK: !c2go.libcall.routes = !{![[MEMCPY_ROUTE:[0-9]+]]}
; CHECK: ![[MEMCPY_ROUTE]] = !{!"memcpy", !"github.com/c2gohq/c2go_libc.memcpy"}
; CHECK-NOT: @"github.com/c2gohq/c2go_libc.memcpy"

!llvm.module.flags = !{!0, !1}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.pkgpath", !"github.com/c2gohq/c2go_libc"}
