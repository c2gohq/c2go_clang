//===- C2GoEmergencyFlag.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/C2GoEmergencyFlag.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<std::string> ClC2GoDisable(
    "c2go-disable", cl::init(""), cl::Hidden,
    cl::desc("c2go #377 emergency-off list (comma-separated). Tokens: "
             "statepoint-gc, ptrslot-liveness, spill-tags, leaf-abi. "
             "All subsystems default ON; this flag exists only so an "
             "operator can disable one without a clang rebuild if a "
             "regression is reported."));

bool llvm::c2go::isC2GoDisabled(StringRef Subsystem) {
  StringRef List(ClC2GoDisable);
  while (!List.empty()) {
    StringRef Tok;
    std::tie(Tok, List) = List.split(',');
    if (Tok.trim() == Subsystem)
      return true;
  }
  return false;
}
