; The WF2 manifest fallback must make the same bridge decision as Clang's AST
; manifest builder. A same-package ABIInternal target uses its local suffix and
; needs no c2go-bind bridge; a cross-package ABIInternal target still does.
;
; REQUIRES: aarch64-registered-target
;
; RUN: c2go-lto %s --c2go-emit-manifest=%t.json
; RUN: FileCheck %s --input-file=%t.json

target triple = "aarch64-unknown-none-goabi"

declare goabi0cc void @LocalTarget() #0
declare goabi0cc void @example_com_other_InternalCross() #1
declare goabi0cc void @"example.com/other.DirectCross"() #2

define void @use_all() {
  call goabi0cc void @LocalTarget()
  call goabi0cc void @example_com_other_InternalCross()
  call goabi0cc void @"example.com/other.DirectCross"()
  ret void
}

attributes #0 = {
  "c2go-c-name"="local_target"
  "c2go-go-sig"="func local_target()"
  "c2go-linkname"="example.com/lib.LocalTarget"
}

attributes #1 = {
  "c2go-c-name"="cross_internal"
  "c2go-go-sig"="func cross_internal()"
  "c2go-linkname"="example.com/other.InternalCross"
}

attributes #2 = {
  "c2go-c-name"="cross_direct"
  "c2go-go-sig"="func cross_direct()"
  "c2go-linkname"="example.com/other.DirectCross"
  "c2go-linkname-abi0"
}

!llvm.module.flags = !{!0, !1}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.pkgpath", !"example.com/lib"}

; CHECK: "linknames": [
; CHECK-NEXT: {
; CHECK-NEXT: "asm_symbol": "·example_com_other_InternalCross"
; CHECK-NEXT: "go_sig": "func cross_internal()"
; CHECK-NEXT: "kind": "func"
; CHECK-NEXT: "linkname": "example.com/other.InternalCross"
; CHECK-NEXT: "name": "cross_internal"
; CHECK-NEXT: }
; CHECK-NEXT: ]
; CHECK-NOT: example.com/lib.LocalTarget
; CHECK-NOT: example.com/other.DirectCross
; CHECK: "pkgpath": "example.com/lib"
