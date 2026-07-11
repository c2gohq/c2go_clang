// Plan 9 .s output regression guard for the Go-owned eligibility narrowing
// on non-null pointer initializers.
//
// A non-null-init `static T *gPtr = &gTarget;` must NOT be Go-owned: the C
// side keeps its storage, so the Plan 9 .s carries BOTH the DATA initializer
// and the GLOBL trailer for gPtr, and the Go side does not emit a colliding
// `var gPtr unsafe.Pointer`. (Tagging it Go-owned while AsmPrinter still ran
// its fallback DATA path produced a link-time duplicate.)
//
// The companion zero-init shape `static T *gPtrNull;` stays Go-owned:
// AsmPrinter routes BSSLocal through emitCommonSymbol, which the Plan 9 .s
// streamer suppression list filters out - so the .s emits NEITHER a DATA nor
// a GLOBL directive for gPtrNull.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fc2go-emit-plan9-asm=%t.s -emit-obj -o %t.o %s
//
// Positive shape (non-zero init): both DATA initializer and GLOBL trailer
// must be present for gPtr.
// RUN: FileCheck %s --check-prefix=POS --input-file=%t.s
//
// Negative shape (null init): no DATA/GLOBL directive for gPtrNull itself -
// the Plan 9 streamer suppression on emitCommonSymbol filters both.
// Text-section references to the symbol (loads/stores) and the gcmask
// companion GV ARE expected to appear; the suppression is per-symbol on the
// storage-owning symbol only.
//
// Match shape: a `DATA ` or `GLOBL ` directive at column 0 whose next-token
// symbol is exactly the Plan 9 gPtrNull symbol (literal middle-dot embedded
// in this UTF-8 source). The trailing `[(+]` class disambiguates from any
// strict-prefix name (none exist today, but pinned defensively).
// RUN: grep -E "^(DATA|GLOBL) ·gPtrNull[(+]" %t.s | count 0
//
// #646 P2: the zero-init AGGREGATE gTarget is ceded too — no DATA/GLOBL for
// its storage either (the Go-owned var provides it).
// RUN: grep -E "^(DATA|GLOBL) ·gTarget[(+]" %t.s | count 0

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

// A managed pointee record so the gcmask walker treats `Node *` as a managed
// pointer slot.
#pragma c2go managed(6) push
struct Node {
  struct Node *next;
  long         tag;
};
#pragma c2go pop

// Storage target for the non-zero-init pointer. #646 P2: gTarget is a
// zero-init pointer-carrying aggregate, so it is now ALSO ceded to Go-owned
// storage — its GLOBL trailer is suppressed like gPtrNull's, while the DATA
// initializer of gPtr still references the (Go-provided) symbol.
static struct Node gTarget;

// C-owned path: non-zero pointer init. The IR initializer is `ptr @gTarget`
// (non-null Constant), so it is excluded from !c2go.go_owned_globals and the
// Plan 9 .s must carry BOTH the DATA initializer and the GLOBL trailer. The
// pointer itself is plain (no extra c2go_managed) so the address-space-aware
// initializer &gTarget type-checks; Node is in the managed pragma scope so
// the pointee bit is still set in the gcmask walker.
static struct Node *gPtr = &gTarget;

// Companion null-init shape under the same setup. The IR initializer is a
// null Constant, so it stays in the Go-owned set: AsmPrinter routes it via
// emitCommonSymbol -> suppression -> no GLOBL, and BSS has no DATA either.
// The Go side owns the storage exclusively.
static struct Node *gPtrNull;

// Keep both globals live so the Plan 9 backend actually walks them.
long use_globals(void);
long use_globals(void) {
  gPtrNull = gPtr;
  return gPtr ? gPtr->tag : 0;
}

// --- Plan 9 .s assertions --------------------------------------------------
//
// Non-zero-init (POS): BOTH the DATA initializer line and the GLOBL trailer
// must be present (proves C keeps DATA-section storage). `{{.+}}` matches the
// Plan 9 middle-dot prefix plus the gPtr linker name, staying robust against
// per-target symbol-mangling tweaks.
//
// POS-DAG: DATA {{.+}}gPtr+0(SB)/8, ${{.+}}gTarget(SB)
// POS-DAG: GLOBL {{.+}}gPtr(SB), NOPTR, $8
//
// Zero-init (NEG) is enforced by the `grep | count 0` RUN line above.
