; Every C2Go pass that declares or reuses a GoABI0 helper must run a pass-end
; enforceCallSiteCC sweep - otherwise a pre-existing CallInst (from an upstream
; lowering, a hand-written IR fixture, or a future rewrite) keeps its original
; default-C CC and the Go-linker-generated ABI0 entry on the callee reads
; garbage off the stack at runtime. The sweep rewrites each call site CC to
; match the declared CC and emits one WARNING line per fix.
;
; This test pre-pollutes a managed-pointer store's _c2go_writePtr slow path AND
; a c2go-loop-poll natural-loop body's Gosched slot with a wrong-CC CallInst,
; runs the respective pass, and checks:
;   (a) the call site CC ends up goabi0cc (the sweep rewrote it), and
;   (b) the diagnostic landed on stderr so the upstream emitter is visible.
;
; RUN: opt < %s -passes=c2go-write-barriers -S 2>%t.wb.err | FileCheck %s --check-prefix=WB
; RUN: FileCheck %s --check-prefix=WB-WARN < %t.wb.err
;
; RUN: opt < %s -passes=c2go-loop-poll -c2go-loop-poll=1 -S 2>%t.lp.err | FileCheck %s --check-prefix=LP
; RUN: FileCheck %s --check-prefix=LP-WARN < %t.lp.err

target triple = "aarch64-unknown-linux-gnu"

; -----------------------------------------------------------------------------
; Track 1: C2GoWriteBarriers - pre-existing wrong-CC call site on _c2go_writePtr.
; -----------------------------------------------------------------------------

; Declare the shim with the default C CC AND a pre-existing wrong CC at a call
; site. The pass retrofits the declaration to goabi0cc; the pass-end
; enforceCallSiteCC sweep must also rewrite this pre-existing call.
declare void @_c2go_writePtr(ptr addrspace(1), ptr addrspace(1))

; The pre-pollution caller: a direct call carrying the wrong CC (default C).
; The upstream emitter could be any earlier pass; the canonical failure mode is
; the helper got retrofitted but the older call site did not.
;
; The write-barriers post-insertion sweep only runs when the barrier worklist
; is non-empty, so the caller below also emits a managed-ptr store into managed
; memory (via a second AS1 slot pointer) to enter the sweep path, where the
; sweep observes the wrong-CC call above.
define void @prepolluted_writePtr_callsite(ptr addrspace(1) %slot,
                                            ptr addrspace(1) %slot2,
                                            ptr addrspace(1) %val) {
  call void @_c2go_writePtr(ptr addrspace(1) %slot, ptr addrspace(1) %val)
  ; trigger the barrier Worklist so the post-insertion sweep runs.
  store ptr addrspace(1) %val, ptr addrspace(1) %slot2
  ret void
}

; The helper declaration ends up GoABI0 + gc-leaf-function (the existing shim
; retrofit, pinned here so a regression on that path is also caught). It
; appears first in the module output, so anchor it before the function-level
; checks.
; WB: declare goabi0cc void @_c2go_writePtr({{.*}}) [[WP_ATTRS:#[0-9]+]]

; After the sweep, every direct call to _c2go_writePtr (including the
; pre-polluted one above and any new fast/slow-path calls emitted by the
; pass) must carry goabi0cc.
; WB-LABEL: define void @prepolluted_writePtr_callsite
; WB: call goabi0cc void @_c2go_writePtr(

; Attribute group lookup - comes after the function bodies in opt -S output.
; WB: attributes [[WP_ATTRS]] = {{.*}}"gc-leaf-function"{{.*}}

; The sweep emits exactly one WARNING line for the pre-polluted call site.
; WB-WARN: c2go: WARNING — call site CC mismatch for helper '_c2go_writePtr' in function 'prepolluted_writePtr_callsite'{{.*}}rewrote to match

; -----------------------------------------------------------------------------
; Track 2: C2GoLoopPoll - pre-existing wrong-CC call site on the Gosched bridge.
; -----------------------------------------------------------------------------

declare void @"github.com/c2go_project/c2go_libc.Gosched"()

; Pre-polluted caller: a direct call carrying the wrong CC. The c2go-loop-poll
; pass injects its own GoABI0-CC call into a separate function below; the
; pass-end sweep must rewrite THIS call too.
define void @prepolluted_gosched_callsite() #0 {
  call void @"github.com/c2go_project/c2go_libc.Gosched"()
  ret void
}

; A second function with a c2go-managed natural NoCall loop drives the
; c2go-loop-poll injection path, which retrofits the helper declaration to
; GoABI0. Without the pass-end sweep the pre-polluted call in
; @prepolluted_gosched_callsite would silently keep the wrong CC.
define void @c2go_nocall_loop_driver(ptr %p) #0 {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, 1000000
  br i1 %done, label %exit, label %loop
exit:
  ret void
}

; After the sweep, the pre-polluted call site carries goabi0cc.
; LP-LABEL: define void @prepolluted_gosched_callsite
; LP: call goabi0cc void @"github.com/c2go_project/c2go_libc.Gosched"()

; The injected loop-poll call in the driver function is also goabi0cc.
; LP-LABEL: define void @c2go_nocall_loop_driver
; LP: call goabi0cc void @"github.com/c2go_project/c2go_libc.Gosched"()

; Diagnostic on stderr: at least one WARNING for the pre-polluted Gosched
; callsite (the line names the offending caller so CI can grep it).
; LP-WARN: c2go: WARNING — call site CC mismatch for helper 'github.com/c2go_project/c2go_libc.Gosched' in function 'prepolluted_gosched_callsite'{{.*}}rewrote to match

attributes #0 = { "c2go-c-name"="x" }

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
