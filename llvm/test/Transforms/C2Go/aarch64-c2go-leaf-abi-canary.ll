; Ineligibility edge cases for the AArch64 c2go-leaf-abi CC flip. The companion
; c2go-leaf-abi-newpm.ll covers the happy path (eligible flip / lockstep call
; rewrite / boundary skip / c2go.goabi gate); this file pins the negative cases
; so a regression that widens the eligibility predicate surfaces even when the
; happy-path tests stay green.
;
; Each negative case maps to one ineligibility reason:
;
;   (a) missing c2go-reg-return     -> GoABI0 boundary       - skip
;   (b) external linkage            -> not static            - skip
;   (b') address-taken              -> reachable indirectly  - skip
;   (c) calls an external decl      -> subtree unanalyzable  - skip
;   (c') direct recursion           -> subtree unanalyzable  - skip
;
; A control case (eligible_canary) asserts the pass STILL flips when it should,
; catching the inverse regression where an over-conservative tightening turns
; the pass into a no-op.
;
; First scenario: production-shape NewPM invocation with c2go.goabi ON.
;
; RUN: opt < %s -mtriple=aarch64-unknown-linux-gnu \
; RUN:     -passes=aarch64-c2go-leaf-abi -S | FileCheck %s
;
; Second RUN (OFF gate): sed strips the c2go.goabi flag before opt sees it, so
; the pass must take its entry-gate no-op path. No function may flip to
; c2goabiinternalcc, every input CC stays on goabi0cc, and the boundary
; symbol's external linkage is preserved. When c2go.goabi is absent (the only
; state a non-c2go build is ever in) the pass is a true no-op regardless of any
; other flag the IR carries. The sed drops the flag row and collapses the
; metadata list so it stays well-formed for opt.
;
; RUN: sed -e '/c2go.goabi/d' \
; RUN:     -e 's/!llvm.module.flags = !{!0}/!llvm.module.flags = !{}/' \
; RUN:     %s | opt -mtriple=aarch64-unknown-linux-gnu \
; RUN:         -passes=aarch64-c2go-leaf-abi -S \
; RUN:   | FileCheck %s --check-prefix=OFF \
; RUN:     --implicit-check-not=c2goabiinternalcc

target triple = "aarch64-unknown-linux-gnu"

; --- (control) eligible internal leaf path: must flip ------------------------
; A pass that silently no-ops fails the CHECK below.
; CHECK: define internal c2goabiinternalcc i32 @eligible_canary
define internal goabi0cc i32 @eligible_canary(i32 %a) #0 {
  %r = add i32 %a, 1
  ret i32 %r
}

; --- (a) missing c2go-reg-return path: must NOT flip -------------------------
; Without the attr the analyzer marks this a GoABI0 boundary symbol (the same
; predicate used for true c2go_extern boundaries), exercising the boundary skip
; path independent of linkage.
; CHECK: define internal goabi0cc i32 @no_reg_return_attr
define internal goabi0cc i32 @no_reg_return_attr(i32 %a) {
  %r = add i32 %a, 2
  ret i32 %r
}

; --- (b) external linkage path: must NOT flip --------------------------------
; Cross-TU callers lack the per-callsite metadata describing the private
; register convention, so external functions stay on GoABI0.
; CHECK: define goabi0cc i32 @external_linkage
define goabi0cc i32 @external_linkage(i32 %a) #0 {
  %r = add i32 %a, 3
  ret i32 %r
}

; --- (b') address-taken path: must NOT flip ----------------------------------
; An indirect call site cannot dispatch the private CC, so a function whose
; address escapes stays on the uniform GoABI0.
; CHECK: define internal goabi0cc i32 @addr_taken_leaf
define internal goabi0cc i32 @addr_taken_leaf(i32 %a) #0 {
  %r = add i32 %a, 4
  ret i32 %r
}
; Force address-taken via a global ptr. The variable itself stays as-is.
@addr_taken_ptr = internal global ptr @addr_taken_leaf

; --- (c) calls an external declaration path: must NOT flip -------------------
; The downstream subtree is not statically analyzable in this TU (the callee
; has no body), so the leaf-eligibility analyzer bails.
; CHECK: define internal goabi0cc i32 @calls_external
declare goabi0cc i32 @external_callee(i32)
define internal goabi0cc i32 @calls_external(i32 %a) #0 {
  %r = call goabi0cc i32 @external_callee(i32 %a)
  ret i32 %r
}

; --- (c') direct recursion path: must NOT flip -------------------------------
; The subtree-size DFS rejects recursion (it detects a function already on the
; stack), so a self-recursive function is never eligible even when every other
; predicate holds.
; CHECK: define internal goabi0cc i32 @self_recursive
define internal goabi0cc i32 @self_recursive(i32 %a) #0 {
  %t = icmp sgt i32 %a, 0
  br i1 %t, label %rec, label %base
rec:
  %sub = sub i32 %a, 1
  %r = call goabi0cc i32 @self_recursive(i32 %sub)
  ret i32 %r
base:
  ret i32 0
}

; Cross-check: across the whole module, eligible_canary is the ONLY function
; the pass may flip. A widened predicate would emit a second c2goabiinternalcc
; token above, failing one of the negative checks below.
; CHECK-NOT: define internal c2goabiinternalcc i32 @no_reg_return_attr
; CHECK-NOT: define {{.*}} c2goabiinternalcc i32 @external_linkage
; CHECK-NOT: define internal c2goabiinternalcc i32 @addr_taken_leaf
; CHECK-NOT: define internal c2goabiinternalcc i32 @calls_external
; CHECK-NOT: define internal c2goabiinternalcc i32 @self_recursive

; OFF-gate invariants: with c2go.goabi stripped, every function keeps its
; original CC. The implicit-check-not on the second RUN already forbids any
; flip; these positive checks additionally pin the original CC of all 6
; functions, so a silent rewrite to some other (non-c2goabiinternalcc) CC also
; fails. eligible_canary in particular stays on goabi0cc - without the
; c2go.goabi flag the pass cannot tell this is a c2go module.
; OFF: define internal goabi0cc i32 @eligible_canary
; OFF: define internal goabi0cc i32 @no_reg_return_attr
; OFF: define goabi0cc i32 @external_linkage
; OFF: define internal goabi0cc i32 @addr_taken_leaf
; OFF: define internal goabi0cc i32 @calls_external
; OFF: define internal goabi0cc i32 @self_recursive

attributes #0 = { "c2go-reg-return" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
