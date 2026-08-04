// Boundary-symbol manifest round-trip: c2go-lto must rebuild the symbols[]
// entries of a GC manifest byte-identically from combined bitcode carrying the
// `c2go.func.<X>` NamedMD that clang codegen stamps. All 14 per-function
// manifest fields are encoded into NamedMD and must read back unchanged (not
// just the three per-function IR attrs).
//
// The four boundary functions exercise the field surface that matters:
//   * main(argc, argv)  - c_entry=true, entry_sig="argc_argv"
//   * log_msg(...)      - is_variadic=true
//   * compute(P)        - has_aggregate=true (struct by value)
//   * chain8(a,b,...h)  - argsize beyond register-arg limit, plain scalar shape
//
// REQUIRES: aarch64-registered-target
//
// Reference path: clang front-end emits the manifest directly.
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O2 -fc2go-emit-manifest=%t.ref.json -emit-llvm-bc -o %t.ref.bc %s
//
// Round-trip path: clang emits combined bitcode, c2go-lto rebuilds the same
// manifest from its embedded NamedMD/IR attrs.
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O2 -emit-llvm-bc -o %t.bc %s
// RUN: c2go-lto %t.bc --c2go-emit-manifest=%t.lto.json
//
// Byte-identical hard assertion - the symbols[] block must match. The full
// JSON must also match (other sections - pkgpath / linknames / types - are
// already covered by sibling tests; this guard catches any drift).
// RUN: diff -u %t.ref.json %t.lto.json
// RUN: not grep -E '"(max_go_version|go_contract_epoch)"[[:space:]]*:' %t.lto.json
// RUN: grep -q '"schema_version": 2' %t.lto.json
// RUN: grep -q '"compatibility_model": "provider_epoch"' %t.lto.json
// RUN: grep -q '"min_go_version": "go1.25"' %t.lto.json
// RUN: grep -q '"validation_snapshot_max_exclusive": "go1.27"' %t.lto.json
// RUN: grep -q '"c2go_abi_epoch": 1' %t.lto.json
// RUN: grep -q '"go_toolchain_contract_epoch": 1' %t.lto.json
// RUN: not %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fc2go-target-go-version=1.25.1-1.27 -emit-llvm-bc -o %t.bad.bc %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=BAD-VERSION
// RUN: not %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fc2go-target-go-version=1.27-1.26 -emit-llvm-bc -o %t.backwards.bc %s \
// RUN:   2>&1 | FileCheck %s --check-prefix=BAD-VERSION
//
// FileCheck the round-trip output as an extra readability/regression net:
// every boundary's manifest entry carries the expected fields.
// RUN: FileCheck %s --input-file=%t.lto.json

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif
#define c2go_extern     __attribute__((c2go_extern))

// 1. Variadic boundary: should surface is_variadic=true.
c2go_extern void log_msg(const char *fmt, ...);

struct Point { int x; int y; };

// 2. Aggregate-by-value: should surface has_aggregate=true.
c2go_extern int compute(struct Point p);

// 3. 8 scalar args - drives the boundary argsize math (8 * 8 = 64 bytes args
//    + 8 bytes ret on aarch64 ABI0). Pure scalar; no float / no aggregate.
c2go_extern long chain8(long a, long b, long c, long d,
                        long e, long f, long g, long h);

// 4. C `main`: triggers the c_entry / entry_sig synthesis path. The go_name is
//    `Main`, needs_linkname=true.
c2go_extern int main(int argc, char **argv);

void log_msg(const char *fmt, ...) {}
int compute(struct Point p) { return p.x + p.y; }
long chain8(long a, long b, long c, long d,
            long e, long f, long g, long h) {
  return a + b + c + d + e + f + g + h;
}
int main(int argc, char **argv) { return argc; }

// Symbols are sorted by name; the JSON pretty-printer also sorts keys
// alphabetically inside each entry. Order is: chain8 / compute / log_msg / main.
//
// Schema v2 records an informational validation snapshot, while actual future
// Go admission is delegated to the c2goabi provider.
// BAD-VERSION: error: invalid value '{{.*}}' for -fc2go-target-go-version
// CHECK: "symbols": [
//
// chain8: 8 i64 args -> argsize = 8*8 + 8 (ret) = 72. Pure scalar shape.
// CHECK:        "abi": "abi0"
// CHECK-NEXT:   "argsize": 72
// CHECK-NEXT:   "asm_symbol": "·chain8"
// CHECK-NEXT:   "go_name": "Chain8"
// CHECK-NEXT:   "go_sig": "func chain8({{.*}}) int64"
// CHECK-NEXT:   "kind": "func"
// CHECK-NEXT:   "managed": true
// CHECK-NEXT:   "name": "chain8"
// CHECK-NEXT:   "needs_linkname": true
//
// compute: takes Point by value -> has_aggregate=true.
// CHECK:        "has_aggregate": true
// CHECK:        "name": "compute"
//
// log_msg: variadic -> is_variadic=true.
// CHECK:        "is_variadic": true
// CHECK:        "name": "log_msg"
//
// main: C entry -> c_entry=true, entry_sig="argc_argv".
// CHECK:        "c_entry": true
// CHECK-NEXT:   "entry_sig": "argc_argv"
// CHECK:        "name": "main"
