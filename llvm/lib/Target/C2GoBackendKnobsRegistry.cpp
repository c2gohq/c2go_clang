//===- C2GoBackendKnobsRegistry.cpp - per-arch BackendConfig dispatcher ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #435: lib/Target lives between the backend-private TM subclasses (e.g.
// AArch64TargetMachine) and the c2go front-end callers (clang/BackendUtil +
// c2go-lto). Neither side can include the other directly; this registry is
// the only shared seam where a per-arch `applyC2GoConfig` function pointer
// can be looked up by Triple::ArchType.
//
// Implementation is the smallest viable map. There is at most one entry per
// arch and write-after-write semantics are required (LLVMInitialize<Target>
// is idempotent and reruns in unit tests). A DenseMap keyed on Triple::ArchType
// suffices; no thread synchronisation is needed because every caller runs
// `LLVMInitialize<Target>Target()` from `InitLLVM` (single-thread startup) and
// the dispatch happens during PB::createTargetMachine, also single-thread.
//
//===----------------------------------------------------------------------===//

#include "llvm/Target/C2GoBackendKnobs.h"

#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"

#include <map>

using namespace llvm;
using namespace llvm::c2go;

namespace {

// Process-wide registry; one slot per Triple::ArchType.
// std::map (not DenseMap) because Triple::ArchType is a contiguous enum
// whose `tombstone` sentinel would need a custom DenseMapInfo — overkill
// for a 3-entry map updated once at LLVMInitialize* time.
std::map<Triple::ArchType, ApplyC2GoConfigFn> &getRegistry() {
  static std::map<Triple::ArchType, ApplyC2GoConfigFn> R;
  return R;
}

} // namespace

void llvm::c2go::registerC2GoBackendConfigHook(Triple::ArchType Arch,
                                               ApplyC2GoConfigFn Fn) {
  // Overwrite-on-rebind matches the idempotent LLVMInitialize* contract.
  getRegistry()[Arch] = Fn;
}

void llvm::c2go::applyC2GoBackendConfig(TargetMachine *TM,
                                        const BackendConfig &Cfg) {
  if (!TM)
    return;
  auto &R = getRegistry();
  auto It = R.find(TM->getTargetTriple().getArch());
  if (It == R.end() || !It->second)
    return; // No applier for this arch — c2go non-AArch64 builds today.
  It->second(TM, Cfg);
}
