; InstCombine may fold an (addrspacecast -> GEP -> addrspacecast-back) chain on
; an AS1 base into the canonical (ptrtoaddr -> integer arith -> inttoptr) shape
; ("address arithmetic on managed pointer"). RS4GC's base-defining-value search
; must stay stable across this fold:
;
;   - The folded inttoptr terminal is treated as its OWN base (RS4GC's
;     findBaseDefiningValue treats inttoptr in an integral address space as a
;     defining base pointer).
;   - The AS1 ptrtoaddr source pointer is still tracked as a live AS1 GC value
;     across any non-leaf statepoint it crosses (the c2go-gc strategy tracks
;     both AS0 and AS1 roots).
;
; This test does NOT run InstCombine; it constructs the already-folded IR shape
; (canonical post-InstCombine residue) directly and asserts RS4GC's behavior on
; it. That keeps the test deterministic - independent of any future
; InstCombine fold-strategy change - while still pinning the downstream
; GC-pipeline contract the fold relies on for soundness.
;
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @runtime_safepoint()

; ---- Folded chain: ptrtoaddr(AS1) + add + inttoptr-to-AS1, live across a
;      non-leaf statepoint. Both the AS1 base source and the inttoptr terminal
;      round-trip through the GC pipeline cleanly: the function gets tagged
;      gc "c2go-gc", the safepoint becomes a gc.statepoint, and the live AS1
;      pointers get relocated.

; The function is tagged c2go-gc because it contains an AS1 pointer.
; CHECK-LABEL: define ptr addrspace(1) @fold_chain_as1(ptr addrspace(1) %base) gc "c2go-gc"
;
; The safepoint is wrapped, proving RS4GC built a base map for the AS1 live
; values that survive the call.
; CHECK: gc.statepoint{{.*}}@runtime_safepoint
;
; The AS1 base argument is live across the statepoint and relocated as an AS1
; GC pointer.
; CHECK: relocated = call {{.*}}ptr addrspace(1)
define ptr addrspace(1) @fold_chain_as1(ptr addrspace(1) %base) {
entry:
  ; Canonical InstCombine fold for an offset on an AS1 pointer:
  ; ptrtoaddr -> add -> inttoptr-back-to-AS1. RS4GC sees the inttoptr terminal
  ; as its own base and the AS1 source pointer separately as an AS1 live value.
  %addr   = ptrtoaddr ptr addrspace(1) %base to i64
  %addr2  = add i64 %addr, 24
  %folded = inttoptr i64 %addr2 to ptr addrspace(1)
  call void @runtime_safepoint() [ "deopt"() ]
  ; Return the AS1 base so it is live across the safepoint, forcing RS4GC to
  ; relocate it. The folded inttoptr is intentionally unused after the
  ; safepoint; using it would conflate the two invariants this test separates.
  ret ptr addrspace(1) %base
}

; ---- Second shape: the folded inttoptr terminal IS used after the safepoint.
;      The integer-arith chain pre-safepoint stays in AS0-integer space (no
;      live AS1 across the statepoint via this chain), and the post-safepoint
;      inttoptr is its OWN base - RS4GC treats it as a new root. This is the
;      "own-base" invariant.
;
;      We still keep AS1 %base alive across the statepoint so the function is
;      eligible for c2go-gc tagging (otherwise gc-setup would not promote it
;      and the test would not exercise RS4GC at all).

; CHECK-LABEL: define ptr addrspace(1) @fold_chain_own_base(ptr addrspace(1) %base) gc "c2go-gc"
; CHECK: gc.statepoint{{.*}}@runtime_safepoint
; CHECK: relocated = call {{.*}}ptr addrspace(1)
define ptr addrspace(1) @fold_chain_own_base(ptr addrspace(1) %base) {
entry:
  %a0 = ptrtoaddr ptr addrspace(1) %base to i64
  %a1 = add i64 %a0, 16
  %a2 = add i64 %a1, 8
  call void @runtime_safepoint() [ "deopt"() ]
  ; inttoptr AFTER the statepoint - its own base; the integer arith was
  ; AS0-integer space so it does not constrain the base map.
  %ip = inttoptr i64 %a2 to ptr addrspace(1)
  ; Keep %base alive across the safepoint (RS4GC must relocate it) but return
  ; %ip - proves the own-base treatment is not cross-contaminated by the AS1
  ; root being relocated alongside.
  %live = bitcast ptr addrspace(1) %base to ptr addrspace(1)
  call void @sink_as1(ptr addrspace(1) %live)
  ret ptr addrspace(1) %ip
}

declare void @sink_as1(ptr addrspace(1))

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
