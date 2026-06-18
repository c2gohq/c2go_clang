; c2go-gc per-call ptr-slot liveness: diamond CFG join.
;
;        entry
;        /   \
;       B     C       <- only B defines a managed ptr; C carries the caller ptr
;        \   /
;        join          <- phi of (B-ptr, C-fallback), safepoint, then use
;
; The liveness pass must not crash on a diamond, must emit the Plan 9
; PCDATA $1, $<idx> selector immediately before the safepoint CALL, and must
; emit a gclocals bitmap referenced by the FUNCDATA.
;
; The ptr is live through the join (phi of the two paths), so the selector at
; the safepoint marks the slot (non-empty map).
;
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @runtime_safepoint()
declare ptr addrspace(1) @make_obj() "gc-leaf-function"
declare void @use_ptr(ptr addrspace(1)) "gc-leaf-function"

; CHECK-LABEL: TEXT ·c2go_diamond(SB)
; The ptr is live across the join, so the safepoint selector marks the slot
; (non-empty map), and a locals bitmap is emitted for the function.
; CHECK:      PCDATA $1, ${{[1-9][0-9]*}}
; CHECK-NEXT: CALL ·runtime_safepoint(SB)
; CHECK:      FUNCDATA $1, gclocals·{{[0-9a-f]+}}(SB)
define void @c2go_diamond(i1 %cond, ptr addrspace(1) %fallback) gc "c2go-gc" {
entry:
  br i1 %cond, label %store_path, label %nostore_path

store_path:
  %p = call ptr addrspace(1) @make_obj()
  br label %join

nostore_path:
  br label %join

join:
  %phi = phi ptr addrspace(1) [ %p, %store_path ], [ %fallback, %nostore_path ]
  call void @runtime_safepoint() [ "deopt"() ]
  call void @use_ptr(ptr addrspace(1) %phi)
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
