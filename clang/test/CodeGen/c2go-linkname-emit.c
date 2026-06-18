// CodeGen-side behaviour of c2go_linkname on the three Subject kinds Sema
// accepts (Function / Var / Record). The attribute is the C-side declaration
// that binds a c2go symbol to a Go-side name; the resulting IR contract is
// consumed downstream:
//   * the call site uses the GoABI0 CC;
//   * SetLLVMFunctionAttributes stamps the c2go-linkname function attribute
//     that c2go-lto reads to rebuild the bridge table after LTO has folded
//     translation units;
//   * the Go-owner record path emits only the external @"type:<linkname>"
//     decl and suppresses the C-owner linkonce_odr typeinfo definition;
//   * the linkname is recorded as a second operand of the
//     c2go.struct.<X>.meta named metadata so C2GoMallocReplacement skips its
//     default typeinfo emission for the same record.
//
// This does NOT cover the inverse direction (Go -> C), which is what
// c2go_extern handles, not c2go_linkname. c2go_linkname is strictly "C
// declaration referencing a Go-defined symbol".
//
// We do not #include <c2go.h> because that header redefines c2go_linkname as
// a function-like macro wrapping the attribute in __attribute__((...)), which
// conflicts with the raw attribute spelling used in attribute lists. The Sema
// attribute itself is the same.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

typedef __SIZE_TYPE__ size_t;

// (1) Function with c2go_linkname - C calls a Go symbol via GoABI0.
extern int RuntimeGoHelper(int x)
    __attribute__((c2go_linkname("runtime.helper")));

// (2) Var with c2go_linkname - C reads a Go-side global. The IR symbol name IS
// the linkname (no leading `\01` or wrapper), because Plan 9 asm uses the
// linkname directly when it transforms cleanly.
extern int GoFlag
    __attribute__((c2go_linkname("runtime.flag")));

// (3) Record with c2go_linkname - Go-owner managed struct. The Go runtime owns
// the type descriptor; clang emits only an external bridge.
struct __attribute__((c2go_managed, c2go_linkname("runtime.GoStruct"))) GoStruct {
  int x;
  void *y;
};

extern void *gc_malloc(const void *type_info, size_t n);

int call_all(int x) {
  return RuntimeGoHelper(x) + GoFlag;
}

void *make_struct(void) {
  return gc_malloc(__c2go_typeinfo(struct GoStruct), sizeof(struct GoStruct));
}

// --- (2) Var: external decl named by the raw linkname. -----------------
// CHECK-DAG: @runtime.flag = external global i32

// --- (3) Record Go-owner: external `type:<linkname>` decl, NOT a
// linkonce_odr `@c2go.typeinfo.GoStruct` definition. -------------------
// CHECK-DAG: @"type:runtime.GoStruct" = external constant %c2go._gotype
// CHECK-NOT: @c2go.typeinfo.GoStruct = {{.*}}%c2go._gotype {

// --- (1) Function: call site uses the goabi0cc CC and the bare linkname
// as the IR symbol name; the c2go-linkname fn attr is stamped on it. ----
// CHECK-LABEL: define {{.*}}@call_all(
// CHECK: call goabi0cc i32 @runtime.helper(

// CHECK: declare goabi0cc i32 @runtime.helper(

// The fn-attribute set on the @runtime.helper declaration must carry the
// c2go-linkname attr (the c2go-lto rebuilder reads it after LTO has dropped
// Decl-level state) AND the c2go-c-name / c2go-go-sig manifest-grade attrs.
// The exact attr-set number is unstable, so we match on the linkname literal.
// CHECK: attributes #{{[0-9]+}} = { {{.*}}"c2go-c-name"="RuntimeGoHelper"{{.*}}"c2go-linkname"="runtime.helper"{{.*}} }

// --- (3, cont.) Record Go-owner: the linkname is also threaded into the
// `c2go.struct.GoStruct.meta` named MDNode so the C2GoMallocReplacement pass
// can skip its own typeinfo emission. ----------------------------------
// CHECK-DAG: !c2go.struct.GoStruct.meta = !{![[META:[0-9]+]]}
// CHECK-DAG: ![[META]] = !{!"managed", {{.*}}, !"runtime.GoStruct"}
