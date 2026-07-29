//===--- CodeGenAction.cpp - LLVM Code Generation Frontend Action ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/CodeGen/CodeGenAction.h"
#include "BackendConsumer.h"
#include "CGC2GoManifestHelpers.h"
#include "CGCall.h"
#include "llvm/MC/MCPlan9AsmStreamer.h"
#include "CodeGenModule.h"
#include "CoverageMappingGen.h"
#include "MacroPPCallbacks.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/C2GoUtil.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclGroup.h"
#include "clang/AST/RecordLayout.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "clang/Basic/DiagnosticFrontend.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangStandard.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/BackendUtil.h"
#include "clang/CodeGen/CGFunctionInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Serialization/ASTWriter.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/CodeGen/MachineOptimizationRemarkEmitter.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LLVMRemarkStreamer.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/LTO/LTOBackend.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Pass.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Transforms/C2Go/C2GoExportName.h"
#include "llvm/Transforms/C2Go/C2GoGCMaskUtils.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"
#include "llvm/Transforms/IPO/Internalize.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <map>
#include <optional>
using namespace clang;
using namespace llvm;

#define DEBUG_TYPE "codegenaction"

namespace clang {
class BackendConsumer;
class ClangDiagnosticHandler final : public DiagnosticHandler {
public:
  ClangDiagnosticHandler(const CodeGenOptions &CGOpts, BackendConsumer *BCon)
      : CodeGenOpts(CGOpts), BackendCon(BCon) {}

  bool handleDiagnostics(const DiagnosticInfo &DI) override;

  bool isAnalysisRemarkEnabled(StringRef PassName) const override {
    return CodeGenOpts.OptimizationRemarkAnalysis.patternMatches(PassName);
  }
  bool isMissedOptRemarkEnabled(StringRef PassName) const override {
    return CodeGenOpts.OptimizationRemarkMissed.patternMatches(PassName);
  }
  bool isPassedOptRemarkEnabled(StringRef PassName) const override {
    return CodeGenOpts.OptimizationRemark.patternMatches(PassName);
  }

