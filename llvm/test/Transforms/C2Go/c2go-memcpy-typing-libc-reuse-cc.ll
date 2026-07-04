; When c2go-memcpy-typing reuses a pre-existing libc.Memmove declaration
; (from an earlier pass or a hand-written fixture) it must retrofit the
; calling convention to GoABI0 - symmetric with the typed-helper reuse paths.
; Without the retrofit the pass would emit a goabi0cc call against a default-CC
; declaration, and the Go-linker's ABI0 wrapper for libc.Memmove would read
; garbage from the stack on entry.
;
; Conversely libc.Memmove is NOT gc-leaf-function: it is a real non-leaf libc
; copy path and RS4GC must statepoint-wrap it. Adding gc-leaf-function here
; would wrongly skip-wrap it (see c2go-libc-memmove-statepoint.ll; flipping
; both tests in lock-step is the only way to intentionally change the bucket
; classification).
;
; RUN: opt < %s -passes=c2go-memcpy-typing -S | FileCheck %s

target triple = "arm64-unknown-none-goabi"

; Pre-existing libc.Memmove declaration with the default C calling convention.
; The pass must retrofit this to goabi0cc on reuse, not leave the default CC.
declare ptr @"github.com/c2gohq/c2go_libc.memmove"(ptr, ptr, i64)

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

; Trigger the fallback (libc.Memmove) path: no !c2go.elem.type and size
; (4096) exceeds the inline-byte threshold (64), so the pass routes the
; intrinsic to the libc.Memmove Go-side call.
define void @fallback_libc(ptr %dst, ptr %src) {
  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 4096, i1 false)
  ret void
}

; The declaration line must emit goabi0cc after the reuse-path retrofit.
; CHECK: declare goabi0cc ptr @"github.com/c2gohq/c2go_libc.memmove"(ptr, ptr, i64)

; The call site must also be goabi0cc, pinned here so a regression in either
; direction fails the test.
; CHECK: call goabi0cc ptr @"github.com/c2gohq/c2go_libc.memmove"(ptr {{[^,]+}}, ptr {{[^,]+}}, i64 4096)

; Reverse invariant: libc.Memmove must NOT acquire gc-leaf-function - it is
; the genuinely-non-leaf bucket (see c2go-libc-memmove-statepoint.ll). The
; check is line-anchored so it cannot accidentally match the attribute group
; on some other function.
; CHECK-NOT: declare {{.*}}@"github.com/c2gohq/c2go_libc.memmove"{{.*}}gc-leaf-function

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
