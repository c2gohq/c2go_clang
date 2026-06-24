// clang manifest classification under the export-only c2go_extern model
// (docs/c2go_design.md §2.0.3):
//   * c2go_extern on a DEFINITION  -> kind "func" (an ABI0 export c2gobind
//     turns into a Go stub; managed by default).
//   * c2go_extern on a DECLARATION -> a pure ABI0 marker with NO metadata; it
//     must not appear in symbols[].
//   * `unmanaged extern`           -> kind "unmanaged_extern" (an external host
//     import dispatched through the host-ABI bridge; unmanaged world, so
//     managed=false). clang also surfaces the per-symbol shape bits
//     (is_variadic, has_float, has_aggregate) c2gobind keys its wrapper
//     template on (only emitted when true).
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-emit-manifest=%t.json -emit-obj -o %t.o %s
// RUN: FileCheck %s --input-file=%t.json

#define c2go_extern __attribute__((c2go_extern))
#define unmanaged   __attribute__((c2go_unmanaged))

struct Point { int x, y; };

// Defined here (body in this TU) -> kind "func" export. Attribute on the
// forward declaration so the definition draws no GCC-compat warning.
c2go_extern int defined_fn(int x);
int defined_fn(int x) { return x + 1; }

// Declared-only c2go_extern: a pure ABI0 marker (the symbol is an ABI0 export
// defined in another c2go TU). It carries NO metadata, so it must NOT appear
// in symbols[].
c2go_extern int marker_only(int x);

// `unmanaged extern` imports -> kind "unmanaged_extern", managed=false, one per
// shape dimension.
unmanaged extern long   ue_read(int fd, void *buf, unsigned long n); // plain scalar
unmanaged extern void   ue_log(int code);                            // void return
unmanaged extern int    ue_printf(const char *fmt, ...);             // is_variadic
unmanaged extern double ue_sqrt(double v);                           // has_float
unmanaged extern int    ue_area(struct Point p);                     // has_aggregate

// symbols[] is name-sorted (defined_fn, ue_area, ue_log, ue_printf, ue_read,
// ue_sqrt; marker_only is absent) and each entry's keys are sorted, so
// has_*/is_variadic precede "kind" precede "managed" precede "name" — an
// ordered scan pins each classification.
// CHECK:      "symbols": [

// defined_fn: has a body -> plain func export, managed by default.
// CHECK:        "kind": "func"
// CHECK:        "managed": true
// CHECK:        "name": "defined_fn"

// marker_only is a declared-only c2go_extern -> no metadata. It would sort here
// (before ue_area); assert it is absent from symbols[].
// CHECK-NOT:    marker_only

// ue_area: struct-by-value param -> has_aggregate + unmanaged_extern, unmanaged.
// CHECK:        "has_aggregate": true
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "managed": false
// CHECK:        "name": "ue_area"

// ue_log: void scalar -> unmanaged_extern, no shape bits.
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "managed": false
// CHECK:        "name": "ue_log"

// ue_printf: variadic -> is_variadic + unmanaged_extern.
// CHECK:        "is_variadic": true
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "managed": false
// CHECK:        "name": "ue_printf"

// ue_read: plain scalar -> unmanaged_extern.
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "managed": false
// CHECK:        "name": "ue_read"

// ue_sqrt: float fixed arg/return -> has_float + unmanaged_extern.
// CHECK:        "has_float": true
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "managed": false
// CHECK:        "name": "ue_sqrt"
