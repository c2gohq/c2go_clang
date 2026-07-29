// Single-pointer-word file-scope globals are tagged Go-owned so the generated
// Go package can take over storage ownership (`var X unsafe.Pointer`). The Go
// compiler then registers the var in moduledata.gcdata with the correct
// pointer bit, so runtime.markroot scans it natively as a root rather than
// reading stale heap pointers across the GC pacer's span-recycling window.
//
// #646 P2 widened cession beyond the single-pointer-word case: zero-init
// AGGREGATE globals are now tagged Go-owned too (c2gobind synthesizes a var
// whose layout matches the mask), so the only remaining negative shapes are
// scalar-only globals and NON-ZERO pointer initializers (C keeps storage,
// with a warning). Checks the per-global gcmask bitmap stays intact.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fc2go-emit-manifest=%t.json -emit-llvm -o %t.ll %s
//
// LLVM IR side: check c2go.go_owned_globals covers every zero-initialized
// pointer-carrying global, including aggregates.
//
// RUN: FileCheck %s --check-prefix=IR --input-file=%t.ll
//
// Manifest side: check module_gcmask carries go_owned and the complete layout
// metadata for both single-word and aggregate globals.
//
// RUN: FileCheck %s --check-prefix=JSON --input-file=%t.json

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

#define c2go_extern     __attribute__((c2go_extern))
#define c2go_managed    __attribute__((c2go_managed))

// A managed record so we can build T * globals whose gcmask is "01" (one
// pointer slot at offset 0, exactly one ptr-sized word of storage).
#pragma c2go managed(6) push
struct Node {
  struct Node *next;
  struct Node *prev;
  long         tag;        // non-pointer; doesn't affect ptr-bit
};
#pragma c2go pop

// Positive A: single pointer-word, c2go-managed pointee. Must be tagged
// Go-owned.
static struct Node *gPositiveSingle;

// Positive B: same shape via attr-only managed pointer sugar. Also tagged
// Go-owned (the gcmask walker accepts both signals, storage is still one
// ptr word).
static int *c2go_managed gPositiveAttrOnly;

// Positive C: explicit NULL init. AST has hasInit() but the IR initializer
// folds to a null Constant - the predicate is IR-side so this must still be
// tagged Go-owned. Guards against regressing zero-init coverage to AST-only.
// A plain 0 keeps the addrspace inference from tripping over the cast.
static int *c2go_managed gPositiveExplicitNull = 0;

// Positive D (#646 P2): aggregate global containing multiple pointers,
// zero-init. Storage is 16 bytes (two pointer slots); ceded to a Go-owned
// `var gAggregate [2]unsafe.Pointer` so both slots are scanned as roots.
struct TwoPtrs {
  struct Node *a;
  struct Node *b;
};
static struct TwoPtrs gAggregate;

// Negative: scalar-only global. typeNeedsGCMask filters it out, so no gcmask
// is emitted at all - and no go_owned entry. Regression guard against tagging
// plain scalar storage.
static long gNegativeScalar;

// Negative: non-zero pointer initializer. This is a c2go-tracked global
// (gcmask emitted) but NOT Go-owned - the C side keeps DATA-section storage
// to honour the explicit &gNonZeroTarget init, otherwise AsmPrinter's
// fallback DATA path would duplicate the Go-owned var definition. #646 P2
// diagnoses it (storage is unrooted if runtime code parks a heap object in
// it). gNonZeroTarget itself (zero-init aggregate) IS ceded.
static struct Node gNonZeroTarget;
static struct Node *gNonZeroPtr = &gNonZeroTarget;

// Keep all globals live so the IR/manifest emission actually runs for them.
// The body itself is irrelevant to the assertions.
c2go_extern long ref_globals(void);
long ref_globals(void) {
  gPositiveSingle = 0;
  gPositiveAttrOnly = 0;
  gPositiveExplicitNull = 0;
  gAggregate.a = 0;
  gAggregate.b = 0;
  gNonZeroPtr = 0;
  return gNegativeScalar + (long)(gNonZeroTarget.tag);
}

