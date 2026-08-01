; Verify that optimizer-synthesized raw libc declarations and backend-libcall
; intrinsics are redirected to the frontend-preserved c2go_linkname target and
; normalized to GoABI0 before GC lowering.
;
; RUN: opt < %s -passes=c2go-libcall-routing -S | FileCheck %s

target triple = "aarch64-unknown-linux-goabi"

declare i64 @strlen(ptr)
declare i32 @puts(ptr) #0
declare i32 @"example.com/lib.puts"(ptr)
declare double @llvm.sin.f64(double)
declare double @llvm.experimental.constrained.sin.f64(double, metadata, metadata)
declare double @llvm.experimental.constrained.frem.f64(double, double, metadata, metadata)
declare { double, double } @llvm.modf.f64(double)
declare { double, i32 } @llvm.frexp.f64.i32(double)
declare { double, double } @llvm.sincos.f64(double)

define i64 @use_strlen(ptr %s) {
  %n = tail call i64 @strlen(ptr %s)
  ret i64 %n
}

define i32 @use_puts(ptr %s) {
  %r = call i32 @puts(ptr %s)
  ret i32 %r
}

; A real definition with a libc-like name must not be redirected.
define i32 @strcmp(ptr %a, ptr %b) {
  ret i32 0
}

define double @use_sin(double %x) {
  %r = call double @llvm.sin.f64(double %x)
  ret double %r
}

define double @use_constrained_sin(double %x) strictfp {
  %r = call double @llvm.experimental.constrained.sin.f64(double %x, metadata !"round.dynamic", metadata !"fpexcept.strict") strictfp
  ret double %r
}

define double @use_constrained_fmod(double %x, double %y) strictfp {
  %r = call double @llvm.experimental.constrained.frem.f64(double %x, double %y, metadata !"round.dynamic", metadata !"fpexcept.strict") strictfp
  ret double %r
}

define double @use_fmod(double %x, double %y) {
  %r = frem double %x, %y
  ret double %r
}

define { double, double } @use_modf(double %x) {
  %r = call { double, double } @llvm.modf.f64(double %x)
  ret { double, double } %r
}

define { double, i32 } @use_frexp(double %x) {
  %r = call { double, i32 } @llvm.frexp.f64.i32(double %x)
  ret { double, i32 } %r
}

define { double, double } @use_sincos(double %x) {
  %r = call { double, double } @llvm.sincos.f64(double %x)
  ret { double, double } %r
}

attributes #0 = { nounwind memory(argmem: read) }

; The declaration-only raw symbols disappear. The pre-existing target absorbs
; inferred raw-libcall attributes, while the newly named target retains its
; original function type and gains the c2go route attributes.
; CHECK: declare goabi0cc i64 @"example.com/lib.strlen"(ptr) #[[STR:[0-9]+]]
; CHECK: declare goabi0cc i32 @"example.com/lib.puts"(ptr) #[[PUTS:[0-9]+]]

; CHECK-LABEL: define i64 @use_strlen(
; CHECK: tail call goabi0cc i64 @"example.com/lib.strlen"(ptr %s) #[[NOBUILTIN:[0-9]+]]
; CHECK-LABEL: define i32 @use_puts(
; CHECK: call goabi0cc i32 @"example.com/lib.puts"(ptr %s) #[[NOBUILTIN]]

; A real definition with a standard-library name remains local program code.
; CHECK: define i32 @strcmp(

; CHECK-LABEL: define double @use_sin(
; CHECK: call goabi0cc double @"example.com/lib.sin"(double %x) #[[NOBUILTIN]]
; CHECK-NOT: @llvm.sin
; CHECK-LABEL: define double @use_constrained_sin(
; CHECK: call goabi0cc double @"example.com/lib.sin"(double %x) #[[STRICT:[0-9]+]]
; CHECK-NOT: @llvm.experimental.constrained.sin
; CHECK-LABEL: define double @use_constrained_fmod(
; CHECK: call goabi0cc double @"example.com/lib.fmod"(double %x, double %y) #[[STRICT]]
; CHECK-NOT: @llvm.experimental.constrained.frem
; CHECK-LABEL: define double @use_fmod(
; CHECK: call goabi0cc double @"example.com/lib.fmod"(double %x, double %y)
; CHECK-LABEL: define { double, double } @use_modf(
; CHECK: call goabi0cc double @"example.com/lib.modf"(double %x, ptr %c2go.modf.integral)
; CHECK-LABEL: define { double, i32 } @use_frexp(
; CHECK: call goabi0cc double @"example.com/lib.frexp"(double %x, ptr %c2go.frexp.exp)
; CHECK-LABEL: define { double, double } @use_sincos(
; CHECK: call goabi0cc void @"example.com/lib.sincos"(double %x, ptr %c2go.sincos.sin, ptr %c2go.sincos.cos)

; CHECK: declare goabi0cc double @"example.com/lib.sin"(double) #[[SIN:[0-9]+]]
; CHECK: declare goabi0cc double @"example.com/lib.fmod"(double, double)
; CHECK: declare goabi0cc double @"example.com/lib.modf"(double, ptr)
; CHECK: declare goabi0cc double @"example.com/lib.frexp"(double, ptr)
; CHECK: declare goabi0cc void @"example.com/lib.sincos"(double, ptr, ptr)

; CHECK: attributes #[[STR]] = { "c2go-c-name"="strlen" "c2go-linkname"="example.com/lib.strlen" "c2go-linkname-abi0" }
; CHECK: attributes #[[PUTS]] = {
; CHECK-SAME: nounwind
; CHECK-SAME: memory(argmem: read)
; CHECK-SAME: "c2go-c-name"="puts"
; CHECK-SAME: "c2go-linkname"="example.com/lib.puts"
; CHECK-SAME: "c2go-linkname-abi0"
; CHECK: attributes #[[NOBUILTIN]] = { nobuiltin }
; CHECK: attributes #[[STRICT]] = { nobuiltin strictfp }
; CHECK-NOT: !"c2go.cc.violations"

!llvm.module.flags = !{!0}
!c2go.libcall.routes = !{!1, !2, !3, !4, !5, !6, !7, !8}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{!"strlen", !"example.com/lib.strlen"}
!2 = !{!"puts", !"example.com/lib.puts"}
!3 = !{!"strcmp", !"example.com/lib.strcmp"}
!4 = !{!"sin", !"example.com/lib.sin"}
!5 = !{!"fmod", !"example.com/lib.fmod"}
!6 = !{!"modf", !"example.com/lib.modf"}
!7 = !{!"frexp", !"example.com/lib.frexp"}
!8 = !{!"sincos", !"example.com/lib.sincos"}
