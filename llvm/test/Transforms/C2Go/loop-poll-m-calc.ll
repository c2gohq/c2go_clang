; For a light body and a known-large trip count, the poll period M hits the
; upper clamp (1 << 16 = 65536), so the modulo lowers to `and i64 %cnt, 65535`.
;
; RUN: opt < %s -passes=c2go-loop-poll -c2go-loop-poll=1 -c2go-loop-poll-target-ns=10000000 -S | FileCheck %s

target triple = "aarch64-unknown-linux-gnu"

; CHECK-LABEL: define void @light_known_large(ptr %p)
; CHECK: and i64 %c2go.lp.cnt.new, 65535
; Call-site CC must be goabi0cc to match the GoABI0 declaration of the
; c2go-libc.Gosched bridge.
; CHECK: call goabi0cc void @"github.com/c2gohq/c2go_libc.Gosched"()
define void @light_known_large(ptr %p) #0 {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, 100000000
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

attributes #0 = { "c2go-c-name"="x" }

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