  bool isAnyRemarkEnabled() const override {
    return CodeGenOpts.OptimizationRemarkAnalysis.hasValidPattern() ||
           CodeGenOpts.OptimizationRemarkMissed.hasValidPattern() ||
           CodeGenOpts.OptimizationRemark.hasValidPattern();
  }

private:
  const CodeGenOptions &CodeGenOpts;
  BackendConsumer *BackendCon;
};

static void reportOptRecordError(Error E, DiagnosticsEngine &Diags,
                                 const CodeGenOptions &CodeGenOpts) {
  handleAllErrors(
      std::move(E),
    [&](const LLVMRemarkSetupFileError &E) {
        Diags.Report(diag::err_cannot_open_file)
            << CodeGenOpts.OptRecordFile << E.message();
      },
    [&](const LLVMRemarkSetupPatternError &E) {
        Diags.Report(diag::err_drv_optimization_remark_pattern)
            << E.message() << CodeGenOpts.OptRecordPasses;
      },
    [&](const LLVMRemarkSetupFormatError &E) {
        Diags.Report(diag::err_drv_optimization_remark_format)
            << CodeGenOpts.OptRecordFormat;
      });
}

BackendConsumer::BackendConsumer(CompilerInstance &CI, BackendAction Action,
                                 IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
                                 LLVMContext &C,
                                 SmallVector<LinkModule, 4> LinkModules,
                                 StringRef InFile,
                                 std::unique_ptr<raw_pwrite_stream> OS,
                                 CoverageSourceInfo *CoverageInfo,
                                 llvm::Module *CurLinkModule)
    : CI(CI), Diags(CI.getDiagnostics()), CodeGenOpts(CI.getCodeGenOpts()),
      TargetOpts(CI.getTargetOpts()), LangOpts(CI.getLangOpts()),
      AsmOutStream(std::move(OS)), FS(VFS), Action(Action),
      Gen(CreateLLVMCodeGen(CI, InFile, C, CoverageInfo)),
      LinkModules(std::move(LinkModules)), CurLinkModule(CurLinkModule) {
  TimerIsEnabled = CodeGenOpts.TimePasses;
  llvm::TimePassesIsEnabled = CodeGenOpts.TimePasses;
  llvm::TimePassesPerRun = CodeGenOpts.TimePassesPerRun;
  if (CodeGenOpts.TimePasses)
    LLVMIRGeneration.init("irgen", "LLVM IR generation", CI.getTimerGroup());
}

llvm::Module* BackendConsumer::getModule() const {
  return Gen->GetModule();
}

std::unique_ptr<llvm::Module> BackendConsumer::takeModule() {
  return std::unique_ptr<llvm::Module>(Gen->ReleaseModule());
}

CodeGenerator* BackendConsumer::getCodeGenerator() {
  return Gen.get();
}

void BackendConsumer::HandleCXXStaticMemberVarInstantiation(VarDecl *VD) {
  Gen->HandleCXXStaticMemberVarInstantiation(VD);
}

void BackendConsumer::Initialize(ASTContext &Ctx) {
  assert(!Context && "initialized multiple times");

  Context = &Ctx;

  if (TimerIsEnabled)
    LLVMIRGeneration.startTimer();

  Gen->Initialize(Ctx);

  if (TimerIsEnabled)
    LLVMIRGeneration.stopTimer();
}

bool BackendConsumer::HandleTopLevelDecl(DeclGroupRef D) {
  PrettyStackTraceDecl CrashInfo(*D.begin(), SourceLocation(),
                                 Context->getSourceManager(),
                                 "LLVM IR generation of declaration");

  // Recurse.
  if (TimerIsEnabled && !LLVMIRGenerationRefCount++)
    CI.getFrontendTimer().yieldTo(LLVMIRGeneration);

  Gen->HandleTopLevelDecl(D);

  if (TimerIsEnabled && !--LLVMIRGenerationRefCount)
    LLVMIRGeneration.yieldTo(CI.getFrontendTimer());

  return true;
}

void BackendConsumer::HandleInlineFunctionDefinition(FunctionDecl *D) {
  PrettyStackTraceDecl CrashInfo(D, SourceLocation(),
                                 Context->getSourceManager(),
                                 "LLVM IR generation of inline function");
  if (TimerIsEnabled)
    CI.getFrontendTimer().yieldTo(LLVMIRGeneration);

  Gen->HandleInlineFunctionDefinition(D);

  if (TimerIsEnabled)
    LLVMIRGeneration.yieldTo(CI.getFrontendTimer());
}

void BackendConsumer::HandleInterestingDecl(DeclGroupRef D) {
  HandleTopLevelDecl(D);
}

// Links each entry in LinkModules into our module. Returns true on error.
bool BackendConsumer::LinkInModules(llvm::Module *M) {
  for (auto &LM : LinkModules) {
    assert(LM.Module && "LinkModule does not actually have a module");

    if (LM.PropagateAttrs)
      for (Function &F : *LM.Module) {
        // Skip intrinsics. Keep consistent with how intrinsics are created
        // in LLVM IR.
        if (F.isIntrinsic())
          continue;
        CodeGen::mergeDefaultFunctionDefinitionAttributes(
          F, CodeGenOpts, LangOpts, TargetOpts, LM.Internalize);
      }

    CurLinkModule = LM.Module.get();
    bool Err;

    if (LM.Internalize) {
      Err = Linker::linkModules(
          *M, std::move(LM.Module), LM.LinkFlags,
          [](llvm::Module &M, const llvm::StringSet<> &GVS) {
            internalizeModule(M, [&GVS](const llvm::GlobalValue &GV) {
              return !GV.hasName() || (GVS.count(GV.getName()) == 0);
            });
          });
    } else
      Err = Linker::linkModules(*M, std::move(LM.Module), LM.LinkFlags);

    if (Err)
      return true;
  }

  LinkModules.clear();
  return false; // success
}

// c2go WF2 (#319): the three Go-side spelling helpers (mapC2GoType /
// buildC2GoGoSig / computeC2GoArgSize) used to live as static helpers in this
// file's anonymous namespace, only callable from buildC2GoManifest. WF2 needs
// CodeGenModule::SetLLVMFunctionAttributes to call the same logic so it can
// stamp go_sig / argsize / go_type as IR attributes that survive into bitcode.
// The implementations now live in CGC2GoManifestHelpers.{h,cpp}; we keep
// thin file-local wrappers under the original names so the (many) call sites
// in buildC2GoManifest don't need touching.
static std::string c2goMapType(QualType QT, const ASTContext &Ctx,
                               bool IsUnmanaged = false) {
  return clang::c2go::mapC2GoType(QT, Ctx, IsUnmanaged);
}

static std::string c2goBuildGoSig(const FunctionDecl *FD,
                                  const ASTContext &Ctx) {
  return clang::c2go::buildC2GoGoSig(FD, Ctx);
}

static uint64_t c2goComputeArgSize(const FunctionDecl *FD,
                                   const ASTContext &Ctx) {
  return clang::c2go::computeC2GoArgSize(FD, Ctx);
}

// c2go §E (unmanaged_extern full ABI): emit a per-target parameter-passing
// description derived from clang's own C-ABI lowering (CGFunctionInfo /
// ABIArgInfo). c2gobind generates the dispatch wrapper from this instead of
// re-deriving the SysV / AAPCS64 eightbyte / HFA classification on the Go side
// (which is what purego must do because it only has the Go reflect.Type).
//
// Schema (target-specific, alongside the per-target Plan 9 .s):
//   { "ret":    <slot>,
//     "params": [ <slot>, ... ] }
// where <slot> is one of:
//   { "pass":"void" }                                   // ignored / void return
//   { "pass":"direct", "words":[ <word>, ... ] }        // register-passed
//   { "pass":"byval",  "sz":N }                         // SysV memory: stack bytes
//   { "pass":"indirect","sz":N }                        // AAPCS large: pointer in int reg
//   { "pass":"sret",   "sz":N }                         // hidden-pointer struct return
// and <word> is { "f":bool, "off":N, "sz":N [, "sext":true] } — a machine word
// taken from byte offset `off` (size `sz`) of the argument/return image, routed
// to a float ("f":true) or integer register; "sext" marks a sub-word signed
// scalar the wrapper must sign-extend.
//
// Returns nullopt for ABI forms not yet wired (Expand / CoerceAndExpand /
// InAlloca) so the caller leaves the legacy has_float/has_aggregate fields and
// c2gobind falls back.
static std::optional<llvm::json::Object>
c2goBuildAbiDesc(CodeGen::CodeGenModule &CGM, const FunctionDecl *FD) {
  if (!FD->getType()->getAs<FunctionProtoType>())
    return std::nullopt; // K&R / no prototype: fall back
  CanQual<FunctionProtoType> CanFT =
      FD->getType()->getCanonicalTypeUnqualified().castAs<FunctionProtoType>();
  const CodeGen::CGFunctionInfo &FI =
      CodeGen::arrangeFreeFunctionType(CGM, CanFT);
  const llvm::DataLayout &DL = CGM.getDataLayout();

  auto isFloatClass = [](llvm::Type *T) { return T->isFPOrFPVectorTy(); };

  // Decompose a Direct coerce type into register words (one per element of a
  // struct/array coerce, else the scalar itself). The coerce element offset is
  // the byte offset into the argument image to read the word from.
  auto coerceWords = [&](llvm::Type *T, unsigned BaseOff,
                         llvm::json::Array &Words) {
    if (auto *ST = dyn_cast<llvm::StructType>(T)) {
      const llvm::StructLayout *SL = DL.getStructLayout(ST);
      for (unsigned i = 0, n = ST->getNumElements(); i < n; ++i) {
        llvm::Type *E = ST->getElementType(i);
        llvm::json::Object W;
        W["f"] = isFloatClass(E);
        W["off"] = (int64_t)(BaseOff + SL->getElementOffset(i));
        W["sz"] = (int64_t)DL.getTypeStoreSize(E).getFixedValue();
        Words.push_back(std::move(W));
      }
    } else if (auto *AT = dyn_cast<llvm::ArrayType>(T)) {
      llvm::Type *E = AT->getElementType();
      uint64_t ESz = DL.getTypeAllocSize(E).getFixedValue();
      uint64_t SSz = DL.getTypeStoreSize(E).getFixedValue();
      for (uint64_t i = 0, n = AT->getNumElements(); i < n; ++i) {
        llvm::json::Object W;
        W["f"] = isFloatClass(E);
        W["off"] = (int64_t)(BaseOff + i * ESz);
        W["sz"] = (int64_t)SSz;
        Words.push_back(std::move(W));
      }
    } else {
      llvm::json::Object W;
      W["f"] = isFloatClass(T);
      W["off"] = (int64_t)BaseOff;
      W["sz"] = (int64_t)DL.getTypeStoreSize(T).getFixedValue();
      Words.push_back(std::move(W));
    }
  };

  auto slotOf = [&](const CodeGen::ABIArgInfo &AI, QualType QT,
                    llvm::json::Object &Out) -> bool {
    switch (AI.getKind()) {
    case CodeGen::ABIArgInfo::Ignore:
      Out["pass"] = "void";
      return true;
    case CodeGen::ABIArgInfo::Extend:
    case CodeGen::ABIArgInfo::Direct: {
      Out["pass"] = "direct";
      llvm::json::Array Words;
      coerceWords(AI.getCoerceToType(), AI.getDirectOffset(), Words);
      if (AI.getKind() == CodeGen::ABIArgInfo::Extend && AI.isSignExt() &&
          Words.size() == 1)
        (*Words[0].getAsObject())["sext"] = true;
      Out["words"] = std::move(Words);
      return true;
    }
    case CodeGen::ABIArgInfo::Indirect:
    case CodeGen::ABIArgInfo::IndirectAliased: {
      int64_t Sz = CGM.getContext().getTypeSizeInChars(QT).getQuantity();
      Out["pass"] = (AI.isIndirect() && AI.getIndirectByVal()) ? "byval"
                                                               : "indirect";
      Out["sz"] = Sz;
      return true;
    }
    default:
      return false; // Expand / CoerceAndExpand / InAlloca: not wired
    }
  };

  llvm::json::Object Abi;
  {
    const CodeGen::ABIArgInfo &RI = FI.getReturnInfo();
    llvm::json::Object Ret;
    if (RI.isIgnore()) {
      Ret["pass"] = "void";
    } else if (RI.isIndirect()) {
      Ret["pass"] = "sret";
      Ret["sz"] = (int64_t)CGM.getContext()
                      .getTypeSizeInChars(FD->getReturnType())
                      .getQuantity();
    } else if (RI.isDirect() || RI.isExtend()) {
      Ret["pass"] = "direct";
      llvm::json::Array Words;
      coerceWords(RI.getCoerceToType(), RI.getDirectOffset(), Words);
      Ret["words"] = std::move(Words);
    } else {
      return std::nullopt;
    }
    Abi["ret"] = std::move(Ret);
  }
  {
    llvm::json::Array Params;
    for (const auto &A : FI.arguments()) {
      llvm::json::Object P;
      if (!slotOf(A.info, A.type, P))
        return std::nullopt;
      Params.push_back(std::move(P));
    }
    Abi["params"] = std::move(Params);
  }
  return Abi;
}

// c2go #444 — three Go-export-name spelling helpers
// (c2goCapitalizeUnderscore / c2goExportGoName / c2goInitMainRenamedSymbol)
// live in the shared header `llvm/Transforms/C2Go/C2GoExportName.h`, which
// keeps the WF1 manifest path here, the WF2 fallback path in
// `llvm/tools/c2go-lto/c2go-lto.cpp`, and the #317 init/main rename
// emitter in `CodeGenModule.cpp` byte-for-byte in lock-step.
using llvm::c2go::c2goCapitalizeUnderscore;
using llvm::c2go::c2goExportGoName;
using llvm::c2go::c2goInitMainRenamedSymbol;

// c2go §B4 phase 2: collect per-global GC pointer-mask bitmaps emitted
// by CodeGenModule::emitC2GoGlobalGCMask into a manifest array so
// c2gobind can stitch them into a synthetic moduledata at runtime
// init (phase 3 wiring). The IR carries each mask as an internal
// `@c2go.global.gcmask.<varname>` global of i8 array type; we just
// re-serialise the constant bytes here.
//
// `M` may be nullptr (e.g. when the manifest is built before IR gen
// completes for some unusual entry point); in that case we silently
// emit an empty module_gcmask section. The phase-1 IR emission stays
// the source of truth — phase 2 only surfaces the metadata.
//
// #401(b) aux audit cleanup: the body lives in the shared helper
// `llvm::c2go::collectGCMaskVarsFromModule`
// (llvm/Transforms/C2Go/C2GoGCMaskUtils.{h,cpp}); the WF2 mirror in
// llvm/tools/c2go-lto/c2go-lto.cpp delegates to the same helper.
static llvm::json::Array
collectC2GoModuleGCMaskVars(const llvm::Module *M) {
  if (!M)
    return {};
  return llvm::c2go::collectGCMaskVarsFromModule(*M);
}

// Preserve every direct-GoABI0 c2go_linkname route independently of whether
// CodeGen happened to materialise the declaration as an llvm::Function. LLVM
// optimisations are allowed to synthesize standard libc calls after AST
// lowering (LoopIdiomRecognize's loop -> strlen and InstCombine's printf ->
// puts are canonical examples). Such calls use the original C spelling and
// therefore cannot recover the declaration's AsmLabel or calling convention
// from IR alone.
//
// The compact named-metadata table is consumed after the normal optimisation
// pipeline by C2GoLibCallRoutingPass. SelectionDAG treats a surviving routed
// libcall as an error because creating a Go call after RS4GC would bypass GC
// relocation. Recording only C2GO_GOABI0 routes is essential: a linkname
// without that selector targets a Go ABIInternal symbol and must continue
// through the existing local wrapper.
static void emitC2GoLibCallRoutes(ASTContext &Ctx, llvm::Module &M) {
  std::map<std::string, std::string> Routes;
  for (const Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
    const auto *FD = dyn_cast<FunctionDecl>(D);
    if (!FD)
      continue;
    const auto *LA = FD->getAttr<C2GoLinknameAttr>();
    if (!LA || LA->getHasAbi0() == 0)
      continue;

    std::string CName = FD->getNameAsString();
    std::string Target = LA->getName().str();
    if (CName.empty() || Target.empty())
      continue;

    auto [It, Inserted] = Routes.emplace(CName, Target);
    if (!Inserted && It->second != Target)
      M.getContext().emitError(
          "conflicting c2go libcall routes for C function '" + CName + "': '" +
          It->second + "' versus '" + Target + "'");
  }

  if (Routes.empty())
    return;

  llvm::LLVMContext &LLVMCtx = M.getContext();
  llvm::NamedMDNode *NMD =
      M.getOrInsertNamedMetadata(llvm::c2go::kLibCallRoutesMDName);
  for (const auto &[CName, Target] : Routes) {
    llvm::Metadata *Ops[] = {llvm::MDString::get(LLVMCtx, CName),
                             llvm::MDString::get(LLVMCtx, Target)};
    NMD->addOperand(llvm::MDNode::get(LLVMCtx, Ops));
  }
}

// c2go: build the sidecar manifest JSON described in c2go_design.md
// v14 §4.7, consumed by c2gobind to produce Go declarations AND by
// the in-clang Plan 9 .s emitter (BackendUtil::RunC2GoPlan9Pipeline
// + MCPlan9AsmStreamer). Always returns the constructed JSON
// object; the caller decides whether to write it to disk.
static llvm::json::Object buildC2GoManifest(ASTContext &Ctx,
                                            const LangOptions &LangOpts,
                                            DiagnosticsEngine &Diags,
                                            llvm::Module *Mod,
                                            CodeGen::CodeGenModule *CGM) {
  if (Mod)
    emitC2GoLibCallRoutes(Ctx, *Mod);

  llvm::json::Object Root;
  Root["pkgpath"] = LangOpts.C2GoPackagePath.empty()
                       ? "main"
                       : LangOpts.C2GoPackagePath;

  // c2go version anchoring (docs/c2go/versioning.md): stamp the toolchain's
  // epoch constants into every manifest (carried verbatim into WF2 bitcode).
  // c2go-bind asserts the consumer's c2go_abi_epoch lies in the linked
  // c2go-libc's accepted [C2GoABIEpochMin, C2GoABIEpochMax] range; go_contract_
  // epoch records the Go-internal contract generation this artifact was emitted
  // for. Bump c2go_abi_epoch ONLY on an intentional c2go ABI break; keep both
  // in sync with c2go-libc's version consts and c2go-bind's defaults.
  Root["c2go_abi_epoch"] = 1;
  Root["go_contract_epoch"] = 1;

  // Parse "<lo>-<hi>" from -fc2go-target-go-version (default "1.22-1.25").
  StringRef VerRange = LangOpts.C2GoTargetGoVersion;
  if (VerRange.empty()) VerRange = "1.22-1.25";
  auto Dash = VerRange.find('-');
  StringRef Lo = Dash == StringRef::npos ? VerRange : VerRange.substr(0, Dash);
  StringRef Hi = Dash == StringRef::npos ? VerRange : VerRange.substr(Dash + 1);
  Root["min_go_version"] = ("go" + Lo).str();
  Root["max_go_version"] = ("go" + Hi).str();

  // Record the target as Go's GOOS/GOARCH so c2go-bind derives the per-OS
  // extern dispatch (unix cgocall vs windows syscall.SyscallN) and the
  // per-(OS,arch) output naming directly from the manifest — no -extern-os flag.
  const llvm::Triple &TT = Ctx.getTargetInfo().getTriple();
  StringRef GOOS = TT.isOSWindows() ? "windows"
                   : TT.isOSDarwin() ? "darwin"
                   : TT.isOSLinux()  ? "linux"
                                     : "";
  StringRef GOARCH = TT.getArch() == llvm::Triple::x86_64    ? "amd64"
                     : TT.getArch() == llvm::Triple::aarch64 ? "arm64"
                                                             : "";
  if (!GOOS.empty())
    Root["goos"] = GOOS;
  if (!GOARCH.empty())
    Root["goarch"] = GOARCH;

  llvm::json::Array Symbols;
  llvm::json::Array Types;

  // Deduplicate by canonical declaration so a prototype + definition
  // pair doesn't produce two manifest entries.
  llvm::DenseSet<const Decl *> Emitted;

  // c2go §A5: queue of c2go-tracked RecordDecls awaiting manifest
  // emission. Seeded from the TU's top-level decls; the record-emit
  // path appends any nested anonymous c2go records it finds in fields
  // so consumers see the synthetic-name types `c2goMapType` references.
  llvm::SmallVector<const RecordDecl *, 16> RecordWorklist;

  for (const Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
    if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
      const FunctionDecl *DefFD = nullptr;
      bool HasBody = FD->hasBody(DefFD);
      // Two symbol classes reach the manifest (docs/c2go_design.md §2.0.3):
      //   * EXPORT — a c2go_extern function DEFINITION (the attr may be
      //     inherited from a forward declaration). It emits a Plan 9 .s ABI0
      //     entry that c2gobind turns into a Go export stub. A declared-only
      //     c2go_extern is a pure ABI0 *marker* with no metadata and is
      //     intentionally NOT emitted here.
      //   * IMPORT — an `unmanaged extern` external C symbol (libsystem_kernel
      //     `read`, libcurl `curl_easy_init`, ...) the c2go world calls
      //     through the host-ABI bridge. The call-site IR uses GoABI0 because
      //     c2gobind emits an ABI0-entry dispatch wrapper that walks the frame,
      //     packs args and dispatches through `c2go-libc/external` (purego
      //     SyscallN + dlsym).
      // c2go_linkname decls (e.g. <stdlib.h>'s cmalloc/atoi/...) bind to
      // symbols in c2go-libc and are excluded from both classes.
      const bool IsExport = FD->hasAttr<C2GoExternAttr>() && HasBody;
      const bool IsImport = clang::c2go::isC2GoUnmanagedExternImport(FD);
      if (!IsExport && !IsImport)
        continue;
      // An import earns a manifest entry when it is EXPLICIT (`unmanaged extern`
      // — the user declared an import to bind for Go, which may call it even
      // when this C TU does not) OR when it is actually referenced in C. Under
      // model B every plain declared-only function is an implicit-default import;
      // without this gate a header full of prototypes would bloat the manifest
      // with imports the program never uses. The *.s dispatch wrapper* stays
      // gated on use_empty (EmitC2GoUnmanagedExternWrappers); `wrapper_in_asm`
      // below records which path applies, so a C-unreferenced explicit import
      // falls back to c2gobind's Go-side dispatcher rather than bloating the .s.
      if (IsImport) {
        const auto *UA = FD->getAttr<C2GoUnmanagedAttr>();
        const bool Explicit = UA && !UA->isImplicit();
        bool Referenced = false;
        if (Mod) {
          if (llvm::Function *IF = Mod->getFunction(FD->getNameAsString()))
            Referenced = !IF->use_empty();
          // `&import` redirects to the c2go_stub_<name> trampoline (the raw host
          // address) instead of the wrapper @<name>, so the import is referenced
          // even when @<name> itself is unused — count the stub's uses too.
          if (!Referenced)
            if (llvm::Function *SF = Mod->getFunction(
                    ("c2go_stub_" +
                     CodeGen::CodeGenModule::c2goHostImportName(FD->getName()))
                        .str()))
              Referenced = !SF->use_empty();
        }
        if (!Explicit && !Referenced)
          continue;
      }
      const Decl *Canonical = FD->getCanonicalDecl();
      if (!Emitted.insert(Canonical).second)
        continue;
      llvm::json::Object Sym;
      std::string CName = FD->getNameAsString();
      Sym["name"] = CName;
      Sym["kind"] = IsImport ? "unmanaged_extern" : "func";
      Sym["go_sig"] = c2goBuildGoSig(FD, Ctx);
      // c2go (#269): the generated Go function name, casing controlled
      // by c2go_extern's optional int (1=exported/upper-first default,
      // 0=keep C casing). When the Go name's case differs from the C
      // symbol, c2gobind must emit `//go:linkname go_name <pkg>.<cName>`
      // to bind them.
      // c2go (#317): C `init`/`main` are renamed at the symbol-emission
      // level (CodeGenModule::c2goInitMainRename). The manifest's
      // asm_symbol must reflect the renamed bare symbol so c2gobind's
      // //go:linkname target (`<pkg>.c2go_cinit` / `c2go_cmain`) lines
      // up with the actually-emitted Plan 9 symbol. The Go binding name
      // is fixed to `Init`/`Main` (a normal exported func, UNRELATED to
      // Go's language-level init/main). Sema guarantees init/main only
      // reach here with Export==1 (c2go_extern / c2go_extern(1)); the
      // capitalized form for `init`/`main` happens to be `Init`/`Main`.
      StringRef RenamedSym = c2goInitMainRenamedSymbol(CName);
      {
        // Exports carry c2go_extern (and its export-case knob). Imports
        // (`unmanaged extern`) have no c2go_extern attr, so default to the
        // upper-first export case (1) — c2gobind names the dispatch wrapper
        // the same capitalized way.
        const auto *EA = FD->getAttr<C2GoExternAttr>();
        int Export = EA ? EA->getExportCase() : 1;
        std::string GoName = c2goExportGoName(CName, Export);
        Sym["go_name"] = GoName;
        // The on-symbol name to link against: the renamed bare symbol for
        // init/main, else the C name.
        StringRef LinkBase = RenamedSym.empty() ? StringRef(CName) : RenamedSym;
        if (GoName != LinkBase)
          Sym["needs_linkname"] = true;
        // c2go (#317): mark the program-entry main so c2gobind emits the
        // `func main()` wrapper (only when generating `package main`).
        // `init` is NOT an entry; c2gobind must never synthesise a
        // `func init()` for it.
        if (CName == "main") {
          Sym["c_entry"] = true;
          // Entry signature variant, so the wrapper marshals argc/argv/
          // envp correctly. Distinguish by parameter count (the c2go ABI
          // only allows int/char**/char** here).
          unsigned NParams = FD->getNumParams();
          if (NParams == 0)
            Sym["entry_sig"] = "void";
          else if (NParams == 2)
            Sym["entry_sig"] = "argc_argv";
          else if (NParams == 3)
            Sym["entry_sig"] = "argc_argv_envp";
          else
            Sym["entry_sig"] = "unknown";
        }
      }
      Sym["managed"] = !FD->hasAttr<C2GoUnmanagedAttr>();
      Sym["abi"] = "abi0";
      Sym["asm_symbol"] =
          "\xc2\xb7" + (RenamedSym.empty() ? FD->getNameAsString()
                                           : RenamedSym.str()); // U+00B7
      // ABI0 frame size — Go assembler validates `$0-<argsize>`. This
      // is the precise count from AST type sizes; c2go-plan9asm uses
      // it directly instead of parsing the sig string.
      Sym["argsize"] = (int64_t)c2goComputeArgSize(FD, Ctx);
      // c2go §E phase 2: surface variadic / float-arg / aggregate-by-
      // value bits so c2gobind can pick the right wrapper template for
      // unmanaged_extern targets. Variadic is implemented (printf-style
      // dispatch via SyscallN); the other two are recorded so c2gobind
      // can emit a structured panic stub instead of producing broken
      // wrappers. Booleans are written only when true to keep the
      // sidecar JSON diff small for the common scalar/pointer case.
      if (FD->isVariadic())
        Sym["is_variadic"] = true;
      {
        // Detect floating-point and aggregate-by-value slots. Anything
        // that the Itanium / AAPCS ABI would route through FP / SIMD
        // registers (float, double, long double, _Complex, _Float16,
        // vectors) needs out-of-band handling because SyscallN treats
        // every uintptr slot as an integer-class register. Aggregate-
        // by-value (struct / union by value) is rejected outright: the
        // ABI decomposes the value into multiple register / stack
        // slots in a target-specific way, which c2gobind cannot
        // synthesise from the manifest alone.
        bool HasFloat = false;
        bool HasAggregate = false;
        auto Check = [&](QualType QT) {
          QT = QT.getCanonicalType();
          if (QT->isVoidType()) return;
          if (QT->isFloatingType() || QT->isAnyComplexType() ||
              QT->isVectorType())
            HasFloat = true;
          if (QT->isRecordType())
            HasAggregate = true;
        };
        for (auto *PVD : FD->parameters())
          Check(PVD->getType());
        Check(FD->getReturnType());
        if (HasFloat)
          Sym["has_float"] = true;
        if (HasAggregate)
          Sym["has_aggregate"] = true;
      }
      // c2go §E full ABI: for unmanaged_extern targets, attach the per-target
      // parameter-passing description from clang's C-ABI lowering so c2gobind
      // generates a real (float / struct-aware) dispatch wrapper instead of a
      // panic stub. Falls back silently (legacy has_float/has_aggregate stay)
      // for not-yet-wired ABI forms or when no CodeGenModule is available.
      if (CGM && IsImport) {
        if (auto Abi = c2goBuildAbiDesc(*CGM, FD))
          Sym["cabi"] = std::move(*Abi);
        // By-value struct params/returns of an unmanaged_extern need their Go
        // type emitted so the wrapper signature is correctly sized. Plain C
        // structs carry no C2GoStructAttr, so seed the record worklist
        // directly (its field-walking path builds the layout); the Emitted set
        // dedups against managed records.
        auto SeedRecord = [&](QualType QT) {
          QT = QT.getCanonicalType();
          if (const auto *RT = QT->getAs<RecordType>())
            if (const RecordDecl *Def = RT->getDecl()->getDefinition())
              RecordWorklist.push_back(Def);
        };
        for (const auto *PVD : FD->parameters())
          SeedRecord(PVD->getType());
        SeedRecord(FD->getReturnType());
        // c2go §E (.s-wrapper model): if clang synthesized the GoABI0 dispatch
        // wrapper for this symbol (EmitC2GoUnmanagedExternWrappers tagged the
        // IR function with `c2go-wrapper-in-asm`), tell c2gobind to emit only
        // the bodyless decl + fn-address glue, NOT its own Go dispatch wrapper.
        if (Mod)
          if (llvm::Function *WF = Mod->getFunction(CName))
            if (WF->hasFnAttribute("c2go-wrapper-in-asm"))
              Sym["wrapper_in_asm"] = true;
      }
      (void)DefFD;
      // c2go WF2 (#367 Bug A): mirror the just-built JSON symbols[]
      // entry into a `c2go.func.<CName>` NamedMD so c2go-lto can rebuild
      // the same 14 fields from combined bitcode byte-identically — the
      // per-function string attrs alone only cover c-name / go-sig /
      // unmanaged-return (3 of 14 fields), which is why prior reader
      // output silently dropped go_name / asm_symbol / argsize etc.
      if (Mod)
        clang::c2go::emitC2GoFuncManifest(*Mod, CName, Sym);
      Symbols.push_back(std::move(Sym));
    } else if (const auto *VD = dyn_cast<VarDecl>(D)) {
      if (!VD->hasAttr<C2GoExternAttr>())
        continue;
      const Decl *Canonical = VD->getCanonicalDecl();
      if (!Emitted.insert(Canonical).second)
        continue;
      bool Unmanaged = VD->hasAttr<C2GoUnmanagedAttr>();
      llvm::json::Object Sym;
      Sym["name"] = VD->getNameAsString();
      Sym["kind"] = "var";
      Sym["go_type"] = c2goMapType(VD->getType(), Ctx, Unmanaged);
      Sym["managed"] = !Unmanaged;
      Sym["abi"] = "abi0";
      Sym["asm_symbol"] = "\xc2\xb7" + VD->getNameAsString();
      Symbols.push_back(std::move(Sym));
    } else if (const auto *RD = dyn_cast<RecordDecl>(D)) {
      // c2go §A5: defer record emission to the worklist below so the
      // emit path can recurse into nested anonymous c2go records (the
      // §A3-promoted ones that don't appear at TU level) and produce
      // matching manifest entries for them.
      if (!RD->hasAttr<C2GoStructAttr>())
        continue;
      const RecordDecl *Def = RD->getDefinition();
      if (!Def)
        continue;
      RecordWorklist.push_back(Def);
    }
  }

