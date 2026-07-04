; RUN: llc -mtriple=aarch64-unknown-unknown-elf < %s | FileCheck %s

; c2go #584: a GoABI0 function's va_list is a single 8-byte pointer. clang's
; front-end (a Darwin-triple cc1) lays it out Darwin-style because c2go passes
; varargs on the stack (the void** argument pack), and inlines va_start/va_arg
; to match; only llvm.va_copy reaches the backend. c2go-lto then retargets the
; module to a neutral aarch64-unknown-ELF triple, so the backend sees
; isTargetDarwin()==false. Keying LowerVACOPY off the GoABI0 calling convention
; keeps the 8-byte pointer copy; without it the AAPCS 32-byte __va_list struct
; copy overruns the slot and clobbers adjacent frame objects.

declare void @llvm.va_copy.p0(ptr, ptr)

; GoABI0: a single 8-byte pointer copy (x-register load/store), never the
; 32-byte q-register struct copy.
; CHECK-LABEL: goabi0_vacopy:
; CHECK:       ldr x{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK-NEXT:  str x{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK-NEXT:  ret
define goabi0cc void @goabi0_vacopy(ptr %dst, ptr %src) {
  call void @llvm.va_copy.p0(ptr %dst, ptr %src)
  ret void
}

; AAPCS control: the default calling convention still copies the full 32-byte
; __va_list struct (two q-registers) on the same neutral-ELF triple — proving
; the GoABI0 CC, not the triple, is what selects the pointer-sized copy above.
; CHECK-LABEL: aapcs_vacopy:
; CHECK:       ldp q{{[0-9]+}}, q{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK-NEXT:  stp q{{[0-9]+}}, q{{[0-9]+}}, [x{{[0-9]+}}]
; CHECK-NEXT:  ret
define void @aapcs_vacopy(ptr %dst, ptr %src) {
  call void @llvm.va_copy.p0(ptr %dst, ptr %src)
  ret void
}
