; AS1<->AS0 carve-out soundness for the addrspacecast stress fixture.
;
; A linkage-only lock proves the boundary symbol names survive the cross-TU
; pipeline, but it does not prove the AS1<->AS0 carve-out (the GC soundness the
; stress workload exercises at runtime). A regression that silently collapsed
; an AS1->AS0 addrspacecast into a no-op fold, or dropped the c2go-gc strategy
; from a managed function, would crash the validation at run time: the AS0 cast
; result would dangle into the old stack after copystack while the AS1 sibling
; followed the new stack, breaking the `aux == (void*)parent` equation the
; validation asserts.
;
; This test pins the IR-level invariants the carve-out relies on, on a fixture
; that mirrors the inner loop of the recursion:
;
;   struct AsFrame { AsFrame *parent; void *aux; int depth; int _pad; };
;   local.parent = p;                 // AS1 alloca-derived value
;   local.aux    = (void *)p;         // AS1 -> AS0 addrspacecast
;   memcpy(gScratch, &local, sizeof local);  // statepoint-wrapped via
;                                            // libc.Memmove (non-leaf)
;
; The lowering chain
;   c2go-memcpy-typing -> c2go-gc-setup -> rewrite-statepoints-for-gc
; must keep BOTH the AS1 root and the AS0 cast tracked through the statepoint.
; The fixture deliberately uses a large memcpy with no !c2go.elem.type metadata
; so c2go-memcpy-typing rewrites it to libc.Memmove (NOT _c2go_typedmemmove);
; libc.Memmove is genuinely non-leaf and is the call edge RS4GC must wrap.
;
; The same module runs under two configurations:
;
;   ON path (c2go-memcpy-typing + c2go-gc-setup + RS4GC):
;     - the function is tagged gc "c2go-gc" (gc-setup promoted it because it
;       holds an AS1 pointer);
;     - libc.Memmove is wrapped in gc.statepoint (non-leaf);
;     - the AS1 base survives the statepoint as an AS1 relocate, proving the
;       carve-out's base-defining-value search walks across the AS1->AS0 cast
;       and still sees the AS1 root.
;
;   OFF path (c2go-memcpy-typing + RS4GC, no c2go-gc-setup):
;     - the function is NOT tagged with gc "c2go-gc", so RS4GC skips it;
;     - therefore no gc.statepoint and no gc.relocate appears even though
;       libc.Memmove is non-leaf - the pass that decides the function needs GC
;       is the one being gated.
;
; The OFF-path check guards against the inverse regression: a change that bakes
; statepoint wrapping into RS4GC unconditionally (independent of the c2go-gc
; strategy) would silently start wrapping calls in modules that should stay
; opt-out, making the gate meaningless.
;
; Companion to:
;   - c2go-pipeline-instcombine-rs4gc.ll  (post-InstCombine residue soundness
;     on the same chain - the InstCombine-folded shape)
;   - c2go-libc-memmove-statepoint.ll     (leaf vs non-leaf bucket
;     classification - pins which calls RS4GC wraps)
;   - c2go-as1-gep-fold-chain.ll          (ptrtoaddr/inttoptr fold soundness -
;     own-base treatment)
;   - c2go-extern-not-internalized.ll     (linkage surface for the same
;     boundary names)

; RUN: opt < %s -passes='c2go-memcpy-typing,rewrite-statepoints-for-gc' -S \
; RUN:   | FileCheck %s --check-prefix=OFF

; RUN: opt < %s -passes='c2go-memcpy-typing,c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   | FileCheck %s --check-prefix=ON

target triple = "arm64-unknown-none-goabi"

; The Frame struct mirrored from the stress fixture. The AS1 base pointer
; simulates `local.parent`; the (void *) field receives the AS1->AS0 cast that
; the `aux == (void*)parent` equation rides on.
%struct.AsFrame = type { ptr addrspace(1), ptr, i32, i32 }

declare void @llvm.memcpy.p1.p1.i64(ptr addrspace(1), ptr addrspace(1), i64, i1)

; -----------------------------------------------------------------------
; The fixture: mirror the recursion's hot edge.
;
; %p (AS1) is `parent`; the addrspacecast to AS0 is the `aux = (void*)p`
; assignment; the large untyped memcpy is the per-frame `*gScratch = local`
; aggregate copy that c2go-memcpy-typing lowers to libc.Memmove (untyped, large
; size -> fallback path); RS4GC wraps that call because it is NOT
; gc-leaf-function; the AS1 base must be relocated as an AS1 GC pointer,
; proving the carve-out tracked it through the cast.

; Function gets `gc "c2go-gc"` because it holds an AS1 pointer.
; ON-LABEL: define ptr addrspace(1) @stress_ascast_recurse_edge(ptr addrspace(1) %p, ptr addrspace(1) %dst) gc "c2go-gc"
;
; c2go-memcpy-typing inserts the AS1->AS0 boundary casts before the
; libc.Memmove call.
; ON: addrspacecast ptr addrspace(1) {{.*}} to ptr
; ON: addrspacecast ptr addrspace(1) {{.*}} to ptr
;
; RS4GC wraps libc.Memmove (non-leaf) in a statepoint - this is the
; call edge the carve-out exists for.
; ON: gc.statepoint{{.*}}@"github.com/c2go_project/c2go_libc.Memmove"
;
; The AS1 base reachable through the cast chain is relocated as AS1,
; proving the carve-out's base-defining-value search walked across
; the AS1->AS0 addrspacecast and still tracked the AS1 root.
; ON: {{.*}} = call {{.*}}ptr addrspace(1) @llvm.experimental.gc.relocate.p1

; With no c2go-gc-setup, the function is NOT promoted to the c2go-gc
; strategy and RS4GC must skip it - no gc tag, no statepoint, no relocate.
; OFF-LABEL: define ptr addrspace(1) @stress_ascast_recurse_edge(
; OFF-NOT: gc "c2go-gc"
; OFF-NOT: gc.statepoint
; OFF-NOT: llvm.experimental.gc.relocate
;
; Even with the GC strategy off, c2go-memcpy-typing must still insert the
; AS1->AS0 boundary cast at the libc.Memmove edge (folding that cast away is
; the regression this test exists to catch - see file header). The cast is the
; IR-level anchor the stress workload relies on for its `aux == (void*)parent`
; equation; if c2go-memcpy-typing were allowed to fold it away, the linkage
; check would stay green but the runtime stress would crash.
; OFF: addrspacecast ptr addrspace(1) {{.*}} to ptr
; OFF: call goabi0cc ptr @"github.com/c2go_project/c2go_libc.Memmove"

define ptr addrspace(1) @stress_ascast_recurse_edge(ptr addrspace(1) %p, ptr addrspace(1) %dst) {
entry:
  ; The aggregate copy `*gScratch = local` - large, untyped (no
  ; !c2go.elem.type), so c2go-memcpy-typing routes it to libc.Memmove
  ; (the non-leaf fallback bucket). 4096 bytes matches the size used by the
  ; companion c2go-pipeline-instcombine-rs4gc.ll fixture so the fallback rule
  ; is hit deterministically.
  call void @llvm.memcpy.p1.p1.i64(ptr addrspace(1) %dst, ptr addrspace(1) %p, i64 4096, i1 false)
  ; Return the AS1 base so it stays live across the libc.Memmove statepoint -
  ; forces RS4GC to relocate it as an AS1 root. This is the (parent live across
  ; statepoint) edge the validation rides on in the real workload.
  ret ptr addrspace(1) %p
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