  // #227: scan the emitted LLVM Module for `@c2go.typeinfo.<X>` globals
  // (emitted by CGC2GoTypeInfo) and ensure each X has a corresponding
  // RecordDecl in the worklist. Without this, anonymous records that
  // clang synthesised on-the-fly (e.g. through composite-literal codegen)
  // may have a typeinfo global but no manifest entry — c2gobind would
  // see a dangling `_typeinfo_<X>` reference. Resolve by looking up the
  // synthetic record by name in the AST's anonymous-record registry.
  if (Mod) {
    for (const llvm::GlobalVariable &GV : Mod->globals()) {
      StringRef Name = GV.getName();
      if (!Name.consume_front(llvm::c2go::kTypeinfoGVPrefix))
        continue;
      // Find a RecordDecl by stable name. c2go.anon.<hash> records are
      // synthesised; their stable name is keyed on spelling location.
      // The simplest re-resolution path is to walk the TU again and
      // match getStableRecordName(RD) == Name. This is O(N²) worst-case
      // but only runs once at end-of-codegen for the small set of
      // c2go-tracked records.
      bool Found = false;
      std::function<bool(const DeclContext *)> Walk =
          [&](const DeclContext *DC) -> bool {
        for (const Decl *D : DC->decls()) {
          if (const auto *RD = dyn_cast<RecordDecl>(D)) {
            const RecordDecl *Def = RD->getDefinition();
            if (Def && Def->hasAttr<C2GoStructAttr>()) {
              std::string SN = c2go::getStableRecordName(Def, Ctx);
              if (SN == Name) {
                if (!Emitted.count(Def->getCanonicalDecl()))
                  RecordWorklist.push_back(Def);
                return true;
              }
            }
          }
          if (const auto *NDC = dyn_cast<DeclContext>(D))
            if (Walk(NDC))
              return true;
        }
        return false;
      };
      Found = Walk(Ctx.getTranslationUnitDecl());
      (void)Found; // diagnostic emit deferred; c2gobind nil-fallback handles it
    }
  }

