// c2go (#654): taking the address of a FORWARD-DECLARED static function before
// its definition must not classify it as an unmanaged-extern *import*.
//
// The trigger: mutually recursive static helpers (Lua's warnfoff/warnfon,
// io_readline) decay to a function pointer while the decl is mid-parse
// "not-yet-defined". Before the fix, Sema's c2goAdjustImportFnPointee restamped
// the pointee to the import world at that point (the parse-point-isDefined twin
// of #601), and the manifest side (isC2GoUnmanagedExternImport) agreed — so the
// TU grew a c2go_fn_* dispatch global and an import wrapper for a function whose
// definition lives right here.
//
// The fix: an internal-linkage function can NEVER be an external import — its
// definition is in this TU by construction. Both mirror points now bail on
// !isExternallyVisible().
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o - %s | FileCheck %s --implicit-check-not=c2go_fn_

static void warnfoff(void *ud, const char *msg, int tocont);
static void warnfon(void *ud, const char *msg, int tocont);

typedef void (*WarnFunction)(void *ud, const char *msg, int tocont);
WarnFunction g_warnf;

// Address-of BEFORE warnfoff's definition: must store the plain function
// address, with no import machinery anywhere in the module.
// CHECK-LABEL: define {{.*}}@warnfon
// CHECK: store ptr @warnfoff, ptr @g_warnf
static void warnfon(void *ud, const char *msg, int tocont) {
  g_warnf = warnfoff;
}

// CHECK-LABEL: define {{.*}}@warnfoff
// CHECK: store ptr @warnfon, ptr @g_warnf
static void warnfoff(void *ud, const char *msg, int tocont) {
  g_warnf = warnfon;
}

void set_default(void) { g_warnf = warnfon; }

// Both stay internal definitions; no c2go_fn_* import dispatch globals emitted.
// CHECK-NOT: c2go_fn_
