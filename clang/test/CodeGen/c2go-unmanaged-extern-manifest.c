// clang manifest classification of c2go_extern functions (§E1). A c2go_extern
// function with NO body in the TU is an "unmanaged_extern" — implemented by an
// external host library; c2gobind emits a Go wrapper that dispatches through
// c2go-libc/external (purego SyscallN + dlsym). One WITH a body is a plain
// "func" export (the .s carries the body). clang also surfaces the per-symbol
// shape bits c2gobind keys its wrapper template on: is_variadic, has_float,
// and has_aggregate (only emitted when true).
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-emit-manifest=%t.json -emit-obj -o %t.o %s
// RUN: FileCheck %s --input-file=%t.json

#define c2go_extern __attribute__((c2go_extern))

struct Point { int x, y; };

// Defined here (body in this TU) -> kind "func". Attribute on the forward
// declaration so the definition draws no GCC-compat warning.
c2go_extern int defined_fn(int x);
int defined_fn(int x) { return x + 1; }

// Bodyless c2go_extern -> kind "unmanaged_extern", one per shape dimension.
c2go_extern long   ue_read(int fd, void *buf, unsigned long n); // plain scalar
c2go_extern void   ue_log(int code);                            // void return
c2go_extern int    ue_printf(const char *fmt, ...);             // is_variadic
c2go_extern double ue_sqrt(double v);                           // has_float
c2go_extern int    ue_area(struct Point p);                     // has_aggregate

// symbols[] is name-sorted (defined_fn, ue_area, ue_log, ue_printf, ue_read,
// ue_sqrt) and each entry's keys are sorted, so has_*/is_variadic precede
// "kind" which precedes "name" — an ordered scan pins each classification.
// CHECK:      "symbols": [

// defined_fn: has a body -> plain func export (not unmanaged).
// CHECK:        "kind": "func"
// CHECK:        "name": "defined_fn"

// ue_area: struct-by-value param -> has_aggregate + unmanaged_extern.
// CHECK:        "has_aggregate": true
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "name": "ue_area"

// ue_log: void scalar -> unmanaged_extern, no shape bits.
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "name": "ue_log"

// ue_printf: variadic -> is_variadic + unmanaged_extern.
// CHECK:        "is_variadic": true
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "name": "ue_printf"

// ue_read: plain scalar -> unmanaged_extern.
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "name": "ue_read"

// ue_sqrt: float fixed arg/return -> has_float + unmanaged_extern.
// CHECK:        "has_float": true
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "name": "ue_sqrt"
