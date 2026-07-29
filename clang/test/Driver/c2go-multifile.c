// A `-fc2go` compile that requests the Plan 9 .s / manifest outputs is routed
// internally through c2go-lto (the WF2 bitcode linker) — the single internal
// path. Each TU is compiled to bitcode and c2go-lto links them, emitting the
// .s + manifest. This holds for several inputs (one merged .s/manifest) and for
// a single input alike. A `-fc2go` compile WITHOUT the emit flags keeps the
// ordinary object pipeline (no c2go-lto job).

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

// Without the emit flags the ordinary object pipeline is used.
// RUN: %clang --target=aarch64-unknown-none-goabi -fc2go -### -c \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=PLAIN
//
// PLAIN-NOT: c2go-lto

void f(void) {}
