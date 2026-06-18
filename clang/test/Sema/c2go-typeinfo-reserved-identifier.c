// Sema reserves the __c2go_typeinfo_ identifier prefix. The __c2go_typeinfo
// builtin synthesizes an implicit VarDecl with that prefix to carry the RTTI
// descriptor's asm label and typeinfo attribute. A user-written declaration
// sharing the prefix would either steal the asm label (sending the descriptor
// to a dangling extern) or shadow the cached entry and land in the cache-hit
// branch without the carrier attribute, defeating the typeinfo emission. The
// reserved set is exactly the __c2go_typeinfo_ prefix, matching the
// __builtin_ family's reserved-identifier policy; non-prefix uses still work.
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

#pragma c2go managed(6) push
struct N {
  struct N *n;
};
#pragma c2go pop

// expected-error@+1 {{identifier '__c2go_typeinfo_N' is reserved for the c2go '__c2go_typeinfo' descriptor}}
extern struct N __c2go_typeinfo_N;

// expected-error@+1 {{identifier '__c2go_typeinfo_Foo' is reserved for the c2go '__c2go_typeinfo' descriptor}}
int __c2go_typeinfo_Foo;

// expected-error@+1 {{identifier '__c2go_typeinfo_' is reserved for the c2go '__c2go_typeinfo' descriptor}}
static const void *__c2go_typeinfo_ = 0;

// Non-prefix uses MUST still be accepted: the reserved set is exactly the
// __c2go_typeinfo_ prefix.
extern int c2go_typeinfo_user_var;          // OK: no leading underscores
extern int __c2go_other_Foo;                // OK: different c2go-internal prefix
