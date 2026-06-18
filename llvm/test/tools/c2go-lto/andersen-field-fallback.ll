; Soundness of the field-sensitive Andersen escape analysis: when a GEP
; has a dynamic (non-constant) index, or computes an offset outside the
; known object size, cellAt() must fall back to the collapsed cell so the
; offending store still flags as an escape. This pins that the fallback
; path does not silently drop an escape (no missed escape).

; RUN: not c2go-lto --c2go-andersen-field-sensitive=true %s 2>&1 \
; RUN:   | FileCheck %s

; CHECK: c2go-lto: stack->heap in dyn_idx
; CHECK: c2go-lto: stack->heap in oob_idx
; CHECK: c2go-lto: 2 stack-address escape point(s)

target triple = "aarch64-unknown-linux"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128"

declare ptr @malloc(i64)

; Dynamic index GEP - accumulateConstantOffset fails; field-sensitive path
; must fall back to a collapsed copy edge so the store still reports.
define void @dyn_idx(i64 %i) {
  %a = alloca i64
  %h = call ptr @malloc(i64 64)
  %h.i = getelementptr inbounds ptr, ptr %h, i64 %i
  store ptr %a, ptr %h.i
  ret void
}

; Constant offset outside the malloc'd size - cellAt() detects the
; out-of-bounds offset and falls back to the collapsed heap cell, so the
; store still reports the escape.
define void @oob_idx() {
  %b = alloca i64
  %h = call ptr @malloc(i64 8)
  %h.far = getelementptr inbounds i8, ptr %h, i64 64
  store ptr %b, ptr %h.far
  ret void
}
