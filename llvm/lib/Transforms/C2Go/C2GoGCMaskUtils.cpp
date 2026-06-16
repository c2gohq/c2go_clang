//===- C2GoGCMaskUtils.cpp - Shared module_gcmask manifest helper ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #401(b): shared implementation of `collectGCMaskVarsFromModule`. Lifted
// verbatim from the two pre-existing character-for-character mirrors in
// `clang/lib/CodeGen/CodeGenAction.cpp::collectC2GoModuleGCMaskVars` and
// `llvm/tools/c2go-lto/c2go-lto.cpp::collectC2GoModuleGCMaskVars`. Includes
// the #387 §B4 phase 6 sub-step 1 `go_owned` surfacing and #389 WF2 mirror.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoGCMaskUtils.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"

using namespace llvm;

namespace llvm {
namespace c2go {

json::Array collectGCMaskVarsFromModule(const Module &M) {
  json::Array Vars;
  // #465: spelling lives in C2GoProtocol.h (kGlobalGcmaskPrefix).
  StringRef kPrefix = kGlobalGcmaskPrefix;

  // c2go #387 §B4 phase 6 sub-step 1 (#389 mirror in WF2): pre-collect
  // the set of var names marked Go-owned in `!c2go.go_owned_globals`.
  // CGC2GoTypeInfo emits this named-metadata for any single-pointer-word
  // global so the generated Go package can take over storage ownership
  // (Go-owned bodyless `var X unsafe.Pointer`, registered automatically
  // into moduledata.gcdata).
  StringSet<> GoOwned;
  if (const NamedMDNode *NMD = M.getNamedMetadata(kGoOwnedGlobalsMDName)) {
    for (const MDNode *Op : NMD->operands()) {
      if (!Op || Op->getNumOperands() == 0)
        continue;
      if (const auto *MS = dyn_cast<MDString>(Op->getOperand(0)))
        GoOwned.insert(MS->getString());
    }
  }

  // Deterministic order: walk the module's named-global list, then sort
  // by var name once we have all the entries.
  for (const GlobalVariable &GV : M.globals()) {
    StringRef Name = GV.getName();
    if (!Name.starts_with(kPrefix))
      continue;
    if (!GV.hasInitializer())
      continue;
    const Constant *Init = GV.getInitializer();
    StringRef VarName = Name.drop_front(kPrefix.size());

    // The initialiser is either a ConstantDataArray (byte-array, common
    // case for non-zero masks) or a ConstantAggregateZero (every byte
    // zero). Both expose getNumElements / element access, but
    // ConstantDataArray is the precise path; for safety we handle the
    // rare ConstantArray fallback too.
    SmallString<32> HexBytes;
    uint64_t BitCount = 0;
    static const char *Hex = "0123456789abcdef";
    if (const auto *CDA = dyn_cast<ConstantDataArray>(Init)) {
      uint64_t N = CDA->getNumElements();
      HexBytes.reserve(N * 2);
      for (uint64_t I = 0; I < N; ++I) {
        uint8_t B = (uint8_t)CDA->getElementAsInteger(I);
        for (int Bit = 0; Bit < 8; ++Bit)
          if (B & (1u << Bit))
            ++BitCount;
        HexBytes.push_back(Hex[(B >> 4) & 0xF]);
        HexBytes.push_back(Hex[B & 0xF]);
      }
    } else if (const auto *CAZ = dyn_cast<ConstantAggregateZero>(Init)) {
      // All-zero mask: still emit so the consumer can see the var is
      // tracked but has no managed words (rare — the §B4 emitter already
      // skips empty bitmaps, but be defensive).
      if (auto *AT = dyn_cast<ArrayType>(CAZ->getType())) {
        uint64_t N = AT->getNumElements();
        HexBytes.reserve(N * 2);
        for (uint64_t I = 0; I < N; ++I) {
          HexBytes.push_back('0');
          HexBytes.push_back('0');
        }
      }
    } else if (const auto *CA = dyn_cast<ConstantArray>(Init)) {
      uint64_t N = CA->getNumOperands();
      HexBytes.reserve(N * 2);
      for (uint64_t I = 0; I < N; ++I) {
        if (const auto *CI = dyn_cast<ConstantInt>(CA->getOperand(I))) {
          uint8_t B = (uint8_t)CI->getZExtValue();
          for (int Bit = 0; Bit < 8; ++Bit)
            if (B & (1u << Bit))
              ++BitCount;
          HexBytes.push_back(Hex[(B >> 4) & 0xF]);
          HexBytes.push_back(Hex[B & 0xF]);
        }
      }
    } else {
      // Unknown initialiser shape — skip rather than half-emit. This is
      // a defensive path; CGC2GoTypeInfo only ever builds
      // ConstantDataArray-shaped masks.
      continue;
    }
    if (HexBytes.empty())
      continue;
    json::Object V;
    V["name"] = VarName.str();
    V["mask_hex"] = HexBytes.str().str();
    V["ptr_bits"] = (int64_t)BitCount;
    // c2go #387 §B4 phase 6 sub-step 1 (#389 WF2 mirror): surface the
    // Go-owned bit so c2gobind can emit a bodyless `var <name>
    // unsafe.Pointer` declaration in the generated Go package (Go
    // compiler then owns the storage and the runtime scans it as a
    // root).
    if (GoOwned.contains(VarName))
      V["go_owned"] = true;
    Vars.push_back(std::move(V));
  }
  // Deterministic order: sort by var name. Mirrors the Symbols/Types sort
  // done at the end of buildC2GoManifest so consumers see a stable
  // manifest regardless of IR walk order.
  llvm::sort(Vars, [](const json::Value &A, const json::Value &B) {
    const auto *AO = A.getAsObject();
    const auto *BO = B.getAsObject();
    StringRef AN = AO ? AO->getString("name").value_or("") : "";
    StringRef BN = BO ? BO->getString("name").value_or("") : "";
    return AN < BN;
  });
  return Vars;
}

} // namespace c2go
} // namespace llvm
