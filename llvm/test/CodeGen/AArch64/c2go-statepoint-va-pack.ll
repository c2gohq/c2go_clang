; A c2go vararg argptrs[] array contains pointers to value slots in the same
; Go stack frame. At a growing callee, those fields must be present in the
; caller's locals bitmap so copystack rewrites them to the new stack copy.
;
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @consume(ptr)

; CHECK-LABEL: TEXT ·va_pack(SB)
; CHECK: PCDATA $1, $1
; CHECK-NEXT: CALL ·consume(SB)
; CHECK: FUNCDATA $1, gclocals·[[MAP:[0-9a-f]+]](SB)
; CHECK-NEXT: DATA gclocals·[[MAP]]+0(SB)/4, $2
; CHECK-NEXT: DATA gclocals·[[MAP]]+4(SB)/4, $5
; CHECK-NEXT: DATA gclocals·[[MAP]]+8(SB)/1, $0x00
; CHECK-NEXT: DATA gclocals·[[MAP]]+9(SB)/1, $0x{{[0-9a-f]*[1-9a-f][0-9a-f]*}}
define void @va_pack(ptr %p) #0 gc "c2go-gc" {
entry:
  %pack = alloca [2 x ptr], align 8, !c2go.ptr.managed !1, !c2go.va.pack !2
  %elt0 = getelementptr inbounds [2 x ptr], ptr %pack, i64 0, i64 0
  %elt1 = getelementptr inbounds [2 x ptr], ptr %pack, i64 0, i64 1
  store ptr %p, ptr %elt0, align 8
  store ptr null, ptr %elt1, align 8
  call void @consume(ptr %pack) [ "deopt"() ]
  ret void
}

attributes #0 = { noinline optnone }

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{!"c2go.va"}
!2 = !{}