  // c2go §A5: drain the record worklist. Each emit may append to the
  // worklist (nested anonymous c2go records), so use index iteration.
  for (size_t WI = 0; WI < RecordWorklist.size(); ++WI) {
    const RecordDecl *RD = RecordWorklist[WI];
    {
      const Decl *Canonical = RD->getCanonicalDecl();
      if (!Emitted.insert(Canonical).second)
        continue;

      // Resolve a usable Go type name. c2go §A3: anonymous records that
      // survived AddPragmaC2GoAttribute carry a managed pointer (Sema
      // already filtered out the pure-scalar tables), so we *do* need a
      // stable name for them — for the typeinfo global, the elem-type
      // metadata, and the Go-binding emission. `getStableRecordName`
      // returns:
      //   * the record's own name when present,
      //   * the typedef name (`typedef struct { ... } T;`) otherwise,
      //   * else a synthesized `c2go.anon.<hash>` keyed on the spelling
      //     location, identical across TUs that include the same header.
      std::string TypeName = c2go::getStableRecordName(RD, Ctx);
      if (TypeName.empty())
        continue; // defensive — should not happen post-§A3

      std::string GoDef;
      // c2go §A5: per-record manifest metadata to surface alongside
      // GoDef so c2gobind can pick a generation strategy without
      // re-parsing the GoDef body. Defaults match the "C-owner,
      // non-anonymous, not-a-union" common case; the branches below
      // override the fields they care about.
      c2go::C2GoUnionClassification UnionClass;
      bool IsUnion = RD->isUnion();
      // §3.9 / T3 — a `c2go_variant` union is converted to a struct whose
      // slots are partitioned by GC class (pointer slots first as
      // `unsafe.Pointer`, then one trailing no-scan `[N]uint8` blob). This
      // makes the pointer slots precisely scannable and bypasses the
      // PunHardError below (which the un-opted-in punning union still gets).
      bool IsVariant = IsUnion && c2go::isC2GoVariantUnion(RD);
      if (IsVariant) {
        auto VL = c2go::computeC2GoVariantLayout(RD, Ctx);
        GoDef = "struct {\n";
        if (VL.AlignBytes >= 8)
          GoDef += "\t_align [0]uint64\n";
        else if (VL.AlignBytes >= 4)
          GoDef += "\t_align [0]uint32\n";
        else if (VL.AlignBytes >= 2)
          GoDef += "\t_align [0]uint16\n";
        unsigned PtrNo = 0;
        for (const auto &Slot : VL.Slots) {
          if (Slot.Kind == c2go::C2GoVariantSlot::Kind::Ptr)
            GoDef += "\t_p" + std::to_string(PtrNo++) +
                     " unsafe.Pointer // c2go_variant ptr slot\n";
          else
            GoDef += "\t_blob [" + std::to_string(Slot.SizeBytes) +
                     "]uint8 // c2go_variant scalar/funcptr slot\n";
        }
        GoDef += "}";
      } else if (IsUnion) {
        // c2go §A4 / §3.9: pick a representation scheme automatically.
        //
        //   * Scheme1 — every scanned-pointer alternative lives at the
        //     same byte offset N and no scalar alternative overlaps
        //     that offset. Emit opaque storage; CGC2GoTypeInfo sets one
        //     bit at offset N in the GC bitmap so Go's GC scans the
        //     pointer slot precisely. No diagnostic — the union is
        //     exactly representable.
        //
        //   * PunHardError — a scanned pointer type-puns a scalar, or scan
        //     pointers live at multiple offsets. A single static bitmap
        //     cannot encode this → HARD ERROR (see below). The abandoned
        //     "scheme2 → any-subtype box" fallback is deleted (2026-06-16).
        //
        //   * NotApplicable — no scanned-pointer alternative at all.
        //     The union is a plain opaque slab from the GC's point of
        //     view; nothing to encode.
        UnionClass = c2go::classifyC2GoUnion(RD, Ctx);
        auto &Class = UnionClass;
        if (Class.Scheme == c2go::C2GoUnionScheme::PunHardError) {
          // 2026-06-16 (§3.9): a union that type-puns a SCANNED pointer
          // slot with a scalar (or places scan pointers at >1 offset)
          // cannot be represented as a static GC bitmap. Under the new
          // force-scan model the pointer slot WOULD be scanned, so a
          // scalar alias at the same bytes feeds garbage to Go's GC and
          // trips `invalidptr`. This is a HARD ERROR (the old "scheme2 →
          // Go-any box" fallback is deleted). The fix is one of: split
          // the alternatives so the pointer lives in its own pure slot,
          // change the punned pointer member to `uintptr_t` (no-scan), or
          // opt the union in to `__attribute__((c2go_variant))` (T3
          // convert-to-struct — separate feature).
          Diags.Report(RD->getLocation(),
                       Diags.getCustomDiagID(
                           DiagnosticsEngine::Error,
                           "c2go: union %0 type-puns a scanned pointer slot "
                           "(%1, blocker=%2); a static GC bitmap cannot "
                           "encode it. Give the pointer its own pure slot, "
                           "change the punned member to uintptr_t, or mark "
                           "the union __attribute__((c2go_variant))"))
              << TypeName << Class.BlockerReason << Class.BlockerFieldName;
          continue;
        }
        // Scheme1 / NotApplicable: the union's typeinfo bitmap will
        // encode the precise pointer slot (Scheme1) or be empty
        // (NotApplicable). PunHardError already errored out above.
        const ASTRecordLayout &Layout = Ctx.getASTRecordLayout(RD);
        uint64_t SizeBytes = Layout.getSize().getQuantity();
        uint64_t AlignBytes = Layout.getAlignment().getQuantity();
        // Use exact byte storage so size matches C; rely on a trailing
        // alignment hint via [N]uintN where N matches the C alignment.
        // For simplicity emit "[size]byte" — Go arrays of byte have
        // alignment 1, so callers may need to add their own
        // explicit-alignment field. For records whose C alignment is
        // > 1 we prepend an alignment marker field.
        GoDef = "struct {\n";
        if (AlignBytes >= 8) {
          GoDef += "\t_align [0]uint64\n";
        } else if (AlignBytes >= 4) {
          GoDef += "\t_align [0]uint32\n";
        } else if (AlignBytes >= 2) {
          GoDef += "\t_align [0]uint16\n";
        }
        GoDef += "\t_storage [" + std::to_string(SizeBytes) +
                 "]byte // C union\n}";
      } else {
        // Build a real Go struct definition by walking fields. v0 maps
        // primitive types and pointers; complex cases (arrays beyond
        // simple element types, bitfields, flex-array members) fall
        // through to "uintptr" placeholders — c2gobind cannot refine
        // them without further sidecar metadata.
        GoDef = "struct {\n";
        for (const FieldDecl *F : RD->fields()) {
          std::string FName = F->getNameAsString();
          if (FName.empty()) FName = "_";
          QualType FT = F->getType().getCanonicalType();
          if (F->isBitField()) {
            Diags.Report(F->getLocation(),
                         Diags.getCustomDiagID(
                             DiagnosticsEngine::Warning,
                             "c2go: bitfield %0 in c2go_struct %1 is "
                             "exported as an opaque uintptr — the Go "
                             "side cannot access it correctly"))
                << FName << TypeName;
          } else if (FT->isIncompleteArrayType()) {
            // Flexible array member (`T arr[];`). v0 maps to uintptr
            // placeholder; Go side cannot reach the trailing storage.
            // Use a separate allocation + pointer field instead.
            Diags.Report(F->getLocation(),
                         Diags.getCustomDiagID(
                             DiagnosticsEngine::Warning,
                             "c2go: flexible-array member %0 in "
                             "c2go_struct %1 is exported as opaque "
                             "uintptr — the trailing storage is not "
                             "visible to Go; restructure as an "
                             "explicit pointer + length pair"))
                << FName << TypeName;
          } else if (FT->isFunctionPointerType()) {
            // Function pointer field. v0 cannot translate the
            // callee's signature to a Go func type without further
            // metadata; emit a uintptr placeholder.
            Diags.Report(F->getLocation(),
                         Diags.getCustomDiagID(
                             DiagnosticsEngine::Warning,
                             "c2go: function-pointer field %0 in "
                             "c2go_struct %1 is exported as opaque "
                             "uintptr — Go side cannot call through "
                             "it directly; wrap the call in a c2go_"
                             "extern shim"))
                << FName << TypeName;
          }
          bool FieldUnmanaged = F->hasAttr<C2GoUnmanagedAttr>() ||
                                RD->hasAttr<C2GoUnmanagedAttr>();
          std::string T = c2goMapType(F->getType(), Ctx, FieldUnmanaged);
          GoDef += "\t" + FName + " " + T + "\n";

          // c2go §A5: surface any nested c2go-tracked record reachable
          // through this field (either directly, or via a pointer) so
          // the worklist visits it. Without this, an anonymous nested
          // struct that §A3 promoted to c2go-tracked would be referenced
          // in GoDef by its synthetic `c2go.anon.<hash>` name but never
          // appear as its own manifest entry — c2gobind would see a
          // dangling type reference.
          QualType FieldType = F->getType().getCanonicalType();
          while (FieldType->isPointerType())
            FieldType = FieldType->getPointeeType().getCanonicalType();
          if (const RecordType *NRT = FieldType->getAs<RecordType>()) {
            if (RecordDecl *Nested = NRT->getDecl()) {
              // §3.9 / T3: a `c2go_variant` union field references a converted
              // struct named after the union (e.g. `v V`). Surface it as its
              // own manifest entry too, otherwise c2gobind sees a dangling
              // `V` type reference.
              if (Nested->hasAttr<C2GoStructAttr>() ||
                  c2go::isC2GoVariantUnion(Nested)) {
                if (const RecordDecl *NDef = Nested->getDefinition()) {
                  if (!Emitted.count(NDef->getCanonicalDecl()))
                    RecordWorklist.push_back(NDef);
                }
              }
            }
          }
        }
        GoDef += "}";
      }

      // c2go WF2 (#319 C4a): stamp the serialized Go struct text onto the
      // module as `c2go.struct.<X>.godef` so c2go-lto can recover the
      // `types[].go_def` field directly from combined bitcode without
      // re-running this AST walk. CodeGenModule::Release emitted the
      // sibling `.meta` (managed/scheme/ptr_offset/linkname) earlier; the
      // two NamedMDs together fully describe the record's manifest entry.
      if (Mod && !GoDef.empty()) {
        clang::c2go::emitC2GoStructGoDef(*const_cast<llvm::Module *>(Mod), RD,
                                         Ctx, GoDef);
      }

      llvm::json::Object Ty;
      Ty["name"] = TypeName;
      Ty["go_def"] = GoDef;
      Ty["managed_record"] = !RD->hasAttr<C2GoUnmanagedAttr>();

      // c2go §A5 — manifest protocol extension. These fields are
      // additive: legacy c2gobind versions ignoring unknown JSON keys
      // continue to behave as before; c2gobind v0.2+ uses them to
      // decide:
      //   * Whether to emit a Go-side `type X struct {...}` (C-owner)
      //     or skip and import the Go-owner declaration verbatim
      //     (e.g. `c2go_libc.FILE`).
      //   * Whether the record's manifest name is a synthesized
      //     `c2go.anon.<hash>` (§A3) so the binding can pick an
      //     emission style that doesn't collide with user-typed names.
      //   * The union representation scheme picked by §A4 so the
      //     binding can emit a precise GC bitmap hint (scheme1) or
      //     defer to opaque storage (not_applicable; pun_hard_error
      //     never reaches the manifest — it errored out above).

      // (1) linkage owner: defaults to C-owner. A c2go_linkname on the
      // record means the canonical Go-side declaration lives in
      // another Go package; c2gobind must NOT redeclare it.
      if (const auto *LN = RD->getAttr<C2GoLinknameAttr>()) {
        Ty["linkage_owner"] = "go";
        Ty["linkname"] = LN->getName().str();
      } else {
        Ty["linkage_owner"] = "c";
      }

      // (2) anonymous synthetic name: only set when the record has no
      // source identifier of its own (true anonymous, no surrounding
      // typedef-name either). §A3's getStableRecordName returned a
      // synthesized `c2go.anon.<hash>` which is already the value of
      // `name`; we mirror it here so the consumer doesn't need to
      // pattern-match the prefix.
      if (!RD->getIdentifier() && !RD->getTypedefNameForAnonDecl())
        Ty["anon_synthetic_name"] = TypeName;

      // (3) union scheme classification (§A4). Non-unions: omit the
      // field entirely so JSON stays tight; c2gobind treats
      // missing-key as "not_applicable / not a union".
      if (IsVariant) {
        // §3.9 / T3 — convert-to-struct: GoDef already carries the precise
        // partitioned struct; c2gobind just emits it verbatim. The bitmap is
        // exact (pointer slots scan, blob no-scan) so no scheme hint is needed
        // beyond the explicit "variant" marker.
        Ty["union_scheme"] = "variant";
      } else if (IsUnion) {
        const char *SchemeStr = "not_applicable";
        switch (UnionClass.Scheme) {
        case c2go::C2GoUnionScheme::Scheme1:
          SchemeStr = "scheme1";
          break;
        case c2go::C2GoUnionScheme::PunHardError:
          SchemeStr = "pun_hard_error";
          break;
        case c2go::C2GoUnionScheme::NotApplicable:
          SchemeStr = "not_applicable";
          break;
        }
        Ty["union_scheme"] = SchemeStr;
        if (UnionClass.Scheme == c2go::C2GoUnionScheme::Scheme1)
          Ty["union_ptr_offset"] = (int64_t)UnionClass.PointerOffsetBytes;
        // PunHardError errored out earlier; only Scheme1 / NotApplicable
        // reach here, so there are no per-alternative subtypes to emit.
      }

      Types.push_back(std::move(Ty));
    }
  }

  // Phase H3: emit deterministically so c2go-ar / c2gobind can merge
  // sidecars by literal compare. Sort symbols and types by their
  // "name" field; the JSON key order inside each entry is already
  // determined by json::Object's stable iteration.
  auto byName = [](const llvm::json::Value &A, const llvm::json::Value &B) {
    const auto *AO = A.getAsObject();
    const auto *BO = B.getAsObject();
    StringRef AN = AO ? AO->getString("name").value_or("") : "";
    StringRef BN = BO ? BO->getString("name").value_or("") : "";
    return AN < BN;
  };
  llvm::sort(Symbols, byName);
  llvm::sort(Types, byName);

  // Path-(b) linkname bridges: scan c2go_linkname attributes whose
  // target Go symbol path contains characters Plan 9 assembler can't
  // represent (primarily `-`). For these, the .s emit produces a
  // sanitised current-pkg local symbol; c2gobind reads the bridges
  // here and emits `//go:linkname <local> <raw>` declarations in
  // the consumer Go package, so the Go linker can resolve the raw
  // cross-pkg target via Go's linkname mechanism.
  //
  // Targets with no `-` go through path (a) — .s emits the raw path
  // directly with Unicode substitutes (·/∕), no Go-side bridge
  // needed — so they don't appear here.
  auto sanitiseToIdent = [](std::string S) {
    for (char &C : S) {
      if ((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
          (C >= '0' && C <= '9') || C == '_')
        continue;
      C = '_';
    }
    return S;
  };

  llvm::json::Array Linknames;
  llvm::DenseSet<const Decl *> LinknameEmitted;
  for (const Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
    const auto *LA = D->getAttr<C2GoLinknameAttr>();
    if (!LA) continue;
    StringRef Target = LA->getName();
    // #274: route to path (b) (//go:linkname bridge in generated .go) when the
    // raw Go target contains ANY character the Plan 9 assembler can't carry in
    // a `·name(SB)` symbol — i.e. anything outside [A-Za-z0-9_/.] (the dotted
    // package-path form transforms `/`→`∕`, `.`→`·`, but cannot represent
    // `-` (hyphenated paths) or method symbols' `(`,`*`,`)`). Path (a) burns
    // the symbol directly into the .s, so only fully-transformable targets may
    // take it; everything else needs the clean-alias bridge.
    auto plan9Direct = [](StringRef T) {
      for (char C : T)
        if (!(std::isalnum((unsigned char)C) || C == '_' || C == '/' ||
              C == '.'))
          return false;
      return true;
    };
    // Direct (path a) — referenced raw in the .s, no Go-side bridge — only
    // when the bound symbol is GoABI0-reachable by name: a clean-named
    // variable (data has no ABI), or a function explicitly marked C2GO_GOABI0
    // (the target provides an ABI0 entry). A function WITHOUT C2GO_GOABI0
    // imports an external ABIInternal Go symbol and needs the alias-then-wrap
    // stub even when its name is clean; a '-'/method name always needs the
    // local-symbol path. Mirrors handleC2GoLinknameAttr's AsmLabel routing.
    bool VarKind = isa<VarDecl>(D);
    bool Direct = plan9Direct(Target) && (VarKind || LA->getHasAbi0() != 0);
    if (Direct) continue; // path (a), nothing to bridge
    const Decl *Canonical = D->getCanonicalDecl();
    if (!LinknameEmitted.insert(Canonical).second) continue;

    llvm::json::Object Bridge;
    if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
      Bridge["name"] = FD->getNameAsString();
      Bridge["kind"] = "func";
      Bridge["go_sig"] = c2goBuildGoSig(FD, Ctx);
      // Import vs export direction. When no definition exists in this TU the
      // function body lives in another package and the .s only CALLs the
      // sanitised local symbol. On Go 1.25 a bodyless `//go:linkname` no
      // longer satisfies a .s-referenced symbol (same breakage the runtime
      // helper preamble works around), so c2gobind must emit an
      // alias-then-wrap stub rather than a bodyless bridge. A definition
      // present here is the export direction: the .s provides the body and
      // the plain bodyless bridge is correct.
      Bridge["imported"] = (FD->getDefinition() == nullptr);
    } else if (const auto *VD = dyn_cast<VarDecl>(D)) {
      bool Unmanaged = VD->hasAttr<C2GoUnmanagedAttr>();
      Bridge["name"] = VD->getNameAsString();
      Bridge["kind"] = "var";
      Bridge["go_type"] = c2goMapType(VD->getType(), Ctx, Unmanaged);
      Bridge["imported"] = (VD->getDefinition() == nullptr);
    } else {
      continue;
    }
    Bridge["linkname"] = Target.str();
    Bridge["asm_symbol"] = "\xc2\xb7" + sanitiseToIdent(Target.str());
    Linknames.push_back(std::move(Bridge));
  }
  llvm::sort(Linknames, byName);

