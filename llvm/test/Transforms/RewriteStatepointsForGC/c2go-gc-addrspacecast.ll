; Under the c2go-gc strategy, an addrspacecast result (AS1<->AS0) live
; across a statepoint must be treated as its own base, so the cast result
; gets a gc.relocate in its own address space. Without the carve-out,
; RewriteStatepointsForGC fails the "unsupported addrspacecast" assert.
;
; The cases below cover: positive AS1->AS0 and AS0->AS1 casts; casts fed
; as call arguments across a memmove-shaped statepoint; addrspacecast
; composed with phi/select/GEP/load; a negative case proving the carve-out
; is gated on the c2go-gc strategy; and an AS1->AS0->AS1 round-trip proving
; both intermediate cast results are independently base-tracked when both
; are live across the same statepoint (the carve-out short-circuits the
; outer cast without descending into the inner cast operand).
;
; RUN: opt < %s -passes=rewrite-statepoints-for-gc -spp-rematerialization-threshold=0 -S | FileCheck %s

declare void @foo()
declare void @libc_memmove(ptr, ptr, i64)

; ----------------------------------------------------------------------------
; Positive: managed (AS1) source cast to AS0 result, AS0 result live across
; a statepoint. The cast result is its own base and gets an AS0 relocate
; (the AS1 source is not itself live here).
; ----------------------------------------------------------------------------
define ptr @as1_to_as0_live(ptr addrspace(1) %src) gc "c2go-gc" {
; CHECK-LABEL: @as1_to_as0_live
entry:
  %unmanaged = addrspacecast ptr addrspace(1) %src to ptr
; CHECK: gc.statepoint
; CHECK: %unmanaged.relocated = call coldcc ptr @llvm.experimental.gc.relocate.p0
  call void @foo() [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret ptr %unmanaged
}

; ----------------------------------------------------------------------------
; Positive: AS0 cast results fed as call arguments across a memmove-shaped
; statepoint. Both cast results must survive the statepoint as independent
; AS0 relocates.
; ----------------------------------------------------------------------------
define void @adapt_arg_as_memmove(ptr addrspace(1) %dst1, ptr addrspace(1) %src1, i64 %n) gc "c2go-gc" {
; CHECK-LABEL: @adapt_arg_as_memmove
entry:
  %dst0 = addrspacecast ptr addrspace(1) %dst1 to ptr
  %src0 = addrspacecast ptr addrspace(1) %src1 to ptr
; CHECK: gc.statepoint
; CHECK-DAG: %dst0.relocated = call coldcc ptr @llvm.experimental.gc.relocate.p0
; CHECK-DAG: %src0.relocated = call coldcc ptr @llvm.experimental.gc.relocate.p0
  call void @libc_memmove(ptr %dst0, ptr %src0, i64 %n) [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret void
}

; ----------------------------------------------------------------------------
; Positive: AS0 -> AS1 direction (symmetric). The AS1 cast result is the
; live value across the statepoint and gets an AS1 relocate.
; ----------------------------------------------------------------------------
define ptr addrspace(1) @as0_to_as1_live(ptr %src) gc "c2go-gc" {
; CHECK-LABEL: @as0_to_as1_live
entry:
  %managed = addrspacecast ptr %src to ptr addrspace(1)
; CHECK: gc.statepoint
; CHECK: %managed.relocated = call coldcc ptr addrspace(1) @llvm.experimental.gc.relocate.p1
  call void @foo() [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret ptr addrspace(1) %managed
}

; ----------------------------------------------------------------------------
; addrspacecast composed with phi/select/GEP/load across a statepoint. Each
; case relies on the own-base carve-out for the addrspacecast result; this
; closes a coverage gap for the composed shapes.
;
; Round-trip note: AS1->AS0->AS1 is exercised by the round-trip case below.
; InstCombine normally folds the recursive cast pair before RS4GC runs, but
; a hand-emitted chained-cast shape can still reach RS4GC (e.g. from a custom
; pass, or before InstCombine in a non-standard pipeline). The carve-out
; short-circuits the outer cast as its own base without descending into the
; inner cast operand, so each cast result gets an independent relocate in its
; own address space.
; ----------------------------------------------------------------------------

; phi of AS1 values where one incoming is an AS0->AS1 cast result.
; The cast result is its own base; the phi gets a synthesised base phi
; per the standard base-phi rule, and the phi result has an AS1 relocate
; across the statepoint.
define ptr addrspace(1) @phi_addrspacecast_incoming(i1 %c, ptr %raw, ptr addrspace(1) %other) gc "c2go-gc" {
; CHECK-LABEL: @phi_addrspacecast_incoming
entry:
  %cast_in = addrspacecast ptr %raw to ptr addrspace(1)
  br i1 %c, label %left, label %right
left:
  br label %join
right:
  br label %join
join:
  %p = phi ptr addrspace(1) [ %cast_in, %left ], [ %other, %right ]
; CHECK: gc.statepoint
; CHECK: %p.relocated = call coldcc ptr addrspace(1) @llvm.experimental.gc.relocate.p1
  call void @foo() [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret ptr addrspace(1) %p
}

; select of two AS1 ptrs, then addrspacecast to AS0; the AS0 cast result
; is the value live across the statepoint and gets an AS0 relocate.
define ptr @select_then_addrspacecast(i1 %c, ptr addrspace(1) %a, ptr addrspace(1) %b) gc "c2go-gc" {
; CHECK-LABEL: @select_then_addrspacecast
entry:
  %sel = select i1 %c, ptr addrspace(1) %a, ptr addrspace(1) %b
  %as0 = addrspacecast ptr addrspace(1) %sel to ptr
; CHECK: gc.statepoint
; CHECK: %as0.relocated = call coldcc ptr @llvm.experimental.gc.relocate.p0
  call void @foo() [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret ptr %as0
}

; GEP on an AS1 base, then addrspacecast to AS0, then fed as call arg
; across a memmove-shaped statepoint. The AS0 cast result is its own base
; and gets an AS0 relocate; the AS1 GEP value is independently base-tracked.
define ptr addrspace(1) @gep_then_addrspacecast_arg(ptr addrspace(1) %base, ptr %dst, i64 %n) gc "c2go-gc" {
; CHECK-LABEL: @gep_then_addrspacecast_arg
entry:
  %gep = getelementptr i8, ptr addrspace(1) %base, i64 16
  %src = addrspacecast ptr addrspace(1) %gep to ptr
; The AS0 cast result and the AS1 GEP value are both live across the statepoint
; and are independently base-tracked: each gets a relocate in its own AS.
; CHECK: gc.statepoint
; CHECK-DAG: %src.relocated = call coldcc ptr @llvm.experimental.gc.relocate.p0
; CHECK-DAG: %gep.relocated = call coldcc ptr addrspace(1) @llvm.experimental.gc.relocate.p1
  call void @libc_memmove(ptr %dst, ptr %src, i64 %n) [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret ptr addrspace(1) %gep
}

; load of a managed field (AS1), then addrspacecast to AS0, then fed
; as a call arg across a statepoint. The loaded value is its own base
; under c2go-gc; the AS0 cast result is its own base; the cast gets an
; AS0 relocate.
define ptr addrspace(1) @load_managed_then_cast_arg(ptr addrspace(1) %container, ptr %dst, i64 %n) gc "c2go-gc" {
; CHECK-LABEL: @load_managed_then_cast_arg
entry:
  %slot = getelementptr i8, ptr addrspace(1) %container, i64 8
  %loaded = load ptr addrspace(1), ptr addrspace(1) %slot
  %as0 = addrspacecast ptr addrspace(1) %loaded to ptr
; The loaded AS1 value is its own base under c2go-gc and the AS0 cast result is
; its own base; both are live across the statepoint and each gets a relocate.
; CHECK: gc.statepoint
; CHECK-DAG: %as0.relocated = call coldcc ptr @llvm.experimental.gc.relocate.p0
; CHECK-DAG: %loaded.relocated = call coldcc ptr addrspace(1) @llvm.experimental.gc.relocate.p1
  call void @libc_memmove(ptr %dst, ptr %as0, i64 %n) [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret ptr addrspace(1) %loaded
}

; ----------------------------------------------------------------------------
; Round-trip AS1 -> AS0 -> AS1 with BOTH cast results live across the same
; statepoint. The carve-out fires on the outer cast (returns the outer cast
; as its own base) without descending into the inner cast operand; the inner
; cast is independently base-tracked because it is also live across the
; statepoint via a second use. Each gets a relocate at its own address space.
;
; This shape is not redundant with the casts above: the operand of the outer
; addrspacecast is itself an addrspacecast - the exact recursion that the
; bare stripPointerCasts()-then-assert fallback cannot survive (it does not
; strip cross-address-space pointer casts, so the assert fires). With the
; carve-out the outer cast is its own base.
; ----------------------------------------------------------------------------
declare void @use_p0(ptr)
define ptr addrspace(1) @round_trip_as1_as0_as1(ptr addrspace(1) %src) gc "c2go-gc" {
; CHECK-LABEL: @round_trip_as1_as0_as1
entry:
  %mid_as0 = addrspacecast ptr addrspace(1) %src to ptr
  %back_as1 = addrspacecast ptr %mid_as0 to ptr addrspace(1)
; CHECK: gc.statepoint
; CHECK-DAG: %back_as1.relocated = call coldcc ptr addrspace(1) @llvm.experimental.gc.relocate.p1
; CHECK-DAG: %mid_as0.relocated = call coldcc ptr @llvm.experimental.gc.relocate.p0
  call void @foo() [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  call void @use_p0(ptr %mid_as0)
  ret ptr addrspace(1) %back_as1
}

; ----------------------------------------------------------------------------
; Negative: the carve-out is gated on the c2go-gc strategy. Under
; gc "statepoint-example" (the default RS4GC test strategy), AS1 is the only
; managed address space; an AS1->AS0 cast result is in unmanaged AS0 and so
; is not a GC live value at all - findBaseDefiningValue is never invoked on
; the cast, no relocate is emitted for it, and the c2go-gc-specific carve-out
; never fires. The function shape matches the first positive case; only the
; gc strategy differs. This proves the carve-out is strategy-scoped and does
; not pollute other GC strategies.
;
; Note: the symmetric negative - AS0->AS1 under gc "statepoint-example" -
; intentionally hits the address-space mismatch assert (no carve-out match,
; bare stripPointerCasts) and so cannot be exercised by FileCheck; that is
; the original "unsupported addrspacecast" behaviour the c2go-gc carve-out
; was introduced to bypass. It is covered by c2go-gc-addrspacecast-neg.ll.
; ----------------------------------------------------------------------------
define ptr @as1_to_as0_default_gc_no_relocate(ptr addrspace(1) %src) gc "statepoint-example" {
; CHECK-LABEL: @as1_to_as0_default_gc_no_relocate
entry:
  %unmanaged = addrspacecast ptr addrspace(1) %src to ptr
; CHECK: gc.statepoint
; CHECK-NOT: %unmanaged.relocated
  call void @foo() [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret ptr %unmanaged
}
