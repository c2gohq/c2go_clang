; c2go-loop-poll injects a cooperative-preemption poll into a call-free natural
; loop of a c2go-managed function, and leaves other loops alone:
;   - non-c2go-managed function (no c2go-c-name)  -> not touched
;   - c2go-boundary function                      -> not touched
;   - loop body already has a real call           -> not touched
;
; RUN: opt < %s -passes=c2go-loop-poll -c2go-loop-poll=1 -S | FileCheck %s

target triple = "aarch64-unknown-linux-gnu"

declare void @other()

; CHECK-LABEL: define void @c2go_nocall_loop(ptr %p)
; CHECK: c2go.lp.cnt = alloca i64
; CHECK: store i64 0, ptr %c2go.lp.cnt
; The injected call site must be goabi0cc - the Go-linker generates an ABI0
; entry on the c2go-libc.Gosched bridge, and a default-CC call site would read
; garbage on entry.
; CHECK: call goabi0cc void @"github.com/c2go_project/c2go_libc.Gosched"()
define void @c2go_nocall_loop(ptr %p) #0 {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, 1000000
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; CHECK-LABEL: define void @nonc2go_nocall_loop(ptr %p)
; CHECK-NOT: c2go.lp.cnt
; CHECK-NOT: c2go_libc.Gosched(
define void @nonc2go_nocall_loop(ptr %p) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, 1000000
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; CHECK-LABEL: define void @boundary_nocall_loop(ptr %p)
; CHECK-NOT: c2go.lp.cnt
; CHECK-NOT: c2go_libc.Gosched(
define void @boundary_nocall_loop(ptr %p) #1 {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, 1000000
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; CHECK-LABEL: define void @c2go_call_loop(ptr %p)
; CHECK-NOT: c2go.lp.cnt
; CHECK-NOT: c2go_libc.Gosched(
define void @c2go_call_loop(ptr %p) #0 {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  call void @other()
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, 1000000
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

attributes #0 = { "c2go-c-name"="x" }
attributes #1 = { "c2go-c-name"="y" "c2go-boundary" }

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
