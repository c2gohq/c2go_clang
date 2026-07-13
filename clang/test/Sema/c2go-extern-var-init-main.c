// c2go #676: c2go_extern cannot export a C VARIABLE named `init`/`main`.
// Exported c2go data is Go-owned storage — c2gobind declares a
// package-level Go `var <cname>` — and Go reserves `init` (and `main` in
// package main) at package scope, so no export case can represent it.
// The DATA symbol itself is renamed regardless (CodeGen
// c2goInitMainRename, the #317 data extension); only the export face is
// rejected. Functions are unaffected (#317: renamed + exported
// Init/Main wrapper).
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

#define c2go_extern __attribute__((c2go_extern))

c2go_extern int init; // expected-error {{'c2go_extern' cannot export a C variable named init}}
c2go_extern int main; // expected-error {{'c2go_extern' cannot export a C variable named main}} \
                         expected-warning {{variable named 'main' with external linkage has undefined behavior}}

/* the rejection is name-exact, not a prefix rule */
c2go_extern int initx;
/* only the lowercase names are language-special on the Go side */
c2go_extern int Init;
