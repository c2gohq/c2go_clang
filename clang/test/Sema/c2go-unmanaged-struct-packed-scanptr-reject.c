// An unmanaged (POD, no typeinfo) struct that holds a scannable data pointer
// (any non-function pointer field, directly or through an embedded record)
// cannot be packed or custom-aligned. The Go GC conservatively force-scans the
// pointer word and can only find it at a naturally aligned word boundary;
// packing shifts it off the word grid and the GC would miss the live pointer.
// Unlike a c2go_managed struct (whose precise per-type gcdata bitmap honors
// packing, hence only a warning), an unmanaged struct has no gcdata at all, so
// this is a hard error. Function pointers are no-scan and do not trigger it.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s

// Scannable pointer + packed -> error.
struct __attribute__((packed)) PackedPtr { // expected-error{{record 'PackedPtr' holds a scannable pointer and cannot use the 'packed' attribute}}
  char c;
  void *p;
};

// Scannable pointer + custom struct alignment -> error.
struct __attribute__((aligned(32))) AlignedPtr { // expected-error{{record 'AlignedPtr' holds a scannable pointer and cannot use custom alignment attributes}}
  int *p;
};

// Scannable pointer + custom field alignment -> error.
struct FieldAligned {
  char c;
  int * __attribute__((aligned(16))) p; // expected-error{{record 'FieldAligned' holds a scannable pointer and cannot give member 'p' custom alignment}}
};

// Embedded scannable pointer (through a nested record) + packed -> error.
struct Inner { void *p; };
struct __attribute__((packed)) PackedEmbed { // expected-error{{record 'PackedEmbed' holds a scannable pointer and cannot use the 'packed' attribute}}
  char c;
  struct Inner in;
};

// Packed but no scannable pointer -> OK.
struct __attribute__((packed)) PackedNoPtr {
  char c;
  int x;
  long l;
};

// Packed with only a function pointer (no-scan) -> OK.
struct __attribute__((packed)) PackedFnPtr {
  char c;
  void (*fn)(void);
};

// Scannable pointer but natural layout -> OK.
struct PlainPtrOK {
  char c;
  void *p;
};
