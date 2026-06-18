// Per-global @c2go.global.gcmask.<X> GVs must survive mid-end DCE and stay
// un-mergeable, so cat-merged multi-TU -O2 does not collapse identical-mask
// GVs into one survivor (which would erase per-TU manifest entries and break
// the Go-side root decls).
//
// Background: the per-global gcmask byte array is emitted as `internal
// constant` with no IR-level users; only the !c2go.go_owned_globals named
// metadata references the GVs by name. At -O2 GlobalDCE would delete them
// before c2go-lto can scrape them into the manifest. Each gcmask GV is
// appended to @llvm.compiler.used at FE emission time, keeping it live
// through compile-time optimization while still letting the linker drop it
// post-link.
//
// The GV must NOT carry unnamed_addr: its name encodes which C variable the
// mask belongs to (manifest matching is name-based), so the address is
// semantically observable. With unnamed_addr, -O2 ConstantMerge would
// coalesce identical initializers (every single-ptr-word global gets c"\01")
// into one survivor. ConstantMerge exempts llvm.used / llvm.compiler.used,
// but its FindUsedValues looks up the exact name llvm.compiler.used only -
// when clang emits both @llvm.compiler.used and @llvm.compiler.used.1 the
// latter's entries are invisible, so we cannot rely on that exemption.
//
// Pinned at -O2 (the level that triggered the regression) plus a -O0 sanity
// check that the path is unaffected there.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O2 -emit-llvm -o %t.O2.ll %s
// RUN: FileCheck %s --check-prefix=O2 --input-file=%t.O2.ll
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o %t.O0.ll %s
// RUN: FileCheck %s --check-prefix=O0 --input-file=%t.O0.ll

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

// A single-pointer-word global: gcmask = "\01" (one ptr at offset 0).
static struct Node *gA;

// An aggregate global with TWO pointer slots: gcmask = "\03" (bits 0,1).
// Distinct mask bytes ensure the two gcmask GVs are not collapsed by
// constant merging at -O2 (different initializer contents), so each one
// independently exercises the compiler.used pin.
struct TwoPtrs {
  struct Node *a;
  struct Node *b;
};
static struct TwoPtrs gB;

c2go_extern long ref_globals(void);
long ref_globals(void) {
  gA = 0;
  gB.a = 0;
  gB.b = 0;
  return 0;
}

// --- -O2 assertions --------------------------------------------------------
//
// Both gcmask GVs must survive mid-end DCE and must NOT carry unnamed_addr
// (that would let ConstantMerge collapse identical-content GVs across TUs).
// O2-DAG: @c2go.global.gcmask.gA = internal constant
// O2-DAG: @c2go.global.gcmask.gB = internal constant
// O2-NOT: @c2go.global.gcmask.{{.*}} = {{.*}}unnamed_addr{{.*}}constant
//
// An @llvm.compiler.used (or its numbered variant @llvm.compiler.used.N,
// when the c2go FE entry is merged alongside an existing compiler.used)
// must reference BOTH gcmask GVs on the same initializer line. Order is
// unspecified (gA may precede or follow gB), so we only pin "both present".
// O2: @llvm.compiler.used{{(\.[0-9]+)?}} = appending global {{.*}}@c2go.global.gcmask.{{[gAB]+}}{{.*}}@c2go.global.gcmask.{{[gAB]+}}{{.*}}section "llvm.metadata"

// --- -O0 sanity ------------------------------------------------------------
//
// Same shape at -O0 - the FE always emits the compiler.used entry; only the
// mid-end behaviour differs.
// O0-DAG: @c2go.global.gcmask.gA = internal constant
// O0-DAG: @c2go.global.gcmask.gB = internal constant
// O0-NOT: @c2go.global.gcmask.{{.*}} = {{.*}}unnamed_addr{{.*}}constant
// O0: @llvm.compiler.used{{(\.[0-9]+)?}} = appending global {{.*}}@c2go.global.gcmask.{{[gAB]+}}{{.*}}@c2go.global.gcmask.{{[gAB]+}}{{.*}}section "llvm.metadata"
