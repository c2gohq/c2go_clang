// c2go #676: file-scope DATA named `init`/`main` must be renamed exactly
// like the #317 function rename, with a distinct spelling
// (c2go_dinit/c2go_dmain vs c2go_cinit/c2go_cmain).
//
// The live failure this pins (musl random.c, #675/prng): `DATA ·init`
// shares the symbol slot with the package's language-level init TEXT, so
// the `$·init+4` relocation of a pointer-into-the-array global resolved
// into the code segment and the first store through it SIGBUSed (fault
// address inside TEXT is the fingerprint). The rename is UNCONDITIONAL —
// plain statics, no c2go attribute involved (the c2go_extern export face
// is separately rejected in Sema, err_c2go_extern_var_init_main).
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fc2go-emit-plan9-asm=%t.s -emit-obj -o %t.o %s
// RUN: FileCheck %s --input-file=%t.s
//
// The array's storage and the pointer-into-it relocation both carry the
// renamed data symbol; the function rename (#317) is pinned alongside.
// CHECK-DAG: GLOBL ·c2go_dinit
// CHECK-DAG: DATA ·c2go_dinit+0(SB)
// CHECK-DAG: DATA ·xp+0(SB)/8, $·c2go_dinit+4(SB)
// CHECK-DAG: TEXT ·c2go_cmain
//
// No bare `init`/`main` symbol may survive anywhere in the .s — neither
// as data storage nor as the function's TEXT.
// RUN: grep -E "^(DATA|GLOBL) ·init[+(<]" %t.s | count 0
// RUN: grep -E "^TEXT ·main\(" %t.s | count 0

/* the musl random.c shape: non-zero-initialized static array named init,
 * plus a static pointer aimed one element into it (x = init+1) */
static unsigned init[3] = {1, 2, 3};
static unsigned *xp = init + 1;

int main(void) {
  init[0] = 7; /* the store that SIGBUSed pre-rename */
  *xp = 9;
  return (int)(init[0] + init[1]);
}