  // c2go #533: callbacks[] — one entry per distinct c2go_callback(fn) target.
  // ActOnC2GoCallback synthesized a bodyless `c2go_cb_<name>` trampoline decl
  // carrying a C2GoCallback attr holding the target FunctionDecl. c2gobind emits
  // the per-fn cdecl trampoline .s (mechanical C-ABI spill + crosscall2); clang
  // emits the per-fn GoABI0 converter (#536). The trampoline's address is what
  // the extern library calls; the converter re-enters the target's ABI0 entry.
  {
    llvm::json::Array Callbacks;
    llvm::DenseSet<const Decl *> SeenCB;
    for (const Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
      const auto *FD = dyn_cast<FunctionDecl>(D);
      if (!FD)
        continue;
      const auto *CB = FD->getAttr<C2GoCallbackAttr>();
      if (!CB || !CB->getTarget())
        continue;
      const FunctionDecl *Target = CB->getTarget();
      if (!SeenCB.insert(Target->getCanonicalDecl()).second)
        continue;
      std::string TName = Target->getNameAsString();
      llvm::json::Object E;
      E["tramp"] = "c2go_cb_" + TName;             // extern fn-ptr target symbol
      E["target_asm_symbol"] = "\xc2\xb7" + TName; // destFn Go ABI0 entry
      E["converter"] = "c2go_cbconv_" + TName;     // per-fn GoABI0 converter (unix, #536)
      // windows (#541): c2gobind generates a typed Go converter (the
      // syscall.NewCallback fn) from this go_sig; the flags let it fail-closed
      // on signatures NewCallback rejects (float args/returns, >uintptr struct).
      E["go_sig"] = c2goBuildGoSig(Target, Ctx);
      {
        bool HasFloat = false, HasAggregate = false;
        auto Chk = [&](QualType QT) {
          QT = QT.getCanonicalType();
          if (QT->isVoidType())
            return;
          if (QT->isFloatingType() || QT->isAnyComplexType() ||
              QT->isVectorType())
            HasFloat = true;
          if (QT->isRecordType())
            HasAggregate = true;
        };
        for (auto *PVD : Target->parameters())
          Chk(PVD->getType());
        Chk(Target->getReturnType());
        if (HasFloat)
          E["has_float"] = true;
        if (HasAggregate)
          E["has_aggregate"] = true;
      }
      // Multi-word struct return: emit the per-word int/float classes so the
      // cdecl trampoline reads result[k] into the right native return register.
      // (Single-word returns use the uniform result[0]->R0+F0 path, no list.)
      if (CGM) {
        if (auto RW = CGM->c2goCallbackReturnWords(Target);
            RW && RW->size() > 1) {
          llvm::json::Array Words;
          for (bool F : *RW)
            Words.push_back(F ? "float" : "int");
          E["ret_words"] = std::move(Words);
        }
      }
      Callbacks.push_back(std::move(E));
    }
    if (!Callbacks.empty())
      Root["callbacks"] = std::move(Callbacks);
  }

  Root["symbols"] = std::move(Symbols);
  Root["types"] = std::move(Types);
  Root["linknames"] = std::move(Linknames);

  // c2go §B4 phase 2: surface the per-global GC pointer-mask bitmaps
  // emitted by CodeGenModule::emitC2GoGlobalGCMask. c2gobind reads this
  // section to build a Go-side data table that phase 3 will register as
  // a synthetic moduledata entry on `runtime.activeModules()`. Always
  // emit the section (even when empty) so consumers can distinguish
  // "no managed globals in this TU" from "older clang that doesn't
  // know about §B4".
  {
    llvm::json::Object ModGC;
    ModGC["vars"] = collectC2GoModuleGCMaskVars(Mod);
    Root["module_gcmask"] = std::move(ModGC);
  }
  return Root;
}

// c2go (WF2 unification): embed the WF1 manifest JSON verbatim into the module
// as a `c2go.manifest.json` named-metadata operand. c2go-lto extracts and merges
// these instead of reconstructing the manifest field-by-field from per-symbol
// metadata (which silently drifts from this builder — e.g. cabi/callbacks/var-
// linknames/imported were all lost). Each TU stamps one operand; llvm-link
// appends them across TUs, so the Composite carries one operand per linked TU and
// c2go-lto merges them. Stamped whenever c2go mode is on (independent of whether
// a manifest FILE is requested), so a plain `-emit-llvm-bc` carries it for WF2.
static void embedC2GoManifest(llvm::Module &M, const llvm::json::Object &Root) {
  llvm::json::Object Copy(Root);
  llvm::json::Value V(std::move(Copy));
  std::string S;
  llvm::raw_string_ostream(S) << V; // compact; c2go-lto re-pretty-prints to match
  llvm::LLVMContext &Ctx = M.getContext();
  llvm::NamedMDNode *NMD = M.getOrInsertNamedMetadata("c2go.manifest.json");
  NMD->addOperand(llvm::MDNode::get(Ctx, llvm::MDString::get(Ctx, S)));
}

// writeC2GoManifest serializes the prebuilt manifest to `OutPath`.
static void writeC2GoManifest(const llvm::json::Object &Root,
                              StringRef OutPath, DiagnosticsEngine &Diags) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(OutPath, EC);
  if (EC) {
    Diags.Report(diag::err_fe_error_opening) << OutPath << EC.message();
    return;
  }
  // formatv requires a non-const Value, so copy. Watch out for:
  //   * `Value V(Object(Root))` — most-vexing-parse, becomes a fn decl.
  //   * `Value V{Object(Root)}` — matches Value's initializer-list
  //     constructor, producing a JSON ARRAY containing one element.
  // Solution: name the temporary explicitly.
  llvm::json::Object Copy(Root);
  llvm::json::Value V(std::move(Copy));
  OS << llvm::formatv("{0:2}", V) << "\n";
}

void BackendConsumer::HandleTranslationUnit(ASTContext &C) {
  {
    llvm::TimeTraceScope TimeScope("Frontend");
    PrettyStackTraceString CrashInfo("Per-file LLVM IR generation");
    if (TimerIsEnabled && !LLVMIRGenerationRefCount++)
      CI.getFrontendTimer().yieldTo(LLVMIRGeneration);

    Gen->HandleTranslationUnit(C);

    if (TimerIsEnabled && !--LLVMIRGenerationRefCount)
      LLVMIRGeneration.yieldTo(CI.getFrontendTimer());
  }

  // c2go: build the sidecar manifest JSON while the AST is still live
  // (ClearASTBeforeBackend wipes it below). Keep the object alive so
  // the Plan 9 emit pass at the end of HandleTranslationUnit can use
  // it. Skip the whole manifest path when an error already occurred: the
  // output is invalid anyway, and walking a half-formed AST (e.g. an
  // error-invalidated function definition, like defining an `unmanaged extern`
  // import) through the manifest builder can crash.
  llvm::json::Object C2GoManifest;
  bool C2GoManifestBuilt = false;
  if (CI.getLangOpts().C2GoMode && !Diags.hasErrorOccurred()) {
    // §B4 phase 2: pass the LLVM module so buildC2GoManifest can
    // scoop up the `@c2go.global.gcmask.<var>` bitmaps emitted by
    // CodeGenModule::emitC2GoGlobalGCMask into the manifest's
    // `module_gcmask` section.
    C2GoManifest =
        buildC2GoManifest(C, CI.getLangOpts(), Diags, getModule(), &Gen->CGM());
    C2GoManifestBuilt = true;
    // Carry the manifest into the bitcode so the WF2 path (c2go-lto) reads it
    // back verbatim rather than reconstructing (and drifting from) it.
    embedC2GoManifest(*getModule(), C2GoManifest);
    if (!CI.getLangOpts().C2GoEmitManifestPath.empty())
      writeC2GoManifest(C2GoManifest,
                        CI.getLangOpts().C2GoEmitManifestPath, Diags);
  }

  // Silently ignore if we weren't initialized for some reason.
  if (!getModule())
    return;

  LLVMContext &Ctx = getModule()->getContext();
  std::unique_ptr<DiagnosticHandler> OldDiagnosticHandler =
    Ctx.getDiagnosticHandler();
  Ctx.setDiagnosticHandler(std::make_unique<ClangDiagnosticHandler>(
      CodeGenOpts, this));

  Ctx.setDefaultTargetCPU(TargetOpts.CPU);
  Ctx.setDefaultTargetFeatures(llvm::join(TargetOpts.Features, ","));

  Expected<LLVMRemarkFileHandle> OptRecordFileOrErr =
      setupLLVMOptimizationRemarks(
          Ctx, CodeGenOpts.OptRecordFile, CodeGenOpts.OptRecordPasses,
          CodeGenOpts.OptRecordFormat, CodeGenOpts.DiagnosticsWithHotness,
          CodeGenOpts.DiagnosticsHotnessThreshold);

  if (Error E = OptRecordFileOrErr.takeError()) {
    reportOptRecordError(std::move(E), Diags, CodeGenOpts);
    return;
  }

  LLVMRemarkFileHandle OptRecordFile = std::move(*OptRecordFileOrErr);

  if (OptRecordFile && CodeGenOpts.getProfileUse() !=
                           llvm::driver::ProfileInstrKind::ProfileNone)
    Ctx.setDiagnosticsHotnessRequested(true);

  if (CodeGenOpts.MisExpect) {
    Ctx.setMisExpectWarningRequested(true);
  }

  if (CodeGenOpts.DiagnosticsMisExpectTolerance) {
    Ctx.setDiagnosticsMisExpectTolerance(
      CodeGenOpts.DiagnosticsMisExpectTolerance);
  }

  // Link each LinkModule into our module.
  if (!CodeGenOpts.LinkBitcodePostopt && LinkInModules(getModule()))
    return;

  for (auto &F : getModule()->functions()) {
    if (const Decl *FD = Gen->GetDeclForMangledName(F.getName())) {
      auto Loc = FD->getASTContext().getFullLoc(FD->getLocation());
      // TODO: use a fast content hash when available.
      auto NameHash = llvm::hash_value(F.getName());
      ManglingFullSourceLocs.push_back(std::make_pair(NameHash, Loc));
    }
  }

  if (CodeGenOpts.ClearASTBeforeBackend) {
    LLVM_DEBUG(llvm::dbgs() << "Clearing AST...\n");
    // Access to the AST is no longer available after this.
    // Other things that the ASTContext manages are still available, e.g.
    // the SourceManager. It'd be nice if we could separate out all the
    // things in ASTContext used after this point and null out the
    // ASTContext, but too many various parts of the ASTContext are still
    // used in various parts.
    C.cleanup();
    C.getAllocator().Reset();
  }

  EmbedBitcode(getModule(), CodeGenOpts, llvm::MemoryBufferRef());

  // c2go Phase E v0+1 step 4 (#95), #376: enqueue boundary-symbol metadata
  // (kind=func) onto MCPlan9AsmStreamer's thread_local pending queue. The
  // streamer constructor (inside emitBackendOutput below) drains the queue
  // into its per-instance C2GoFnMeta. emitLabel can then emit the correct
  // `TEXT … $0-argsize` directive for each c2go_extern. We always set
  // framesize=0 (NOFRAME — the c2go arm64 prologue manages its own frame
  // explicitly). RAII drain via scope_exit so a thrown emitBackendOutput
  // doesn't leak metadata into the next call.
  auto ClearMetadataGuard = llvm::make_scope_exit([&] {
    if (C2GoManifestBuilt &&
        !CI.getLangOpts().C2GoEmitPlan9AsmPath.empty()) {
      (void)llvm::MCPlan9AsmStreamer::drainPendingC2GoBoundaries();
      // c2go #387: defensive drain in case emitBackendOutput threw
      // before constructing the streamer.
      (void)llvm::MCPlan9AsmStreamer::drainPendingC2GoGoOwnedGlobals();
    }
  });
  if (C2GoManifestBuilt &&
      !CI.getLangOpts().C2GoEmitPlan9AsmPath.empty()) {
    // Drain any stale entries first (defensive — should be empty here).
    (void)llvm::MCPlan9AsmStreamer::drainPendingC2GoBoundaries();
    (void)llvm::MCPlan9AsmStreamer::drainPendingC2GoGoOwnedGlobals();
    if (const llvm::json::Array *Symbols =
            C2GoManifest.getArray("symbols")) {
      for (const llvm::json::Value &SV : *Symbols) {
        const llvm::json::Object *S = SV.getAsObject();
        if (!S) continue;
        std::optional<llvm::StringRef> Kind = S->getString("kind");
        if (!Kind || *Kind != "func") continue;
        std::optional<llvm::StringRef> Name = S->getString("name");
        if (!Name) continue;
        int64_t ArgSize = S->getInteger("argsize").value_or(0);
        llvm::C2GoFunctionMetadata M;
        M.Name = std::string(*Name);
        M.FrameSize = 0;
        M.ArgSize = (int)ArgSize;
        llvm::MCPlan9AsmStreamer::enqueueC2GoBoundary(std::move(M));
      }
    }
    // c2go #387 §B4 phase 6 sub-step 1: enqueue every Go-owned global
    // name from the manifest's module_gcmask section so the streamer
    // skips its GLOBL trailer. Names emitted at sub-step 1 are bare C
    // identifiers (e.g. "gRing"); the streamer matches them against
    // the bare suffix of the rendered Plan 9 symbol ("·gRing").
    if (const llvm::json::Object *MG = C2GoManifest.getObject("module_gcmask")) {
      if (const llvm::json::Array *Vars = MG->getArray("vars")) {
        for (const llvm::json::Value &VV : *Vars) {
          const llvm::json::Object *V = VV.getAsObject();
          if (!V) continue;
          std::optional<bool> GoOwned = V->getBoolean("go_owned");
          if (!GoOwned || !*GoOwned) continue;
          std::optional<llvm::StringRef> NameSR = V->getString("name");
          if (!NameSR) continue;
          llvm::MCPlan9AsmStreamer::enqueueC2GoGoOwnedGlobal(*NameSR);
        }
      }
    }
  }

  emitBackendOutput(CI, CI.getCodeGenOpts(),
                    C.getTargetInfo().getDataLayoutString(), getModule(),
                    Action, FS, std::move(AsmOutStream), this);

  Ctx.setDiagnosticHandler(std::move(OldDiagnosticHandler));

  if (OptRecordFile)
    OptRecordFile->keep();
}

