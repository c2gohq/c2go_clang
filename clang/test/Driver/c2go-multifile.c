// A `-fc2go` compile that requests the Plan 9 .s / manifest outputs is routed
// internally through c2go-lto (the WF2 bitcode linker) — the single internal
// path. Each TU is compiled to bitcode and c2go-lto links them, emitting the
// .s + manifest. This holds for several inputs (one merged .s/manifest) and for
// a single input alike. A plain `-fc2go -c` stops at pre-link bitcode with an
// ordinary `.o` suffix; it does not invoke c2go-lto until the later archive or
// link step.

// RUN: %clang --target=aarch64-unknown-none-goabi -fc2go -fc2go-package=t \
// RUN:   -fc2go-emit-plan9-asm=%t.s -fc2go-emit-manifest=%t.json -### \
// RUN:   %s %S/Inputs/c2go-extra.c 2>&1 | FileCheck %s --check-prefix=MULTI
//
// MULTI: "-cc1"{{.*}}"-emit-llvm-bc"{{.*}}"-fc2go-lto-prelink"{{.*}}c2go-multifile.c
// MULTI: "-cc1"{{.*}}"-emit-llvm-bc"{{.*}}"-fc2go-lto-prelink"{{.*}}c2go-extra.c
// MULTI: c2go-lto"
// MULTI-SAME: "--c2go-emit-asm={{.*}}.s"
// MULTI-SAME: "--c2go-emit-manifest={{.*}}.json"

// A single-file compile takes the same path: one bitcode job, one c2go-lto job.
// RUN: %clang --target=aarch64-unknown-none-goabi -fc2go -fc2go-package=t \
// RUN:   -fc2go-emit-plan9-asm=%t.s -fc2go-emit-manifest=%t.json -### -c \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=SINGLE
//
// SINGLE: "-cc1"{{.*}}"-emit-llvm-bc"{{.*}}"-fc2go-lto-prelink"{{.*}}c2go-multifile.c
// SINGLE: c2go-lto"
// SINGLE-SAME: "--c2go-emit-asm={{.*}}.s"
// SINGLE-SAME: "--c2go-emit-manifest={{.*}}.json"

// The default compile-only workflow emits pre-link bitcode named like an
// ordinary object and leaves it for a later standalone c2go-lto invocation.
// RUN: %clang --target=aarch64-unknown-none-goabi -fc2go -### -c \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=PLAIN
//
// PLAIN: "-cc1"{{.*}}"-emit-llvm-bc"{{.*}}"-fc2go-lto-prelink"
// PLAIN-SAME: "-o" "c2go-multifile.o"
// PLAIN-NOT: "-emit-obj"
// PLAIN-NOT: c2go-lto

// -flto remains accepted for build-system compatibility and reaches the same
// c2go pre-link boundary. It is not required to make the `.o` bitcode.
// RUN: %clang --target=aarch64-unknown-none-goabi -fc2go -flto -### -c \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=FLTO
//
// FLTO: "-cc1"{{.*}}"-emit-llvm-bc"{{.*}}"-fc2go-lto-prelink"
// FLTO-SAME: "-o" "c2go-multifile.o"
// FLTO-NOT: "-emit-obj"

// Explicit -emit-llvm still emits a .bc by default, but it observes the same
// pre-link phase boundary when used with c2go.
// RUN: %clang --target=aarch64-unknown-none-goabi -fc2go -emit-llvm -### -c \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=EXPLICIT-BC
//
// EXPLICIT-BC: "-cc1"{{.*}}"-emit-llvm-bc"{{.*}}"-fc2go-lto-prelink"
// EXPLICIT-BC-SAME: "-o" "c2go-multifile.bc"

// Non-c2go compilation is unchanged.
// RUN: %clang --target=aarch64-unknown-none-elf -### -c \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=NATIVE
//
// NATIVE: "-cc1"{{.*}}"-emit-obj"

// End-to-end WF2 compile/archive gate: the default `.o` must be parseable as
// bitcode, carry the phase marker, and be accepted by c2go-lto's ar CLI. Debug
// line tables stay available to the escape audit but must not make Plan 9
// codegen enter unsupported DWARF object-section paths.
// RUN: %clang --target=aarch64-unknown-none-goabi -fc2go -fc2go-package=t \
// RUN:   -O2 -gline-tables-only -c %s -o %t.o
// RUN: %clang --target=aarch64-unknown-none-goabi -fc2go -fc2go-package=t \
// RUN:   -O2 -gline-tables-only -c %S/Inputs/c2go-extra.c -o %t.extra.o
// RUN: llvm-dis %t.o -o - | FileCheck %s --check-prefix=PRELINK-IR
// RUN: c2go-lto rcs %t.a %t.o %t.extra.o
// RUN: llvm-ar t %t.a | FileCheck %s --check-prefix=ARCHIVE
//
// PRELINK-IR: !{i32 1, !"c2go.lto.prelink", i32 1}
// PRELINK-IR: !{i32 1, !"c2go.manifest.schema", i32 2}
// PRELINK-IR-NOT: !"c2go.target_go_version"
// ARCHIVE: {{.*}}.s
// ARCHIVE: {{.*}}.c2go-export.json

// Schema-v2 validation snapshots are provenance rather than identity. Objects
// with the same contract epochs may carry different snapshots; c2go-lto takes
// their conservative intersection instead of forcing regeneration.
// RUN: %clang --target=aarch64-unknown-none-goabi -fc2go -fc2go-package=t \
// RUN:   -fc2go-target-go-version=1.25-1.27 -O2 -c %s -o %t.snapshot-a.o
// RUN: %clang --target=aarch64-unknown-none-goabi -fc2go -fc2go-package=t \
// RUN:   -fc2go-target-go-version=1.26-1.28 -O2 -c \
// RUN:   %S/Inputs/c2go-extra.c -o %t.snapshot-b.o
// RUN: c2go-lto %t.snapshot-a.o %t.snapshot-b.o \
// RUN:   --c2go-emit-manifest=%t.snapshot.json
// RUN: grep -q '"min_go_version": "go1.26"' %t.snapshot.json
// RUN: grep -q '"validation_snapshot_max_exclusive": "go1.27"' %t.snapshot.json

void f(void) {}
