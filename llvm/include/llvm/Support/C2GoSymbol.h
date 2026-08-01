//===- C2GoSymbol.h - Shared c2go Go-symbol helpers -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Small, layer-neutral helpers for resolving a c2go_linkname target against
// the Go import path of the package currently being compiled.  Clang Sema,
// Clang manifest emission, LLVM's late libcall router, and c2go-lto must make
// this decision identically: a target in the current package is represented by
// its package-local suffix, while a target in another package keeps its full Go
// linker name.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_C2GOSYMBOL_H
#define LLVM_SUPPORT_C2GOSYMBOL_H

#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>

namespace llvm {
namespace c2go {

inline bool isC2GoSymbolIdentByte(char C) {
  return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
         (C >= '0' && C <= '9') || C == '_';
}

/// If \p Target names a symbol in \p PackagePath, return the symbol suffix
/// following the exact "<package>." prefix.  Import paths may themselves
/// contain dots, so this deliberately does not split at the last dot.
///
/// A slash in the suffix means the apparent prefix was only the beginning of a
/// longer import path, not a symbol in the current package.
inline std::optional<StringRef>
getC2GoSamePackageSymbol(StringRef Target, StringRef PackagePath) {
  if (PackagePath.empty())
    PackagePath = "main";
  if (!Target.consume_front(PackagePath) || !Target.consume_front(".") ||
      Target.empty() || Target.contains('/'))
    return std::nullopt;
  return Target;
}

/// Whether a full Go linker target can be represented directly by the c2go
/// Plan 9 spelling (`.` -> middle dot, `/` -> division slash).
inline bool isC2GoPlan9PathSymbol(StringRef Name) {
  for (char C : Name)
    if (!(isC2GoSymbolIdentByte(C) || C == '/' || C == '.'))
      return false;
  return true;
}

/// Whether a current-package suffix can be emitted as `·name(SB)`.  Dots and
/// slashes are path separators in Plan 9 syntax, not bytes of a local suffix.
inline bool isC2GoPlan9LocalSymbol(StringRef Name) {
  if (Name.empty())
    return false;
  for (char C : Name)
    if (!isC2GoSymbolIdentByte(C))
      return false;
  return true;
}

inline std::string sanitiseC2GoSymbolToIdent(StringRef Name) {
  std::string Result = Name.str();
  for (char &C : Result)
    if (!isC2GoSymbolIdentByte(C))
      C = '_';
  return Result;
}

} // namespace c2go
} // namespace llvm

#endif // LLVM_SUPPORT_C2GOSYMBOL_H
