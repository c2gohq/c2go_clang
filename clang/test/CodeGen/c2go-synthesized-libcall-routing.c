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
// RUN: %clang_cc1 -triple aarch64-unknown-linux-goabi -fc2go -std=c2go23 \
// RUN:   -fc2go-package=example.com/lib -O2 -emit-llvm -o - %s | \
// RUN:   FileCheck %s --check-prefix=LOCAL

typedef __SIZE_TYPE__ size_t;

extern size_t strlen(const char *)
    __attribute__((c2go_linkname("example.com/lib.strlen", 1)));
extern double sin(double)
    __attribute__((c2go_linkname("example.com/lib.sin", 1)));
extern int abi_internal_only(int)
    __attribute__((c2go_linkname("example.com/lib.abiInternalOnly")));

size_t loop_strlen(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  return (size_t)(p - s);
}

double builtin_sin_with_live_pointer(char *p, double x) {
  double y = __builtin_sin(x);
  *p = (char)y;
  return y + *p;
}

#pragma STDC FENV_ACCESS ON
double constrained_sin_with_live_pointer(char *p, double x) {
  double y = __builtin_sin(x);
  *p = (char)y;
  return y;
}

// The declaration is unused at AST lowering time, so no target Function is
// required in the unoptimized module; the route table itself is the contract.
// The ABIInternal-only declaration is deliberately absent: it must still use
// the existing alias-then-wrapper path instead of being called directly.
// META: !c2go.libcall.routes = !{![[SIN_MD:[0-9]+]], ![[STRLEN_MD:[0-9]+]]}
// META: ![[SIN_MD]] = !{!"sin", !"example.com/lib.sin"}
// META: ![[STRLEN_MD]] = !{!"strlen", !"example.com/lib.strlen"}

// At -O2 the loop becomes strlen, then C2GoLibCallRouting runs before RS4GC.
// A non-leaf GoABI0 target is therefore represented by a statepoint naming the
// package-qualified target, never a package-local raw `strlen`.
// ROUTE-LABEL: define {{.*}}goabi0cc i64 @loop_strlen(
// ROUTE: @llvm.experimental.gc.statepoint{{.*}}ptr elementtype(i64 (ptr)) @"example.com/lib.strlen"

// llvm.sin must become a real Go call before RS4GC. The live pointer is then
// represented in the statepoint instead of crossing an untracked late call.
// ROUTE-LABEL: define {{.*}}goabi0cc double @builtin_sin_with_live_pointer(
// ROUTE: @llvm.experimental.gc.statepoint{{.*}}ptr elementtype(double (double)) @"example.com/lib.sin"
// ROUTE-NOT: @llvm.sin
// ROUTE-LABEL: define {{.*}}goabi0cc double @constrained_sin_with_live_pointer(
// ROUTE: @llvm.experimental.gc.statepoint{{.*}}ptr elementtype(double (double)) @"example.com/lib.sin"
// ROUTE-NOT: @llvm.experimental.constrained.sin
// ROUTE: declare goabi0cc i64 @"example.com/lib.strlen"(ptr
// ROUTE: declare goabi0cc double @"example.com/lib.sin"(double
// ROUTE-NOT: declare {{.*}} @strlen(
// ROUTE-NOT: @llvm.sin
// ROUTE: !c2go.libcall.routes = !{![[SIN_MD:[0-9]+]], ![[STRLEN_MD:[0-9]+]]}
// ROUTE: ![[SIN_MD]] = !{!"sin", !"example.com/lib.sin"}
// ROUTE: ![[STRLEN_MD]] = !{!"strlen", !"example.com/lib.strlen"}

// With the linkname package selected as the current package, late synthesized
// calls resolve to local LLVM symbols while retaining the raw linkname attr.
// LOCAL-LABEL: define {{.*}}goabi0cc i64 @loop_strlen(
// LOCAL: @llvm.experimental.gc.statepoint{{.*}}ptr elementtype(i64 (ptr)) @strlen
// LOCAL-LABEL: define {{.*}}goabi0cc double @builtin_sin_with_live_pointer(
// LOCAL: @llvm.experimental.gc.statepoint{{.*}}ptr elementtype(double (double)) @sin
// LOCAL: declare goabi0cc i64 @strlen(ptr{{.*}}){{.*}}#[[LOCAL_STR:[0-9]+]]
// LOCAL: declare goabi0cc double @sin(double) #[[LOCAL_SIN:[0-9]+]]
// LOCAL: attributes #[[LOCAL_STR]] = {
// LOCAL-SAME: "c2go-c-name"="strlen"
// LOCAL-SAME: "c2go-linkname"="example.com/lib.strlen"
// LOCAL-SAME: "c2go-linkname-abi0"
// LOCAL: attributes #[[LOCAL_SIN]] = {
// LOCAL-SAME: "c2go-c-name"="sin"
// LOCAL-SAME: "c2go-linkname"="example.com/lib.sin"
// LOCAL-SAME: "c2go-linkname-abi0"
