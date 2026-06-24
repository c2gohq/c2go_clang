//===--- C2Go.h - c2go-lto bitcode-linker tool ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_C2GO_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_C2GO_H

#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"

namespace clang {
namespace driver {
namespace tools {
namespace c2go {
// Runs c2go-lto over the per-TU bitcodes of a multi-file `-fc2go` compile,
// emitting one merged Plan 9 .s + GC manifest. This is the internal WF2 path:
// each .c is compiled to bitcode (carrying its embedded manifest) and c2go-lto
// links them. Mirrors tools::ifstool::Merger (a combine-via-tool final step).
class LLVM_LIBRARY_VISIBILITY Linker : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("c2go::Linker", "c2go-lto", TC) {}

  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return false; }

  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};
} // end namespace c2go
} // end namespace tools
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_C2GO_H
