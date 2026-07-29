; X86 mirror of AArch64/c2go-statepoint-va-pack.ll: argptrs[] contains
; pointers to c2go.va.slot objects in the caller frame, so its fields must be
; present in the Go locals bitmap at a call that can grow the stack.
;
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=x86_64-unknown-linux-gnu \
; RUN:   | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

declare void @consume(ptr)

; CHECK-LABEL: TEXT ·va_pack(SB)
; CHECK: PCDATA $1, $1
; CHECK-NEXT: CALL ·consume(SB)
; CHECK: FUNCDATA $1, gclocals·[[MAP:[0-9a-f]+]](SB)
; CHECK-NEXT: DATA gclocals·[[MAP]]+0(SB)/4, $2
; CHECK-NEXT: DATA gclocals·[[MAP]]+4(SB)/4, $4
; CHECK-NEXT: DATA gclocals·[[MAP]]+8(SB)/1, $0x00
; CHECK-NEXT: DATA gclocals·[[MAP]]+9(SB)/1, $0x{{[0-9a-f]*[1-9a-f][0-9a-f]*}}
define internal c2goabiinternalcc void @va_pack(ptr %p) #0 gc "c2go-gc" {
entry:
  %pack = alloca [2 x ptr], align 8, !c2go.ptr.managed !2, !c2go.va.pack !3
  %elt0 = getelementptr inbounds [2 x ptr], ptr %pack, i64 0, i64 0
  %elt1 = getelementptr inbounds [2 x ptr], ptr %pack, i64 0, i64 1
  store ptr %p, ptr %elt0, align 8
  store ptr null, ptr %elt1, align 8
  call void @consume(ptr %pack) [ "deopt"() ]
  ret void
}

attributes #0 = { noinline optnone "c2go-reg-return" "c2go-argsize"="8" "c2go-argptrmask"="01" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
!2 = !{!"c2go.va"}
!3 = !{}