void BackendConsumer::HandleTagDeclDefinition(TagDecl *D) {
  PrettyStackTraceDecl CrashInfo(D, SourceLocation(),
                                 Context->getSourceManager(),
                                 "LLVM IR generation of declaration");
  Gen->HandleTagDeclDefinition(D);
}

void BackendConsumer::HandleTagDeclRequiredDefinition(const TagDecl *D) {
  Gen->HandleTagDeclRequiredDefinition(D);
}

void BackendConsumer::CompleteTentativeDefinition(VarDecl *D) {
  Gen->CompleteTentativeDefinition(D);
}

void BackendConsumer::CompleteExternalDeclaration(DeclaratorDecl *D) {
  Gen->CompleteExternalDeclaration(D);
}

void BackendConsumer::AssignInheritanceModel(CXXRecordDecl *RD) {
  Gen->AssignInheritanceModel(RD);
}

void BackendConsumer::HandleVTable(CXXRecordDecl *RD) {
  Gen->HandleVTable(RD);
}

void BackendConsumer::anchor() { }

} // namespace clang

bool ClangDiagnosticHandler::handleDiagnostics(const DiagnosticInfo &DI) {
  BackendCon->DiagnosticHandlerImpl(DI);
  return true;
}

/// ConvertBackendLocation - Convert a location in a temporary llvm::SourceMgr
/// buffer to be a valid FullSourceLoc.
static FullSourceLoc ConvertBackendLocation(const llvm::SMDiagnostic &D,
                                            SourceManager &CSM) {
  // Get both the clang and llvm source managers.  The location is relative to
  // a memory buffer that the LLVM Source Manager is handling, we need to add
  // a copy to the Clang source manager.
  const llvm::SourceMgr &LSM = *D.getSourceMgr();

  // We need to copy the underlying LLVM memory buffer because llvm::SourceMgr
  // already owns its one and clang::SourceManager wants to own its one.
  const MemoryBuffer *LBuf =
  LSM.getMemoryBuffer(LSM.FindBufferContainingLoc(D.getLoc()));

  // Create the copy and transfer ownership to clang::SourceManager.
  // TODO: Avoid copying files into memory.
  std::unique_ptr<llvm::MemoryBuffer> CBuf =
      llvm::MemoryBuffer::getMemBufferCopy(LBuf->getBuffer(),
                                           LBuf->getBufferIdentifier());
  // FIXME: Keep a file ID map instead of creating new IDs for each location.
  FileID FID = CSM.createFileID(std::move(CBuf));

  // Translate the offset into the file.
  unsigned Offset = D.getLoc().getPointer() - LBuf->getBufferStart();
  SourceLocation NewLoc =
  CSM.getLocForStartOfFile(FID).getLocWithOffset(Offset);
  return FullSourceLoc(NewLoc, CSM);
}

#define ComputeDiagID(Severity, GroupName, DiagID)                             \
  do {                                                                         \
    switch (Severity) {                                                        \
    case llvm::DS_Error:                                                       \
      DiagID = diag::err_fe_##GroupName;                                       \
      break;                                                                   \
    case llvm::DS_Warning:                                                     \
      DiagID = diag::warn_fe_##GroupName;                                      \
      break;                                                                   \
    case llvm::DS_Remark:                                                      \
      llvm_unreachable("'remark' severity not expected");                      \
      break;                                                                   \
    case llvm::DS_Note:                                                        \
      DiagID = diag::note_fe_##GroupName;                                      \
      break;                                                                   \
    }                                                                          \
  } while (false)

#define ComputeDiagRemarkID(Severity, GroupName, DiagID)                       \
  do {                                                                         \
    switch (Severity) {                                                        \
    case llvm::DS_Error:                                                       \
      DiagID = diag::err_fe_##GroupName;                                       \
      break;                                                                   \
    case llvm::DS_Warning:                                                     \
      DiagID = diag::warn_fe_##GroupName;                                      \
      break;                                                                   \
    case llvm::DS_Remark:                                                      \
      DiagID = diag::remark_fe_##GroupName;                                    \
      break;                                                                   \
    case llvm::DS_Note:                                                        \
      DiagID = diag::note_fe_##GroupName;                                      \
      break;                                                                   \
    }                                                                          \
  } while (false)

void BackendConsumer::SrcMgrDiagHandler(const llvm::DiagnosticInfoSrcMgr &DI) {
  const llvm::SMDiagnostic &D = DI.getSMDiag();

  unsigned DiagID;
  if (DI.isInlineAsmDiag())
    ComputeDiagID(DI.getSeverity(), inline_asm, DiagID);
  else
    ComputeDiagID(DI.getSeverity(), source_mgr, DiagID);

  // This is for the empty BackendConsumer that uses the clang diagnostic
  // handler for IR input files.
  if (!Context) {
    D.print(nullptr, llvm::errs());
    Diags.Report(DiagID).AddString("cannot compile inline asm");
    return;
  }

  // There are a couple of different kinds of errors we could get here.
  // First, we re-format the SMDiagnostic in terms of a clang diagnostic.

  // Strip "error: " off the start of the message string.
  StringRef Message = D.getMessage();
  (void)Message.consume_front("error: ");

  // If the SMDiagnostic has an inline asm source location, translate it.
  FullSourceLoc Loc;
  if (D.getLoc() != SMLoc())
    Loc = ConvertBackendLocation(D, Context->getSourceManager());

  // If this problem has clang-level source location information, report the
  // issue in the source with a note showing the instantiated
  // code.
  if (DI.isInlineAsmDiag()) {
    SourceLocation LocCookie =
        SourceLocation::getFromRawEncoding(DI.getLocCookie());
    if (LocCookie.isValid()) {
      Diags.Report(LocCookie, DiagID).AddString(Message);

      if (D.getLoc().isValid()) {
        DiagnosticBuilder B = Diags.Report(Loc, diag::note_fe_inline_asm_here);
        // Convert the SMDiagnostic ranges into SourceRange and attach them
        // to the diagnostic.
        for (const std::pair<unsigned, unsigned> &Range : D.getRanges()) {
          unsigned Column = D.getColumnNo();
          B << SourceRange(Loc.getLocWithOffset(Range.first - Column),
                           Loc.getLocWithOffset(Range.second - Column));
        }
      }
      return;
    }
  }

  // Otherwise, report the backend issue as occurring in the generated .s file.
  // If Loc is invalid, we still need to report the issue, it just gets no
  // location info.
  Diags.Report(Loc, DiagID).AddString(Message);
}

bool
BackendConsumer::InlineAsmDiagHandler(const llvm::DiagnosticInfoInlineAsm &D) {
  unsigned DiagID;
  ComputeDiagID(D.getSeverity(), inline_asm, DiagID);
  std::string Message = D.getMsgStr().str();

  // If this problem has clang-level source location information, report the
  // issue as being a problem in the source with a note showing the instantiated
  // code.
  SourceLocation LocCookie =
      SourceLocation::getFromRawEncoding(D.getLocCookie());
  if (LocCookie.isValid())
    Diags.Report(LocCookie, DiagID).AddString(Message);
  else {
    // Otherwise, report the backend diagnostic as occurring in the generated
    // .s file.
    // If Loc is invalid, we still need to report the diagnostic, it just gets
    // no location info.
    FullSourceLoc Loc;
    Diags.Report(Loc, DiagID).AddString(Message);
  }
  // We handled all the possible severities.
  return true;
}

bool
BackendConsumer::StackSizeDiagHandler(const llvm::DiagnosticInfoStackSize &D) {
  if (D.getSeverity() != llvm::DS_Warning)
    // For now, the only support we have for StackSize diagnostic is warning.
    // We do not know how to format other severities.
    return false;

  auto Loc = getFunctionSourceLocation(D.getFunction());
  if (!Loc)
    return false;

  Diags.Report(*Loc, diag::warn_fe_frame_larger_than)
      << D.getStackSize() << D.getStackLimit()
      << llvm::demangle(D.getFunction().getName());
  return true;
}

bool BackendConsumer::ResourceLimitDiagHandler(
    const llvm::DiagnosticInfoResourceLimit &D) {
  auto Loc = getFunctionSourceLocation(D.getFunction());
  if (!Loc)
    return false;
  unsigned DiagID = diag::err_fe_backend_resource_limit;
  ComputeDiagID(D.getSeverity(), backend_resource_limit, DiagID);

  Diags.Report(*Loc, DiagID)
      << D.getResourceName() << D.getResourceSize() << D.getResourceLimit()
      << llvm::demangle(D.getFunction().getName());
  return true;
}

const FullSourceLoc BackendConsumer::getBestLocationFromDebugLoc(
    const llvm::DiagnosticInfoWithLocationBase &D, bool &BadDebugInfo,
    StringRef &Filename, unsigned &Line, unsigned &Column) const {
  SourceManager &SourceMgr = Context->getSourceManager();
  FileManager &FileMgr = SourceMgr.getFileManager();
  SourceLocation DILoc;

  if (D.isLocationAvailable()) {
    D.getLocation(Filename, Line, Column);
    if (Line > 0) {
      auto FE = FileMgr.getOptionalFileRef(Filename);
      if (!FE)
        FE = FileMgr.getOptionalFileRef(D.getAbsolutePath());
      if (FE) {
        // If -gcolumn-info was not used, Column will be 0. This upsets the
        // source manager, so pass 1 if Column is not set.
        DILoc = SourceMgr.translateFileLineCol(*FE, Line, Column ? Column : 1);
      }
    }
    BadDebugInfo = DILoc.isInvalid();
  }

  // If a location isn't available, try to approximate it using the associated
  // function definition. We use the definition's right brace to differentiate
  // from diagnostics that genuinely relate to the function itself.
  FullSourceLoc Loc(DILoc, SourceMgr);
  if (Loc.isInvalid()) {
    if (auto MaybeLoc = getFunctionSourceLocation(D.getFunction()))
      Loc = *MaybeLoc;
  }

  if (DILoc.isInvalid() && D.isLocationAvailable())
    // If we were not able to translate the file:line:col information
    // back to a SourceLocation, at least emit a note stating that
    // we could not translate this location. This can happen in the
    // case of #line directives.
    Diags.Report(Loc, diag::note_fe_backend_invalid_loc)
        << Filename << Line << Column;

  return Loc;
}

