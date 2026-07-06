// c2go (#601 co-fix): a c2go_extern EXPORT that is ALSO declared c2go_linkname
// in a header (the whole stdio family: the header spells the Go symbol +
// GoABI0 via c2go_linkname, the .c defines it c2go_extern) must still carry its
// boundary attrs on the DEFINITION even when an earlier call site materialized
// the llvm::Function from the bodyless linkname decl (which has no
// C2GoExternAttr). Otherwise the creation-time stamping skips c2go-boundary /
// c2go-boundary-argsize and the X86 frame stager falls back to `NOFRAME, $0`
// while the body uses BP addressing — the #599 crash shape, re-exposed for
// boundary symbols once #601 defers body emission (snprintf regressed exactly
// this way). EmitGlobalFunctionDefinition re-stamps the boundary attrs from the
// definition decl.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o - %s | FileCheck %s

#define c2go_extern __attribute__((c2go_extern))

// "header": bodyless c2go_linkname decl naming this package's own export (1 =
// C2GO_GOABI0). No C2GoExternAttr here.
int myexport(int x) __attribute__((c2go_linkname("t.myexport", 1)));

// An earlier function references myexport, materializing its llvm::Function from
// the bodyless linkname decl above (before the c2go_extern definition below).
c2go_extern int early(int x) { return myexport(x) + 1; }

// The c2go_extern DEFINITION. Its boundary attrs must be present despite the
// earlier materialization: the re-stamp fires here.
// CHECK: define {{.*}}@myexport({{.*}}) [[ATTR:#[0-9]+]]
c2go_extern int myexport(int x) { return x * 2; }

// CHECK: attributes [[ATTR]] = {
// CHECK-SAME: "c2go-boundary"
// CHECK-SAME: "c2go-boundary-argsize"=
