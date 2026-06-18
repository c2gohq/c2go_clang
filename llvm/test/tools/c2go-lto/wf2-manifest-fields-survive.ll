; The boundary c2go.func.<X> NamedMD must survive whatever the WF2 lto
; pipeline does on the way to manifest emission (globaldce, inliner, etc.).
; The c2go-lto reader binds entries by name from the function attr set, so
; the MD has to be in the module at manifest emit time even if the function
; was internalised / inlined / DCE'd. In practice c2go-boundary functions
; are external by construction (kept alive in llvm.compiler.used) and never
; inlined, but this proves the NamedMD survives the standard pipeline.
;
; This test feeds c2go-lto a single .ll with one static-inline helper + one
; boundary that calls it. The inliner folds the helper, leaving only the
; boundary; the boundary's c2go.func.boundary_fn NamedMD must still appear
; in the manifest.

; REQUIRES: aarch64-registered-target

; RUN: c2go-lto %s --c2go-emit-manifest=%t.json
; RUN: FileCheck %s --input-file=%t.json

target triple = "aarch64-unknown-none-elf"

; Internal helper that would normally get folded by the inliner. Not a
; boundary - no c2go-boundary attr, no NamedMD.
define internal i32 @helper(i32 %x) {
  %r = add i32 %x, 1
  ret i32 %r
}

; The boundary. External linkage + c2go-boundary attrs match what
; CodeGenModule stamps for a `c2go_extern` function.
define i32 @boundary_fn(i32 %x) #0 {
  %r = call i32 @helper(i32 %x)
  ret i32 %r
}

attributes #0 = {
  "c2go-boundary"
  "c2go-c-name"="boundary_fn"
  "c2go-go-sig"="func boundary_fn(x int32) int32"
  "c2go-boundary-argsize"="16"
  "c2go-export-case"="1"
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.target_go_version", !"1.22-1.25"}
!2 = !{i32 1, !"c2go.pkgpath", !"wf2mf"}

; The 14-operand schema clang emits for a boundary function.
; Operand 0..13 in fixed order.
!c2go.func.boundary_fn = !{!10}
!10 = !{
  !"boundary_fn",                              ; 0 name
  !"func boundary_fn(x int32) int32",          ; 1 go_sig
  !"func",                                     ; 2 kind
  i1 true,                                     ; 3 managed
  !"BoundaryFn",                               ; 4 go_name
  !"abi0",                                     ; 5 abi
  !"·boundary_fn",                             ; 6 asm_symbol
  i32 16,                                      ; 7 argsize
  i1 true,                                     ; 8 needs_linkname
  i1 false,                                    ; 9 is_variadic
  i1 false,                                    ; 10 has_float
  i1 false,                                    ; 11 has_aggregate
  i1 false,                                    ; 12 c_entry
  !""                                          ; 13 entry_sig
}

; All 9 NamedMD-driven manifest fields must appear with the values
; encoded above. Without the NamedMD path the reader would only emit
; name / go_sig / kind / managed (from per-function string attrs),
; dropping the other 5 - abi / argsize / asm_symbol / go_name /
; needs_linkname. This is the minimum-shape proof that the reader
; consults the 14-op NamedMD.
;
; JSON pretty-printer sorts keys alphabetically within each entry, so
; the CHECK order below mirrors:
;   abi / argsize / asm_symbol / go_name / go_sig / kind / managed /
;   name / needs_linkname.
;
; CHECK:      "abi": "abi0"
; CHECK-NEXT: "argsize": 16
; CHECK-NEXT: "asm_symbol": "·boundary_fn"
; CHECK-NEXT: "go_name": "BoundaryFn"
; CHECK-NEXT: "go_sig": "func boundary_fn(x int32) int32"
; CHECK-NEXT: "kind": "func"
; CHECK-NEXT: "managed": true
; CHECK-NEXT: "name": "boundary_fn"
; CHECK-NEXT: "needs_linkname": true
