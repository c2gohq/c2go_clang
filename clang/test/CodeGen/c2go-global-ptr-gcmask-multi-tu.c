// Cat-merged multi-TU at -O2 must preserve every per-var
// @c2go.global.gcmask.<X> GV individually, even when several globals share
// an identical mask byte pattern.
//
// Repro shape: simulate two C TUs cat-merged into one clang invocation.
// Each "TU" declares single-pointer-word static globals, all with the same
// gcmask byte (c"\01"). Emitted as `internal unnamed_addr constant`, -O2
// ConstantMerge would coalesce them into one survivor and erase the rest;
// the gcmask collector then walks named globals by prefix and the manifest
// loses every coalesced entry, breaking the Go-side root decls. The gcmask
// GV name is load-bearing (manifest matching is name-based), so it must NOT
// carry unnamed_addr.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O2 -emit-llvm -o %t.O2.ll %s
// RUN: FileCheck %s --check-prefix=O2 --input-file=%t.O2.ll

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

#define c2go_extern     __attribute__((c2go_extern))

#pragma c2go managed(6) push
struct Node {
  struct Node *next;
  long         tag;
};
#pragma c2go pop

// "TU A" single-pointer-word statics.
static struct Node *gA1;
static struct Node *gA2;

// "TU B" single-pointer-word statics. Same mask byte (c"\01") as A's; the
// buggy path collapsed all four into one via ConstantMerge.
static struct Node *gB1;
static struct Node *gB2;

c2go_extern void use_all(struct Node *n);
void use_all(struct Node *n) {
  gA1 = n; gA2 = n; gB1 = n; gB2 = n;
}

// All four gcmask GVs must survive into the bitcode at -O2 - one per
// variable, no ConstantMerge coalescence.
//
// O2-DAG: @c2go.global.gcmask.gA1 = internal constant
// O2-DAG: @c2go.global.gcmask.gA2 = internal constant
// O2-DAG: @c2go.global.gcmask.gB1 = internal constant
// O2-DAG: @c2go.global.gcmask.gB2 = internal constant
//
// And none of them may carry unnamed_addr (that's what unlocks
// ConstantMerge's identical-initializer coalescence).
// O2-NOT: @c2go.global.gcmask.{{.*}} = {{.*}}unnamed_addr{{.*}}constant