std::optional<FullSourceLoc>
BackendConsumer::getFunctionSourceLocation(const Function &F) const {
  auto Hash = llvm::hash_value(F.getName());
  for (const auto &Pair : ManglingFullSourceLocs) {
    if (Pair.first == Hash)
      return Pair.second;
  }
  return std::nullopt;
}

void BackendConsumer::UnsupportedDiagHandler(
    const llvm::DiagnosticInfoUnsupported &D) {
  // We only support warnings or errors.
  assert(D.getSeverity() == llvm::DS_Error ||
         D.getSeverity() == llvm::DS_Warning);

  StringRef Filename;
  unsigned Line, Column;
  bool BadDebugInfo = false;
  FullSourceLoc Loc;
  std::string Msg;
  raw_string_ostream MsgStream(Msg);

  // Context will be nullptr for IR input files, we will construct the diag
  // message from llvm::DiagnosticInfoUnsupported.
  if (Context != nullptr) {
    Loc = getBestLocationFromDebugLoc(D, BadDebugInfo, Filename, Line, Column);
    MsgStream << D.getMessage();
  } else {
    DiagnosticPrinterRawOStream DP(MsgStream);
    D.print(DP);
  }

  auto DiagType = D.getSeverity() == llvm::DS_Error
                      ? diag::err_fe_backend_unsupported
                      : diag::warn_fe_backend_unsupported;
  Diags.Report(Loc, DiagType) << Msg;

  if (BadDebugInfo)
    // If we were not able to translate the file:line:col information
    // back to a SourceLocation, at least emit a note stating that
    // we could not translate this location. This can happen in the
    // case of #line directives.
    Diags.Report(Loc, diag::note_fe_backend_invalid_loc)
        << Filename << Line << Column;
}

void BackendConsumer::EmitOptimizationMessage(
    const llvm::DiagnosticInfoOptimizationBase &D, unsigned DiagID) {
  // We only support warnings and remarks.
  assert(D.getSeverity() == llvm::DS_Remark ||
         D.getSeverity() == llvm::DS_Warning);

  StringRef Filename;
  unsigned Line, Column;
  bool BadDebugInfo = false;
  FullSourceLoc Loc;
  std::string Msg;
  raw_string_ostream MsgStream(Msg);

  // Context will be nullptr for IR input files, we will construct the remark
  // message from llvm::DiagnosticInfoOptimizationBase.
  if (Context != nullptr) {
    Loc = getBestLocationFromDebugLoc(D, BadDebugInfo, Filename, Line, Column);
    MsgStream << D.getMsg();
  } else {
    DiagnosticPrinterRawOStream DP(MsgStream);
    D.print(DP);
  }

  if (D.getHotness())
    MsgStream << " (hotness: " << *D.getHotness() << ")";

  Diags.Report(Loc, DiagID) << AddFlagValue(D.getPassName()) << Msg;

  if (BadDebugInfo)
    // If we were not able to translate the file:line:col information
    // back to a SourceLocation, at least emit a note stating that
    // we could not translate this location. This can happen in the
    // case of #line directives.
    Diags.Report(Loc, diag::note_fe_backend_invalid_loc)
        << Filename << Line << Column;
}

void BackendConsumer::OptimizationRemarkHandler(
    const llvm::DiagnosticInfoOptimizationBase &D) {
  // Without hotness information, don't show noisy remarks.
  if (D.isVerbose() && !D.getHotness())
    return;

  if (D.isPassed()) {
    // Optimization remarks are active only if the -Rpass flag has a regular
    // expression that matches the name of the pass name in \p D.
    if (CodeGenOpts.OptimizationRemark.patternMatches(D.getPassName()))
      EmitOptimizationMessage(D, diag::remark_fe_backend_optimization_remark);
  } else if (D.isMissed()) {
    // Missed optimization remarks are active only if the -Rpass-missed
    // flag has a regular expression that matches the name of the pass
    // name in \p D.
    if (CodeGenOpts.OptimizationRemarkMissed.patternMatches(D.getPassName()))
      EmitOptimizationMessage(
          D, diag::remark_fe_backend_optimization_remark_missed);
  } else {
    assert(D.isAnalysis() && "Unknown remark type");

    bool ShouldAlwaysPrint = false;
    if (auto *ORA = dyn_cast<llvm::OptimizationRemarkAnalysis>(&D))
      ShouldAlwaysPrint = ORA->shouldAlwaysPrint();

    if (ShouldAlwaysPrint ||
        CodeGenOpts.OptimizationRemarkAnalysis.patternMatches(D.getPassName()))
      EmitOptimizationMessage(
          D, diag::remark_fe_backend_optimization_remark_analysis);
  }
}

void BackendConsumer::OptimizationRemarkHandler(
    const llvm::OptimizationRemarkAnalysisFPCommute &D) {
  // Optimization analysis remarks are active if the pass name is set to
  // llvm::DiagnosticInfo::AlwasyPrint or if the -Rpass-analysis flag has a
  // regular expression that matches the name of the pass name in \p D.

  if (D.shouldAlwaysPrint() ||
      CodeGenOpts.OptimizationRemarkAnalysis.patternMatches(D.getPassName()))
    EmitOptimizationMessage(
        D, diag::remark_fe_backend_optimization_remark_analysis_fpcommute);
}

void BackendConsumer::OptimizationRemarkHandler(
    const llvm::OptimizationRemarkAnalysisAliasing &D) {
  // Optimization analysis remarks are active if the pass name is set to
  // llvm::DiagnosticInfo::AlwasyPrint or if the -Rpass-analysis flag has a
  // regular expression that matches the name of the pass name in \p D.

  if (D.shouldAlwaysPrint() ||
      CodeGenOpts.OptimizationRemarkAnalysis.patternMatches(D.getPassName()))
    EmitOptimizationMessage(
        D, diag::remark_fe_backend_optimization_remark_analysis_aliasing);
}

void BackendConsumer::OptimizationFailureHandler(
    const llvm::DiagnosticInfoOptimizationFailure &D) {
  EmitOptimizationMessage(D, diag::warn_fe_backend_optimization_failure);
}

void BackendConsumer::DontCallDiagHandler(const DiagnosticInfoDontCall &D) {
  SourceLocation LocCookie =
      SourceLocation::getFromRawEncoding(D.getLocCookie());

  // FIXME: we can't yet diagnose indirect calls. When/if we can, we
  // should instead assert that LocCookie.isValid().
  if (!LocCookie.isValid())
    return;

  Diags.Report(LocCookie, D.getSeverity() == DiagnosticSeverity::DS_Error
                              ? diag::err_fe_backend_error_attr
                              : diag::warn_fe_backend_warning_attr)
      << llvm::demangle(D.getFunctionName()) << D.getNote();
}

void BackendConsumer::MisExpectDiagHandler(
    const llvm::DiagnosticInfoMisExpect &D) {
  StringRef Filename;
  unsigned Line, Column;
  bool BadDebugInfo = false;
  FullSourceLoc Loc =
      getBestLocationFromDebugLoc(D, BadDebugInfo, Filename, Line, Column);

  Diags.Report(Loc, diag::warn_profile_data_misexpect) << D.getMsg().str();

  if (BadDebugInfo)
    // If we were not able to translate the file:line:col information
    // back to a SourceLocation, at least emit a note stating that
    // we could not translate this location. This can happen in the
    // case of #line directives.
    Diags.Report(Loc, diag::note_fe_backend_invalid_loc)
        << Filename << Line << Column;
}

/// This function is invoked when the backend needs
/// to report something to the user.
void BackendConsumer::DiagnosticHandlerImpl(const DiagnosticInfo &DI) {
  unsigned DiagID = diag::err_fe_inline_asm;
  llvm::DiagnosticSeverity Severity = DI.getSeverity();
  // Get the diagnostic ID based.
  switch (DI.getKind()) {
  case llvm::DK_InlineAsm:
    if (InlineAsmDiagHandler(cast<DiagnosticInfoInlineAsm>(DI)))
      return;
    ComputeDiagID(Severity, inline_asm, DiagID);
    break;
  case llvm::DK_SrcMgr:
    SrcMgrDiagHandler(cast<DiagnosticInfoSrcMgr>(DI));
    return;
  case llvm::DK_StackSize:
    if (StackSizeDiagHandler(cast<DiagnosticInfoStackSize>(DI)))
      return;
    ComputeDiagID(Severity, backend_frame_larger_than, DiagID);
    break;
  case llvm::DK_ResourceLimit:
    if (ResourceLimitDiagHandler(cast<DiagnosticInfoResourceLimit>(DI)))
      return;
    ComputeDiagID(Severity, backend_resource_limit, DiagID);
    break;
  case DK_Linker:
    ComputeDiagID(Severity, linking_module, DiagID);
    break;
  case llvm::DK_OptimizationRemark:
    // Optimization remarks are always handled completely by this
    // handler. There is no generic way of emitting them.
    OptimizationRemarkHandler(cast<OptimizationRemark>(DI));
    return;
  case llvm::DK_OptimizationRemarkMissed:
    // Optimization remarks are always handled completely by this
    // handler. There is no generic way of emitting them.
    OptimizationRemarkHandler(cast<OptimizationRemarkMissed>(DI));
    return;
  case llvm::DK_OptimizationRemarkAnalysis:
    // Optimization remarks are always handled completely by this
    // handler. There is no generic way of emitting them.
    OptimizationRemarkHandler(cast<OptimizationRemarkAnalysis>(DI));
    return;
  case llvm::DK_OptimizationRemarkAnalysisFPCommute:
    // Optimization remarks are always handled completely by this
    // handler. There is no generic way of emitting them.
    OptimizationRemarkHandler(cast<OptimizationRemarkAnalysisFPCommute>(DI));
    return;
  case llvm::DK_OptimizationRemarkAnalysisAliasing:
    // Optimization remarks are always handled completely by this
    // handler. There is no generic way of emitting them.
    OptimizationRemarkHandler(cast<OptimizationRemarkAnalysisAliasing>(DI));
    return;
  case llvm::DK_MachineOptimizationRemark:
    // Optimization remarks are always handled completely by this
    // handler. There is no generic way of emitting them.
    OptimizationRemarkHandler(cast<MachineOptimizationRemark>(DI));
    return;
  case llvm::DK_MachineOptimizationRemarkMissed:
    // Optimization remarks are always handled completely by this
    // handler. There is no generic way of emitting them.
    OptimizationRemarkHandler(cast<MachineOptimizationRemarkMissed>(DI));
    return;
  case llvm::DK_MachineOptimizationRemarkAnalysis:
    // Optimization remarks are always handled completely by this
    // handler. There is no generic way of emitting them.
    OptimizationRemarkHandler(cast<MachineOptimizationRemarkAnalysis>(DI));
    return;
  case llvm::DK_OptimizationFailure:
    // Optimization failures are always handled completely by this
    // handler.
    OptimizationFailureHandler(cast<DiagnosticInfoOptimizationFailure>(DI));
    return;
  case llvm::DK_Unsupported:
    UnsupportedDiagHandler(cast<DiagnosticInfoUnsupported>(DI));
    return;
  case llvm::DK_DontCall:
    DontCallDiagHandler(cast<DiagnosticInfoDontCall>(DI));
    return;
  case llvm::DK_MisExpect:
    MisExpectDiagHandler(cast<DiagnosticInfoMisExpect>(DI));
    return;
  default:
    // Plugin IDs are not bound to any value as they are set dynamically.
    ComputeDiagRemarkID(Severity, backend_plugin, DiagID);
    break;
  }
  std::string MsgStorage;
  {
    raw_string_ostream Stream(MsgStorage);
    DiagnosticPrinterRawOStream DP(Stream);
    DI.print(DP);
  }

  if (DI.getKind() == DK_Linker) {
    assert(CurLinkModule && "CurLinkModule must be set for linker diagnostics");
    Diags.Report(DiagID) << CurLinkModule->getModuleIdentifier() << MsgStorage;
    return;
  }

  // Report the backend message using the usual diagnostic mechanism.
  FullSourceLoc Loc;
  Diags.Report(Loc, DiagID).AddString(MsgStorage);
}
#undef ComputeDiagID

CodeGenAction::CodeGenAction(unsigned _Act, LLVMContext *_VMContext)
    : Act(_Act), VMContext(_VMContext ? _VMContext : new LLVMContext),
      OwnsVMContext(!_VMContext) {}

CodeGenAction::~CodeGenAction() {
  TheModule.reset();
  if (OwnsVMContext)
    delete VMContext;
}

bool CodeGenAction::loadLinkModules(CompilerInstance &CI) {
  if (!LinkModules.empty())
    return false;

  for (const CodeGenOptions::BitcodeFileToLink &F :
       CI.getCodeGenOpts().LinkBitcodeFiles) {
    auto BCBuf = CI.getFileManager().getBufferForFile(F.Filename);
    if (!BCBuf) {
      CI.getDiagnostics().Report(diag::err_cannot_open_file)
          << F.Filename << BCBuf.getError().message();
      LinkModules.clear();
      return true;
    }

    Expected<std::unique_ptr<llvm::Module>> ModuleOrErr =
        getOwningLazyBitcodeModule(std::move(*BCBuf), *VMContext);
    if (!ModuleOrErr) {
      handleAllErrors(ModuleOrErr.takeError(), [&](ErrorInfoBase &EIB) {
        CI.getDiagnostics().Report(diag::err_cannot_open_file)
            << F.Filename << EIB.message();
      });
      LinkModules.clear();
      return true;
    }
    LinkModules.push_back({std::move(ModuleOrErr.get()), F.PropagateAttrs,
                           F.Internalize, F.LinkFlags});
  }
  return false;
}

