// A union whose alternatives are all pointers sharing one pure slot (no scalar
// overlap) compiles cleanly, and the containing struct's GC bitmap scans
// exactly that pointer word.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O0 -emit-llvm -o %t.ll %s
// RUN: FileCheck %s --input-file=%t.ll

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

typedef __SIZE_TYPE__ size_t;
struct Node { int x; };

struct __attribute__((c2go_managed)) HostOK {
  struct HostOK *real;        // word 0: managed -> scan
  union {                     // word 1: pure pointer slot -> scan
    struct Node *p;
    void        *q;
  } u;
  long after;                 // word 2: scalar -> no scan
};

extern void *gc_malloc(const void *ti, size_t n);
void *mk(void) { return gc_malloc(__c2go_typeinfo(struct HostOK), sizeof(struct HostOK)); }

// No error. Bitmap scans words 0 and 1 (managed self-ptr + pure union slot):
// `\03`. PtrBytes = 16.
// CHECK: @c2go.gcbitmap.HostOK = {{.*}}constant [1 x i8] c"\03"
// CHECK: @c2go.typeinfo.HostOK = {{.*}}%c2go._gotype { i64 24, i64 16,
