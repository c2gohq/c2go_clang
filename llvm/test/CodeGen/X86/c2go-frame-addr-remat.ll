; RUN: llc -O0 -stop-after=c2go-frame-addr-remat %s -o - | FileCheck %s
; RUN: sed 's/"c2go.goabi"/"c2go.gone"/' %s | llc -O0 -stop-after=c2go-frame-addr-remat - -o - | FileCheck --check-prefix=NOFLAG %s

; X86 mirror of CodeGen/AArch64/c2go-frame-addr-remat.ll — the pass itself is
; target-independent (any trivially-rematerializable def with a FrameIndex
; operand); this checks the LEA64r materialization is cloned after the call in
; a goabi module and left alone without the module flag.
;
; CHECK-LABEL: name: deepx
; CHECK: LEA64r %stack.2.buf
; CHECK: CALL64pcrel32 @growx
; CHECK: LEA64r %stack.2.buf
;
; NOFLAG-LABEL: name: deepx
; NOFLAG: CALL64pcrel32 @growx
; NOFLAG-NOT: LEA64r %stack

target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx"

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