// --- IR-side assertions ----------------------------------------------------
//
// gcmask bytes for all c2go-tracked globals must be present. The
// scalar-only global has no gcmask (typeNeedsGCMask filter).
// IR-DAG: @c2go.global.gcmask.gPositiveSingle = {{.*}}c"\01"
// IR-DAG: @c2go.global.gcmask.gPositiveAttrOnly = {{.*}}c"\01"
// IR-DAG: @c2go.global.gcmask.gPositiveExplicitNull = {{.*}}c"\01"
// IR-DAG: @c2go.global.gcmask.gAggregate = {{.*}}c"\03"
// IR-DAG: @c2go.global.gcmask.gNonZeroTarget = {{.*}}c"\03"
// IR-DAG: @c2go.global.gcmask.gNonZeroPtr = {{.*}}c"\01"
// IR-NOT: @c2go.global.gcmask.gNegativeScalar
//
// The Go-owned NamedMD lists every zero/null-init pointer-carrying var —
// aggregates included (#646 P2). gNonZeroPtr has a non-null init
// (&gNonZeroTarget), so it is the one exclusion (plus the scalar).
// IR: !c2go.go_owned_globals = !{
// IR-DAG: !{!"gPositiveSingle"}
// IR-DAG: !{!"gPositiveAttrOnly"}
// IR-DAG: !{!"gPositiveExplicitNull"}
// IR-DAG: !{!"gAggregate"}
// IR-DAG: !{!"gNonZeroTarget"}
// IR-NOT: !{!"gNegativeScalar"}
// IR-NOT: !{!"gNonZeroPtr"}

// --- JSON-side assertions --------------------------------------------------
//
// The module_gcmask vars are pretty-printed sorted by name; pretty-printer
// also sorts keys alphabetically inside each entry. Order is therefore:
//   gAggregate, gNonZeroPtr, gNonZeroTarget,
//   gPositiveAttrOnly, gPositiveExplicitNull, gPositiveSingle.
//
// JSON: "module_gcmask":
//
// Aggregate entry (#646 P2): ceded, with the full allocation size for the
// c2gobind var synthesis.
// JSON:        "go_owned": true
// JSON-NEXT:   "mask_hex": "03"
// JSON-NEXT:   "name": "gAggregate"
// JSON-NEXT:   "ptr_bits": 2
// JSON-NEXT:   "size_bytes": 16
//
// NonZeroPtr entry: single ptr word but non-null init, so MUST NOT carry
// go_owned. mask_hex/ptr_bits identical to a Go-owned entry - only the
// absence of "go_owned" distinguishes the case.
// JSON:        "mask_hex": "01"
// JSON-NEXT:   "name": "gNonZeroPtr"
// JSON-NEXT:   "ptr_bits": 1
// JSON-NEXT:   "size_bytes": 8
//
// NonZeroTarget entry: zero-init aggregate — ceded (#646 P2).
// JSON:        "go_owned": true
// JSON-NEXT:   "mask_hex": "03"
// JSON-NEXT:   "name": "gNonZeroTarget"
// JSON-NEXT:   "ptr_bits": 2
// JSON-NEXT:   "size_bytes": 24
//
// AttrOnly entry: carries go_owned: true and lives at "01" mask.
// JSON:        "go_owned": true
// JSON-NEXT:   "mask_hex": "01"
// JSON-NEXT:   "name": "gPositiveAttrOnly"
// JSON-NEXT:   "ptr_bits": 1
// JSON-NEXT:   "size_bytes": 8
//
// ExplicitNull entry (positive C): same Go-owned shape - explicit
// `= (int *)0` folds to a null Constant and remains eligible.
// JSON:        "go_owned": true
// JSON-NEXT:   "mask_hex": "01"
// JSON-NEXT:   "name": "gPositiveExplicitNull"
// JSON-NEXT:   "ptr_bits": 1
// JSON-NEXT:   "size_bytes": 8
//
// Single entry: same shape.
// JSON:        "go_owned": true
// JSON-NEXT:   "mask_hex": "01"
// JSON-NEXT:   "name": "gPositiveSingle"
// JSON-NEXT:   "ptr_bits": 1
// JSON-NEXT:   "size_bytes": 8
