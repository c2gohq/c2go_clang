// The `c2go_variant` convert-to-struct layout descends into each alternative's
// natural layout, recording which pointer-sized words hold a scannable DATA
// pointer, and overlays alternatives by signature. Nested structs and arrays
// (including array-of-pointer and struct-with-pointer) are therefore precisely
// scanned and accepted. Only genuinely unrepresentable shapes remain a hard
// error: a nested union that puns a scan pointer with a scalar at the same word
// (an ambiguous byte), a pointer at a non-pointer-aligned (packed) offset, or a
// flexible/VLA member.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -fsyntax-only -verify %s

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

struct __attribute__((c2go_managed)) Foo { struct Foo *next; };
typedef void (*fn_t)(void);

// ACCEPTED: nested struct hiding a managed data pointer. The nested struct
// keeps its natural layout (tag @0, p @8); the converted struct scans `p`'s
// word precisely. Because the nested anon struct is a variant alternative,
// Sema suppresses the "not scanned by the Go GC" warning on `p` (it would be a
// false positive - the variant conversion is exactly what scans `p`). No
// diagnostic is asserted on `p`, so -verify proves the warning is gone.
union __attribute__((c2go_variant)) Ok1 {
  struct {
    int tag;
    struct Foo *p;
  } s;
  double d;
};

// CONTROL: a genuine plain record (NOT a variant alternative) holding a
// pointer-to-c2go_managed must still warn - the suppression is scoped strictly
// to variant alternatives and never weakens the diagnostic for real plain
// records.
struct PlainHolder {
  int x;
  struct Foo *q; // expected-warning {{is not scanned by the Go GC}}
};

// ACCEPTED: array-of-pointer - N consecutive scan words.
union __attribute__((c2go_variant)) Ok2 {
  void *arr[4];
  double d;
};

// ACCEPTED: array of nested struct holding a data pointer.
union __attribute__((c2go_variant)) Ok3 {
  struct {
    void *p;
  } tab[2];
  long n;
};

// REJECTED: nested union punning a scan pointer with a scalar (ambiguous).
// `void *q` and `long bits` overlay the same word - that byte is "pointer in
// one alternative, scalar in another", which a static GC bitmap cannot encode.
union __attribute__((c2go_variant)) Bad1 {
  union {
    void *q;
    long bits;
  } u; // expected-error {{member 'u' has an ambiguous pointer layout}}
  long n;
};

// A packed struct that shifts a pointer off the word grid is also rejected,
// but clang's pre-existing "packed record holds a scannable pointer" error
// fires on the struct definition before the variant guard runs, so that
// fail-closed path is exercised by the packed-record diagnostics, not here.
// The misaligned-pointer arm of the layout is kept as defense-in-depth.

// ACCEPTED: nested aggregate with no pointer -> safe no-scan blob.
union __attribute__((c2go_variant)) Good1 {
  struct { int a; int b; } s;
  double d;
};

// ACCEPTED: bare data pointer (gets its own scan slot) + scalar blob.
union __attribute__((c2go_variant)) Good2 {
  void *p;
  double d;
  long n;
};

// ACCEPTED: function pointer is no-scan; nested struct of funcptrs only.
union __attribute__((c2go_variant)) Good3 {
  fn_t f;
  struct { fn_t g; int x; } s;
  long n;
};

// ACCEPTED: array of plain scalars -> no-scan blob, no pointer hidden.
union __attribute__((c2go_variant)) Good4 {
  int v[8];
  double d;
};

// Force the records to be completed/used.
unsigned long sizes(void) {
  return sizeof(union Ok1) + sizeof(union Ok2) + sizeof(union Ok3) +
         sizeof(union Bad1) + sizeof(union Good1) +
         sizeof(union Good2) + sizeof(union Good3) + sizeof(union Good4) +
         sizeof(struct PlainHolder);
}
