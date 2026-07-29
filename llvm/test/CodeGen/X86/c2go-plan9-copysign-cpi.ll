; Optimized copysign lowers its sign/magnitude masks to packed logical
; operations with RIP-relative constant-pool operands. Those MCInsts carry
; fixups, so the Plan 9 printer must render the symbolic memory tuple instead
; of falling through to the raw-byte path.
;
; RUN: llc < %s -mtriple=x86_64-apple-darwin -output-asm-variant=2 -o - \
; RUN:   | FileCheck %s

declare double @llvm.copysign.f64(double, double)

; Darwin's ordinary external symbol acquires the Mach-O leading underscore;
; c2go_linkname definitions use LLVM's no-mangle marker and do not.
; CHECK-LABEL: TEXT ·_copysign_mask(SB)
; CHECK-COUNT-2: ANDPS LCPI0_{{[01]}}<>(SB), X{{[0-9]+}}
; CHECK-NOT: PLAN9-ERROR
define goabi0cc double @copysign_mask(double %magnitude, double %sign) #0 {
entry:
  %result = call double @llvm.copysign.f64(double %magnitude, double %sign)
  ret double %result
}

; Integer-vector mask manipulation selects PXORrm rather than XORPSrm.
; CHECK-LABEL: TEXT ·_integer_vector_xor(SB)
; CHECK: PXOR LCPI1_0<>(SB), X{{[0-9]+}}
define goabi0cc void @integer_vector_xor(ptr %values) #1 {
entry:
  %input = load <4 x i32>, ptr %values, align 16
  %next = getelementptr <4 x i32>, ptr %values, i64 1
  %other = load <4 x i32>, ptr %next, align 16
  %sum = add <4 x i32> %input, %other
  %result = xor <4 x i32> %sum,
      <i32 -2147483648, i32 -2147483648,
       i32 -2147483648, i32 -2147483648>
  store <4 x i32> %result, ptr %values, align 16
  ret void
}

attributes #0 = { "c2go-argsize"="24" }
attributes #1 = { "c2go-argsize"="8" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"c2go.goabi", i32 1}
