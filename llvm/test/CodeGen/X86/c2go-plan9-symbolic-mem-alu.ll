; X86 Plan-9 printer: translate the symbolic-displacement memory-direct
; MOV-imm / integer-ALU forms that X86 ISel folds global RMW / compare /
; store-imm into:
;
;   MOV64mi32 / MOV32mi  - gSink[i] = 0     -> MOVQ/MOVL $imm, ·sym(SB)
;   XOR64mr              - gSink[2] ^= v    -> XORQ Rs, ·sym(SB)
;   INC64m               - gSink[0]++       -> INCQ ·sym(SB)
;   ADD64rm              - v + gSink[0]      -> ADDQ ·sym(SB), Rd
;   CMP64mi8 / CMP64mr   - gSink[1] != 0    -> CMPQ ·sym(SB), $0 / Rs
;   CMP32rm              - n > gMaxDepth    -> CMPL Rs, ·sym(SB)
;
; AArch64 has no memory-direct ALU (every global RMW expands to already
; covered LDR/op/STR), so only the X86 port had this coverage gap. Pre-fix
; these fixup-bearing MCInsts fell to emitRawBytesOrFail, which (fail-open)
; swallowed them behind a comment the Go assembler ignores: an init became
; a no-op and dropped CMP64mi8/CMP64mr left stale EFLAGS for the following
; JEQ/CMOV - a deterministic miscompile at every depth, -O0 and -O2 alike.
; The streamer is now fail-closed for unhandled symbol-bearing
; instructions (see c2go-plan9-unhandled-symbolic-fatal.ll for that lock).
;
; Plan-9 operand-order contract pinned here (mirrors Go runtime asm):
;   * compares: subject first - flags = left - right, preserving the Intel
;     predicate of the original encoding for the following Jcc
;     (CMPQ ·sym(SB), $0 / CMPQ ·sym(SB), Rs / CMPL Rs, ·sym(SB)).
;   * RMW / stores: data flows left -> right (XORQ AX, ·sym(SB),
;     MOVQ $0, ·sym(SB), ADDQ ·sym(SB), Rd).
;
; `-relocation-model=pic` pins RIP-relative lowering (mirrors the darwin
; production triple; the absolute non-PIC form prints identically).
;
; RUN: rm -rf %t && mkdir -p %t
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t/flipped.bc
;
; RUN: llc %t/flipped.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -relocation-model=pic -output-asm-variant=2 -o - 2>&1 \
; RUN:   | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

@gSink = internal global [3 x i64] zeroinitializer, align 8
@gMaxDepth = internal global i32 0, align 4

; MOV64mi32 + MOV32mi - store-immediate straight into the global. Pre-fix
; both were swallowed and the init compiled to a bare RET (a no-op).
; CHECK-LABEL: TEXT ·sym_init(SB)
; CHECK:       MOVQ $0, ·gSink(SB)
; CHECK:       MOVL $0, ·gMaxDepth(SB)
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc void @sym_init() #0 {
  store i64 0, ptr @gSink, align 8
  store i32 0, ptr @gMaxDepth, align 4
  ret void
}

; XOR64mr (RMW: src reg first, mem dest second) + INC64m.
; CHECK-LABEL: TEXT ·sym_rmw(SB)
; CHECK:       XORQ {{[A-Z0-9]+}}, ·gSink+16(SB)
; CHECK:       INCQ ·gSink(SB)
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc void @sym_rmw(i64 %v) #0 {
  %p2 = getelementptr inbounds [3 x i64], ptr @gSink, i64 0, i64 2
  %h = load i64, ptr %p2, align 8
  %x = xor i64 %h, %v
  store i64 %x, ptr %p2, align 8
  %f = load i64, ptr @gSink, align 8
  %n = add i64 %f, 1
  store i64 %n, ptr @gSink, align 8
  ret void
}

; ADD64rm (reg-dest RMW: mem source first, Rd second).
; CHECK-LABEL: TEXT ·sym_addload(SB)
; CHECK:       ADDQ ·gSink(SB), {{[A-Z0-9]+}}
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc i64 @sym_addload(i64 %v) #0 {
  %f = load i64, ptr @gSink, align 8
  %s = add i64 %v, %f
  ret i64 %s
}

; CMP64mi8 / CMP64mr (subject = mem, first) and CMP32rm (subject = reg,
; first). These drive the following Jcc predicates; pre-fix the dropped
; compares left stale EFLAGS.
; CHECK-LABEL: TEXT ·sym_validate(SB)
; CHECK:       CMPQ ·gSink+8(SB), $0
; CHECK:       CMPQ ·gSink+16(SB), {{[A-Z0-9]+}}
; CHECK:       CMPL {{[A-Z0-9]+}}, ·gMaxDepth(SB)
; CHECK:       MOVL {{[A-Z0-9]+}}, ·gMaxDepth(SB)
; CHECK-NOT:   PLAN9-ERROR
define internal goabi0cc i32 @sym_validate(i64 %h, i32 %n) #0 {
entry:
  %p1 = getelementptr inbounds [3 x i64], ptr @gSink, i64 0, i64 1
  %m = load i64, ptr %p1, align 8
  %c = icmp ne i64 %m, 0
  br i1 %c, label %zero, label %next
next:
  %p2 = getelementptr inbounds [3 x i64], ptr @gSink, i64 0, i64 2
  %h2 = load i64, ptr %p2, align 8
  %ceq = icmp eq i64 %h2, %h
  br i1 %ceq, label %zero, label %next2
next2:
  %g = load i32, ptr @gMaxDepth, align 4
  %cgt = icmp sgt i32 %n, %g
  br i1 %cgt, label %st, label %done
st:
  store i32 %n, ptr @gMaxDepth, align 4
  br label %done
done:
  ret i32 1
zero:
  ret i32 0
}

attributes #0 = { "c2go-reg-return" "c2go-argsize"="24" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
