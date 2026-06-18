; A GoABI0 call to an internal function carries the per-call-site
; c2go-reg-return attribute: its result comes back in a register (X0), not in a
; GoABI0 stack-return slot. RewriteStatepointsForGC wraps the call in a
; gc.statepoint and propagates c2go-reg-return onto it, but statepoint lowering
; leaves the CallBase null, so the AArch64 result-ABI decision must read the
; attribute from the call-site attribute set, not from the CallBase. Reading it
; wrong takes the result from a stack slot the register-returning callee never
; wrote (garbage). The result must be taken from R0, with no post-call stack
; load of the result slot.
;
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare goabi0cc i32 @internal_callee(ptr addrspace(1)) "c2go-reg-return"
declare goabi0cc void @sink(i32)

; CHECK-LABEL: TEXT ·caller(SB)
; The reg-return result is consumed directly out of R0 after the call; there is
; NO MOV{,W,WU} <n>(R8)/<n>(RSP) reading a GoABI0 stack-return slot.
; CHECK: CALL ·internal_callee(SB)
; CHECK-NOT: MOV{{W?U?}} {{[0-9]+}}(R8)
define goabi0cc i32 @caller(ptr addrspace(1) %obj) "c2go-reg-return" gc "c2go-gc" {
entry:
  ; %obj is live across the call -> RS4GC spills/relocates it (statepoint).
  %r = call goabi0cc i32 @internal_callee(ptr addrspace(1) %obj) "c2go-reg-return"
  call goabi0cc void @sink(i32 %r)
  ret i32 %r
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
