; --c2go-andersen-field-sensitive on/off contrast for the escape audit.
;
; The differentiating pattern: a stack-resident struct with two pointer
; fields (stack@0, stack@8) acts as a transient container; the program
; writes a third stack address into stack@0, then reloads stack@8 and
; stores the loaded value into a heap. Under field-INSENSITIVE analysis
; stack@0 and stack@8 collapse to one contents cell - the load at stack@8
; picks up the address written to stack@0, so the final store into the
; heap looks like a transitive escape of that address. Under
; field-SENSITIVE analysis stack@0 and stack@8 are distinct cells, so the
; load at stack@8 has empty points-to and the store does NOT report.
;
; Expected dedup-default counts (one report per (function, source-alloca)):
;   field-insensitive: 3 -- a, b, target_a all flag as escaping (the latter
;                          pulled transitively by the collapsed-stack load).
;   field-sensitive:   2 -- a and b only; target_a never appears in the load.

; RUN: not c2go-lto --c2go-andersen-field-sensitive=false %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=INSENS
; RUN: not c2go-lto --c2go-andersen-field-sensitive=true %s 2>&1 \
; RUN:   | FileCheck %s --check-prefix=SENS

; INSENS: c2go-lto: 3 stack-address escape point(s)
; SENS: c2go-lto: 2 stack-address escape point(s)

target triple = "aarch64-unknown-linux"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128"

%struct.SS = type { ptr, ptr }

declare ptr @malloc(i64)

define void @field_sens_demo() {
entry:
  %a = alloca i64
  %b = alloca i64
  %target_a = alloca i64

  %h = call ptr @malloc(i64 32)

  ; Two heap pointer fields: heap.p1 (off=0), heap.p2 (off=8).
  %h.p1 = getelementptr inbounds %struct.SS, ptr %h, i32 0, i32 0
  %h.p2 = getelementptr inbounds %struct.SS, ptr %h, i32 0, i32 1

  ; Two stack-resident pointer fields in one alloca'd struct.
  %s = alloca %struct.SS
  %s.p1 = getelementptr inbounds %struct.SS, ptr %s, i32 0, i32 0
  %s.p2 = getelementptr inbounds %struct.SS, ptr %s, i32 0, i32 1

  ; (1) Direct escape: a stored straight into heap field. Both modes report.
  store ptr %a, ptr %h.p1

  ; (2) Direct escape: b stored straight into the OTHER heap field.
  store ptr %b, ptr %h.p2

  ; (3) Write target_a into stack@0 only; never write into stack@8.
  store ptr %target_a, ptr %s.p1

  ; Reload stack@8 (empty under field-sensitive). Field-insensitive, the s
  ; contents-cell is one bag that already holds %target_a from the prior
  ; store, so this load picks it up. Field-sensitive: stack@0 and stack@8 are
  ; distinct contents cells - the load at stack@8 has no stack alloca in pts.
  %loaded = load ptr, ptr %s.p2

  ; Store the loaded value into heap. Field-insensitive: pts(loaded)
  ; contains %target_a (new (fn, alloca) tuple, increments the dedup count).
  ; Field-sensitive: pts(loaded) has no stack cells - no escape.
  store ptr %loaded, ptr %h.p2

  ret void
}
