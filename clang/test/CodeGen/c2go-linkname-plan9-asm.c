// c2go_linkname Plan 9 (.s) lowering across subject kinds, name shapes, and
// the C2GO_GOABI0 selector (spelled as the literal int 1 here, since cc1 tests
// use the raw attribute rather than the c2go.h macro). A function WITH
// C2GO_GOABI0 (the target has a Go ABI0 entry) and a clean name is referenced
// directly (. -> middle-dot). WITHOUT it, the function imports an external
// ABIInternal Go symbol and is routed through a sanitized current-package
// local that c2gobind satisfies with an alias-then-wrap stub. A name carrying
// '-' or a method symbol ('(' '*' ')') the assembler cannot represent always
// takes the sanitized-local path. A clean-named variable is referenced
// directly. A managed struct's __c2go_typeinfo loads the per-type
// _typeinfo_<X> reflect-pin var c2gobind emits, for both a C-owner struct
// (local descriptor) and a Go-owner (c2go_linkname) struct whose descriptor is
// GOT-indirect: the AArch64 GOT path must route to the SAME ·_typeinfo_<X>
// form as the direct path (it previously leaked the raw c2go_typeinfo·<X>
// mangling the Go linker can't resolve).
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-emit-plan9-asm=%t.s -fc2go-package=demo -emit-obj -o %t.o %s
// RUN: FileCheck %s --input-file=%t.s

typedef __SIZE_TYPE__ size_t;
extern void *gc_malloc(const void *type_info, size_t n);

// (1) function, clean name, C2GO_GOABI0 (the 1): target has an ABI0 entry, so
// the symbol is referenced directly.
extern int f_abi0(int x) __attribute__((c2go_linkname("runtime.fabi0", 1)));
// CHECK: CALL runtime·fabi0(SB)

// (1b) function, clean name, default (no C2GO_GOABI0): external ABIInternal Go
// symbol, routed through a sanitized current-package local stub.
extern int f_stub(int x) __attribute__((c2go_linkname("example.com/pkg.FStub")));
// CHECK: CALL ·example_com_pkg_FStub(SB)

// (2) function, hyphenated import path -> sanitized current-package local.
extern int f_hyphen(int x)
    __attribute__((c2go_linkname("github.com/c2go-project/c2go-libc.FHyphen")));
// CHECK: CALL ·github_com_c2go_project_c2go_libc_FHyphen(SB)

// (3) function, method symbol ( * ) -> sanitized local symbol too.
extern int f_method(int x) __attribute__((c2go_linkname("mypkg.(*T).Method")));
// CHECK: CALL ·mypkg___T__Method(SB)

// (4) variable, clean name -> direct address-of the transformed symbol.
extern int v_clean __attribute__((c2go_linkname("runtime.vclean")));
// CHECK: MOVD $runtime·vclean(SB), {{R[0-9]+}}

// (5) variable, hyphenated -> pointer-indirected. The assembler can't carry
// the '-' as a data symbol, and a Go 1.25 bodyless //go:linkname can't satisfy
// a .s-referenced var, so the reference loads c2gobind's _c2go_ptr_<local>
// (holding the remote variable's address) and dereferences it.
extern int v_hyphen
    __attribute__((c2go_linkname("github.com/c2go-project/c2go-libc.VHyphen")));
// CHECK: MOVD $·_c2go_ptr_github_com_c2go_project_c2go_libc_VHyphen(SB), {{R[0-9]+}}

int call_fns(int x) {
  return f_abi0(x) + f_stub(x) + f_hyphen(x) + f_method(x) + v_clean + v_hyphen;
}

// (6) C-owner managed struct: __c2go_typeinfo loads the local descriptor's
// reflect-pin var directly (ADRP+ADD path).
struct __attribute__((c2go_managed)) Local {
  int a;
  void *b;
};
// CHECK: MOVD ·_typeinfo_Local(SB), {{R[0-9]+}}

// (7) Go-owner (c2go_linkname) managed struct: the descriptor is external /
// GOT-indirect, yet must route to the SAME ·_typeinfo_<X> form. A regression
// to the raw c2go_typeinfo·Ext mangling fails this positive check.
struct __attribute__((c2go_managed, c2go_linkname("runtime.Ext"))) Ext {
  int a;
  void *b;
};
// CHECK: MOVD ·_typeinfo_Ext(SB), {{R[0-9]+}}

void *alloc(void) {
  return (char *)gc_malloc(__c2go_typeinfo(struct Local), sizeof(struct Local)) +
         (size_t)gc_malloc(__c2go_typeinfo(struct Ext), sizeof(struct Ext));
}
