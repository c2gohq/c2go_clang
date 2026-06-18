; c2go-gc per-call ptr-slot liveness: same-MI load/store ordering.
;
; Some register-allocator reload+spill snippets (mergeable spills, hoisted-spill
; remats) appear as a single MachineInstr with two frame-index MachineMemOperands:
; a load from FI_x feeding a store to FI_x (or two FIs). The per-MI transfer
; processes mem-operands in order; this guards against an off-by-one where the
; store closes the ready window before the load contributes to use-after on the
; same MI.
;
; Heavy register pressure across the safepoint forces the snippet.
;
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @runtime_safepoint()
declare ptr addrspace(1) @make_obj() "gc-leaf-function"
declare void @clobber(ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1)) "gc-leaf-function"

; CHECK-LABEL: TEXT ·c2go_same_mi_load_store(SB)
; The ptrs are live across the safepoint, so despite the merged load+store
; mem-op MI the selector marks the slot (non-empty map) and a locals bitmap
; is emitted.
; CHECK:      PCDATA $1, ${{[1-9][0-9]*}}
; CHECK-NEXT: CALL ·runtime_safepoint(SB)
; CHECK:      FUNCDATA $1, gclocals·{{[0-9a-f]+}}(SB)
define void @c2go_same_mi_load_store() gc "c2go-gc" {
entry:
  %p0  = call ptr addrspace(1) @make_obj()
  %p1  = call ptr addrspace(1) @make_obj()
  %p2  = call ptr addrspace(1) @make_obj()
  %p3  = call ptr addrspace(1) @make_obj()
  %p4  = call ptr addrspace(1) @make_obj()
  %p5  = call ptr addrspace(1) @make_obj()
  %p6  = call ptr addrspace(1) @make_obj()
  %p7  = call ptr addrspace(1) @make_obj()
  %p8  = call ptr addrspace(1) @make_obj()
  %p9  = call ptr addrspace(1) @make_obj()
  %p10 = call ptr addrspace(1) @make_obj()
  %p11 = call ptr addrspace(1) @make_obj()
  call void @runtime_safepoint() [ "deopt"() ]
  call void @clobber(ptr addrspace(1) %p0,  ptr addrspace(1) %p1,
                     ptr addrspace(1) %p2,  ptr addrspace(1) %p3,
                     ptr addrspace(1) %p4,  ptr addrspace(1) %p5,
                     ptr addrspace(1) %p6,  ptr addrspace(1) %p7,
                     ptr addrspace(1) %p8,  ptr addrspace(1) %p9,
                     ptr addrspace(1) %p10, ptr addrspace(1) %p11)
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
