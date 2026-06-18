// Build-time pin of the cross-repo //go:nosplit <-> gc-leaf-function contract
// for the three c2go-libc helper symbols emitted by the Clang c2go pipeline:
//
//   _c2go_writePtr          (write-barrier slow-path shim)
//   _c2go_typedmemmove      (memcpy-typing singleton routing)
//   _c2go_typedMemmoveArray (memcpy-typing array routing)
//
// Each helper is declared with the GoABI0 calling convention (goabi0cc) and
// the gc-leaf-function attribute. That attribute lets RewriteStatepointsForGC
// skip wrapping each call in a gc.statepoint. The skip is only sound because
// the Go-side bodies carry //go:nosplit, so the helpers neither morestack nor
// relocate the caller's stack across the call.
//
// The IR-level invariants in
//   llvm/test/Transforms/C2Go/c2go-write-barriers.ll
//   llvm/test/Transforms/C2Go/c2go-memcpy-typing-leaf-attr.ll
// pin the individual pass-output shapes but do NOT cover the full FE-to-
// optimizer pipeline; this is the FE-end pin. The Go-side half lives in
// llvm/test/tools/c2go-bind/c2go-bind-nosplit-emit.test (gated on the
// external c2go-bind binary). Together they catch a regression on either
// side at build time.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -mllvm -c2go-managed-addrspace -Wno-c2go-managed-as1 \
// RUN:   -emit-llvm -o - %s | FileCheck %s

struct __attribute__((c2go_managed)) N { struct N *next; int v; };

// Triggers c2go-write-barriers -> a slow-path call to _c2go_writePtr on the
// managed-store-to-heap path.
void store_to_heap(struct N *p, struct N *q) { p->next = q; }

// Triggers c2go-memcpy-typing singleton routing: an aggregate copy of a single
// c2go_managed N -> _c2go_typedmemmove(typeinfo.N, dst, src).
void copy_single(struct N *d, struct N *s) { *d = *s; }

// Triggers c2go-memcpy-typing array routing: an explicit memmove of
// N * sizeof(struct N) bytes with the typed dst pointer kept (no void* cast)
// -> _c2go_typedMemmoveArray(typeinfo.N, dst, src, N).
void copy_array(struct N *d, struct N *s) {
  __builtin_memmove(d, s, sizeof(struct N) * 10);
}

// All three helpers must be declared with the GoABI0 calling convention
// (printed goabi0cc; the wildcard also absorbs the legacy numeric cc128).
//
// CHECK-DAG: declare {{(cc128 |goabi0cc )}}void @_c2go_writePtr(ptr addrspace(1), ptr addrspace(1)) [[ATTRS:#[0-9]+]]
// CHECK-DAG: declare {{(cc128 |goabi0cc )}}void @_c2go_typedmemmove(ptr, ptr, ptr) [[ATTRS]]
// CHECK-DAG: declare {{(cc128 |goabi0cc )}}void @_c2go_typedMemmoveArray(ptr, ptr, ptr, i64) [[ATTRS]]

// The shared attribute group MUST contain gc-leaf-function. A failure here
// means the gc-leaf-function attribute was dropped on one of the helper
// shims without also removing the matching //go:nosplit pragma on the Go
// side. Keep both sides, or remove both - never mismatch.
//
// CHECK: attributes [[ATTRS]] = { "gc-leaf-function" }
