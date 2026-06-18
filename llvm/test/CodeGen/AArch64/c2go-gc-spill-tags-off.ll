; c2go-gc escape hatch: with both the spill-tag-driven marking path and the
; per-PC ptr-slot liveness pass disabled, codegen falls back to the
; RS4GC-only path. RS4GC is unaffected by these flags, so a managed-pointer
; statepoint is still emitted, but no spill-tag-derived locals bitmap update
; runs. Verify the negative shape: the locals bitmap must be the plain RS4GC
; version, i.e. the gclocalsdead sentinel that the ptr-slot liveness pass
; emits is absent.
;
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:        -c2go-disable=spill-tags,ptrslot-liveness \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @runtime_safepoint()

; CHECK-LABEL: TEXT ·c2go_off(SB)
; RS4GC-only path still produces a statepoint with PCDATA + FUNCDATA, but the
; locals bitmap is the unaugmented version: the gclocals-dead sentinel is
; absent.
; CHECK:      PCDATA $1, $1
; CHECK-NEXT: CALL ·runtime_safepoint(SB)
; CHECK:      FUNCDATA $1, gclocals·
; CHECK-NOT:  gclocalsdead·
define ptr addrspace(1) @c2go_off(ptr addrspace(1) %obj) gc "c2go-gc" {
entry:
  %slot = alloca i64, align 8
  store i64 0, ptr %slot, align 8
  call void @runtime_safepoint() [ "deopt"() ]
  ret ptr addrspace(1) %obj
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
