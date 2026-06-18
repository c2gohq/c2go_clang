; A managed addrspace(1) pointer live across a safepoint call, under the c2go-gc
; strategy, is spilled by RewriteStatepointsForGC and drives a Go-format locals
; pointer map in the Plan 9 (.s) output: PCDATA $1 is emitted before the CALL (Go
; reads the stack map at the call PC) and a gclocals FUNCDATA bitmap is attached.
;
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @runtime_safepoint()

; %obj is live across the safepoint: its spill slot is marked in the locals map
; (bitmap index >= 1), while the non-pointer alloca %slot is not. The map takes
; effect at the call, so PCDATA precedes the CALL.
; CHECK-LABEL: TEXT ·c2go_demo(SB)
; CHECK:      PCDATA $1, $1
; CHECK-NEXT: CALL ·runtime_safepoint(SB)
; CHECK:      FUNCDATA $1, gclocals·{{[0-9a-f]+}}(SB)
define ptr addrspace(1) @c2go_demo(ptr addrspace(1) %obj) gc "c2go-gc" {
entry:
  %slot = alloca i64, align 8
  store i64 0, ptr %slot, align 8
  call void @runtime_safepoint() [ "deopt"() ]
  ret ptr addrspace(1) %obj
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
