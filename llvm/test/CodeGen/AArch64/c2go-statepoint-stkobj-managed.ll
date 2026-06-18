; A managed-record alloca (an LLVM struct type whose type-info path already
; emitted c2go.gcbitmap.<RecName>) that becomes a Direct(SP, off) gc-live
; operand of a statepoint must drive a FUNCDATA $2, gcstkobj·<hash> stack-objects
; table, with the corresponding rodata body (N x 16-byte entries: frameOffset /
; size / ptrBytes / SymPtrOff-to-gcdata).
;
; The full clang-driven *.c path is unreachable for this guard: the managed /
; struct attributes interact with several unrelated clang-side limits (RS4GC
; addrspacecast assert, a Sema diagnostic, -O2 mem2reg killing the alloca, -O0
; not running RS4GC). So the IR here is what c2go-gc-setup + RS4GC would have
; produced - a gc "c2go-gc" function with a named %struct.S alloca whose address
; survives a non-intrinsic call - plus the gcbitmap global the type-info path
; emits, which is exactly what the stack-object collector keys on
; (getNamedGlobal("c2go.gcbitmap." + RecName)).
;
; Guard fields (stable, hash-independent):
;   * FUNCDATA $2, gcstkobj·...     - the table is emitted (non-empty case).
;   * header DATA ...+0(SB)/8, $1   - N == 1 entry.
;   * DATA ...+8(SB)/4, $0xfffffff0 - frameOffset == -16. The collector computes
;     SpOff - (FrameSize - 8); with FrameSize=32 and SpOff=16 this is
;     16 - (32 - 8) = -16. Pinned to the observed value.
;   * DATA ...+12(SB)/4, $16        - size == sizeof(struct.S) == 16.
;   * DATA ...+16(SB)/4, $8         - ptrBytes == 8 (one ptr addrspace(1) at off 0).
;   * DATA ...+20(SB)/4, $c2go.gcbitmap.S(SB) - SymPtrOff to the gcbitmap.
;   * GLOBL gcstkobj·..., DUPOK|RODATA, $24 - total wire = 8 + 16*1 = 24.
;
; RUN: opt < %s -passes='c2go-gc-setup,rewrite-statepoints-for-gc' -S \
; RUN:   | llc --output-asm-variant=2 -mtriple=arm64-unknown-none-goabi -c2go-funcdata2 \
; RUN:   | FileCheck %s

target triple = "arm64-unknown-none-goabi"

; A managed-record shape: one managed-pointer field + one scalar field. The
; LLVM struct name (struct.S) has its struct. prefix stripped by the collector
; to form the gcbitmap lookup key (S).
%struct.S = type { ptr addrspace(1), i64 }

; The gcbitmap the type-info path would emit for struct S: one set bit means
; "word 0 is a pointer" (ptrBytes = 8). linkonce_odr / hidden / unnamed_addr
; mirror the type-info emission for the ELF case.
@c2go.gcbitmap.S = linkonce_odr hidden unnamed_addr constant [1 x i8] c"\01"

declare void @runtime_safepoint()

; CHECK-LABEL: TEXT ·c2go_stkobj_demo(SB)
; CHECK:      FUNCDATA $2, gcstkobj·{{[0-9a-f]+}}(SB)
; CHECK-NEXT: DATA gcstkobj·{{[0-9a-f]+}}+0(SB)/8, $1
; CHECK-NEXT: DATA gcstkobj·{{[0-9a-f]+}}+8(SB)/4, $0xfffffff0
; CHECK-NEXT: DATA gcstkobj·{{[0-9a-f]+}}+12(SB)/4, $16
; CHECK-NEXT: DATA gcstkobj·{{[0-9a-f]+}}+16(SB)/4, $8
; CHECK-NEXT: DATA gcstkobj·{{[0-9a-f]+}}+20(SB)/4, $c2go.gcbitmap.S(SB)
; CHECK-NEXT: GLOBL gcstkobj·{{[0-9a-f]+}}(SB), DUPOK|RODATA, $24
define void @c2go_stkobj_demo() gc "c2go-gc" {
entry:
  ; Named alloca of the managed record. Under the c2go-gc strategy RS4GC's
  ; findBaseDefiningValue treats the alloca as a base-defining value, so the
  ; alloca address enters the statepoint's gc-live set -> Direct(SP, off)
  ; location at lowering.
  %local = alloca %struct.S, align 8
  ; The non-intrinsic call is the safepoint RS4GC wraps.
  call void @runtime_safepoint() [ "deopt"() ]
  ; A real use of the alloca after the call keeps %local live across it, so
  ; RS4GC puts it in the gc-live bundle.
  %p = getelementptr inbounds %struct.S, ptr %local, i32 0, i32 0
  store ptr addrspace(1) null, ptr %p, align 8
  ret void
}

!llvm.module.flags = !{!0}
!0 = !{i32 2, !"c2go.goabi", i32 1}
