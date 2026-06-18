// Unmanaged DATA pointers are force-scanned; function pointers stay no-scan.
//
// A GC-tracked struct carrying (a) a managed self-pointer, (b) a
// `c2go_unmanaged` data pointer, (c) a function pointer and (d) a scalar must
// produce a GC bitmap that scans words 0 and 1 only - `\03`:
//   word 0 = managed ptr   -> scan
//   word 1 = unmanaged ptr  -> scan (force-scan)
//   word 2 = function ptr   -> NO scan
//   word 3 = scalar         -> NO scan
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
typedef void (*fnptr_t)(void);

struct __attribute__((c2go_managed)) Mix {
  struct Mix      *mptr;                         // word 0: managed -> scan
  void * __attribute__((c2go_unmanaged)) uptr;   // word 1: unmanaged -> scan
  fnptr_t          fp;                           // word 2: func ptr -> no scan
  long             scalar;                        // word 3: scalar -> no scan
};

extern void *gc_malloc(const void *type_info, size_t n);

void *alloc_mix(void) {
  return gc_malloc(__c2go_typeinfo(struct Mix), sizeof(struct Mix));
}

// Bitmap byte is `\03` (bits 0 and 1 set), and PtrBytes (2nd typeinfo field)
// is 16 = two scanned pointer words.
// CHECK: @c2go.gcbitmap.Mix = {{.*}}constant [1 x i8] c"\03"
// CHECK: @c2go.typeinfo.Mix = {{.*}}%c2go._gotype { i64 32, i64 16,
