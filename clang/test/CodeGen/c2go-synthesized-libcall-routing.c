// An unused c2go_linkname declaration must still leave enough IR protocol for
// LoopIdiomRecognize's synthesized `strlen` to recover its Go symbol and ABI.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-linux-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -disable-llvm-passes -emit-llvm -o - %s | \
// RUN:   FileCheck %s --check-prefix=META
// RUN: %clang_cc1 -triple aarch64-unknown-linux-goabi -fc2go -std=c2go23 \
// RUN:   -O2 -emit-llvm -o - %s | FileCheck %s --check-prefix=ROUTE

typedef __SIZE_TYPE__ size_t;

extern size_t strlen(const char *)
    __attribute__((c2go_linkname("example.com/lib.strlen", 1)));
extern int abi_internal_only(int)
    __attribute__((c2go_linkname("example.com/lib.abiInternalOnly")));

size_t loop_strlen(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  return (size_t)(p - s);
}

// The declaration is unused at AST lowering time, so no target Function is
// required in the unoptimized module; the route table itself is the contract.
// The ABIInternal-only declaration is deliberately absent: it must still use
// the existing alias-then-wrapper path instead of being called directly.
// META: !c2go.libcall.routes = !{![[ROUTE_MD:[0-9]+]]}
// META: ![[ROUTE_MD]] = !{!"strlen", !"example.com/lib.strlen"}

// At -O2 the loop becomes strlen, then C2GoLibCallRouting runs before RS4GC.
// A non-leaf GoABI0 target is therefore represented by a statepoint naming the
// package-qualified target, never a package-local raw `strlen`.
// ROUTE-LABEL: define {{.*}}goabi0cc i64 @loop_strlen(
// ROUTE: @llvm.experimental.gc.statepoint{{.*}}ptr elementtype(i64 (ptr)) @"example.com/lib.strlen"
// ROUTE: declare goabi0cc i64 @"example.com/lib.strlen"(ptr
// ROUTE-NOT: declare {{.*}} @strlen(
// ROUTE: !c2go.libcall.routes = !{![[ROUTE_MD:[0-9]+]]}
// ROUTE: ![[ROUTE_MD]] = !{!"strlen", !"example.com/lib.strlen"}
