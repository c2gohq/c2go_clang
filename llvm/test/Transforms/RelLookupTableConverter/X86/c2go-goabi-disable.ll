; The relative-lookup-table converter rewrites a [N x ptr] table into a
; [N x i32] PC-relative-offset table, which the AsmPrinter emits as
; 4-byte symbol-relative values. The Go assembler rejects 4-byte address
; data on 64-bit targets (pointer-sized address data must be 8 bytes), so
; under the GoABI0 path (the c2go.goabi module flag) the converter must be
; a no-op and keep the original [N x ptr] table.
;
; This test pins that gate: with c2go.goabi set, the original
; switch.table.string_table global survives unchanged and no relative
; companion table is created. Without the flag the pass converts as usual
; (covered by the sibling relative_lookup_table.ll test).

; REQUIRES: x86-registered-target
; RUN: opt < %s -passes=rel-lookup-table-converter -relocation-model=pic -S | FileCheck %s
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@.str = private unnamed_addr constant [5 x i8] c"zero\00", align 1
@.str.1 = private unnamed_addr constant [4 x i8] c"one\00", align 1
@.str.2 = private unnamed_addr constant [4 x i8] c"two\00", align 1

@switch.table.string_table = private unnamed_addr constant [3 x ptr]
  [
  ptr @.str,
  ptr @.str.1,
  ptr @.str.2
  ], align 8

; c2go.goabi gate active => pass must not convert the table.
; CHECK: @switch.table.string_table = private unnamed_addr constant [3 x ptr] [ptr @.str, ptr @.str.1, ptr @.str.2], align 8
; CHECK-NOT: @switch.table.string_table.rel
; CHECK-NOT: [3 x i32]

define ptr @string_table_lookup(i64 %idx) {
entry:
  %arrayidx = getelementptr inbounds [3 x ptr], ptr @switch.table.string_table, i64 0, i64 %idx
  %0 = load ptr, ptr %arrayidx, align 8
  ret ptr %0
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
