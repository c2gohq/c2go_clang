; CC_X86_64_C2GoABIInternal_TD must hard-fail on types outside its
; supported range (scalable vector / YMM <8 x i32> / ZMM / x86 mask /
; other exotic types) rather than silently fall through to SysV CC_X86_64.
;
; The CC_X86 td entry handles C2GoABIInternal via the custom hook, but
; the next td fall-through delegates to CC_X86_64. If the custom hook
; returned false for an unsupported MVT, the value would land in the SysV
; reg file while the function CC has already been flipped to
; c2goabiinternalcc - a caller/callee mismatch and silent memory
; corruption. The hook instead report_fatal_error's, so the regression is
; caught at build/test time. This LIT checks it via `not llc` + the error
; message.
;
; The YMM <8 x i32> (AVX2) arg + result has a 256-bit LocVT v8i32; the
; reg-pass table only covers up to 128-bit (XMM0..XMM14, no YMM), so it
; hits the Common catch-all and must report_fatal_error. i128 would be
; split into 2x i64 by the type legalizer and reg-pass via the i64 path,
; bypassing the catch-all; v8i32 is not split under AVX2 and goes
; straight to CC dispatch, making it the stable trigger. A regression
; back to silent fall-through is caught here immediately.
;
; RUN: not --crash llc < %s -mtriple=x86_64-unknown-linux-gnu \
; RUN:     -mattr=+avx2 -global-isel=0 -fast-isel=false -O0 2>&1 \
; RUN:   | FileCheck %s --check-prefix=ERR \
; RUN:     --implicit-check-not=FALL-THROUGH
;
; ERR: c2go ABIInternal (x86_64): unsupported arg/result type
; ERR-SAME: caller has been flipped to `c2goabiinternalcc`
; ERR-SAME: silently corrupt the call ABI

target triple = "x86_64-unknown-linux-gnu"

; --- YMM <8 x i32> arg + result, already c2goabiinternalcc: backend
;     ISel reaches RetCC_X86_64_C2GoABIInternal_TD -> the Common
;     catch-all, where v8i32 triggers the fatal error.
define internal c2goabiinternalcc <8 x i32>
@takes_ymm(<8 x i32> %a, <8 x i32> %b) #0 {
  %s = add <8 x i32> %a, %b
  ret <8 x i32> %s
}

attributes #0 = { "c2go-reg-return" }

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.x86-leaf-abi", i32 1}
