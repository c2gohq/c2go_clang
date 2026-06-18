; Negative companion to c2go-gc-addrspacecast.ll: the addrspacecast
; own-base carve-out in RewriteStatepointsForGC is scoped to the c2go-gc
; strategy only. Under gc "statepoint-example", an AS0->AS1 cast result is
; a managed value, so findBaseDefiningValue runs on it, fails the c2go-gc
; gate (the carve-out does not apply), and reaches the address-space
; mismatch assertion ("unsupported addrspacecast"). This pins that crash,
; so any change that widens the carve-out to all GC strategies (or removes
; the assert) fails the build instead of silently regressing soundness.
;
; The crash is an assertion failure, so the test is only meaningful on an
; assertions build.
;
; REQUIRES: asserts
; RUN: not --crash opt -disable-output -passes=rewrite-statepoints-for-gc %s 2>&1 | FileCheck %s

; CHECK: unsupported addrspacecast

declare void @foo()

; AS0 (unmanaged) source cast to AS1 result, live across a statepoint, under
; the default test GC strategy. The c2go-gc carve-out does not fire and
; RewriteStatepointsForGC reaches the address-space mismatch assert.
define ptr addrspace(1) @as0_to_as1_default_gc_crash(ptr %src) gc "statepoint-example" {
entry:
  %managed = addrspacecast ptr %src to ptr addrspace(1)
  call void @foo() [ "deopt"(i32 0, i32 -1, i32 0, i32 0, i32 0) ]
  ret ptr addrspace(1) %managed
}
