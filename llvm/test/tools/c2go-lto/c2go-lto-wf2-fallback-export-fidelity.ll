; WF2 fallback manifest path: when the c2go.func.<X> NamedMD is absent on a
; boundary function, c2go-lto must reconstruct the export-identity fields
; go_name, asm_symbol, argsize, abi, and needs_linkname from the
; per-function string attrs CodeGenModule already stamps (c2go-c-name,
; c2go-go-sig, c2go-export-case, c2go-boundary-argsize). Otherwise only
; name / go_sig / kind / managed survive, diverging from the WF1 manifest
; (which always carries the AST-derived 9-field shape).
;
; The fallback also surfaces (a) the variadic bit via F->isVarArg() and
; (b) the program-entry markers c_entry + entry_sig for a C `main`
; boundary. my_va_fn pins the variadic recovery; @main pins the c_entry /
; entry_sig recovery from the argument count.
;
; This is intentionally the FALLBACK path: no !c2go.func NamedMD is
; provided, so the c2go-lto reader cannot take the NamedMD shortcut and
; must rebuild from string attrs.
;
; ExportCase=1 + a snake_case CName (my_boundary_fn) exercises the
; capitalisation rule (-> MyBoundaryFn); the resulting go_name differing
; from the link-base CName triggers needs_linkname: true. keepCase_fn uses
; ExportCase=0 (verbatim) to pin that case-preservation suppresses
; needs_linkname.

; REQUIRES: aarch64-registered-target

; RUN: c2go-lto %s --c2go-emit-manifest=%t.json
; RUN: FileCheck %s --input-file=%t.json

target triple = "aarch64-unknown-none-elf"

; Boundary with snake_case CName + ExportCase=1 -> go_name=MyBoundaryFn.
define i32 @my_boundary_fn(ptr %p) #0 {
entry:
  %v = load i32, ptr %p, align 4
  ret i32 %v
}

; Boundary with verbatim ExportCase=0 -> go_name=keepCase_fn (unchanged).
define i32 @keepCase_fn(i32 %x) #1 {
entry:
  ret i32 %x
}

; Variadic fallback recovery - F->isVarArg() surfaces is_variadic: true
; even without the c2go.func.my_va_fn NamedMD.
define void @my_va_fn(ptr %fmt, ...) #2 {
entry:
  ret void
}

; C main(argc, argv) entry -> c_entry + entry_sig from the argument count.
; Also exercises the init/main rename: the renamed bare symbol is
; c2go_cmain, so asm_symbol carries ·c2go_cmain and needs_linkname: true
; (go_name Main != link-base c2go_cmain).
define i32 @main(i32 %argc, ptr %argv) #3 {
entry:
  ret i32 0
}

attributes #0 = {
  "c2go-boundary"
  "c2go-c-name"="my_boundary_fn"
  "c2go-go-sig"="func my_boundary_fn(p unsafe.Pointer) int32"
  "c2go-boundary-argsize"="16"
  "c2go-export-case"="1"
}

attributes #1 = {
  "c2go-boundary"
  "c2go-c-name"="keepCase_fn"
  "c2go-go-sig"="func keepCase_fn(x int32) int32"
  "c2go-boundary-argsize"="16"
  "c2go-export-case"="0"
}

attributes #2 = {
  "c2go-boundary"
  "c2go-c-name"="my_va_fn"
  "c2go-go-sig"="func my_va_fn(fmt unsafe.Pointer, args ...interface{})"
  "c2go-boundary-argsize"="8"
  "c2go-export-case"="1"
}

attributes #3 = {
  "c2go-boundary"
  "c2go-c-name"="main"
  "c2go-go-sig"="func main(argc int32, argv unsafe.Pointer) int32"
  "c2go-boundary-argsize"="24"
  "c2go-export-case"="1"
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.target_go_version", !"1.22-1.25"}
!2 = !{i32 1, !"c2go.pkgpath", !"wf2fb"}

; --- manifest JSON assertions ------------------------------------------------
;
; AllSyms is sorted by name, so the order is: keepCase_fn, main,
; my_boundary_fn, my_va_fn. The JSON pretty-printer sorts keys
; alphabetically within each entry: abi, argsize, asm_symbol, c_entry,
; entry_sig, go_name, go_sig, is_variadic, kind, managed, name
; [, needs_linkname].
;
; ExportCase=0 path - go_name == CName, no needs_linkname.
; CHECK:      "abi": "abi0"
; CHECK-NEXT: "argsize": 16
; CHECK-NEXT: "asm_symbol": "·keepCase_fn"
; CHECK-NEXT: "go_name": "keepCase_fn"
; CHECK-NEXT: "go_sig": "func keepCase_fn(x int32) int32"
; CHECK-NEXT: "kind": "func"
; CHECK-NEXT: "managed": true
; CHECK-NEXT: "name": "keepCase_fn"
;
; C main(argc, argv) entry: c_entry + entry_sig="argc_argv" (from the
; argument count), asm_symbol carries the renamed bare symbol c2go_cmain,
; needs_linkname set because go_name "Main" != link-base "c2go_cmain".
; CHECK:      "abi": "abi0"
; CHECK-NEXT: "argsize": 24
; CHECK-NEXT: "asm_symbol": "·c2go_cmain"
; CHECK-NEXT: "c_entry": true
; CHECK-NEXT: "entry_sig": "argc_argv"
; CHECK-NEXT: "go_name": "Main"
; CHECK-NEXT: "go_sig": "func main(argc int32, argv unsafe.Pointer) int32"
; CHECK-NEXT: "kind": "func"
; CHECK-NEXT: "managed": true
; CHECK-NEXT: "name": "main"
; CHECK-NEXT: "needs_linkname": true
;
; ExportCase=1 path - snake_case -> CamelCase, needs_linkname set.
; CHECK:      "abi": "abi0"
; CHECK-NEXT: "argsize": 16
; CHECK-NEXT: "asm_symbol": "·my_boundary_fn"
; CHECK-NEXT: "go_name": "MyBoundaryFn"
; CHECK-NEXT: "go_sig": "func my_boundary_fn(p unsafe.Pointer) int32"
; CHECK-NEXT: "kind": "func"
; CHECK-NEXT: "managed": true
; CHECK-NEXT: "name": "my_boundary_fn"
; CHECK-NEXT: "needs_linkname": true
;
; Variadic recovery: is_variadic: true surfaces from F->isVarArg() with no
; NamedMD present.
; CHECK:      "abi": "abi0"
; CHECK-NEXT: "argsize": 8
; CHECK-NEXT: "asm_symbol": "·my_va_fn"
; CHECK-NEXT: "go_name": "MyVaFn"
; CHECK-NEXT: "go_sig": "func my_va_fn(fmt unsafe.Pointer, args ...interface{})"
; CHECK-NEXT: "is_variadic": true
; CHECK-NEXT: "kind": "func"
; CHECK-NEXT: "managed": true
; CHECK-NEXT: "name": "my_va_fn"
; CHECK-NEXT: "needs_linkname": true
