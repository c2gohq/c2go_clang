; A canonical libc name is normally classified as GC-leaf by
; TargetLibraryInfo. c2go's libc functions are GoABI0 functions and may run a
; Go morestack prologue, so C2GoGCSetup's explicit safepoint decision must take
; precedence over that inferred TLI leaf classification.
;
; RUN: opt < %s -passes=c2go-gc-setup -S | FileCheck %s --check-prefix=SETUP
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S | FileCheck %s --check-prefix=LOWERED

target triple = "aarch64-unknown-none-goabi"

declare i64 @strlen(ptr)
declare void @llvm.va_end(ptr)

define ptr @call_libc(ptr %p) {
entry:
  %n = call i64 @strlen(ptr %p)
  %nonempty = icmp ne i64 %n, 0
  %result = select i1 %nonempty, ptr %p, ptr null
  ret ptr %result
}

define void @end_va_list(ptr %ap) {
entry:
  call void @llvm.va_end(ptr %ap)
  ret void
}

; SETUP-LABEL: define ptr @call_libc(ptr %p) gc "c2go-gc"
; SETUP: call i64 @strlen(ptr %p) #[[SAFE:[0-9]+]] [ "deopt"() ]

; LOWERED-LABEL: define ptr @call_libc(ptr %p) gc "c2go-gc"
; LOWERED: @llvm.experimental.gc.statepoint{{.*}}elementtype(i64 (ptr)) @strlen
; LOWERED-SAME: [ "deopt"(), "gc-live"(ptr %p) ]
; LOWERED-NOT: "gc-safepoint"

; LLVM intrinsics retain their intrinsic-specific GC-leaf classification. In
; particular, c2go must not turn llvm.va_end into a statepoint merely because
; GC setup gives safepoint candidates a deopt bundle.
;
; SETUP-LABEL: define void @end_va_list(ptr %ap) gc "c2go-gc"
; SETUP: call void @llvm.va_end.p0(ptr %ap) [ "deopt"() ]
; SETUP: attributes #[[SAFE]] = { "gc-safepoint" }
;
; LOWERED-LABEL: define void @end_va_list(ptr %ap) gc "c2go-gc"
; LOWERED: call void @llvm.va_end.p0(ptr %ap) [ "deopt"() ]
; LOWERED-NEXT: ret void

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
