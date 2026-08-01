// A c2go_linkname target whose import-path prefix exactly matches
// -fc2go-package is a current-package symbol. Strip the path before IR and
// Plan 9 emission; this also makes a hyphen in the current import path harmless.
// Cross-package targets keep the existing direct/bridge behavior.
//
// REQUIRES: aarch64-registered-target
// REQUIRES: x86-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-package=github.com/acme/my-pkg -disable-llvm-passes \
// RUN:   -emit-llvm -o - %s | FileCheck %s --check-prefix=IR
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-package=github.com/acme/my-pkg \
// RUN:   -fc2go-emit-plan9-asm=%t.arm64.s -fc2go-emit-manifest=%t.json \
// RUN:   -emit-obj -o %t.arm64.o %s
// RUN: FileCheck %s --check-prefix=ARM64 --input-file=%t.arm64.s
// RUN: FileCheck %s --check-prefix=JSON --input-file=%t.json
// RUN: %clang_cc1 -triple x86_64-unknown-none-goabi -fc2go -std=c2go23 -O0 \
// RUN:   -fc2go-package=github.com/acme/my-pkg \
// RUN:   -fc2go-emit-plan9-asm=%t.amd64.s -emit-obj -o %t.amd64.o %s
// RUN: FileCheck %s --check-prefix=AMD64 --input-file=%t.amd64.s

extern int same_abi0(int)
    __attribute__((c2go_linkname("github.com/acme/my-pkg.abi0Target", 1)));
extern int same_internal(int)
    __attribute__((c2go_linkname("github.com/acme/my-pkg.InternalTarget")));
extern int same_var
    __attribute__((c2go_linkname("github.com/acme/my-pkg.sameVar")));

extern int cross_abi0(int)
    __attribute__((c2go_linkname("github.com/acme/other.crossTarget", 1)));
extern int cross_internal(int)
    __attribute__((c2go_linkname("github.com/acme/other.InternalCross")));

int use(int x) {
  return same_abi0(x) + same_internal(x) + same_var + cross_abi0(x) +
         cross_internal(x);
}

// Same-package declarations use their target suffix as the LLVM symbol, not
// the C spelling and not the full Go import path.
// IR: @sameVar = external global i32
// IR: call goabi0cc i32 @abi0Target(
// IR: call goabi0cc i32 @InternalTarget(
// IR: call goabi0cc i32 @"github.com/acme/other.crossTarget"(
// IR: call goabi0cc i32 @github_com_acme_other_InternalCross(
// IR: declare goabi0cc i32 @abi0Target(i32
// IR: declare goabi0cc i32 @InternalTarget(i32

// ARM64: CALL ·abi0Target(SB)
// ARM64: CALL ·InternalTarget(SB)
// ARM64: MOVD $·sameVar(SB), {{R[0-9]+}}
// ARM64: CALL github·com∕acme∕other·crossTarget(SB)
// ARM64: CALL ·github_com_acme_other_InternalCross(SB)

// AMD64: CALL ·abi0Target(SB)
// AMD64: CALL ·InternalTarget(SB)
// AMD64: LEAQ ·sameVar(SB), {{[A-Z][A-Z0-9]*}}
// AMD64: CALL github·com∕acme∕other·crossTarget(SB)
// AMD64: CALL ·github_com_acme_other_InternalCross(SB)

// Only the cross-package ABIInternal function needs a bind bridge.
// JSON: "linknames": [
// JSON-COUNT-1: "asm_symbol": "·github_com_acme_other_InternalCross"
// JSON: "linkname": "github.com/acme/other.InternalCross"
// JSON-NOT: github.com/acme/my-pkg.InternalTarget
// JSON: "pkgpath": "github.com/acme/my-pkg"
