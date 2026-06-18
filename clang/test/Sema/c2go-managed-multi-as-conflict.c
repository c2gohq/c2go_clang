// c2go_managed lowers to addrspace(1), so combining it with a conflicting
// __attribute__((address_space(N))) on the same pointee is a source-order
// independent conflict: both orders must be rejected with the same diagnostic.
// An explicit address_space(1) is redundant and accepted silently. Plain
// address_space(N) without c2go_managed is left to the existing machinery.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

// AS(2) BEFORE c2go_managed.
int * __attribute__((address_space(2))) __attribute__((c2go_managed)) p1; // expected-error{{multiple address spaces specified for type}}

// c2go_managed BEFORE AS(2). Same error, opposite source order.
int * __attribute__((c2go_managed)) __attribute__((address_space(2))) p2; // expected-error{{multiple address spaces specified for type}}

// Explicit AS(1) on a c2go_managed pointer is redundant, not conflicting,
// since c2go_managed itself lowers to AS=1. Accept it silently.
int * __attribute__((c2go_managed)) __attribute__((address_space(1))) p3; // no-error

// Plain AS(2) without c2go_managed: control case, left to the existing
// address_space machinery.
int * __attribute__((address_space(2))) p4;
