// c2go (#654b): a static C function whose name starts with a lowercase `l`
// followed by `_`/uppercase (`l_alloc`, `l_message`) or an uppercase `L`
// (`LTnum`, `LEnum`) is an ORDINARY package symbol, NOT a compiler-generated
// Mach-O/ELF private label. It must emit `TEXT ·name(SB)` and be referenced as
// `·name(SB)`; the buggy predicate treated it as a private label and split the
// definition (`·l_alloc`) from its references (`l_alloc<>`), so the Go linker
// rejected the call with "relocation target l_alloc not defined". Lua's amalgam
// (l_alloc / l_message / LTnum / LEnum) tripped exactly this.
//
// The genuine compiler privates DO carry a `.` (`l_.str.N`, `l_c2go.*`) or one
// of the dot-free label/constant-pool prefixes (`lCPI`, `lJTI`, `LBB`, `Ltmp`);
// a C identifier can never contain a `.`, which is the discriminator.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-emit-plan9-asm=%t.s -fc2go-package=demo -emit-obj -o %t.o %s
// RUN: FileCheck %s --input-file=%t.s

// Definitions must be package symbols (middle-dot), never file-local `<>`.
// CHECK-DAG: TEXT ·l_alloc(SB)
// CHECK-DAG: TEXT ·l_message(SB)
// CHECK-DAG: TEXT ·LTnum(SB)

static int l_alloc(int n);
static int l_message(int n);
static int LTnum(int n);

int demo_entry(int x) {
  // Cross-references from another function force (SB) operand emission; these
  // must match the `·name` definitions above, not a `name<>` file-local form.
  // CHECK-DAG: CALL ·l_alloc(SB)
  // CHECK-DAG: CALL ·l_message(SB)
  // CHECK-DAG: CALL ·LTnum(SB)
  return l_alloc(x) + l_message(x) + LTnum(x);
}

static int l_alloc(int n) { return n + 1; }
static int l_message(int n) { return n + 2; }
static int LTnum(int n) { return n + 3; }
