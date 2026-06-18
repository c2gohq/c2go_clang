; The c2go-lto driver must enqueue per-function c2go-boundary argsize metadata
; onto MCPlan9AsmStreamer's pending queue BEFORE the streamer constructor
; drains it. Without that, every c2go_extern boundary emits TEXT ·foo(SB), $N-0
; (argsize=0) instead of TEXT ·foo(SB), $N-<argsize> - and the Go runtime stack
; scanner then sees "0 args" for the boundary frame and skips relocating the
; incoming managed pointer arg during copystack, so a stale stack pointer can
; leak into heap state on the next store ("found bad pointer in Go heap").
;
; This is the c2go-lto mirror of the WF1 clang path: WF1 reads argsize from the
; manifest, this driver reads the per-function c2go-boundary-argsize IR attr
; (stamped on every c2go_extern function). Same source of truth, different
; reader.
;
; boundary_p takes a single managed pointer arg (argsize = 16 bytes = 8B ptr +
; 8B padding) and stores into it, forcing a non-trivial frame so the streamer
; actually emits TEXT + FUNCDATA. A second boundary boundary_scalar (argsize=8,
; no pointer args) guards that argsize is read per-function, not globally.
;
; REQUIRES: aarch64-registered-target

; RUN: c2go-lto %s --c2go-emit-asm=%t.s
; RUN: FileCheck %s --check-prefix=ASM --input-file=%t.s

target triple = "aarch64-unknown-none-elf"

; A small helper the boundary calls so the boundary is not a strict leaf
; (forces a frame to be allocated by the c2go prologue path).
declare void @sink(ptr)

; Boundary with one pointer arg -> argsize=16 (one 8-byte ptr in the Go arg
; area, padded to 16 to match the pointer-aligned computation). c2go-argptrmask
; "01" marks word 0 as a managed pointer.
define void @boundary_p(ptr %p) #0 {
entry:
  call void @sink(ptr %p)
  store i64 42, ptr %p, align 8
  ret void
}

; Boundary with one scalar arg -> argsize=8 (an int32 padded to a single 8-byte
; word). No managed pointer args, mask is "00". Pins per-function argsize
; reading (must NOT collapse to a single global value).
define void @boundary_scalar(i32 %x) #1 {
entry:
  %slot = alloca i32, align 4
  store i32 %x, ptr %slot, align 4
  call void @sink(ptr %slot)
  ret void
}

attributes #0 = {
  "c2go-boundary"
  "c2go-c-name"="boundary_p"
  "c2go-go-sig"="func boundary_p(p unsafe.Pointer)"
  "c2go-boundary-argsize"="16"
  "c2go-export-case"="1"
  "c2go-argptrmask"="01"
}

attributes #1 = {
  "c2go-boundary"
  "c2go-c-name"="boundary_scalar"
  "c2go-go-sig"="func boundary_scalar(x int32)"
  "c2go-boundary-argsize"="8"
  "c2go-export-case"="1"
  "c2go-argptrmask"="00"
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.target_go_version", !"1.22-1.25"}
!2 = !{i32 1, !"c2go.pkgpath", !"wf2bd"}

; --- ASM-side assertions ----------------------------------------------------
;
; The two TEXT directives must carry their per-function argsize (16 / 8), not
; $0. The framesize ($N) depends on backend scheduling of the call sequence, so
; the patterns match it with {{[0-9]+}} and pin only the argsize.
;
; ASM:     TEXT ·boundary_p(SB), ${{[0-9]+}}-16
; ASM:     TEXT ·boundary_scalar(SB), ${{[0-9]+}}-8
;
; Negative check: no boundary may carry $N-0 (which would mean the
; argsize enqueue regressed).
;
; ASM-NOT: TEXT ·boundary_p(SB), ${{[0-9]+}}-0
; ASM-NOT: TEXT ·boundary_scalar(SB), ${{[0-9]+}}-0
