//===--- C2Go.cpp - c2go-lto bitcode-linker tool ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "C2Go.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

using namespace clang::driver;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

void c2go::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                const InputInfo &Output,
                                const InputInfoList &Inputs,
                                const ArgList &Args,
                                const char *LinkingOutput) const {
  std::string Exec = getToolChain().GetProgramPath(getShortName());
  ArgStringList CmdArgs;

  // The per-TU bitcodes to link.
  for (const auto &II : Inputs)
    if (II.isFilename())
      CmdArgs.push_back(II.getFilename());

  // Inside the clang driver a stack->heap escape is a diagnostic, not a build
  // failure: the legacy cc1-direct -fc2go emit ran no escape audit, so a hard
  // exit-1 here would be a new, surprising error on existing inputs (e.g.
  // SQLite's sqlite3VdbeExec carries one accepted escape). Standalone build-
  // gating callers (the WF2 archive build) keep the default exit-1 contract.
  CmdArgs.push_back("--c2go-escape-nonfatal");

  // The late GC pipeline now runs in this process for driver-routed pre-link
  // bitcode. Preserve the shared c2go emergency switch that cc1 also saw. Do
  // not forward arbitrary -mllvm options: c2go-lto intentionally exposes a
  // smaller command-line surface than clang's backend.
  for (const Arg *A : Args.filtered(options::OPT_mllvm)) {
    llvm::StringRef V = A->getValue(0);
    if (V.starts_with("-c2go-disable="))
      CmdArgs.push_back(Args.MakeArgString(V));
  }

  // Forward the c2go output paths. c2go-lto writes the merged Plan 9 .s and GC
  // manifest to these, exactly as cc1 does for a single-file -fc2go compile.
  if (Arg *A = Args.getLastArg(options::OPT_fc2go_emit_plan9_asm_EQ))
    CmdArgs.push_back(
        Args.MakeArgString(llvm::Twine("--c2go-emit-asm=") + A->getValue()));
  if (Arg *A = Args.getLastArg(options::OPT_fc2go_emit_manifest_EQ))
    CmdArgs.push_back(Args.MakeArgString(llvm::Twine("--c2go-emit-manifest=") +
                                         A->getValue()));

  C.addCommand(std::make_unique<Command>(JA, *this, ResponseFileSupport::None(),
                                         Args.MakeArgString(Exec), CmdArgs,
                                         Inputs, Output));
}
