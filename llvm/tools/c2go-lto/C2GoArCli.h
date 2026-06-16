//===- C2GoArCli.h - WF2 ar-CLI compatibility shim ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go WF2 (#319 M5, #478 split): translate a Unix-`ar`-style CLI into
// c2go-lto's native flag set so build systems can drop in `AR=c2go-lto`
// without changes. Lifted verbatim from c2go-lto.cpp:1097-1174.
// Refactor-only — no semantic change.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_C2GO_LTO_C2GO_AR_CLI_H
#define LLVM_TOOLS_C2GO_LTO_C2GO_AR_CLI_H

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace llvm {
namespace c2go {

// Returns true when `S` is a recognised `ar` operation string — a contiguous
// run of mode chars (r/c/s/u/q/D/U/v/S/o/T/P/a/b/i/N), optionally prefixed
// with `-`, with at least one of r/c/q present. `s` alone (regen symtab) and
// t/x/d (list/extract/delete) are deliberately rejected — we do not implement
// those, only the write-archive shape `ar rcs <archive> <inputs…>`.
bool isArOperation(StringRef S);

// Rewrite (argc, argv) in place when argv[1] is an ar operation string.
// The rewritten arg list looks like:
//   argv[0]  argv[0]
//   argv[1]  --c2go-emit-archive=<archive>
//   argv[2]  <input1>
//   argv[3]  <input2>
//   ...
// Backing storage lives in `Storage` / `Pointers` (kept alive by the caller
// for the rest of main()). Returns true on rewrite, false on no-op.
bool rewriteArCliInPlace(int &argc, const char **&argv,
                         std::vector<std::string> &Storage,
                         std::vector<const char *> &Pointers);

} // namespace c2go
} // namespace llvm

#endif // LLVM_TOOLS_C2GO_LTO_C2GO_AR_CLI_H
