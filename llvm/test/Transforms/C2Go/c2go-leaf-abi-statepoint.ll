; The leaf-abi CC flip must see through gc.statepoint.
;
; In the -O2 codegen pipeline RewriteStatepointsForGC (RS4GC) runs BEFORE the
; backend leaf-abi pass. RS4GC rewrites `call @leaf` into
;   call @llvm.experimental.gc.statepoint(..., @leaf, ...)
; moving the real callee into the statepoint's called-function operand. The
; eligibility analysis must therefore look through statepoints (address-taken
; detection, direct-callee collection, unanalyzable-call detection, and the
; lockstep call-site rewrite); otherwise it would flip nothing post-RS4GC.
;
; This test runs the real production order and pins:
;   (a) the eligible internal leaf is flipped to c2goabiinternalcc,
;   (b) the statepoint call site is rewritten to c2goabiinternalcc in lockstep,
;   (c) a near-leaf making a TRUE INDIRECT call (via fn-ptr) stays on goabi0cc,
;   (d) an address-taken leaf (stored to a global) stays on goabi0cc.
;
; The statepoint must actually appear (else the test is vacuous) - checked
; explicitly below.
;
; RUN: opt < %s -mtriple=aarch64-unknown-linux-gnu \
; RUN:     -passes='rewrite-statepoints-for-gc,aarch64-c2go-leaf-abi' \
; RUN:     -S | FileCheck %s

target triple = "aarch64-unknown-linux-gnu"

; --- eligible internal leaf: static, has c2go-reg-return, no calls, NOT
;     gc-leaf-function (so RS4GC wraps calls to it in a statepoint). Takes a
;     pointer arg to exercise the register-ABI path.
; CHECK: define internal c2goabiinternalcc i32 @leaf_load
define internal goabi0cc i32 @leaf_load(ptr %p) #0 {
  %v = load i32, ptr %p, align 4
  ret i32 %v
}

; --- GC-tagged caller: a direct call to leaf_load. After RS4GC the call is a
;     gc.statepoint whose called-function operand is @leaf_load. The leaf-abi
;     pass must (1) flip leaf_load and (2) set the statepoint call-site CC. The
;     caller is itself eligible (static + c2go-reg-return + its only callee
;     leaf_load is analyzable in-TU), so it flips too.
;
; The statepoint really exists (test is non-vacuous) and its call site carries
; the register ABI, with @leaf_load as the called-function operand.
; CHECK: define internal c2goabiinternalcc i32 @caller_direct
; CHECK: call c2goabiinternalcc {{.*}}@llvm.experimental.gc.statepoint{{.*}}@leaf_load
define internal goabi0cc i32 @caller_direct(ptr %p) #0 gc "c2go-gc" {
  %r = call goabi0cc i32 @leaf_load(ptr %p)
  ret i32 %r
}

; --- NEGATIVE 1: a static near-leaf that makes a TRUE INDIRECT call through a
;     function pointer. Post-RS4GC the indirect call is a statepoint with no
;     resolvable callee, so the subtree is unanalyzable -> ineligible. Stays
;     goabi0cc.
; CHECK: define internal goabi0cc i32 @caller_indirect
define internal goabi0cc i32 @caller_indirect(ptr %fp) #0 gc "c2go-gc" {
  %r = call goabi0cc i32 %fp()
  ret i32 %r
}

; --- NEGATIVE 2: a static leaf whose address is taken (stored to a global).
;     The address-taken use makes it ineligible. Stays goabi0cc. (No GC tag:
;     not a callee shape; the point is purely the address-taken use below.)
; CHECK: define internal goabi0cc i32 @leaf_addr_taken
define internal goabi0cc i32 @leaf_addr_taken(ptr %p) #0 {
  %v = load i32, ptr %p, align 4
  ret i32 %v
}

@g_fp = global ptr null
define void @stash_addr() {
  store ptr @leaf_addr_taken, ptr @g_fp, align 8
  ret void
}

attributes #0 = { "c2go-reg-return" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
