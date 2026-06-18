// __attribute__((c2go_variant)) is the opt-in convert-to-struct "variant
// container" marker; it only makes sense on a union. Applying it to a struct,
// a variable, or anything else is a hard error.
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

// struct -> error
struct __attribute__((c2go_variant)) BadStruct { // expected-error {{'c2go_variant' attribute can only be applied to a union type}}
  int a;
  int b;
};

// variable -> error
int g __attribute__((c2go_variant)); // expected-error {{'c2go_variant' attribute can only be applied to a union type}}

// union -> OK (no diagnostic)
union __attribute__((c2go_variant)) OkU {
  int   i;
  void *p;
};
