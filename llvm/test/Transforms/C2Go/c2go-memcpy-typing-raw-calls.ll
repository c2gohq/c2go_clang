; Direct memory calls that escaped builtin folding must still be made explicit
; before GC/codegen. Dynamic calls use package-qualified GoABI0 helpers; small
; constant calls become inline-only intrinsics.
;
; RUN: opt < %s -passes=c2go-memcpy-typing -S | FileCheck %s

target triple = "aarch64-unknown-linux-goabi"

declare ptr @memcpy(ptr, ptr, i64)
declare ptr @memmove(ptr, ptr, i64)
declare ptr @memset(ptr, i32, i64)
declare void @bzero(ptr, i64)

define ptr @dynamic_memcpy(ptr %dst, ptr %src, i64 %n) {
  %r = call ptr @memcpy(ptr %dst, ptr %src, i64 %n)
  ret ptr %r
}

define ptr @small_memmove(ptr %dst, ptr %src) {
  %r = call ptr @memmove(ptr %dst, ptr %src, i64 8)
  ret ptr %r
}

define ptr @dynamic_memmove(ptr %dst, ptr %src, i64 %n) {
  %r = call ptr @memmove(ptr %dst, ptr %src, i64 %n)
  ret ptr %r
}

define ptr @dynamic_memset(ptr %dst, i32 %value, i64 %n) {
  %r = call ptr @memset(ptr %dst, i32 %value, i64 %n)
  ret ptr %r
}

define void @small_bzero(ptr %dst) {
  call void @bzero(ptr %dst, i64 8)
  ret void
}

define void @dynamic_bzero(ptr %dst, i64 %n) {
  call void @bzero(ptr %dst, i64 %n)
  ret void
}

; CHECK-LABEL: define ptr @dynamic_memcpy(
; CHECK: call goabi0cc ptr @"github.com/c2gohq/c2go_libc.memcpy"(ptr %dst, ptr %src, i64 %n)
; CHECK: ret ptr %dst

; CHECK-LABEL: define ptr @small_memmove(
; CHECK: call void @llvm.memmove.p0.p0.i64(ptr %dst, ptr %src, i64 8, i1 false)
; CHECK: ret ptr %dst

; CHECK-LABEL: define ptr @dynamic_memmove(
; CHECK: call goabi0cc ptr @"github.com/c2gohq/c2go_libc.memmove"(ptr %dst, ptr %src, i64 %n)
; CHECK: ret ptr %dst

; CHECK-LABEL: define ptr @dynamic_memset(
; CHECK: call goabi0cc ptr @"github.com/c2gohq/c2go_libc.memset"(ptr %dst, i32 %value, i64 %n)
; CHECK: ret ptr %dst

; CHECK-LABEL: define void @small_bzero(
; CHECK: call void @llvm.memset.p0.i64(ptr %dst, i8 0, i64 8, i1 false)

; CHECK-LABEL: define void @dynamic_bzero(
; CHECK: call goabi0cc ptr @"github.com/c2gohq/c2go_libc.memset"(ptr %dst, i32 0, i64 %n)

; CHECK: declare goabi0cc ptr @"github.com/c2gohq/c2go_libc.memcpy"(ptr, ptr, i64)
; CHECK: declare goabi0cc ptr @"github.com/c2gohq/c2go_libc.memmove"(ptr, ptr, i64)
; CHECK: declare goabi0cc ptr @"github.com/c2gohq/c2go_libc.memset"(ptr, i32, i64)
