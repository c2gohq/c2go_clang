// The path-b linkname manifest entry carries an `imported` flag so c2go-bind
// knows whether the symbol's body lives in this TU (export direction; the .s
// defines it and a bodyless //go:linkname bridge is correct) or in another
// package (import direction; on Go 1.25 a bodyless //go:linkname no longer
// satisfies a .s-referenced symbol, so c2go-bind must emit an alias-then-wrap
// stub). clang sets it from whether a definition exists in the TU.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-emit-manifest=%t.json -emit-obj -o %t.o %s
// RUN: FileCheck %s --input-file=%t.json

// Defined here (body in this TU) -> export direction. The attribute is on a
// forward declaration so the definition does not draw a GCC-compat warning.
int f_export(int x) __attribute__((c2go_linkname("github.com/c2gohq/c2go-libc.FExport")));
int f_export(int x) { return x; }

// Declared extern, no body here -> import direction.
extern int f_import(int x)
    __attribute__((c2go_linkname("github.com/c2gohq/c2go-libc.FImport")));

int use(int x) { return f_export(x) + f_import(x); }

// linknames[] is sorted by name (f_export before f_import) and each entry's
// keys are emitted in sorted order, so the import direction of each is pinned
// by an ordered scan.
// CHECK:      "linknames": [
// CHECK:        "asm_symbol": "·github_com_c2gohq_c2go_libc_FExport"
// CHECK:        "imported": false
// CHECK:        "name": "f_export"
// CHECK:        "asm_symbol": "·github_com_c2gohq_c2go_libc_FImport"
// CHECK:        "imported": true
// CHECK:        "name": "f_import"
