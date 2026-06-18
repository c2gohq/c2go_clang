; When enforceCallSiteCC observes more than one wrong-CC call site in a single
; module, it must REPLACE the existing module-flag entry rather than APPEND a
; second entry under the same key. Module flag identifiers must be unique, so a
; second entry would trip "module flag identifiers must be unique" and the IR
; would not survive the verifier at all.
;
; This test pre-pollutes TWO wrong-CC call sites against _c2go_writePtr so the
; sweep increments c2go.cc.violations twice in the same module. An appending
; update would create a duplicate key and fail the verifier; a replacing update
; collapses to a single flag entry the verifier accepts.
;
; RUN: opt < %s -passes=c2go-write-barriers,verify -S 2>%t.err | FileCheck %s
; RUN: FileCheck %s --check-prefix=WARN < %t.err

target triple = "aarch64-unknown-linux-gnu"

declare void @_c2go_writePtr(ptr addrspace(1), ptr addrspace(1))

; Caller #1: wrong CC + a barrier-triggering managed store so the write-barriers
; pass enters the post-insertion sweep path.
define void @prepolluted_1(ptr addrspace(1) %slot,
                            ptr addrspace(1) %slot2,
                            ptr addrspace(1) %val) {
  call void @_c2go_writePtr(ptr addrspace(1) %slot, ptr addrspace(1) %val)
  store ptr addrspace(1) %val, ptr addrspace(1) %slot2
  ret void
}

; Caller #2: second wrong-CC call site in the same module, in a different
; function so the helper's use list holds two call sites to rewrite.
define void @prepolluted_2(ptr addrspace(1) %slot,
                            ptr addrspace(1) %slot2,
                            ptr addrspace(1) %val) {
  call void @_c2go_writePtr(ptr addrspace(1) %slot, ptr addrspace(1) %val)
  store ptr addrspace(1) %val, ptr addrspace(1) %slot2
  ret void
}

; The sweep rewrites both call sites to goabi0cc.
; CHECK-LABEL: define void @prepolluted_1
; CHECK: call goabi0cc void @_c2go_writePtr(
; CHECK-LABEL: define void @prepolluted_2
; CHECK: call goabi0cc void @_c2go_writePtr(

; Module flag present exactly once - the second sweep call replaced (not
; appended) the running counter; otherwise the verifier in the RUN line above
; would reject the module.
; CHECK: !{i32 7, !"c2go.cc.violations", i32 2}

; Two warning lines on stderr, one per rewritten call site. Order follows
; use-list iteration order, so the patterns below accept either ordering.
; WARN-DAG: c2go: WARNING — call site CC mismatch for helper '_c2go_writePtr' in function 'prepolluted_1'{{.*}}rewrote to match
; WARN-DAG: c2go: WARNING — call site CC mismatch for helper '_c2go_writePtr' in function 'prepolluted_2'{{.*}}rewrote to match

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
