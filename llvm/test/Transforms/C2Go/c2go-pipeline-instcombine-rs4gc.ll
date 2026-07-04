; Post-InstCombine soundness for the late GC chain.
;
; The release pipeline (clang -O1+) runs InstCombine before the c2go GC
; passes. InstCombine is free to fold an AS1 GEP+load chain into its canonical
; residue (zero-index GEP elision, redundant bitcast removal, etc.). The
; downstream chain
;
;   c2go-memcpy-typing -> rewrite-statepoints-for-gc
;
; must stay sound across that fold:
;   (1) c2go-memcpy-typing still lowers a post-InstCombine large untyped AS1
;       memcpy to libc.Memmove with AS1->AS0 boundary casts at the call site.
;   (2) RS4GC wraps the non-leaf libc.Memmove in gc.statepoint and relocates
;       the AS1 base pointer that is still live across the statepoint, proving
;       the c2go-gc carve-out keeps the AS1 root reachable through the
;       freshly-inserted AS1->AS0 cast.
;
; Companion to c2go-as1-gep-fold-chain.ll (constructs the folded shape
; directly) and c2go-libc-memmove-statepoint.ll (same downstream chain without
; InstCombine in front of it).
;
; RUN: opt < %s -passes='function(instcombine),c2go-memcpy-typing,rewrite-statepoints-for-gc' -S | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

; AS1 base, GEP to a managed field, load it, then a large untyped AS1 memcpy
; that uses the loaded value as the source - the `*dst = *p->next` (large
; struct) shape after InstCombine has folded any zero-index / redundant-bitcast
; residue away. The loaded AS1 pointer is live across the libc.Memmove
; statepoint and must be relocated as AS1.
;
; CHECK-LABEL: define ptr addrspace(1) @load_then_memmove_as1(ptr addrspace(1) %p, ptr addrspace(1) %dst) gc "c2go-gc"
;
; c2go-memcpy-typing inserts the AS1->AS0 boundary casts for libc.Memmove.
; CHECK: addrspacecast ptr addrspace(1) {{.*}} to ptr
; CHECK: addrspacecast ptr addrspace(1) {{.*}} to ptr
;
; RS4GC must wrap libc.Memmove (it is NOT gc-leaf-function).
; CHECK: gc.statepoint{{.*}}@"github.com/c2gohq/c2go_libc.memmove"
;
; The AS1 base reachable through the load+cast chain is relocated as AS1.
; CHECK: relocated = call {{.*}}ptr addrspace(1) @llvm.experimental.gc.relocate.p1
define ptr addrspace(1) @load_then_memmove_as1(ptr addrspace(1) %p, ptr addrspace(1) %dst) gc "c2go-gc" {
entry:
  ; GEP to the `next` slot (offset 0). InstCombine folds this zero-byte GEP
  ; away, leaving the load directly on %p - the canonical post-InstCombine
  ; residue this test pins.
  %slot = getelementptr inbounds i8, ptr addrspace(1) %p, i64 0
  %src  = load ptr addrspace(1), ptr addrspace(1) %slot, align 8
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dst, ptr addrspace(1) %src, i64 4096, i1 false)
  ret ptr addrspace(1) %src
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
