//===- C2GoManifestRebuilder.h - WF2 manifest rebuild from combined bc ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go WF2 (#319 M4 + M5, #465 split): rebuild the c2go manifest JSON from
// the combined LLVM bitcode's c2go.* module flags, per-function "c2go-..."
// string attrs and NamedMD, then drop declare-only c2go-boundary functions
// that were only kept alive by llvm.compiler.used (Q1 cleanup). Extracted
// from c2go-lto.cpp:1514-2008 in #465 to split the 2k-line tool TU into
// per-protocol slices (manifest / codegen / ar shim / CLI).
//
// Refactor-only: no semantic change. The exact pkgpath / symbols[] /
// linknames[] / types[] / module_gcmask shaping rules are preserved
// byte-identically — see C2GoManifestRebuilder.cpp for the per-field
// commentary that used to live inline in c2go-lto.cpp.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_C2GO_LTO_C2GO_MANIFEST_REBUILDER_H
#define LLVM_TOOLS_C2GO_LTO_C2GO_MANIFEST_REBUILDER_H

#include "llvm/ADT/StringRef.h"
#include <string>

namespace llvm {
class Module;
namespace c2go {

// Exit status mirrored from c2go-lto.cpp's ExitCode enum (kept narrow — the
// rebuilder only ever needs OK / ToolError; the broader ExitAttrConflict /
// ExitEscapesFound codes stay in main()).
enum class ManifestRebuildStatus {
  OK = 0,
  ToolError = 2,
};

// Rebuild the c2go manifest JSON from `Composite` when `Build` is true:
//
//   * render into `ManifestText` (in-memory; caller fans out to the archive
//     member when `--c2go-emit-archive` is set, so file + archive carry
//     byte-identical JSON without re-rendering)
//   * when `EmitManifestPath` is non-empty, write `ManifestText` to that
//     file (OF_Text)
//   * always (Q1 cleanup): drop declare-only c2go-boundary functions that
//     were only kept alive by `llvm.compiler.used` — they have no further
//     use in the combined bitcode and would otherwise persist as dead
//     declares. Done only after the manifest builder has snapshotted the
//     boundaries so the reader still sees them.
//
// When `Build` is false the function is a no-op (returns OK and leaves
// `ManifestText` untouched / no cleanup).
//
// `ProgName` is used as the diagnostic prefix (mirroring `argv[0]`).
ManifestRebuildStatus
rebuildManifestFromIR(Module &Composite, bool Build,
                      StringRef EmitManifestPath, StringRef ProgName,
                      std::string &ManifestText);

} // namespace c2go
} // namespace llvm

#endif // LLVM_TOOLS_C2GO_LTO_C2GO_MANIFEST_REBUILDER_H
