// A builtin that reaches SelectionDAG without a header-provided direct-GoABI0
// route must fail closed instead of emitting an unresolved raw libm call.
//
// REQUIRES: aarch64-registered-target
// RUN: not %clang_cc1 -triple aarch64-unknown-linux-goabi -fc2go \
// RUN:   -std=c2go23 -O2 -S -o /dev/null %s 2>&1 | FileCheck %s

double builtin_sin_without_route(double x) { return __builtin_sin(x); }

// CHECK: fatal error: error in backend: c2go backend synthesized libc call 'sin' without a direct-GoABI0 c2go_linkname route
