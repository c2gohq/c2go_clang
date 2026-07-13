; c2go #665 (#654c-b root fix): a FUNCTION-pointer value — lowered by clang to
; the dedicated addrspace(200) — must NOT be threaded into statepoint gc-live
; sets by RewriteStatepointsForGC. Its run-time values are code addresses or
; POSIX sentinel integers (SIG_IGN == 1); a gc-live relocate spill slot marked
; "pointer" that holds a small non-zero integer makes copystack throw
; "invalid pointer found on stack" (the saved-handler idiom:
; `saved = signal(SIGINT, SIG_IGN); work(); signal(SIGINT, saved)`).
; The data pointer %d, by contrast, must still be relocated.
;
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -S | FileCheck %s

target triple = "arm64-unknown-none-goabi"

declare void @ext()

; CHECK-LABEL: @saved_handler_shape
; The statepoint's gc-live bundle must carry the DATA pointer only — no
; addrspace(200) value anywhere in it, and no relocate for %fp.
; CHECK: "gc-live"(ptr %d)
; CHECK-NOT: gc.relocate{{.*}}addrspace(200)
define i64 @saved_handler_shape(ptr addrspace(200) %fp, ptr %d) gc "c2go-gc" {
entry:
  call void @ext() [ "deopt"() ]
  ; both values live across the safepoint: %fp must stay un-tracked while %d
  ; is relocated.
  %a = ptrtoint ptr addrspace(200) %fp to i64
  %b = ptrtoint ptr %d to i64
  %s = add i64 %a, %b
  ret i64 %s
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
