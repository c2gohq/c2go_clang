; c2go_extern boundary symbols (the export surface clang stamps with
; c2go-boundary) must KEEP their external linkage through the whole
; c2go-lto WF2 pipeline - they must not be silently demoted to internal /
; linkonce_odr / private by the cross-TU inliner or any follow-on rewrite.
;
; This is the linkage analogue of the existing coverage: a banlist test
; proves InternalizePass is hard-rejected (presence), and a manifest test
; proves the boundary NamedMD survives the inliner + globaldce (metadata).
; A linkage demotion (without removing the symbol) would slip past both,
; yet still break the contract Go bridge code relies on: it references the
; boundaries by their C name, so a demotion makes the link fail or, worse,
; pick up a same-named symbol from another TU.
;
; The three boundaries carry the exact attr set CodeGenModule stamps for
; c2go_extern functions, so the c2go-lto reader treats them as it would
; real CodeGenModule output:
;   * stress_ascast_validate  - int (void), the validation entrypoint
;   * stress_ascast_init      - void (void)
;   * stress_ascast_run       - void (int)
;
; REQUIRES: aarch64-registered-target

; Drive the full WF2 pipeline (inliner ON by default, late c2go replay,
; manifest+codegen), then disassemble the cleaned combined bitcode and
; check there is no linkage demotion on the three c2go_extern symbols.
;
; --c2go-emit-manifest is required to satisfy the manifest precondition the
; codegen gcmask collect asserts before running Plan-9 codegen.
;
; RUN: c2go-lto %s --c2go-emit-manifest=%t.json --output-bc=%t.bc
; RUN: llvm-dis %t.bc -o %t.ll
; RUN: FileCheck %s --input-file=%t.ll --check-prefix=LINKAGE

target triple = "aarch64-unknown-none-elf"

; A scratch global the c2go_extern functions touch - keeps the inliner
; honest (if the boundaries were trivial-dead the inliner would be free
; to delete them outright, which would mask the linkage check).
@gScratch = internal global i64 0

; --- the three c2go_extern boundary functions (external linkage) ----------
;
; LINKAGE: define i32 @stress_ascast_validate
; LINKAGE-NOT: define internal {{.*}} @stress_ascast_validate
; LINKAGE-NOT: define linkonce_odr {{.*}} @stress_ascast_validate
; LINKAGE-NOT: define private {{.*}} @stress_ascast_validate
define i32 @stress_ascast_validate() #0 {
entry:
  %v = load i64, ptr @gScratch, align 8
  %t = trunc i64 %v to i32
  ret i32 %t
}

; LINKAGE: define void @stress_ascast_init
; LINKAGE-NOT: define internal {{.*}} @stress_ascast_init
; LINKAGE-NOT: define linkonce_odr {{.*}} @stress_ascast_init
; LINKAGE-NOT: define private {{.*}} @stress_ascast_init
define void @stress_ascast_init() #1 {
entry:
  store i64 0, ptr @gScratch, align 8
  ret void
}

; LINKAGE: define void @stress_ascast_run
; LINKAGE-NOT: define internal {{.*}} @stress_ascast_run
; LINKAGE-NOT: define linkonce_odr {{.*}} @stress_ascast_run
; LINKAGE-NOT: define private {{.*}} @stress_ascast_run
define void @stress_ascast_run(i32 %depth) #2 {
entry:
  %z = zext i32 %depth to i64
  store i64 %z, ptr @gScratch, align 8
  ret void
}

; Attribute set matches the boundary stamps CodeGenModule emits for
; c2go_extern functions.
attributes #0 = {
  "c2go-boundary"
  "c2go-c-name"="stress_ascast_validate"
  "c2go-go-sig"="func stress_ascast_validate() int32"
  "c2go-boundary-argsize"="8"
  "c2go-export-case"="1"
}

attributes #1 = {
  "c2go-boundary"
  "c2go-c-name"="stress_ascast_init"
  "c2go-go-sig"="func stress_ascast_init()"
  "c2go-boundary-argsize"="0"
  "c2go-export-case"="1"
}

attributes #2 = {
  "c2go-boundary"
  "c2go-c-name"="stress_ascast_run"
  "c2go-go-sig"="func stress_ascast_run(depth int32)"
  "c2go-boundary-argsize"="8"
  "c2go-export-case"="1"
}

!llvm.module.flags = !{!0, !1, !2}
!0 = !{i32 1, !"c2go.goabi", i32 1}
!1 = !{i32 1, !"c2go.target_go_version", !"1.22-1.25"}
!2 = !{i32 1, !"c2go.pkgpath", !"stress_ascast_lit"}
