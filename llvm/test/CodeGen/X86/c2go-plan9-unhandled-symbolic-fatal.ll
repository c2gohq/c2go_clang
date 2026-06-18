; MCPlan9AsmStreamer must fail CLOSED on a symbol-bearing instruction the
; target Plan-9 InstPrinter cannot translate.
;
; Pre-fix emitRawBytesOrFail emitted a comment ("unhandled symbol-bearing
; instruction ...") and continued. Go's assembler does not reject
; comments, so the instruction was silently swallowed: RIP-relative
; global stores no-op'd and dropped compares left stale EFLAGS for the
; following Jcc/CMOV - a deterministic, depth- and opt-level-independent
; miscompile. The comment claimed "fail-loud" while the behavior was
; fail-open; the fix makes the policy real with report_fatal_error.
;
; Trigger here: an x86_fp80 store against a RIP-relative global lowers to
; ST_FP80m with a symbolic displacement. Symbolic x87 stores are outside
; the Plan-9 printer's coverage, so the streamer must abort the compile
; instead of shipping a .s with the store missing. (x87 constant loads
; via LD_F80m now print as FMOVX ·sym(SB), F0, so this lock moved to the
; store dual, which no c2go workload produces yet.)
;
; RUN: rm -rf %t && mkdir -p %t
;
; RUN: opt %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -passes=x86-c2go-leaf-abi -o %t/flipped.bc
;
; RUN: not --crash llc %t/flipped.bc -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -relocation-model=pic -output-asm-variant=2 -o /dev/null 2>&1 \
; RUN:   | FileCheck %s
;
; CHECK: LLVM ERROR: MCPlan9AsmStreamer: unhandled symbol-bearing instruction (opcode=ST_FP80m, 1 fixup(s))

target triple = "x86_64-unknown-linux-gnu"

@gLD = internal global x86_fp80 0xK00000000000000000000, align 16

define internal goabi0cc void @st_use(ptr %in) #0 {
  %v = load x86_fp80, ptr %in, align 16
  store x86_fp80 %v, ptr @gLD, align 16
  ret void
}

attributes #0 = { "c2go-reg-return" "c2go-argsize"="8" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
