//===- C2GoArCli.cpp - WF2 ar-CLI compatibility shim ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go WF2 (#319 M5, #478 split): the ar-CLI compatibility shim, lifted
// verbatim from c2go-lto.cpp:1097-1174. Refactor-only — no semantic change.
//
// The classic `ar` invocation a build system emits is:
//
//   ar rcs libfoo.a a.o b.o c.o
//
// Recognised operation strings (a contiguous run of ar mode chars at argv[1])
// are r/c/s/u/q/D/v/S/o/T/P plus an optional leading `-`. When the first
// non-option arg matches this shape we:
//   * take argv[2] as the archive path and emit it via --c2go-emit-archive
//   * shift argv[3..] in as the input bc/ll files
//   * leave any remaining `--foo` flags (after a `--` or interleaved) alone
//
// We deliberately ignore the per-letter semantics (modes like `D` for
// deterministic, `v` for verbose) — c2go-lto's archive is always
// deterministic and we never operate on an existing archive, so `rcs` /
// `rc` / `cr` / `crs` / `rcsD` all collapse to the same behaviour.
//
// Inputs are still expected to be LLVM bitcode. In the default WF2 workflow,
// `c2go-clang -fc2go -c` writes that bitcode with a conventional .o suffix, so
// a build system can pair `CC=c2go-clang` with `AR=c2go-lto`. This is the same
// contract the existing --c2go-emit-archive flag has; the shim is pure CLI
// surface.
//
//===----------------------------------------------------------------------===//

#include "C2GoArCli.h"

namespace llvm {
namespace c2go {

bool isArOperation(StringRef S) {
  if (S.empty())
    return false;
  if (S[0] == '-')
    S = S.drop_front();
  if (S.empty())
    return false;
  for (char C : S) {
    switch (C) {
    case 'r': case 'c': case 's': case 'u': case 'q':
    case 'D': case 'U': case 'v': case 'S': case 'o':
    case 'T': case 'P': case 'a': case 'b': case 'i':
    case 'N':
      continue;
    default:
      return false;
    }
  }
  // Require at least one of r/c/q — the operations that *write* members.
  // `s` alone (regen symtab) and `t`/`x`/`d` (list/extract/delete) are
  // legitimate ar operations we do not implement.
  return S.contains('r') || S.contains('c') || S.contains('q');
}

bool rewriteArCliInPlace(int &argc, const char **&argv,
                         std::vector<std::string> &Storage,
                         std::vector<const char *> &Pointers) {
  if (argc < 3)
    return false;
  if (!isArOperation(argv[1]))
    return false;
  std::string Archive = argv[2];
  Storage.clear();
  Pointers.clear();
  Storage.reserve(argc);
  Storage.push_back(argv[0]);
  Storage.push_back("--c2go-emit-archive=" + Archive);
  for (int I = 3; I < argc; ++I)
    Storage.push_back(argv[I]);
  Pointers.reserve(Storage.size());
  for (auto &S : Storage)
    Pointers.push_back(S.c_str());
  argv = Pointers.data();
  argc = (int)Pointers.size();
  return true;
}

} // namespace c2go
} // namespace llvm
