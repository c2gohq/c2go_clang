//===- C2GoManifestRebuilder.cpp - WF2 manifest rebuild from combined bc --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go WF2 (#319 M4 + M5, #465 split): implementation of the manifest
// rebuilder + Q1 declare-only c2go-boundary cleanup, lifted verbatim from
// c2go-lto.cpp:1514-2008. Refactor-only — no semantic change relative to
// the prior single-TU shape.
//
//===----------------------------------------------------------------------===//

#include "C2GoManifestRebuilder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/C2GoSymbol.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/C2Go/C2GoExportName.h"
#include "llvm/Transforms/C2Go/C2GoGCMaskUtils.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <algorithm>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

using namespace llvm;

namespace llvm {
namespace c2go {

// c2go (WF2 unification): pull the per-TU manifest JSON that clang embedded as
// `c2go.manifest.json` operands and merge them into one manifest object. Returns
// nullopt for pre-embed bitcode carrying no such metadata (caller then falls back
// to the legacy per-field reconstruction). The embedded JSON is clang's
// buildC2GoManifest output verbatim, so the WF2 manifest equals WF1's by
// construction — the only WF2-specific work is this cross-TU merge (the LTO step:
// one operand per linked TU after llvm-link appends them).
static std::optional<json::Object>
extractEmbeddedC2GoManifest(Module &Composite) {
  NamedMDNode *NMD = Composite.getNamedMetadata("c2go.manifest.json");
  if (!NMD)
    return std::nullopt;
  SmallVector<json::Object, 4> Ms;
  for (const MDNode *Op : NMD->operands()) {
    if (Op->getNumOperands() != 1)
      continue;
    auto *S = dyn_cast<MDString>(Op->getOperand(0));
    if (!S)
      continue;
    Expected<json::Value> V = json::parse(S->getString());
    if (!V) {
      consumeError(V.takeError());
      continue;
    }
    if (json::Object *O = V->getAsObject())
      Ms.push_back(std::move(*O));
  }
  if (Ms.empty())
    return std::nullopt;
  if (Ms.size() == 1)
    return std::move(Ms.front());

  // Multiple linked TUs: keep the module-scalar fields (pkgpath, versions,
  // goos/goarch) from the first TU; union each per-entity array — concatenate
  // across TUs, de-duplicate by "name", then re-sort by "name" to match the
  // per-section ordering WF1 emits.
  auto nameOf = [](const json::Value &V) -> StringRef {
    if (const json::Object *O = V.getAsObject())
      if (std::optional<StringRef> N = O->getString("name"))
        return *N;
    return StringRef();
  };
  auto byName = [&](const json::Value &A, const json::Value &B) {
    return nameOf(A) < nameOf(B);
  };
  auto mergeArray = [&](StringRef Key) -> json::Array {
    json::Array Acc;
    StringSet<> Seen;
    for (json::Object &M : Ms)
      if (json::Array *A = M.getArray(Key))
        for (json::Value &E : *A) {
          StringRef N = nameOf(E);
          if (!N.empty() && !Seen.insert(N).second)
            continue;
          Acc.push_back(E); // copy: avoids cross-key move-order hazards
        }
    llvm::stable_sort(Acc, byName);
    return Acc;
  };
  // module_gcmask is `{ "vars": [ ... ] }`; its vars[] is itself a per-entity
  // array, unioned the same way.
  json::Array GCVars;
  {
    StringSet<> Seen;
    for (json::Object &M : Ms)
      if (json::Object *GC = M.getObject("module_gcmask"))
        if (json::Array *A = GC->getArray("vars"))
          for (json::Value &E : *A) {
            StringRef N = nameOf(E);
            if (!N.empty() && !Seen.insert(N).second)
              continue;
            GCVars.push_back(E);
          }
    llvm::stable_sort(GCVars, byName);
  }
  json::Array MS = mergeArray("symbols"), ML = mergeArray("linknames"),
              MC = mergeArray("callbacks"), MT = mergeArray("types");
  json::Object Out(std::move(Ms.front())); // scalar fields from the first TU
  // Match WF1's section presence exactly (CodeGenAction::buildC2GoManifest):
  // symbols / linknames / types are emitted unconditionally (empty [] included);
  // callbacks only when non-empty.
  Out["symbols"] = std::move(MS);
  Out["linknames"] = std::move(ML);
  Out["types"] = std::move(MT);
  if (MC.empty())
    Out.erase("callbacks");
  else
    Out["callbacks"] = std::move(MC);
  json::Object GC;
  GC["vars"] = std::move(GCVars);
  Out["module_gcmask"] = std::move(GC);
  return Out;
}

ManifestRebuildStatus
rebuildManifestFromIR(Module &Composite, bool Build,
                      StringRef EmitManifestPath, StringRef ProgName,
                      std::string &ManifestText) {
  if (!Build)
    return ManifestRebuildStatus::OK;

  // c2go (WF2 unification): when clang embedded the manifest(s) in the bitcode,
  // they ARE the manifest — merge and emit them, skipping the legacy per-field
  // reconstruction (which silently drifted from WF1: cabi/callbacks/var-linknames/
  // imported were all lost). Falls through to reconstruction only for pre-embed
  // bitcode (Embedded == nullopt).
  std::optional<json::Object> Embedded = extractEmbeddedC2GoManifest(Composite);

  // c2go WF2 (#319, M4 minimal): optional manifest rebuild from combined
  // bitcode. Reads c2go.pkgpath / c2go.target_go_version module flags and the
  // per-function "c2go-c-name" / "c2go-go-sig" / "c2go-boundary" attrs that
  // CodeGenModule::SetLLVMFunctionAttributes stamps. The kind is decided from
  // the function's body presence: declare-only c2go_extern becomes
  // unmanaged_extern (the side-channel for declare-only unmanaged externs);
  // defined boundaries become func. This is enough to compare pkgpath +
  // symbols.name + go_sig against the AST-built reference; the full set of
  // manifest fields (types, globals, linknames, etc.) is left to follow-ups.
  //
  // M5: the manifest body is built into an in-memory string buffer first,
  // then routed to (a) the --c2go-emit-manifest file and/or (b) the
  // --c2go-emit-archive member, so file and archive carry byte-identical
  // JSON without re-rendering.
  json::Object Root;

  auto getStringFlag = [&](StringRef Name) -> std::string {
    if (auto *MD = Composite.getModuleFlag(Name))
      if (auto *S = dyn_cast<MDString>(MD))
        return S->getString().str();
    return "";
  };
  std::string PkgPath = getStringFlag(c2go::kPackagePathModuleFlag);
  if (PkgPath.empty())
    PkgPath = "main";
  Root["pkgpath"] = PkgPath;

  std::string VerRange = getStringFlag("c2go.target_go_version");
  if (VerRange.empty()) VerRange = "1.22-1.25";
  auto Dash = VerRange.find('-');
  std::string Lo = Dash == std::string::npos ? VerRange : VerRange.substr(0, Dash);
  std::string Hi = Dash == std::string::npos ? VerRange : VerRange.substr(Dash + 1);
  Root["min_go_version"] = "go" + Lo;
  Root["max_go_version"] = "go" + Hi;

  // Mirror CodeGenAction::buildC2GoManifest: record Go's GOOS/GOARCH from the
  // (still-original, pre-neutralization) module triple so the WF2-rebuilt
  // manifest round-trips byte-identically with clang's WF1 manifest (#544).
  Triple TT(Composite.getTargetTriple());
  StringRef GOOS = TT.isOSWindows()  ? "windows"
                   : TT.isOSDarwin() ? "darwin"
                   : TT.isOSLinux()  ? "linux"
                                     : "";
  StringRef GOARCH = TT.getArch() == Triple::x86_64    ? "amd64"
                     : TT.getArch() == Triple::aarch64 ? "arm64"
                                                       : "";
  if (!GOOS.empty())
    Root["goos"] = GOOS;
  if (!GOARCH.empty())
    Root["goarch"] = GOARCH;

  json::Array Symbols;
  SmallVector<json::Object, 32> AllSyms;
  SmallVector<Function *, 32> Boundaries;
  for (Function &F : Composite) {
    if (!F.hasFnAttribute("c2go-boundary"))
      continue;
    Boundaries.push_back(&F);
  }

  // c2go WF2 (#367 Bug A): index `c2go.func.<CName>` NamedMD nodes once
  // before the boundary loop. The AST writer emits a 14-operand MDNode
  // per c2go_extern function carrying every symbols[] field that the
  // JSON path stamps; reading it back keeps round-trip byte-identical.
  // Functions without the MD (or with <14 operands from a future schema
  // shrink) fall back to the per-function string attrs which only cover
  // c-name / go-sig / unmanaged-return — strictly fewer fields than the
  // AST manifest, so older bitcode keeps the prior (lossy) behaviour
  // rather than crashing.
  DenseMap<StringRef, MDNode *> FuncMetaByName;
  {
    StringRef Prefix = c2go::kFuncMDPrefix;
    for (NamedMDNode &NMD : Composite.named_metadata()) {
      StringRef N = NMD.getName();
      if (!N.starts_with(Prefix))
        continue;
      if (NMD.getNumOperands() == 0)
        continue;
      StringRef CName = N.drop_front(Prefix.size());
      FuncMetaByName[CName] = NMD.getOperand(0);
    }
  }

  for (Function *F : Boundaries) {
    json::Object Sym;
    StringRef CName = F->getFnAttribute("c2go-c-name").getValueAsString();
    // c2go #444 (c): the `c2go-c-name` attr is the WF1↔WF2 join key —
    // every boundary the AST writer stamps with `c2go-boundary` also
    // carries this attr. An empty value means the bitcode is malformed
    // (a hand-crafted `.ll` or a corrupted strip step), and silently
    // emitting an entry keyed off `""` would corrupt the symbols[] sort
    // and the c2gobind linkname stitch downstream. Fail loud instead of
    // half-emitting, matching the existing `inliner removed boundary`
    // diagnostic style.
    if (CName.empty()) {
      errs() << ProgName << ": c2go-boundary function '" << F->getName()
             << "' is missing the c2go-c-name attribute (bitcode "
                "malformed or out-of-date)\n";
      return ManifestRebuildStatus::ToolError;
    }

    auto It = FuncMetaByName.find(CName);
    MDNode *Meta = It != FuncMetaByName.end() ? It->second : nullptr;
    if (Meta && Meta->getNumOperands() >= 14) {
      // 14-operand schema: read each slot by index. Helpers narrow the
      // generic Metadata* into the expected MDString / ConstantInt; on
      // shape mismatch we leave the field absent so any malformed MD
      // surfaces as a JSON diff against the AST reference rather than
      // a hard crash.
      auto asStr = [&](unsigned Idx) -> StringRef {
        if (auto *S = dyn_cast<MDString>(Meta->getOperand(Idx)))
          return S->getString();
        return "";
      };
      auto asBool = [&](unsigned Idx, bool &Out) -> bool {
        auto *CAM = dyn_cast<ConstantAsMetadata>(Meta->getOperand(Idx));
        if (!CAM) return false;
        auto *CI = dyn_cast<ConstantInt>(CAM->getValue());
        if (!CI) return false;
        Out = CI->getZExtValue() != 0;
        return true;
      };
      auto asInt = [&](unsigned Idx, int64_t &Out) -> bool {
        auto *CAM = dyn_cast<ConstantAsMetadata>(Meta->getOperand(Idx));
        if (!CAM) return false;
        auto *CI = dyn_cast<ConstantInt>(CAM->getValue());
        if (!CI) return false;
        Out = CI->getSExtValue();
        return true;
      };

      Sym["name"] = asStr(0).str();
      Sym["go_sig"] = asStr(1).str();
      Sym["kind"] = asStr(2).str();
      bool Managed = true;
      if (asBool(3, Managed))
        Sym["managed"] = Managed;
      StringRef GoName = asStr(4);
      if (!GoName.empty())
        Sym["go_name"] = GoName.str();
      StringRef Abi = asStr(5);
      if (!Abi.empty())
        Sym["abi"] = Abi.str();
      StringRef AsmSym = asStr(6);
      if (!AsmSym.empty())
        Sym["asm_symbol"] = AsmSym.str();
      int64_t ArgSize = 0;
      if (asInt(7, ArgSize))
        Sym["argsize"] = ArgSize;
      bool B = false;
      if (asBool(8, B) && B) Sym["needs_linkname"] = true;
      if (asBool(9, B) && B) Sym["is_variadic"] = true;
      if (asBool(10, B) && B) Sym["has_float"] = true;
      if (asBool(11, B) && B) Sym["has_aggregate"] = true;
      if (asBool(12, B) && B) {
        Sym["c_entry"] = true;
        StringRef ESig = asStr(13);
        if (!ESig.empty())
          Sym["entry_sig"] = ESig.str();
      }
    } else {
      // Fallback: per-function attr backstop. Pre-#412 this shape only
      // filled name/go_sig/kind/managed (4 of 14 fields), silently
      // dropping go_name / asm_symbol / argsize / needs_linkname / abi
      // for any bitcode without `c2go.func.<X>` NamedMD (older clang or
      // post-strip pipelines). #412 — Track D self-audit revealed those
      // 5 export-identity fields are recoverable byte-identically from
      // the per-function string attrs CodeGenModule already stamps:
      //   * `c2go-c-name`            → name
      //   * `c2go-go-sig`            → go_sig
      //   * `c2go-export-case`       → go_name (via c2goExportGoName)
      //   * `c2go-boundary-argsize`  → argsize
      // asm_symbol mirrors WF1's "·" + (renamed for init/main, else CName);
      // needs_linkname follows the WF1 rule (go_name != link-base CName).
      Sym["name"] = CName.str();
      Sym["go_sig"] =
          F->getFnAttribute("c2go-go-sig").getValueAsString().str();
      // Kind: declare-only c2go_extern is "unmanaged_extern" (raw_read,
      // c2go_linkname stubs, etc.); defined boundary is "func".
      Sym["kind"] = F->isDeclaration() ? "unmanaged_extern" : "func";
      // Managed world: default true. A c2go_unmanaged function (an import) is
      // unmanaged-world, so c2go-unmanaged-world clears the bit.
      // buildC2GoManifest applies the same rule.
      Sym["managed"] = !F->hasFnAttribute("c2go-unmanaged-world");
      Sym["abi"] = "abi0";
      // ExportCase: presence-checked first so missing attr (very old bc)
      // defaults to Export==1 (the WF1 default), preserving prior fallback
      // behaviour when only c2go-go-sig / c2go-boundary are present.
      int Export = 1;
      if (F->hasFnAttribute("c2go-export-case")) {
        StringRef Raw =
            F->getFnAttribute("c2go-export-case").getValueAsString();
        int Parsed = 0;
        if (!Raw.empty() && !Raw.getAsInteger(10, Parsed))
          Export = Parsed;
      }
      // c2go (#317): C `init`/`main` are renamed at the symbol-emission
      // level. The shared helper c2goInitMainRenamedSymbol returns the
      // renamed bare symbol (or empty for any other name); the .s
      // carries the renamed symbol, so asm_symbol must reflect it.
      StringRef RenamedSym = c2goInitMainRenamedSymbol(CName);
      std::string GoName = c2goExportGoName(CName, Export);
      Sym["go_name"] = GoName;
      StringRef LinkBase = RenamedSym.empty() ? CName : RenamedSym;
      if (GoName != LinkBase)
        Sym["needs_linkname"] = true;
      Sym["asm_symbol"] = "\xc2\xb7" + (RenamedSym.empty()
                                            ? CName.str()
                                            : RenamedSym.str());
      // c2go #444 (a): mirror WF1 CodeGenAction.cpp:448 — surface the
      // variadic bit from the LLVM function type. c2gobind picks the
      // SyscallN-printf wrapper template when this is set on an
      // unmanaged_extern target.
      if (F->isVarArg())
        Sym["is_variadic"] = true;
      // c2go #444 (a): mirror WF1 CodeGenAction.cpp:416-430 — only `main`
      // is the program entry; emit `c_entry`+`entry_sig` so c2gobind
      // generates the argc/argv/envp marshalling wrapper. The fallback
      // path cannot tell void/int from the AST, but the LLVM function
      // arg count is enough to disambiguate the three legal C main
      // shapes (0/2/3 params). >3 params is rejected just as the WF1
      // path emits `"unknown"`.
      if (CName == "main") {
        Sym["c_entry"] = true;
        unsigned NParams = F->arg_size();
        if (NParams == 0)
          Sym["entry_sig"] = "void";
        else if (NParams == 2)
          Sym["entry_sig"] = "argc_argv";
        else if (NParams == 3)
          Sym["entry_sig"] = "argc_argv_envp";
        else
          Sym["entry_sig"] = "unknown";
      }
      // c2go #444 (c): a missing `c2go-boundary-argsize` attr means the
      // attr-writer never ran for this function (old bitcode or a
      // hand-crafted stub). Pre-#444 we silently emitted `argsize: 0`,
      // which downstream c2go-bind cannot distinguish from a true
      // void()-ABI0 frame. Omit the field instead so c2gobind can
      // diagnose "unknown frame size" loudly rather than mis-parse a
      // zero as authoritative.
      StringRef ArgSizeStr =
          F->getFnAttribute("c2go-boundary-argsize").getValueAsString();
      if (!ArgSizeStr.empty()) {
        int Parsed = 0;
        if (!ArgSizeStr.getAsInteger(10, Parsed))
          Sym["argsize"] = (int64_t)Parsed;
      }
    }

    AllSyms.push_back(std::move(Sym));
  }

  // c2go WF2 (#319 C3): rebuild `symbols[] kind=var` entries from the
  // `c2go.var` GV metadata stamped by CodeGenModule::EmitGlobalVarDefinition.
  // Each MDNode is (name MDString, go_type MDString, managed-bit i1).
  for (GlobalVariable &GV : Composite.globals()) {
    MDNode *Var = GV.getMetadata(llvm::c2go::kVarGVMD);
    if (!Var || Var->getNumOperands() < 3)
      continue;
    auto *NameMD = dyn_cast<MDString>(Var->getOperand(0));
    auto *TypeMD = dyn_cast<MDString>(Var->getOperand(1));
    auto *ManagedMD = dyn_cast<ConstantAsMetadata>(Var->getOperand(2));
    if (!NameMD || !TypeMD || !ManagedMD)
      continue;
    auto *ManagedCI = dyn_cast<ConstantInt>(ManagedMD->getValue());
    if (!ManagedCI)
      continue;
    json::Object Sym;
    Sym["name"] = NameMD->getString().str();
    Sym["kind"] = "var";
    Sym["go_type"] = TypeMD->getString().str();
    Sym["managed"] = ManagedCI->getZExtValue() != 0;
    AllSyms.push_back(std::move(Sym));
  }

  // Merged stable order by name across functions + vars so diff vs
  // ref.json (which sorts the unified symbols[] by name) matches.
  std::sort(AllSyms.begin(), AllSyms.end(),
            [](const json::Object &A, const json::Object &B) {
              return A.find("name")->second.getAsString().value_or("") <
                     B.find("name")->second.getAsString().value_or("");
            });
  for (auto &S : AllSyms)
    Symbols.push_back(std::move(S));
  Root["symbols"] = std::move(Symbols);

  // c2go WF2 (#319 C2 follow-up): rebuild the top-level `linknames[]`
  // bridge table from per-function attrs. Mirrors
  // CodeGenAction::buildC2GoManifest: same-package targets use their local
  // suffix, while a cross-package ABIInternal target or a Plan-9-illegal
  // spelling needs c2go-bind's bridge.
  {
    json::Array Linknames;
    for (Function &F : Composite) {
      if (!F.hasFnAttribute("c2go-linkname"))
        continue;
      StringRef Target = F.getFnAttribute("c2go-linkname").getValueAsString();
      std::optional<StringRef> Local =
          c2go::getC2GoSamePackageSymbol(Target, PkgPath);
      StringRef EmittedName = Local ? *Local : Target;
      bool Plan9Direct = Local ? c2go::isC2GoPlan9LocalSymbol(EmittedName)
                               : c2go::isC2GoPlan9PathSymbol(EmittedName);
      bool Direct = Plan9Direct && (Local.has_value() ||
                                    F.hasFnAttribute("c2go-linkname-abi0"));
      if (Target.empty() || Direct)
        continue;
      json::Object Bridge;
      Bridge["name"] = F.getFnAttribute("c2go-c-name").getValueAsString().str();
      Bridge["kind"] = "func";
      Bridge["go_sig"] =
          F.getFnAttribute("c2go-go-sig").getValueAsString().str();
      Bridge["linkname"] = Target.str();
      Bridge["asm_symbol"] =
          "\xc2\xb7" + c2go::sanitiseC2GoSymbolToIdent(Target);
      Linknames.push_back(std::move(Bridge));
    }
    std::sort(Linknames.begin(), Linknames.end(),
              [](const json::Value &A, const json::Value &B) {
                StringRef AN = A.getAsObject()
                                   ->find("name")
                                   ->second.getAsString()
                                   .value_or("");
                StringRef BN = B.getAsObject()
                                   ->find("name")
                                   ->second.getAsString()
                                   .value_or("");
                return AN < BN;
              });
    Root["linknames"] = std::move(Linknames);
  }

  // c2go WF2 (#319 C4a): rebuild `types[]` from the
  // `c2go.struct.<X>.meta` (+ `.godef`) NamedMD pairs emitted by
  // CodeGenModule::Release / CodeGenAction::buildC2GoManifest. We scan
  // module-level NamedMD with the `c2go.struct.` prefix and `.meta`
  // suffix; each match yields one types[] entry. The sibling `.godef`
  // (one operand MDString) supplies the Go-side struct text.
  {
    json::Array Types;
    StringRef Prefix = llvm::c2go::kStructMDPrefix;
    StringRef MetaSuffix = ".meta";
    for (NamedMDNode &NMD : Composite.named_metadata()) {
      StringRef N = NMD.getName();
      if (!N.starts_with(Prefix) || !N.ends_with(MetaSuffix))
        continue;
      StringRef Stripped = N.drop_front(Prefix.size());
      Stripped = Stripped.drop_back(MetaSuffix.size());
      if (Stripped.empty())
        continue;
      if (NMD.getNumOperands() == 0)
        continue;
      MDNode *Meta = NMD.getOperand(0);
      if (!Meta || Meta->getNumOperands() < 4)
        continue;
      auto *ManagedMD = dyn_cast<MDString>(Meta->getOperand(0));
      auto *SchemeMD = dyn_cast<MDString>(Meta->getOperand(1));
      auto *PtrOffMD = dyn_cast<ConstantAsMetadata>(Meta->getOperand(2));
      auto *LNMD = dyn_cast<MDString>(Meta->getOperand(3));
      if (!ManagedMD || !SchemeMD || !PtrOffMD || !LNMD)
        continue;
      auto *PtrOffCI = dyn_cast<ConstantInt>(PtrOffMD->getValue());
      if (!PtrOffCI)
        continue;

      json::Object Ty;
      Ty["name"] = Stripped.str();
      Ty["managed_record"] = ManagedMD->getString() != "unmanaged";

      // Pair the .godef sibling if present so types[].go_def is round-
      // trip-stable with the AST manifest path.
      std::string GoDefName = (Prefix + Stripped + ".godef").str();
      if (NamedMDNode *Godef = Composite.getNamedMetadata(GoDefName)) {
        if (Godef->getNumOperands() > 0) {
          MDNode *GdN = Godef->getOperand(0);
          if (GdN && GdN->getNumOperands() > 0)
            if (auto *S = dyn_cast<MDString>(GdN->getOperand(0)))
              Ty["go_def"] = S->getString().str();
        }
      }

      StringRef Scheme = SchemeMD->getString();
      // Only stamp union_scheme when this is a union; non-unions remain
      // "not_applicable" which buildC2GoManifest omits — keep parity.
      // 2026-06-16 (§3.9): scheme2 (type-punning) unions are now a hard
      // error in clang, so only scheme1 reaches the manifest. The
      // per-alternative `.union_alts` / `.union_alts_go_type` MD (scheme2
      // box scaffolding) is deleted; nothing to recover here.
      if (Scheme == "scheme1") {
        Ty["union_scheme"] = Scheme.str();
        Ty["union_ptr_offset"] = (int64_t)PtrOffCI->getSExtValue();
      }

      if (!LNMD->getString().empty()) {
        Ty["linkage_owner"] = "go";
        Ty["linkname"] = LNMD->getString().str();
      } else {
        Ty["linkage_owner"] = "c";
      }

      Types.push_back(std::move(Ty));
    }
    std::sort(Types.begin(), Types.end(),
              [](const json::Value &A, const json::Value &B) {
                StringRef AN =
                    A.getAsObject()->find("name")->second.getAsString().value_or("");
                StringRef BN =
                    B.getAsObject()->find("name")->second.getAsString().value_or("");
                return AN < BN;
              });
    Root["types"] = std::move(Types);
  }

  // c2go WF2 (#319 M4): rebuild `module_gcmask.vars[]` from the
  // `@c2go.global.gcmask.<var>` GVs CodeGenModule stamped into the
  // bitcode. Mirrors collectC2GoModuleGCMaskVars in
  // CodeGenAction.cpp; kept in sync deliberately.
  {
    json::Object ModGC;
    ModGC["vars"] = llvm::c2go::collectGCMaskVarsFromModule(Composite);
    Root["module_gcmask"] = std::move(ModGC);
  }

  // c2go (WF2 unification): when clang embedded the manifest, discard everything
  // the legacy reconstruction just built and emit the embedded (and cross-TU-
  // merged) manifest instead — it is the WF1 builder's output verbatim, so the
  // WF2 manifest is byte-identical to WF1's by construction.
  if (Embedded)
    Root = std::move(*Embedded);

  // Render once into the in-memory buffer; file + archive members are
  // served from the same bytes (M5 byte-identical guarantee).
  {
    raw_string_ostream OS(ManifestText);
    OS << formatv("{0:2}", json::Value(std::move(Root))) << "\n";
  }

  if (!EmitManifestPath.empty()) {
    std::error_code EC;
    raw_fd_ostream Out(EmitManifestPath, EC, sys::fs::OF_Text);
    if (EC) {
      errs() << ProgName << ": cannot open '" << EmitManifestPath << "': "
             << EC.message() << "\n";
      return ManifestRebuildStatus::ToolError;
    }
    Out << ManifestText;
  }

  // Q1 fix: now that the manifest is emitted, drop declare-only
  // c2go-boundary functions that were only kept alive by llvm.compiler.used
  // (the keep-alive side-channel installed by CodeGenModule::Release for
  // declare-only c2go_extern). They have no further use in the combined
  // bitcode and would otherwise persist as dead declares. We do this only
  // after the manifest has been built so the reader still sees them.
  {
    SmallVector<GlobalValue *, 8> ToErase;
    for (Function &F : Composite.functions()) {
      if (F.isDeclaration() && F.hasFnAttribute("c2go-boundary"))
        ToErase.push_back(&F);
    }
    if (!ToErase.empty()) {
      SmallPtrSet<Constant *, 8> EraseSet(ToErase.begin(), ToErase.end());
      removeFromUsedLists(Composite, [&](Constant *C) {
        return EraseSet.count(C) > 0;
      });
      // appendToCompilerUsed wraps each GV in a ConstantExpr pointer cast
      // (see ModuleUtils.cpp appendToUsedList). After the used-list GV is
      // erased, those wrapper ConstantExprs are now dead but still hold a
      // single use on each underlying Function. Drop them so use_empty()
      // returns true and we can erase the declares.
      for (GlobalValue *GV : ToErase)
        GV->removeDeadConstantUsers();
      for (GlobalValue *GV : ToErase)
        if (GV->use_empty())
          GV->eraseFromParent();
    }
  }

  return ManifestRebuildStatus::OK;
}

} // namespace c2go
} // namespace llvm
