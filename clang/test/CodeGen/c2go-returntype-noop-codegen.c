// `c2go_returntype` is a Sema-only marker that documents multi-return intent
// on a GoABI0 boundary function; it has NO CodeGen consumer of its own. The
// GoABI0 multi-return lowering fires for every GoABI0 boundary returning a
// record, whether or not the attribute is present.
//
// This pins both facts: the attribute compiles to IR with no diagnostic, and
// the boundary call site lowers the record return as a direct struct (no sret
// pointer, no coerce to [N x iN]) carrying no attribute-specific marker. A
// future change that wired a CodeGen consumer for the attribute would stamp a
// new attr/metadata (breaking a NOT) or change the call shape, flagging the
// behaviour shift here.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

#include <c2go.h>

typedef struct {
  int a;
  void *b;
} TupAB;

// GoABI0 boundary returning the tuple struct. The `c2go_return_type`
// attribute documents multi-return intent for Sema; this test pins it as an
// IR no-op.
extern TupAB FuncTwoRet(int x)
    c2go_linkname("pkg.FuncTwoRet")
    c2go_return_type(TupAB);

int caller(int x) {
  TupAB r = FuncTwoRet(x);
  return r.a + (int)(long)r.b;
}

// (1) The struct type is emitted as a normal aggregate - no special
// `c2go.returntype.*` synthesized type appears.
// CHECK: %struct.TupAB = type { i32, ptr }
// CHECK-NOT: c2go.returntype

// (2) The boundary call site uses the GoABI0 CC and returns the literal
// struct directly (no `sret(...)` indirect-return arg, no coerce to
// [N x i64]). This is the GoABI0-record return path - it fires for every
// GoABI0 boundary, not because of c2go_returntype.
// CHECK-LABEL: define {{.*}} @caller(
// CHECK: call goabi0cc %struct.TupAB @pkg.FuncTwoRet(
// CHECK-NOT: sret(

// (3) The function-attribute set on the boundary decl carries the
// c2go-linkname and c2go-c-name stamps but NO `c2go-returntype` /
// `c2go-multireturn` / similar attribute - the validator does not
// leak into the IR. (Match on the attributes-line of the declaration.)
// CHECK: attributes #{{[0-9]+}} = { {{.*}}"c2go-linkname"="pkg.FuncTwoRet"{{.*}} }
// CHECK-NOT: c2go-returntype
// CHECK-NOT: c2go-multireturn
