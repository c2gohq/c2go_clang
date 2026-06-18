; NewPM entry point for the AArch64 c2go-leaf-abi CC flip. The same logic also
; runs in the production codegen pipeline as a legacy ModulePass; this test
; reaches it via opt -passes=aarch64-c2go-leaf-abi and checks the happy path:
;   (a) the pass name resolves,
;   (b) an eligible internal NOSPLIT-leaf has its CC flipped to
;       c2goabiinternalcc,
;   (c) the direct call site is rewritten in lockstep,
;   (d) a c2go-boundary (no c2go-reg-return) function stays on goabi0cc.
;
; The module flag c2go.goabi gates the pass on c2go-mode modules; without it
; the pass is a no-op.
;
; RUN: opt < %s -mtriple=aarch64-unknown-linux-gnu \
; RUN:     -passes=aarch64-c2go-leaf-abi -S | FileCheck %s

target triple = "aarch64-unknown-linux-gnu"

; --- eligible internal leaf: small, static, has c2go-reg-return, no calls ----
; CHECK: define internal c2goabiinternalcc i32 @leaf_add
define internal goabi0cc i32 @leaf_add(i32 %a, i32 %b) #0 {
  %s = add i32 %a, %b
  ret i32 %s
}

; --- internal caller: the direct call to leaf_add is rewritten to
;     c2goabiinternalcc in lockstep with the callee's CC flip. The caller is
;     itself eligible (static + c2go-reg-return + strict-leaf once the analysis
;     treats leaf_add as in-TU).
; CHECK: define internal c2goabiinternalcc i32 @caller_of_leaf
; CHECK: call c2goabiinternalcc i32 @leaf_add
define internal goabi0cc i32 @caller_of_leaf(i32 %x) #0 {
  %r = call goabi0cc i32 @leaf_add(i32 %x, i32 1)
  ret i32 %r
}

; --- boundary symbol: external linkage + no c2go-reg-return attr, must NOT be
;     flipped. The pipeline treats these as GoABI0-stack callees that real Go
;     code reaches through the declared ABI0 stack layout.
; CHECK: define goabi0cc i32 @boundary_fn
define goabi0cc i32 @boundary_fn(i32 %a) {
  ret i32 %a
}

attributes #0 = { "c2go-reg-return" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
