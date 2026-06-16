//===- C2GoEscapeAudit.h - WF2 stack->heap escape audit -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #289 client B (#478 split): whole-program Andersen-lite stack-address
// escape audit, lifted verbatim from c2go-lto.cpp:234-781. Refactor-only — no
// semantic change relative to the prior single-TU shape. See C2GoEscapeAudit.cpp
// for the per-phase commentary (cell seeding / constraint construction /
// worklist propagation / escape reporting) that used to live inline in
// c2go-lto.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_C2GO_LTO_C2GO_ESCAPE_AUDIT_H
#define LLVM_TOOLS_C2GO_LTO_C2GO_ESCAPE_AUDIT_H

namespace llvm {
class Module;
class raw_ostream;
namespace c2go {

// Run the Andersen-lite stack-address escape audit on `M`. Diagnostics
// (`c2go-lto: stack->heap in <fn> at <loc>` lines + the final
// `c2go-lto: <N> stack-address escape point(s)` summary) are written to
// `Out`. Returns true when the audit found zero escapes (i.e. the build may
// proceed); false when at least one stack-to-heap escape was reported (the
// caller maps this to a non-zero exit code so a build can gate on it).
//
// `--c2go-print-stats` solver statistics still go to errs() — they are
// debug-only and intentionally untouched by the split.
bool runAndersenEscapeAudit(Module &M, raw_ostream &Out);

} // namespace c2go
} // namespace llvm

#endif // LLVM_TOOLS_C2GO_LTO_C2GO_ESCAPE_AUDIT_H
