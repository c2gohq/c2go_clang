// c2go (#601): a call site must not be arranged per the unmanaged-extern
// *import* ABI just because the callee's definition appears LATER in the TU.
//
// The trigger: an eagerly-emitted body (a c2go_extern boundary function) calls
// a forward-declared variadic function whose definition comes later. Before the
// fix, EmitGlobal emitted the boundary body DURING parse, when the callee's
// isDefined() was still false; usesC2GoVoidPtrVararg then classified the callee
// as an import and marshalled platform varargs (`i32 10, i32 20, ...`) even
// though the callee body — emitted later as an internal function — consumes the
// void** argument pack. Both sides carry the same GoABI0 CC, so the c2go-lto
// call-site-CC sweep does NOT catch it: a silent miscompile.
//
// The fix defers c2go function-body emission to end-of-TU so isDefined() is
// accurate for every call site. The call must therefore pass the void** pack
// pointer, matching the callee's trailing `__c2go_va` parameter.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o - %s | FileCheck %s

#define c2go_extern __attribute__((c2go_extern))

// forward declaration only — no body yet, external linkage => implicit unmanaged
int impl(const char *fmt, ...);

// The callee is internal (defined below), so the call must pass the void** pack,
// NOT inline platform varargs.
// CHECK-LABEL: define {{.*}}@caller
// CHECK: call {{.*}}@impl(ptr noundef {{%[0-9a-z.]+}}, ptr noundef %c2go.va.argptrs)
// CHECK-NOT: call {{.*}}@impl(ptr {{.*}}, i32 noundef 10
c2go_extern int caller(const char *fmt) {
  return impl(fmt, 10, 20, 30);
}

// definition LATER in the TU: an internal variadic function reads the void**
// pack (its lowered signature gains the trailing __c2go_va pointer parameter).
// CHECK-LABEL: define {{.*}}@impl(ptr noundef {{%[a-z.]+}}, ptr noundef %__c2go_va)
int impl(const char *fmt, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int a = __builtin_va_arg(ap, int);
  int b = __builtin_va_arg(ap, int);
  int c = __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return a + b + c;
}
