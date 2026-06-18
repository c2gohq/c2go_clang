; c2go-gc per-call ptr-slot liveness: safepoint before first store.
;
;   entry: safepoint A   <- ptr slot uninitialised here
;            |
;          make ptr; safepoint B   <- ptr now live
;
; A body-wide OR-in would mark the ptr frame index at every call, including
; safepoint A where the slot is uninitialised garbage. The forward reach-defs
; (ready) predicate must exclude A.
;
; Safepoint A (before any store) selects the empty map (index $0): the slot is
; excluded. Safepoint B (ptr live) selects a non-empty map: the slot is marked.
; llc must also not crash on the store-unreachable predecessor path.
;
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @runtime_safepoint()
declare ptr addrspace(1) @make_obj() "gc-leaf-function"
declare void @use_ptr(ptr addrspace(1)) "gc-leaf-function"

; CHECK-LABEL: TEXT ·c2go_before_first_store(SB)
; Safepoint A precedes any ptr store: the slot is excluded (empty map, $0).
; CHECK:      PCDATA $1, $0
; CHECK-NEXT: CALL ·runtime_safepoint(SB)
; Safepoint B follows the store: the slot is live and marked (non-empty map).
; CHECK:      PCDATA $1, ${{[1-9][0-9]*}}
; CHECK-NEXT: CALL ·runtime_safepoint(SB)
; CHECK:      FUNCDATA $1, gclocals·{{[0-9a-f]+}}(SB)
define void @c2go_before_first_store() gc "c2go-gc" {
entry:
  call void @runtime_safepoint() [ "deopt"() ] ; safepoint A: before first store
  %p = call ptr addrspace(1) @make_obj()
  call void @runtime_safepoint() [ "deopt"() ] ; safepoint B: ptr live
  call void @use_ptr(ptr addrspace(1) %p)
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
