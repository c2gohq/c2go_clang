// c2go Go signatures must follow the target C data model.  In particular,
// Windows x86_64 is LLP64 (`long` is 32 bits) while Linux x86_64 is LP64
// (`long` is 64 bits).  A mismatched go_sig makes the Go ABI0 wrapper and the
// generated assembly disagree about argument/result offsets.
//
// REQUIRES: x86-registered-target
//
// RUN: %clang_cc1 -triple x86_64-pc-windows-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-emit-manifest=%t.windows.json -emit-llvm-bc -o %t.windows.bc %s
// RUN: FileCheck %s --check-prefix=WINDOWS --input-file=%t.windows.json
//
// RUN: %clang_cc1 -triple x86_64-unknown-linux-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-emit-manifest=%t.linux.json -emit-llvm-bc -o %t.linux.bc %s
// RUN: FileCheck %s --check-prefix=LINUX --input-file=%t.linux.json

#define c2go_extern __attribute__((c2go_extern))

c2go_extern long pass_long(long value);
c2go_extern unsigned long pass_ulong(unsigned long value);

long pass_long(long value) { return value; }
unsigned long pass_ulong(unsigned long value) { return value; }

// WINDOWS: "go_sig": "func pass_long(value int32) int32"
// WINDOWS: "go_sig": "func pass_ulong(value uint32) uint32"

// LINUX: "go_sig": "func pass_long(value int64) int64"
// LINUX: "go_sig": "func pass_ulong(value uint64) uint64"
