// In c2go mode LLVM must not synthesize a raw libc symbol when the TU has no
// declaration/linkname route for it. Such a symbol would compile successfully
// but fail later as a package-local C-ABI reference.
//
// REQUIRES: aarch64-registered-target
// RUN: %clang_cc1 -triple aarch64-unknown-linux-goabi -fc2go -std=c2go23 \
// RUN:   -O2 -emit-llvm -o - %s | FileCheck %s

typedef __SIZE_TYPE__ size_t;

size_t loop_without_strlen_route(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  return (size_t)(p - s);
}

// CHECK-LABEL: define {{.*}}goabi0cc {{.*}}i64 @loop_without_strlen_route(
// CHECK-NOT: @strlen
// CHECK: br i1
// CHECK-NOT: @strlen
// CHECK: ret i64
// CHECK-NOT: @strlen
