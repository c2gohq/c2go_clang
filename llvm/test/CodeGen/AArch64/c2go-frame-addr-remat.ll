; RUN: llc -O0 -stop-after=c2go-frame-addr-remat %s -o - | FileCheck %s
; RUN: sed 's/"c2go.goabi"/"c2go.gone"/' %s | llc -O0 -stop-after=c2go-frame-addr-remat - -o - | FileCheck --check-prefix=NOFLAG %s

; A materialized frame address (ADDXri %stack.N) whose value is live across a
; call must be recomputed after the call rather than kept in a register: every
; call may grow and relocate the goroutine stack, and a register-allocator
; spill of the address lands in an anonymous slot no stackmap covers, so
; copystack cannot relocate it (FastRA at -O0 spills every cross-call vreg and
; never rematerializes). The c2go-frame-addr-remat pass clones the definition
; in front of each use that crosses a call, so no RA ever needs to carry the
; value over a safepoint. Storing %base as a VALUE (not merely accessing
; through it) forces the materialization into a register; the second store
; after the call is the cross-call use that must be rewritten.
;
; CHECK-LABEL: name: deepx
; CHECK: ADDXri %stack.2.buf, 0, 0
; CHECK: BL @growx
; CHECK: ADDXri %stack.2.buf, 0, 0
;
; Without the c2go.goabi module flag the pass must not fire: the address stays
; in the pre-call vreg and nothing recomputes it after the call.
;
; NOFLAG-LABEL: name: deepx
; NOFLAG: BL @growx
; NOFLAG-NOT: ADDXri %stack

target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx"

define hidden goabi0cc i64 @deepx(ptr noundef %p, i32 noundef %d) #0 {
entry:
  %p.addr = alloca ptr, align 8
  %slot = alloca ptr, align 8
  %buf = alloca [16 x i64], align 8
  store ptr %p, ptr %p.addr, align 8
  %base = getelementptr inbounds [16 x i64], ptr %buf, i64 0, i64 0
  store ptr %base, ptr %slot, align 8
  %0 = load ptr, ptr %p.addr, align 8
  call goabi0cc void @growx(ptr noundef %0, i32 noundef %d)
  store ptr %base, ptr %slot, align 8
  %1 = load ptr, ptr %slot, align 8
  %2 = load i64, ptr %1, align 8
  ret i64 %2
}

declare hidden goabi0cc void @growx(ptr noundef, i32 noundef)

attributes #0 = { noinline nounwind optnone "frame-pointer"="non-leaf-no-reserve" "no-jump-tables"="true" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
