// X86 Plan 9 .s: a SETcc-to-memory (SETCCm) whose memory operand is a
// RIP-relative global must print symbolically (the global name on an (SB)
// base, see CHECK below), reusing the Plan 9 JCC condition-suffix table (J<cc>
// and SET<cc> share the same suffix in Go's assembler). A SETCCm with a plain
// frame-slot operand keeps the raw-byte fallback. Before this, a symbol-bearing
// SETCCm hit the streamer's fail-closed guard and aborted.
//
// REQUIRES: x86-registered-target
//
// RUN: %clang_cc1 -triple x86_64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -O2 -fc2go-emit-plan9-asm=%t.s -emit-obj -o %t.o %s
// RUN: FileCheck %s --input-file=%t.s

static char gFlag;
static long gCount;
long getc2(void);

// The call keeps the comparison non-foldable; ISel stores the EFLAGS
// predicate straight into the i8 global.
// CHECK: TEXT ·upd(SB)
// CHECK: SETNE ·gFlag(SB)
void upd(void) { gFlag = (getc2() != 0); }

// Keep both globals referenced so neither folds away.
long rd(void) { return gFlag + gCount; }

// The old failure mode was a fatal error, so reaching FileCheck at all is
// half the lock; the mnemonic match above pins the printed form (no
// PLAN9-ERROR comment, no raw WORD bytes).
// CHECK-NOT: PLAN9-ERROR
