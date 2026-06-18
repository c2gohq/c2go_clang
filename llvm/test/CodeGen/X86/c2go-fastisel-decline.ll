; X86 FastISel must decline in c2go-mode modules.
;
; At -O0, X86FastISel lowers every non-volatile @llvm.memset to a SysV
; register-arg memset libcall (unlike memcpy, it has no small-constant
; inline path). In a c2go-mode module that libcall becomes `CALL
; ·memset(SB)` against the Go-side stack-ABI0 wrapper, which reads
; garbage args off the goroutine stack and sprays it (e.g. a 24-byte
; va_list zero-init -> memset of a huge length -> crash).
;
; Mirroring AArch64, FastISel is declined for any function in a module
; carrying the c2go.goabi flag (or with a GoABI0 CC) so SelectionDAG -
; the only ISel that knows the GoABI0 marshalling and inlines small
; memsets - handles everything.
;
; With the c2go.goabi module flag: FastISel declined, the 24-byte memset
; is inlined as plain stores, no libcall.
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -O0 < %s \
; RUN:   | FileCheck %s --check-prefix=C2GO
;
; Control (sensitivity proof): the same module WITHOUT the flag keeps
; stock -O0 behavior - FastISel runs and emits the memset libcall. If
; upstream FastISel ever grows a small-memset inline path this control
; flags the test for re-evaluation rather than going silently vacuous.
; RUN: sed -e 's/^!llvm.module.flags.*$//' -e 's/^!0 = .*$//' %s \
; RUN:   | llc -mtriple=x86_64-unknown-linux-gnu -O0 \
; RUN:   | FileCheck %s --check-prefix=STOCK

; C2GO-LABEL: zinit:
; C2GO-NOT: callq
; C2GO-DAG: movq $0, (%rdi)
; C2GO-DAG: movq $0, 8(%rdi)
; C2GO-DAG: movq $0, 16(%rdi)
; C2GO-NOT: callq

; STOCK-LABEL: zinit:
; STOCK: callq memset
define void @zinit(ptr %dst) {
entry:
  call void @llvm.memset.p0.i64(ptr align 8 %dst, i8 0, i64 24, i1 false)
  ret void
}

declare void @llvm.memset.p0.i64(ptr writeonly captures(none), i8, i64, i1 immarg)

!llvm.module.flags = !{!0}
!0 = !{i32 7, !"c2go.goabi", i32 1}
