; For an unknown (data-dependent) trip count, the poll period M falls back to a
; bounded value (1024), so the modulo lowers to `and i64 %cnt, 1023`. The
; period is then independent of TTI noise and stays tight enough to preserve a
; ~10 ms cooperative pause.
;
; RUN: opt < %s -passes=c2go-loop-poll -c2go-loop-poll=1 -S | FileCheck %s

target triple = "aarch64-unknown-linux-gnu"

; CHECK-LABEL: define void @unknown_tc(ptr %p)
; CHECK: and i64 %c2go.lp.cnt.new, 1023
; Call-site CC must be goabi0cc to match the GoABI0 declaration of the
; c2go-libc.Gosched bridge.
; CHECK: call goabi0cc void @"github.com/c2go_project/c2go_libc.Gosched"()
define void @unknown_tc(ptr %p) #0 {
entry:
  br label %loop
loop:
  %cur = phi ptr [ %p, %entry ], [ %next, %loop ]
  %v = load volatile i8, ptr %cur
  %nz = icmp ne i8 %v, 0
  %next = getelementptr inbounds i8, ptr %cur, i64 1
  br i1 %nz, label %loop, label %exit
exit:
  ret void
}

attributes #0 = { "c2go-c-name"="x" }

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
