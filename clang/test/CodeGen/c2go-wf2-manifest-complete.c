// The WF2 manifest (rebuilt by c2go-lto from the bitcode clang emits) must carry
// EVERY section of the WF1 manifest, not just symbols[]. clang embeds its
// manifest verbatim into the bitcode (`c2go.manifest.json`) and c2go-lto extracts
// + merges it, so the two are identical by construction — this test pins the
// sections a previous field-by-field reconstruction silently dropped:
//   * linknames[] `imported` direction flag (export vs import)
//   * path-b *variable* linknames (kind=var), not just functions
//   * callbacks[] (c2go_callback targets)
//   * symbols[] `cabi` (the per-target ABI descriptor) and `wrapper_in_asm`
//
// Single workflow only: the manifest has one source of truth (clang's embedded
// JSON), so re-testing the WF1 cc1-direct emit would be redundant. These fields
// are ABI/boundary descriptors independent of the optimization level, so the
// same expectations are checked at both -O0 (c2go-lto as a plain bitcode linker)
// and -O2.
//
// REQUIRES: aarch64-registered-target

// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -emit-llvm-bc -o %t.O0.bc %s
// RUN: c2go-lto %t.O0.bc --c2go-emit-manifest=%t.O0.json
// RUN: FileCheck %s --input-file=%t.O0.json
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 -O2 \
// RUN:   -emit-llvm-bc -o %t.O2.bc %s
// RUN: c2go-lto %t.O2.bc --c2go-emit-manifest=%t.O2.json
// RUN: FileCheck %s --input-file=%t.O2.json

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif
#define c2go_extern        __attribute__((c2go_extern))
#define c2go_linkname(...) __attribute__((c2go_linkname(__VA_ARGS__)))
#define c2go_callback(fn)  (__c2go_callback(fn))

// extern boundary -> symbols[] entry carrying cabi + wrapper_in_asm.
c2go_extern void reg_cb(void *cb);

// path-b linknames (hyphenated package path is not Plan 9-direct): an exported
// (definition here -> imported=false) and an imported (declaration only ->
// imported=true) function.
int f_export(int x) c2go_linkname("example.com/pkg-x.FExport");
int f_export(int x) { return x; }
extern int f_import(int x) c2go_linkname("example.com/pkg-x.FImport");

// path-b *variable* linkname -> kind=var, imported=true.
extern int v_import c2go_linkname("example.com/pkg-x.VImport");

// c2go_callback target -> callbacks[] entry.
static int my_cmp(int a, int b) { return a - b; }
void install(void) { reg_cb((void *)c2go_callback(my_cmp)); }

int use(int x) { return f_export(x) + f_import(x) + v_import; }

// callbacks[] (dropped entirely by the old reconstruction).
// CHECK:      "callbacks": [
// CHECK:        "converter": "c2go_cbconv_my_cmp"
// CHECK:        "tramp": "c2go_cb_my_cmp"

// linknames[] — the imported flag, both directions, and a var entry.
// CHECK:      "linknames": [
// CHECK:        "go_sig": "func f_export(x int32) int32"
// CHECK:        "imported": false
// CHECK:        "name": "f_export"
// CHECK:        "go_sig": "func f_import(x int32) int32"
// CHECK:        "imported": true
// CHECK:        "name": "f_import"
// CHECK:        "go_type": "int32"
// CHECK:        "imported": true
// CHECK:        "kind": "var"
// CHECK:        "name": "v_import"

// symbols[] — cabi descriptor + wrapper_in_asm flag.
// CHECK:      "symbols": [
// CHECK:        "cabi": {
// CHECK:        "wrapper_in_asm": true
