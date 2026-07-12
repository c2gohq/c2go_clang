; c2go #654c: goabi frame-index known bits must be capped at 8-byte alignment.
;
; The c2go frame places SP-based locals 8 bytes below their layout-assigned
; offsets (the -8 shift keeping them out of the reserved frame-top word), so an
; object whose MachineFrameInfo alignment claims 16+ is physically only
; 8-aligned. If ISel trusts the claimed alignment, DAGCombine folds
; `add (FrameIndex), 8` into a disjoint ORR whose bit 3 is ALREADY SET at run
; time — the +8 silently vanishes and the derived field address collapses onto
; the object base (Lua probe5: &p.dyd computed as &p). The address arithmetic
; must stay an ADD.
;
; RUN: llc < %s --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

%struct.P = type { i32, [108 x i8] }

; CHECK-LABEL: TEXT ·addr_of_field(SB)
; CHECK-NOT:  ORR $8
; CHECK:      ADD $8,
; CHECK-NOT:  ORR $8
; CHECK:      RET
define void @addr_of_field(ptr %sink) {
entry:
  %p = alloca %struct.P, align 16
  %f = getelementptr inbounds i8, ptr %p, i64 8
  store ptr %f, ptr %sink
  call void @use(ptr %p)
  ret void
}

declare void @use(ptr)

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
