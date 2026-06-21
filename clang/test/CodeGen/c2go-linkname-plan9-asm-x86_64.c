// c2go_linkname Plan 9 (.s) lowering on x86_64, mirroring the AArch64
// coverage in c2go-linkname-plan9-asm.c. The symbol forms are shared
// across targets (a clean Go name burns straight in with . -> middle-dot;
// a name carrying - or a method symbol is sanitised to a current-package
// local that c2gobind bridges via //go:linkname); the instruction forms
// differ. On x86_64 a call is CALL sym(SB), an address-of is LEAQ sym(SB),
// reg (no leading $), and a __c2go_typeinfo load is MOVQ ·_typeinfo_<X>(SB),
// reg for both a C-owner (local descriptor) and a Go-owner (c2go_linkname)
// struct whose descriptor is GOT-indirect but must route to the SAME
// ·_typeinfo_<X> var. A regression to the raw c2go_typeinfo·<X> mangling
// the Go linker cannot resolve fails the Ext check below.
//
// REQUIRES: x86-registered-target
//
// RUN: %clang_cc1 -triple x86_64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
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
    __attribute__((c2go_linkname("github.com/c2gohq/c2go-libc.FHyphen")));
// CHECK: CALL ·github_com_c2gohq_c2go_libc_FHyphen(SB)

// (3) function, method symbol ( * ) -> sanitized local symbol too.
extern int f_method(int x) __attribute__((c2go_linkname("mypkg.(*T).Method")));
// CHECK: CALL ·mypkg___T__Method(SB)

// (4) variable, clean name -> address-of the transformed symbol via LEAQ.
extern int v_clean __attribute__((c2go_linkname("runtime.vclean")));
// CHECK: LEAQ runtime·vclean(SB), {{[A-Z][A-Z0-9]*}}

// (5) variable, hyphenated -> pointer-indirected: loads c2gobind's
// _c2go_ptr_<local> (holding the remote variable's address) and dereferences,
// since the assembler can't carry the '-' as a data symbol and a Go 1.25
// bodyless //go:linkname can't satisfy a .s-referenced var.
extern int v_hyphen
    __attribute__((c2go_linkname("github.com/c2gohq/c2go-libc.VHyphen")));
// CHECK: LEAQ ·_c2go_ptr_github_com_c2gohq_c2go_libc_VHyphen(SB), {{[A-Z][A-Z0-9]*}}

int call_fns(int x) {
  return f_abi0(x) + f_stub(x) + f_hyphen(x) + f_method(x) + v_clean + v_hyphen;
}

// (6) C-owner managed struct: __c2go_typeinfo loads the local descriptor's
// reflect-pin var directly.
struct __attribute__((c2go_managed)) Local {
  int a;
  void *b;
};
// CHECK: MOVQ ·_typeinfo_Local(SB), {{[A-Z][A-Z0-9]*}}

// (7) Go-owner (c2go_linkname) managed struct: the descriptor is external /
// GOT-indirect, yet must route to the SAME ·_typeinfo_<X> var.
struct __attribute__((c2go_managed, c2go_linkname("runtime.Ext"))) Ext {
  int a;
  void *b;
};
// CHECK: MOVQ ·_typeinfo_Ext(SB), {{[A-Z][A-Z0-9]*}}

void *alloc(void) {
  return (char *)gc_malloc(__c2go_typeinfo(struct Local), sizeof(struct Local)) +
         (size_t)gc_malloc(__c2go_typeinfo(struct Ext), sizeof(struct Ext));
}
