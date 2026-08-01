// c2go function-world default (docs/c2go_design.md "v15 转折点"): the DEFAULT
// world for a function is UNMANAGED — a plain declared-only function is an
// `unmanaged extern` IMPORT. Bare `#pragma c2go managed push` enables every
// managed bit, including the func bit (1), and opts declared-only functions in
// scope INTO the internal c2go world. A DEFINED function is always internal.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-emit-manifest=%t.json -emit-obj -o %t.o %s
// RUN: FileCheck %s --input-file=%t.json

// Plain declared-only function, no attr, no pragma -> implicit-default IMPORT.
int imp_default(int x);

// Declared-only function inside the default all-managed region -> internal
// reference, NOT an import.
#pragma c2go managed push
int internal_ref(int x);
#pragma c2go pop

// Declared-only function inside managed(6) (Ptr|Record, NO func bit) -> still
// a default IMPORT.
#pragma c2go managed(6) push
int imp_in_6(int x);
#pragma c2go pop

// A defined function is internal regardless (no manifest entry).
int defined_local(int x) { return x + 1; }

// Reference all of them so they materialize.
int driver(int x) {
  return imp_default(x) + internal_ref(x) + imp_in_6(x) + defined_local(x);
}

// symbols[] is name-sorted and holds ONLY the two imports (imp_default,
// imp_in_6). internal_ref (func bit) / defined_local + driver (defined) are
// internal and absent. defined_local/driver would sort before imp_default;
// internal_ref would sort after imp_in_6.
// CHECK:      "symbols": [
// CHECK-NOT:    "name": "defined_local"
// CHECK-NOT:    "name": "driver"
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "managed": false
// CHECK:        "name": "imp_default"
// CHECK:        "kind": "unmanaged_extern"
// CHECK:        "managed": false
// CHECK:        "name": "imp_in_6"
// CHECK-NOT:    "name": "internal_ref"
