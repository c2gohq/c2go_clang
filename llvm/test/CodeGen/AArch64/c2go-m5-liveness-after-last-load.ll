; c2go-gc per-call ptr-slot liveness: safepoint after last load.
;
;   store ptr; use_ptr(); safepoint
;
; The ptr's last live use precedes the safepoint. A body-wide OR-in would
; still mark the slot at the trailing safepoint even though the value is dead;
; the backward use-after predicate must end the live range at the last load so
; the per-call live set excludes the frame index.
;
; Here no ptr frame index reaches the safepoint (the dead-slot case): the pass
; must run cleanly and still emit the live-set selector and locals bitmap.
;
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @runtime_safepoint()
declare ptr addrspace(1) @make_obj() "gc-leaf-function"
declare void @use_ptr(ptr addrspace(1)) "gc-leaf-function"

; CHECK-LABEL: TEXT ·c2go_after_last_load(SB)
; The ptr is dead at the trailing safepoint (its last use precedes it), so the
; live-set selector picks the empty map (index $0): the slot is excluded.
; CHECK:      PCDATA $1, $0
; CHECK-NEXT: CALL ·runtime_safepoint(SB)
; CHECK:      FUNCDATA $1, gclocals·{{[0-9a-f]+}}(SB)
define void @c2go_after_last_load() gc "c2go-gc" {
entry:
  %p = call ptr addrspace(1) @make_obj()
  call void @use_ptr(ptr addrspace(1) %p)
  call void @runtime_safepoint() [ "deopt"() ]
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
