; c2go-gc per-call ptr-slot liveness: loop fixed point.
;
;   entry --> header --(back-edge)--> header
;              |   ^
;              |   | back-edge re-stores the ptr each iteration
;              v   |
;             body (safepoint inside loop, uses the ptr after)
;              |
;             exit
;
; The liveness dataflow must converge on a loop CFG (forward reach-defs across
; the back-edge, backward use-after across the same edge) without iterating
; forever, and must emit a per-call live-set selector and a locals bitmap.
;
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @runtime_safepoint()
declare ptr addrspace(1) @make_obj() "gc-leaf-function"
declare i1 @loop_cond(ptr addrspace(1)) "gc-leaf-function"

; CHECK-LABEL: TEXT ·c2go_loop(SB)
; In-body safepoint: emits a PCDATA live-set selector and the function emits
; a locals bitmap. The ptr is loop-live, so the selector must be non-empty.
; CHECK:      PCDATA $1, ${{[1-9][0-9]*}}
; CHECK-NEXT: CALL ·runtime_safepoint(SB)
; CHECK:      FUNCDATA $1, gclocals·{{[0-9a-f]+}}(SB)
define void @c2go_loop() gc "c2go-gc" {
entry:
  %p0 = call ptr addrspace(1) @make_obj()
  br label %header

header:
  %p = phi ptr addrspace(1) [ %p0, %entry ], [ %p2, %body ]
  br label %body

body:
  call void @runtime_safepoint() [ "deopt"() ]
  %again = call i1 @loop_cond(ptr addrspace(1) %p)
  %p2 = call ptr addrspace(1) @make_obj()
  br i1 %again, label %header, label %exit

exit:
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
