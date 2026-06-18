// c2go_return_type(struct) is rejected unless the struct's field is directly
// representable as a Go ABI0 return slot. The accepted shapes are the scalar
// slots i1/i8/i16/i32/i64/iPtr/f32/f64 (bool, [un]signed char/short/int/long
// long, enum, pointer, float/double) plus the <c2go.h> built-in aggregates
// whose tag is exactly one of __c2go_slice / __c2go_string / __c2go_iface
// (an exact tag set, NOT a __c2go_ prefix match, so a user struct __c2go_foo
// is not silently let through). Anything else (long double, __int128,
// _Complex, _Float16, nested struct, bitfield, ...) has no direct slot in the
// backend lowering and would cause an ABI-mismatched call, so Sema rejects it
// at the attribute site.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

#include <c2go.h>

// --- Reject cases ----------------------------------------------------------

struct R1 { long double x; };
// expected-error@+1{{'struct R1' field 'x' has type 'long double' which is not a Go ABI0 return slot}}
extern struct R1 F1(int) c2go_return_type(struct R1);

struct R2 { __int128 x; };
// expected-error@+1{{'struct R2' field 'x' has type '__int128' which is not a Go ABI0 return slot}}
extern struct R2 F2(int) c2go_return_type(struct R2);

struct R3 { _Complex double x; };
// expected-error@+1{{'struct R3' field 'x' has type '_Complex double' which is not a Go ABI0 return slot}}
extern struct R3 F3(int) c2go_return_type(struct R3);

struct R4 { _Float16 x; };
// expected-error@+1{{'struct R4' field 'x' has type '_Float16' which is not a Go ABI0 return slot}}
extern struct R4 F4(int) c2go_return_type(struct R4);

struct Inner { int a; int b; };
struct R5 { struct Inner x; };
// expected-error@+1{{'struct R5' field 'x' has type 'struct Inner' which is not a Go ABI0 return slot}}
extern struct R5 F5(int) c2go_return_type(struct R5);

// --- Accept cases (positive control) --------------------------------------
// A pointer field, float/double scalar fields, and a c2go.h built-in
// aggregate field are all valid Go ABI0 return slots and must NOT trigger the
// diagnostic.

struct A1 { int *p; };
extern struct A1 A1f(int) c2go_return_type(struct A1);

struct A2 { float f; double d; };
extern struct A2 A2f(int) c2go_return_type(struct A2);

struct A3 { c2go_slice s; };
extern struct A3 A3f(int) c2go_return_type(struct A3);
