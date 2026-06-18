; C2GoEscapeCheckPass instruments every `store ptr %v, ptr %dst` whose
; destination is NOT provably a stack slot (alloca) with a call to the runtime
; helper EscapeCheck(%v, %dst, <site>), so at run time the helper can compare %v
; against [g.stack.lo, g.stack.hi) and report a stack-address escape.
;
; The pass is registered as c2go-escape-check, reachable from opt -passes=.
; This test asserts:
;   1. the pass name resolves from the new-PM pass registry (a RUN line that
;      reaches FileCheck proves PassBuilder accepted the spec);
;   2. the pass is OFF by default (no instrumentation without the
;      -c2go-escape-check option);
;   3. when enabled, a store of a stack pointer (alloca) into a heap-typed dst
;      (a `db->pParse = &localParse` shape: parser context owned by the stack,
;      written into a struct field reachable via the heap-resident db) is
;      wrapped with the EscapeCheck helper call, using GoABI0 CC and a
;      site-string global named c2go.escape.site*;
;   4. a control store whose destination IS provably a stack alloca is NOT
;      instrumented (the cheap stack-to-stack filter).
;
; The fixture is a 2-store reduction of that shape: a stack alloca written into
; a heap-typed db.
;
; RUN: opt < %s -passes=c2go-escape-check -S \
; RUN:   | FileCheck %s --check-prefix=OFF
;
; RUN: opt < %s -passes=c2go-escape-check -c2go-escape-check -S \
; RUN:   | FileCheck %s --check-prefix=ON

target triple = "aarch64-unknown-linux-gnu"

declare void @sink(ptr)

; The "heap" destination: a Go-side global in managed addrspace(1). The pass
; normalizes both args to addrspace(0) before the helper call.
@db = external addrspace(1) global ptr

define void @escape_site(ptr addrspace(1) %parse_in_heap) {
entry:
  %localParse = alloca i64, align 8
  ; Stack-to-stack: must NOT be instrumented.
  %stack_slot = alloca ptr, align 8
  store ptr %localParse, ptr %stack_slot, align 8
  ; Stack-to-heap: db->pParse = &localParse - must be instrumented.
  store ptr %localParse, ptr addrspace(1) %parse_in_heap, align 8
  call void @sink(ptr %localParse)
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}

; OFF path: with no -c2go-escape-check the pass is a strict no-op - the helper
; declaration is absent and no call appears. The patterns below match the
; fully-qualified helper name and the site-string global key rather than a bare
; "Escape" + "Check" substring, which could match an unrelated identifier or a
; value-name suffix; the qualified name is exclusive to the pass's emit shape.
; OFF-LABEL: define void @escape_site(
; OFF-NOT: c2go.escape.site
; OFF-NOT: @"github.com/c2go_project/c2go_libc.EscapeCheck"

; ON path: with -c2go-escape-check the stack-to-heap store is followed by a
; call to the GoABI0 helper, referencing a site-string global named
; c2go.escape.site*. The stack-to-stack store stays plain.
; ON-DAG: @c2go.escape.site
; ON-LABEL: define void @escape_site(
; ON:       store ptr %localParse, ptr %stack_slot
; ON-NOT:   EscapeCheck
; ON:       store ptr %localParse, ptr addrspace(1) %parse_in_heap
; ON:       call goabi0cc void @"github.com/c2go_project/c2go_libc.EscapeCheck"({{.*}}@c2go.escape.site
; ON-DAG: declare goabi0cc void @"github.com/c2go_project/c2go_libc.EscapeCheck"
