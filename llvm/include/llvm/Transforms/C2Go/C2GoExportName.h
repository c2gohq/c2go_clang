//===- C2GoExportName.h - Shared C-name → Go-export-name helpers *- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #444 (track B3 round 2 follow-up): shared spelling helpers used to
// translate a C symbol into the Go-side export identity (capitalisation +
// #317 init/main symbol rename). Pre-#444 three character-for-character
// copies lived in:
//   * clang/lib/CodeGen/CodeGenAction.cpp (WF1 buildC2GoManifest)
//   * llvm/tools/c2go-lto/c2go-lto.cpp    (WF2 manifest rebuild)
//   * clang/lib/CodeGen/CodeGenModule.cpp (the init/main rename emitter
//                                          — single rename site)
// Keeping them in lock-step by hand was a foot-gun: one site silently
// diverging from another flips the link-base CName used to derive
// `needs_linkname`, which in turn breaks the `//go:linkname` stitch
// c2gobind emits for an exported snake_case C symbol.
//
// The bodies are pure string ops with no IR dependency, so the helpers
// stay header-only (mirroring the `walkPointerFields` precedent in
// C2GoGCMaskUtils.h).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOEXPORTNAME_H
#define LLVM_TRANSFORMS_C2GO_C2GOEXPORTNAME_H

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace llvm {
namespace c2go {

/// c2go (#269): capitalize a snake_case C symbol into an exported Go
/// CamelCase name (`sqlite3_open` → `Sqlite3Open`). Must stay byte-for-byte
/// identical to c2gobind's `capitalizeUnderscore` so the manifest's
/// `go_name` matches the name c2gobind would have derived.
inline std::string c2goCapitalizeUnderscore(StringRef Name) {
  std::string Out;
  for (StringRef Part : llvm::split(Name, '_')) {
    if (Part.empty())
      continue;
    Out += llvm::toUpper(Part[0]);
    Out += Part.drop_front();
  }
  return Out;
}

/// c2go (#269): compute the generated Go function/var name for a
/// c2go_extern symbol given the attribute's Export arg. \p ExportCase==1
/// (default) → exported/upper-first via c2goCapitalizeUnderscore;
/// \p ExportCase==0 → keep the C symbol's original casing verbatim.
inline std::string c2goExportGoName(StringRef CName, int ExportCase) {
  if (ExportCase == 0)
    return CName.str();
  return c2goCapitalizeUnderscore(CName);
}

/// c2go (#317): the emitted (Plan 9 / IR-level) symbol of a C function
/// named `init`/`main` is UNCONDITIONALLY renamed (see
/// CodeGenModule::c2goInitMainRename) so it never enters Go's
/// language-special symbol space. Returns the renamed bare symbol (no
/// `·` prefix) or an empty StringRef when \p CName is neither.
inline StringRef c2goInitMainRenamedSymbol(StringRef CName) {
  if (CName == "init")
    return "c2go_cinit";
  if (CName == "main")
    return "c2go_cmain";
  return {};
}

} // namespace c2go
} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOEXPORTNAME_H
