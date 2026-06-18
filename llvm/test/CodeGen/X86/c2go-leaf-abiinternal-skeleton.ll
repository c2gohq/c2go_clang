; X86 NOSPLIT leaf eligibility pass, gated solely by the c2go.goabi module flag
; (mirroring the AArch64 backend). When the flag is on, an eligible internal leaf
; has its calling convention flipped to c2goabiinternalcc and its direct call
; sites are rewritten in lockstep; external boundary symbols stay at goabi0cc.
; When the flag is absent the pass is a no-op and every function and call site
; stays at goabi0cc. Emergency off-switch: -mllvm -c2go-disable=leaf-abi.
;
; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -S 2>&1 | FileCheck %s --check-prefix=FLAG-ON
;
; The off-gate run strips the c2go.goabi flag (emptying the module.flags list)
; before opt, so the pass sees the flag missing at its entry gate and must be a
; no-op. The sed must run before opt; otherwise it would only erase the flag from
; already-flipped IR and would not exercise the off-gate path.
;
; RUN: sed -e '/c2go.goabi/d' \
; RUN:     -e 's/!llvm.module.flags = !{!0}/!llvm.module.flags = !{}/' \
; RUN:     %s | opt -mtriple=x86_64-unknown-linux-gnu \
; RUN:         -passes=x86-c2go-leaf-abi -S 2>&1 \
; RUN:     | FileCheck %s --check-prefix=GATED-OFF

target triple = "x86_64-unknown-linux-gnu"

; Eligible internal leaf: small, static, has c2go-reg-return, makes no calls.
; With the flag on it is flipped from goabi0cc to c2goabiinternalcc.
; FLAG-ON: define internal c2goabiinternalcc i32 @leaf_add
define internal goabi0cc i32 @leaf_add(i32 %a, i32 %b) #0 {
  %s = add i32 %a, %b
  ret i32 %s
}

; Internal caller: its direct call to leaf_add is rewritten to c2goabiinternalcc
; in lockstep, and the caller itself (independently eligible: no external or
; indirect calls) is flipped too.
; FLAG-ON: define internal c2goabiinternalcc i32 @caller_of_leaf
; FLAG-ON: call c2goabiinternalcc i32 @leaf_add
define internal goabi0cc i32 @caller_of_leaf(i32 %x) #0 {
  %r = call goabi0cc i32 @leaf_add(i32 %x, i32 1)
  ret i32 %r
}

; Boundary symbol: external linkage and no c2go-reg-return attribute, so it must
; not be flipped; real Go code reaches it through the declared GoABI0 stack layout.
; FLAG-ON: define goabi0cc i32 @boundary_fn
define goabi0cc i32 @boundary_fn(i32 %a) {
  ret i32 %a
}

; With the flag absent the pass is a no-op: all three functions and the call site
; stay at goabi0cc with no c2goabiinternalcc anywhere.
; GATED-OFF: define internal goabi0cc i32 @leaf_add
; GATED-OFF: define internal goabi0cc i32 @caller_of_leaf
; GATED-OFF: define goabi0cc i32 @boundary_fn
; GATED-OFF-NOT: c2goabiinternalcc

attributes #0 = { "c2go-reg-return" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
