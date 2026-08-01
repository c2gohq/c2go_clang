// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

#define C2GO_PTR 2
#define C2GO_RECORD 4
#define C2GO_BAD_MASK 8

#pragma c2go managed(8) push // expected-warning {{expected integer between 0 and 7 inclusive in '#pragma c2go' - ignored}}
#pragma c2go managed(C2GO_BAD_MASK) push // expected-warning {{expected integer between 0 and 7 inclusive in '#pragma c2go' - ignored}}
#pragma c2go managed(C2GO_PTR | 8) push // expected-warning {{expected integer between 0 and 7 inclusive in '#pragma c2go' - ignored}}
#pragma c2go managed(-1) push // expected-warning {{expected integer between 0 and 7 inclusive in '#pragma c2go' - ignored}}
#pragma c2go managed(C2GO_MISSING) push // expected-warning {{expected integer between 0 and 7 inclusive in '#pragma c2go' - ignored}}
#pragma c2go managed(defined(C2GO_PTR)) push // expected-warning {{expected integer between 0 and 7 inclusive in '#pragma c2go' - ignored}}
#pragma c2go managed() push // expected-warning {{expected integer between 0 and 7 inclusive in '#pragma c2go' - ignored}}
#pragma c2go managed(C2GO_PTR |) push // expected-warning {{expected integer between 0 and 7 inclusive in '#pragma c2go' - ignored}}
#pragma c2go managed(C2GO_PTR + C2GO_RECORD) push // expected-warning {{missing ')' after '#pragma c2go' - ignoring}}
#pragma c2go push // expected-warning {{missing '(' after '#pragma c2go' - ignoring}}

int value;
