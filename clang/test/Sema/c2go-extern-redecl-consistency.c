// c2go declaration<->definition consistency (docs/c2go_design.md §2.0.3).
//   * c2go_extern is a calling-convention type attribute: a definition may
//     inherit it from a forward declaration, but it cannot be introduced on a
//     later (re)declaration (clang's CC-compatibility check rejects that).
//   * `unmanaged extern` (c2go_unmanaged on a function) names an external
//     import with no in-c2go definition. The func-level managed/unmanaged
//     "world" marking was removed (#268), so c2go_unmanaged on a function means
//     import — defining one is a contradiction and is rejected.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

#define c2go_extern __attribute__((c2go_extern))
#define unmanaged   __attribute__((c2go_unmanaged))

// OK: the forward declaration marks c2go_extern; the definition inherits it.
c2go_extern int exp_ok(int x);
int exp_ok(int x) { return x; }

// Reverse direction is rejected: a plain declaration cannot be upgraded to the
// GoABI0 calling convention on a later declaration.
int late_extern(int x);             // expected-note {{previous declaration is here}}
c2go_extern int late_extern(int x); // expected-error {{was previously declared without calling convention}}

// OK: a declared-only `unmanaged extern` is a valid import.
unmanaged extern int imp_ok(int x);

// Rejected: defining an import (the definition inherits c2go_unmanaged from the
// import declaration).
unmanaged extern int imp_def(int x);
int imp_def(int x) { return x; } // expected-error {{cannot define 'imp_def'}}

// Rejected: a definition marked c2go_unmanaged directly is the same
// contradiction (a c2go_unmanaged function is an external import).
unmanaged int u_direct(int x) { return x; } // expected-error {{cannot define 'u_direct'}}
