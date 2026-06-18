// clang synthesizes an internal GoABI0-only forwarding wrapper for a static
// register-return ("leaf-flip-candidate") function, redirects the escaping
// (non-direct-call) uses to the wrapper, and leaves direct calls on the
// original. The original then becomes non address-taken so the backend
// leaf-flip pass can give it the register ABI, while the escaped function
// pointer (a Go-visible ABI0 entry) targets the wrapper.
//
// A wrapper is synthesized ONLY when F satisfies all of:
//   (a) leaf-eligible modulo address-taken (the backend could actually flip it),
//   (b) directly called (>=1 direct call site that benefits from the reg body),
//   (c) address-taken in a rewritable form (escaping use, no ifunc/alias/etc).
// Functions that are only-taken (no direct call), or whose subtree calls an
// external symbol (never flippable), or whose escape is an unsupported constant
// (ifunc) get NO wrapper.
//
// REQUIRES: aarch64-registered-target
//
// The wrapper is synthesized in CodeGenModule::Release() (a frontend step), so
// the assertions run on the pre-optimization IR (-disable-llvm-passes). The
// register-return decision is gated on -O2, hence -O2 here.
//
// RUN: %clang_cc1 -triple aarch64-unknown-linux-gnu -fc2go -std=c2go23 \
// RUN:   -O2 -disable-llvm-passes -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

//===----------------------------------------------------------------------===//
// Positive case: L is a static register-return leaf that is BOTH directly
// called (in direct_caller) AND address-taken (escapes into g_fp). All three
// gates pass -> a wrapper must be synthesized, g_fp must point at the wrapper,
// and direct calls to L stay on L.
//===----------------------------------------------------------------------===//

static int __attribute__((noinline)) L(int *p) { return *p + 1; }

// not_taken has the same shape but its address is NOT taken - it must NOT get
// a wrapper (it is already flippable directly).
static int __attribute__((noinline)) not_taken(int *p) { return *p + 2; }

//===----------------------------------------------------------------------===//
// Negative case (i) - only-taken, never directly called: only_taken is a
// static leaf whose address escapes into g_only but which NO direct call
// reaches. Gate (b) fails -> NO wrapper; only_taken stays plain ABI0 (still
// address-taken).
//===----------------------------------------------------------------------===//

static int __attribute__((noinline)) only_taken(int *p) { return *p + 3; }

//===----------------------------------------------------------------------===//
// Negative case (ii) - directly called + address-taken, but its subtree calls
// an EXTERNAL symbol (ext, an out-of-TU declaration). Gate (a) fails (the
// backend could never flip it) -> NO wrapper, even though it would otherwise
// look like the positive case.
//===----------------------------------------------------------------------===//

extern int ext(int *p);
static int __attribute__((noinline)) calls_ext(int *p) { return ext(p) + 4; }

// Globals appear at the top of the IR. These order-independent checks run
// before the ordered wrapper-body block below (which advances the cursor past
// the globals), so they must be stated first.
//
// g_fp escapes L's address. After the transform it must reference the WRAPPER,
// not L.
// CHECK-DAG: @g_fp = global ptr @L.c2gowrap
//
// g_only escapes only_taken's address. Gate (b) fails -> no wrapper, so g_only
// still references only_taken directly (note the `,` - NOT `.c2gowrap`).
// CHECK-DAG: @g_only = global ptr @only_taken,
//
// g_ce escapes calls_ext's address. Gate (a) fails -> no wrapper, so g_ce
// still references calls_ext directly (note the `,` - NOT `.c2gowrap`).
// CHECK-DAG: @g_ce = global ptr @calls_ext,
//
// Fail-closed: gfn is an ifunc whose resolver is FC directly (the F-use's user
// is the GlobalIFunc - an escape form we do NOT rewrite). FC's address stays
// in the ifunc and NO wrapper is synthesized for FC.
// CHECK-DAG: @gfn = ifunc {{.*}}, ptr @FC{{$}}
//
int (*g_fp)(int *) = L;
int (*g_only)(int *) = only_taken;
int (*g_ce)(int *) = calls_ext;

int direct_caller(int *p) {
  // L and calls_ext are directly called here; only_taken is NOT.
  return L(p) + not_taken(p) + calls_ext(p);
}
int indirect_caller(int *p) { return g_fp(p) + g_only(p) + g_ce(p); }

// L keeps the register-return convention (it will be leaf-flipped by the
// backend now that it is no longer address-taken).
// CHECK-DAG: define internal goabi0cc i32 @L(ptr {{.*}}%p){{.*}}[[LATTR:#[0-9]+]]

// The wrapper is internal, GoABI0, and forwards to L. Its call to L carries the
// `c2go-reg-return` call-site attr so the backend reads L's register result.
// CHECK:      define internal goabi0cc i32 @L.c2gowrap(ptr {{.*}}%0) [[WATTR:#[0-9]+]]
// CHECK-NEXT: entry:
// CHECK-NEXT:   call goabi0cc i32 @L(ptr {{.*}}%0) [[CSATTR:#[0-9]+]]
// CHECK-NEXT:   ret i32

// L.c2gowrap is the ONLY synthesized wrapper: none for the non-escaping leaf
// `not_taken` (no escape), none for the only-taken `only_taken` (gate b), none
// for the external-calling `calls_ext` (gate a), and none for the ifunc
// resolver `FC` (fail-closed). These CHECK-NOTs cover the defines region
// between L's wrapper and the attribute groups.
// CHECK-NOT: @not_taken.c2gowrap
// CHECK-NOT: @only_taken.c2gowrap
// CHECK-NOT: @calls_ext.c2gowrap
// CHECK-NOT: @FC.c2gowrap

//===----------------------------------------------------------------------===//
// Fail-closed case source: FC is used DIRECTLY as a GlobalIFunc resolver. (The
// @gfn / @FC.c2gowrap assertions for this are stated above with the globals.)
//===----------------------------------------------------------------------===//

typedef int (*fnptr)(int *);
static fnptr __attribute__((noinline)) FC(void) { return 0; }
extern int gfn(int *p) __attribute__((ifunc("FC")));

//===----------------------------------------------------------------------===//
// Attribute pinning: the wrapper carries `c2go-reg-return` (argsize 8 = arg
// only, no result slot) because c2go indirect calls default to the
// register-return convention, so the escaped function pointer's indirect call
// expects it. L (the body) carries `c2go-reg-return`; the wrapper's forwarding
// call-site too.
//===----------------------------------------------------------------------===//

// CHECK-DAG: attributes [[LATTR]] = {{.*}}"c2go-argsize"="8"{{.*}}"c2go-reg-return"
// The wrapper carries argsize=8 + `c2go-reg-return` (register result).
// CHECK-DAG: attributes [[WATTR]] = {{.*}}"c2go-argptrmask"="01"{{.*}}"c2go-argsize"="8"{{.*}}"c2go-reg-return"
// CHECK-DAG: attributes [[CSATTR]] = { "c2go-reg-return" }
