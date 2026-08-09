// X86 GoABI0 va_arg callee-side lowering.
//
// In c2go mode, va_list (`ap`) is a void* holding a void** cursor over the
// caller-packed `void* argptrs[]`, so va_arg(ap, T) == *(T*)(*ap++). The X86
// lowering must produce this cursor shape rather than falling back to the SysV
// register-save-area machinery, whose gp_offset / overflow_arg_area /
// reg_save_area reads would misinterpret the cursor. (The va_list storage type
// may stay __va_list_tag; only its interpretation as a cursor slot is
// load-bearing.) The SysV walk is locked out via --implicit-check-not, and the
// AArch64 lowering must produce the same shape for parity.
//
// REQUIRES: x86-registered-target, aarch64-registered-target
//
// RUN: %clang_cc1 -triple x86_64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -emit-llvm -o - %s | FileCheck %s \
// RUN:   --implicit-check-not=gp_offset \
// RUN:   --implicit-check-not=overflow_arg_area \
// RUN:   --implicit-check-not=reg_save_area \
// RUN:   --implicit-check-not=llvm.va_start \
// RUN:   --implicit-check-not=llvm.va_end
//
// Parity: the AArch64 lowering must produce the same cursor shape.
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,AARCH64 \
// RUN:   --implicit-check-not=llvm.va_start \
// RUN:   --implicit-check-not=llvm.va_end \
// RUN:   --implicit-check-not=%%struct.__va_list
// RUN: %clang_cc1 -triple aarch64-unknown-linux-goabi -fc2go -std=c2go23 \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefixes=CHECK,AARCH64 \
// RUN:   --implicit-check-not=llvm.va_start \
// RUN:   --implicit-check-not=llvm.va_end \
// RUN:   --implicit-check-not=%%struct.__va_list
// RUN: %clang_cc1 -triple aarch64-pc-windows-goabi -fc2go -std=c2go23 \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefix=WIN-AARCH64 \
// RUN:   --implicit-check-not=llvm.va_start \
// RUN:   --implicit-check-not=llvm.va_end \
// RUN:   --implicit-check-not=%%struct.__va_list

#include <stdarg.h>

static long g;

// CHECK order: my_config is static, so clang defers its emission until after
// the referencing non-static test_entry. The CHECK blocks below follow IR
// emission order (test_entry, then my_config), not source order.

// Caller side packs one argptr slot per vararg PLUS one trailing sentinel
// slot (locked here so caller/callee stay in lockstep). #588: the callee's
// cursor ends one-past-the-end of the consumed arguments and lives in a
// GC-marked va_list slot across safepoints; the sentinel keeps that final
// cursor value inside the pack object so Go's precise GC never resolves it as
// a next-object reference. 2 varargs -> [3 x ptr], sentinel stored null.
// CHECK-LABEL: define{{.*}} goabi0cc i64 @test_entry()
// CHECK: %c2go.va.argptrs = alloca [3 x ptr]
// CHECK: %c2go.va.sentinel = getelementptr inbounds [3 x ptr], ptr %c2go.va.argptrs, i64 0, i64 2
// CHECK-NEXT: store ptr null, ptr %c2go.va.sentinel
// CHECK: call goabi0cc void @my_config(i32 noundef 4, ptr noundef %c2go.va.argptrs)

// Callee side: va_start binds the synthetic `void** __c2go_va` cursor
// parameter; each va_arg loads the cursor, loads the argptr, advances the
// cursor, then — #613, null-tolerant — compares the argptr to null and selects a
// zero-initialized slot when it IS null (a va_arg past the last real vararg lands
// on the pack's nil sentinel), so the value read is 0 instead of a null deref;
// finally loads through the selected pointer. This makes musl's variadic idiom
// (read an optional trailing arg, ignore it when a runtime flag says it's absent)
// safe. The sentinel stays nil, so the caller-side #588 GC contract is unchanged.
// CHECK-LABEL: define{{.*}} goabi0cc void @my_config(i32 noundef %op, ptr noundef %__c2go_va)
// AARCH64: %ap = alloca ptr, align 8
// WIN-AARCH64-LABEL: define{{.*}} goabi0cc void @my_config(i32 noundef %op, ptr noundef %__c2go_va)
// WIN-AARCH64: %ap = alloca ptr, align 8
// CHECK: %c2go.va.base = load ptr, ptr %__c2go_va.addr
// CHECK: store ptr %c2go.va.base,
//
// va_arg(ap, void*):
// CHECK: %c2go.va.cur = load ptr,
// CHECK: %c2go.va.argp = load ptr, ptr %c2go.va.cur
// CHECK: %c2go.va.next = getelementptr inbounds ptr, ptr %c2go.va.cur, i64 1
// CHECK: %c2go.va.atend = icmp eq ptr %c2go.va.argp, null
// CHECK: %c2go.va.safep = select i1 %c2go.va.atend, ptr %c2go.va.zero, ptr %c2go.va.argp
// CHECK: load ptr, ptr %c2go.va.safep
//
// va_arg(ap, long):
// CHECK: %c2go.va.cur{{[0-9]+}} = load ptr,
// CHECK: %c2go.va.argp{{[0-9]+}} = load ptr, ptr %c2go.va.cur{{[0-9]+}}
// CHECK: %c2go.va.atend{{[0-9]+}} = icmp eq ptr %c2go.va.argp{{[0-9]+}}, null
// CHECK: %c2go.va.safep{{[0-9]+}} = select i1 %c2go.va.atend{{[0-9]+}}, ptr %c2go.va.zero{{[0-9]+}}, ptr %c2go.va.argp{{[0-9]+}}
// CHECK: load i64, ptr %c2go.va.safep{{[0-9]+}}

static void my_config(int op, ...) {
  va_list ap;
  va_start(ap, op);
  if (op == 4) {
    void *p = va_arg(ap, void *);
    long l = va_arg(ap, long);
    g = (long)p + l;
  }
  va_end(ap);
}

long test_entry(void) {
  my_config(4, (void *)0x10, 32L);
  return g;
}
