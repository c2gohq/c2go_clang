; c2go uses LLVM's existing InlineAsm lowering, but an InlineAsm CallBase is
; not by itself a machine CALL/BL and therefore cannot enter a callee's
; morestack prologue. All three c2go safepoint consumers must share that
; answer:
;   * c2go-gc-setup marks inline asm gc-leaf so RS4GC does not wrap it;
;   * c2go-safepoint emits no lightweight stackmap for it;
;   * c2go-loop-poll does not let it suppress a required loop poll.
;
; Cover both musl's empty pointer barrier and a non-empty leaf instruction:
; support is for existing call-free inline asm, not one hard-coded template.
;
; RUN: opt < %s -passes=c2go-gc-setup -S | FileCheck %s --check-prefix=SETUP
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S | FileCheck %s --check-prefix=RS4GC
; RUN: opt < %s -passes=c2go-safepoint -S | FileCheck %s --check-prefix=LIGHT
; RUN: opt < %s -passes=c2go-loop-poll -c2go-loop-poll=1 -c2go-loop-poll-target-ns=10000000 -S | FileCheck %s --check-prefix=POLL

target triple = "aarch64-unknown-linux-gnu"

declare void @real_call()

; SETUP-LABEL: define ptr @inline_asm_gc(ptr %p) gc "c2go-gc"
; SETUP: call void asm sideeffect "", "r,~{memory}"(ptr %p) #[[LEAF:[0-9]+]]
; SETUP: call void asm sideeffect "yield", "~{memory}"() #[[LEAF]]
; SETUP: attributes #[[LEAF]] = { "gc-leaf-function" }
;
; RS4GC-LABEL: define ptr @inline_asm_gc(ptr %p) gc "c2go-gc"
; RS4GC-NOT: gc.statepoint
; RS4GC: call void asm sideeffect "", "r,~{memory}"(ptr %p)
; RS4GC-NOT: gc.statepoint
; RS4GC: call void asm sideeffect "yield", "~{memory}"()
; RS4GC-NOT: gc.statepoint
; RS4GC: ret ptr %p
define ptr @inline_asm_gc(ptr %p) {
entry:
  call void asm sideeffect "", "r,~{memory}"(ptr %p)
  call void asm sideeffect "yield", "~{memory}"()
  ret ptr %p
}

; A pointer produced by inline asm is an ordinary CallInst result and remains
; a valid RS4GC base at a later real safepoint. Only the InlineAsm callee value
; itself must stay out of the parse-point live set.
;
; RS4GC-LABEL: define ptr @inline_asm_pointer_result(ptr %p) gc "c2go-gc"
; RS4GC: %out = call ptr asm sideeffect "", "=r,0"(ptr %p)
; RS4GC: gc.statepoint{{.*}}@real_call
; RS4GC: %out.relocated = call {{.*}}ptr @llvm.experimental.gc.relocate
; RS4GC: ret ptr %out.relocated
define ptr @inline_asm_pointer_result(ptr %p) {
entry:
  %out = call ptr asm sideeffect "", "=r,0"(ptr %p)
  call void @real_call()
  ret ptr %out
}

; Conversely, an InlineAsm callee used after a real safepoint is not itself a
; relocatable pointer. The data pointer operand is live and must be relocated;
; the constant-like InlineAsm descriptor must stay out of the gc-live bundle.
; This is the optimized musl explicit_bzero shape.
;
; RS4GC-LABEL: define void @real_call_before_inline_asm(ptr %p) gc "c2go-gc"
; RS4GC: @real_call
; RS4GC-SAME: "gc-live"(ptr %p)
; RS4GC: %p.relocated = call {{.*}}ptr @llvm.experimental.gc.relocate
; RS4GC: call void asm sideeffect "", "r,~{memory}"(ptr %p.relocated)
; RS4GC: ret void
define void @real_call_before_inline_asm(ptr %p) {
entry:
  call void @real_call()
  call void asm sideeffect "", "r,~{memory}"(ptr %p)
  ret void
}

; A managed local remains live across both asm statements. The lightweight
; path must not manufacture a stackmap for either statement.
;
; LIGHT-LABEL: define ptr @inline_asm_light(ptr %p)
; LIGHT-NOT: @llvm.experimental.stackmap
; LIGHT: call void asm sideeffect "", "r,~{memory}"(ptr %p)
; LIGHT-NOT: @llvm.experimental.stackmap
; LIGHT: call void asm sideeffect "yield", "~{memory}"()
; LIGHT-NOT: @llvm.experimental.stackmap
; LIGHT: ret ptr
define ptr @inline_asm_light(ptr %p) {
entry:
  %slot = alloca ptr, align 8, !c2go.ptr.managed !1
  store ptr %p, ptr %slot, align 8
  call void asm sideeffect "", "r,~{memory}"(ptr %p)
  call void asm sideeffect "yield", "~{memory}"()
  %result = load ptr, ptr %slot, align 8
  ret ptr %result
}

; A call-free loop containing inline asm still needs a cooperative poll.
;
; POLL-LABEL: define void @inline_asm_loop(ptr %p)
; POLL: c2go.lp.cnt = alloca i64
; POLL: call void asm sideeffect "", "r,~{memory}"(ptr %p)
; POLL: call goabi0cc void @"github.com/c2gohq/c2go_libc.Gosched"()
define void @inline_asm_loop(ptr %p) #0 {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %next, %loop ]
  call void asm sideeffect "", "r,~{memory}"(ptr %p)
  %next = add i64 %i, 1
  %done = icmp eq i64 %next, 1000000
  br i1 %done, label %exit, label %loop

exit:
  ret void
}

attributes #0 = { "c2go-c-name"="inline_asm_loop" }

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
!1 = !{}