bool CodeGenAction::hasIRSupport() const { return true; }

void CodeGenAction::EndSourceFileAction() {
  ASTFrontendAction::EndSourceFileAction();

  // If the consumer creation failed, do nothing.
  if (!getCompilerInstance().hasASTConsumer())
    return;

  // Steal the module from the consumer.
  TheModule = BEConsumer->takeModule();
}

std::unique_ptr<llvm::Module> CodeGenAction::takeModule() {
  return std::move(TheModule);
}

llvm::LLVMContext *CodeGenAction::takeLLVMContext() {
  OwnsVMContext = false;
  return VMContext;
}

CodeGenerator *CodeGenAction::getCodeGenerator() const {
  return BEConsumer->getCodeGenerator();
}

bool CodeGenAction::BeginSourceFileAction(CompilerInstance &CI) {
  if (CI.getFrontendOpts().GenReducedBMI)
    CI.getLangOpts().setCompilingModule(LangOptions::CMK_ModuleInterface);
  return ASTFrontendAction::BeginSourceFileAction(CI);
}

static std::unique_ptr<raw_pwrite_stream>
GetOutputStream(CompilerInstance &CI, StringRef InFile, BackendAction Action) {
  switch (Action) {
  case Backend_EmitAssembly:
    return CI.createDefaultOutputFile(false, InFile, "s");
  case Backend_EmitLL:
    return CI.createDefaultOutputFile(false, InFile, "ll");
  case Backend_EmitBC:
    return CI.createDefaultOutputFile(true, InFile, "bc");
  case Backend_EmitNothing:
    return nullptr;
  case Backend_EmitMCNull:
    return CI.createNullOutputFile();
  case Backend_EmitObj:
    return CI.createDefaultOutputFile(true, InFile, "o");
  }

  llvm_unreachable("Invalid action!");
}

std::unique_ptr<ASTConsumer>
CodeGenAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  BackendAction BA = static_cast<BackendAction>(Act);
  std::unique_ptr<raw_pwrite_stream> OS = CI.takeOutputStream();
  if (!OS)
    OS = GetOutputStream(CI, InFile, BA);

  if (BA != Backend_EmitNothing && !OS)
    return nullptr;

  // Load bitcode modules to link with, if we need to.
  if (loadLinkModules(CI))
    return nullptr;

  CoverageSourceInfo *CoverageInfo = nullptr;
  // Add the preprocessor callback only when the coverage mapping is generated.
  if (CI.getCodeGenOpts().CoverageMapping)
    CoverageInfo = CodeGen::CoverageMappingModuleGen::setUpCoverageCallbacks(
        CI.getPreprocessor());

  std::unique_ptr<BackendConsumer> Result(new BackendConsumer(
      CI, BA, CI.getVirtualFileSystemPtr(), *VMContext, std::move(LinkModules),
      InFile, std::move(OS), CoverageInfo));
  BEConsumer = Result.get();

  // Enable generating macro debug info only when debug info is not disabled and
  // also macro debug info is enabled.
  if (CI.getCodeGenOpts().getDebugInfo() != codegenoptions::NoDebugInfo &&
      CI.getCodeGenOpts().MacroDebugInfo) {
    std::unique_ptr<PPCallbacks> Callbacks =
        std::make_unique<MacroPPCallbacks>(BEConsumer->getCodeGenerator(),
                                            CI.getPreprocessor());
    CI.getPreprocessor().addPPCallbacks(std::move(Callbacks));
  }

  if (CI.getFrontendOpts().GenReducedBMI &&
      !CI.getFrontendOpts().ModuleOutputPath.empty()) {
    std::vector<std::unique_ptr<ASTConsumer>> Consumers(2);
    Consumers[0] = std::make_unique<ReducedBMIGenerator>(
        CI.getPreprocessor(), CI.getModuleCache(),
        CI.getFrontendOpts().ModuleOutputPath, CI.getCodeGenOpts());
    Consumers[1] = std::move(Result);
    return std::make_unique<MultiplexConsumer>(std::move(Consumers));
  }

  return std::move(Result);
}

std::unique_ptr<llvm::Module>
CodeGenAction::loadModule(MemoryBufferRef MBRef) {
  CompilerInstance &CI = getCompilerInstance();
  SourceManager &SM = CI.getSourceManager();

  auto DiagErrors = [&](Error E) -> std::unique_ptr<llvm::Module> {
    unsigned DiagID =
        CI.getDiagnostics().getCustomDiagID(DiagnosticsEngine::Error, "%0");
    handleAllErrors(std::move(E), [&](ErrorInfoBase &EIB) {
      CI.getDiagnostics().Report(DiagID) << EIB.message();
    });
    return {};
  };

  // For ThinLTO backend invocations, ensure that the context
  // merges types based on ODR identifiers. We also need to read
  // the correct module out of a multi-module bitcode file.
  if (!CI.getCodeGenOpts().ThinLTOIndexFile.empty()) {
    VMContext->enableDebugTypeODRUniquing();

    Expected<std::vector<BitcodeModule>> BMsOrErr = getBitcodeModuleList(MBRef);
    if (!BMsOrErr)
      return DiagErrors(BMsOrErr.takeError());
    BitcodeModule *Bm = llvm::lto::findThinLTOModule(*BMsOrErr);
    // We have nothing to do if the file contains no ThinLTO module. This is
    // possible if ThinLTO compilation was not able to split module. Content of
    // the file was already processed by indexing and will be passed to the
    // linker using merged object file.
    if (!Bm) {
      auto M = std::make_unique<llvm::Module>("empty", *VMContext);
      M->setTargetTriple(Triple(CI.getTargetOpts().Triple));
      return M;
    }
    Expected<std::unique_ptr<llvm::Module>> MOrErr =
        Bm->parseModule(*VMContext);
    if (!MOrErr)
      return DiagErrors(MOrErr.takeError());
    return std::move(*MOrErr);
  }

  // Load bitcode modules to link with, if we need to.
  if (loadLinkModules(CI))
    return nullptr;

  // Handle textual IR and bitcode file with one single module.
  llvm::SMDiagnostic Err;
  if (std::unique_ptr<llvm::Module> M = parseIR(MBRef, Err, *VMContext)) {
    // For LLVM IR files, always verify the input and report the error in a way
    // that does not ask people to report an issue for it.
    std::string VerifierErr;
    raw_string_ostream VerifierErrStream(VerifierErr);
    if (llvm::verifyModule(*M, &VerifierErrStream)) {
      CI.getDiagnostics().Report(diag::err_invalid_llvm_ir) << VerifierErr;
      return {};
    }
    return M;
  }

  // If MBRef is a bitcode with multiple modules (e.g., -fsplit-lto-unit
  // output), place the extra modules (actually only one, a regular LTO module)
  // into LinkModules as if we are using -mlink-bitcode-file.
  Expected<std::vector<BitcodeModule>> BMsOrErr = getBitcodeModuleList(MBRef);
  if (BMsOrErr && BMsOrErr->size()) {
    std::unique_ptr<llvm::Module> FirstM;
    for (auto &BM : *BMsOrErr) {
      Expected<std::unique_ptr<llvm::Module>> MOrErr =
          BM.parseModule(*VMContext);
      if (!MOrErr)
        return DiagErrors(MOrErr.takeError());
      if (FirstM)
        LinkModules.push_back({std::move(*MOrErr), /*PropagateAttrs=*/false,
                               /*Internalize=*/false, /*LinkFlags=*/{}});
      else
        FirstM = std::move(*MOrErr);
    }
    if (FirstM)
      return FirstM;
  }
  // If BMsOrErr fails, consume the error and use the error message from
  // parseIR.
  consumeError(BMsOrErr.takeError());

  // Translate from the diagnostic info to the SourceManager location if
  // available.
  // TODO: Unify this with ConvertBackendLocation()
  SourceLocation Loc;
  if (Err.getLineNo() > 0) {
    assert(Err.getColumnNo() >= 0);
    Loc = SM.translateFileLineCol(SM.getFileEntryForID(SM.getMainFileID()),
                                  Err.getLineNo(), Err.getColumnNo() + 1);
  }

  // Strip off a leading diagnostic code if there is one.
  StringRef Msg = Err.getMessage();
  Msg.consume_front("error: ");

  unsigned DiagID =
      CI.getDiagnostics().getCustomDiagID(DiagnosticsEngine::Error, "%0");

  CI.getDiagnostics().Report(Loc, DiagID) << Msg;
  return {};
}

void CodeGenAction::ExecuteAction() {
  if (getCurrentFileKind().getLanguage() != Language::LLVM_IR) {
    this->ASTFrontendAction::ExecuteAction();
    return;
  }

  // If this is an IR file, we have to treat it specially.
  BackendAction BA = static_cast<BackendAction>(Act);
  CompilerInstance &CI = getCompilerInstance();
  auto &CodeGenOpts = CI.getCodeGenOpts();
  auto &Diagnostics = CI.getDiagnostics();
  std::unique_ptr<raw_pwrite_stream> OS =
      GetOutputStream(CI, getCurrentFileOrBufferName(), BA);
  if (BA != Backend_EmitNothing && !OS)
    return;

  SourceManager &SM = CI.getSourceManager();
  FileID FID = SM.getMainFileID();
  std::optional<MemoryBufferRef> MainFile = SM.getBufferOrNone(FID);
  if (!MainFile)
    return;

  TheModule = loadModule(*MainFile);
  if (!TheModule)
    return;

  const TargetOptions &TargetOpts = CI.getTargetOpts();
  if (TheModule->getTargetTriple().str() != TargetOpts.Triple) {
    Diagnostics.Report(SourceLocation(), diag::warn_fe_override_module)
        << TargetOpts.Triple;
    TheModule->setTargetTriple(Triple(TargetOpts.Triple));
  }

  EmbedObject(TheModule.get(), CodeGenOpts, CI.getVirtualFileSystem(),
              Diagnostics);
  EmbedBitcode(TheModule.get(), CodeGenOpts, *MainFile);

  LLVMContext &Ctx = TheModule->getContext();

  // Restore any diagnostic handler previously set before returning from this
  // function.
  struct RAII {
    LLVMContext &Ctx;
    std::unique_ptr<DiagnosticHandler> PrevHandler = Ctx.getDiagnosticHandler();
    ~RAII() { Ctx.setDiagnosticHandler(std::move(PrevHandler)); }
  } _{Ctx};

  // Set clang diagnostic handler. To do this we need to create a fake
  // BackendConsumer.
  BackendConsumer Result(CI, BA, CI.getVirtualFileSystemPtr(), *VMContext,
                         std::move(LinkModules), "", nullptr, nullptr,
                         TheModule.get());

  // Link in each pending link module.
  if (!CodeGenOpts.LinkBitcodePostopt && Result.LinkInModules(&*TheModule))
    return;

  // PR44896: Force DiscardValueNames as false. DiscardValueNames cannot be
  // true here because the valued names are needed for reading textual IR.
  Ctx.setDiscardValueNames(false);
  Ctx.setDiagnosticHandler(
      std::make_unique<ClangDiagnosticHandler>(CodeGenOpts, &Result));

  Ctx.setDefaultTargetCPU(TargetOpts.CPU);
  Ctx.setDefaultTargetFeatures(llvm::join(TargetOpts.Features, ","));

  Expected<LLVMRemarkFileHandle> OptRecordFileOrErr =
      setupLLVMOptimizationRemarks(
          Ctx, CodeGenOpts.OptRecordFile, CodeGenOpts.OptRecordPasses,
          CodeGenOpts.OptRecordFormat, CodeGenOpts.DiagnosticsWithHotness,
          CodeGenOpts.DiagnosticsHotnessThreshold);

  if (Error E = OptRecordFileOrErr.takeError()) {
    reportOptRecordError(std::move(E), Diagnostics, CodeGenOpts);
    return;
  }
  LLVMRemarkFileHandle OptRecordFile = std::move(*OptRecordFileOrErr);

  emitBackendOutput(CI, CI.getCodeGenOpts(),
                    CI.getTarget().getDataLayoutString(), TheModule.get(), BA,
                    CI.getFileManager().getVirtualFileSystemPtr(),
                    std::move(OS));
  if (OptRecordFile)
    OptRecordFile->keep();
}

//

void EmitAssemblyAction::anchor() { }
EmitAssemblyAction::EmitAssemblyAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitAssembly, _VMContext) {}

void EmitBCAction::anchor() { }
EmitBCAction::EmitBCAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitBC, _VMContext) {}

void EmitLLVMAction::anchor() { }
EmitLLVMAction::EmitLLVMAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitLL, _VMContext) {}

void EmitLLVMOnlyAction::anchor() { }
EmitLLVMOnlyAction::EmitLLVMOnlyAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitNothing, _VMContext) {}

void EmitCodeGenOnlyAction::anchor() { }
EmitCodeGenOnlyAction::EmitCodeGenOnlyAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitMCNull, _VMContext) {}

void EmitObjAction::anchor() { }
EmitObjAction::EmitObjAction(llvm::LLVMContext *_VMContext)
  : CodeGenAction(Backend_EmitObj, _VMContext) {}
