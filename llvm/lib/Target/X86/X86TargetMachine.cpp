//===-- X86TargetMachine.cpp - Define TargetMachine for the X86 -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the X86 specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#include "X86TargetMachine.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "TargetInfo/X86TargetInfo.h"
#include "X86.h"
#include "X86C2GoLeafABI.h"
#include "X86MachineFunctionInfo.h"
#include "X86MacroFusion.h"
#include "X86Subtarget.h"
#include "X86TargetObjectFile.h"
#include "X86TargetTransformInfo.h"
#include "llvm-c/Visibility.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/ExecutionDomainFix.h"
#include "llvm/CodeGen/GlobalISel/CSEInfo.h"
#include "llvm/CodeGen/GlobalISel/CallLowering.h"
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelect.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/RegBankSelect.h"
#include "llvm/CodeGen/MIRParser/MIParser.h"
#include "llvm/CodeGen/MIRYamlMapping.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Pass.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/C2GoBackendKnobs.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/CFGuard.h"
#include <memory>
#include <optional>

using namespace llvm;

static cl::opt<bool> EnableMachineCombinerPass("x86-machine-combiner",
                               cl::desc("Enable the machine combiner pass"),
                               cl::init(true), cl::Hidden);

static cl::opt<bool>
    EnableTileRAPass("x86-tile-ra",
                     cl::desc("Enable the tile register allocation pass"),
                     cl::init(true), cl::Hidden);

// c2go #298 / #435 (port skeleton): forward-decl the X86 applier so the
// LLVMInitializeX86Target registration below can reference it. The body is
// defined further down. Today every Cfg field is a no-op on X86 — the hook
// exists so clang/c2go-lto can dispatch BackendConfig at the X86 TM without
// the registry silently falling back to "no applier for this arch". When
// the X86 leaf-ABI / Plan-9 emit pass lands (see
// `project_298_x86_port_progress_2026_06_07.md`) the booleans will start
// flipping real X86 state, mirroring `applyAArch64C2GoConfig`.
namespace llvm {
namespace c2go {
void applyX86C2GoConfig(TargetMachine *TM, const BackendConfig &Cfg);
} // namespace c2go
} // namespace llvm

extern "C" LLVM_C_ABI void LLVMInitializeX86Target() {
  // Register the target.
  RegisterTargetMachine<X86TargetMachine> X(getTheX86_32Target());
  RegisterTargetMachine<X86TargetMachine> Y(getTheX86_64Target());

  // c2go #298 / #435: register the X86 applier for both 32-bit (i386) and
  // 64-bit (x86_64) ArchTypes. Both share the same `X86TargetMachine`
  // subclass, so one applier suffices. Re-registration is idempotent
  // (see `C2GoBackendKnobsRegistry.cpp`).
  llvm::c2go::registerC2GoBackendConfigHook(llvm::Triple::x86,
                                            &llvm::c2go::applyX86C2GoConfig);
  llvm::c2go::registerC2GoBackendConfigHook(llvm::Triple::x86_64,
                                            &llvm::c2go::applyX86C2GoConfig);

  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeX86LowerAMXIntrinsicsLegacyPassPass(PR);
  initializeX86LowerAMXTypeLegacyPassPass(PR);
  initializeX86PreTileConfigLegacyPass(PR);
  initializeGlobalISel(PR);
  initializeWinEHStatePassPass(PR);
  initializeX86FixupBWInstLegacyPass(PR);
  initializeCompressEVEXLegacyPass(PR);
  initializeFixupLEAsLegacyPass(PR);
  initializeX86FPStackifierLegacyPass(PR);
  initializeX86FixupSetCCLegacyPass(PR);
  initializeX86CallFrameOptimizationLegacyPass(PR);
  initializeX86CmovConversionLegacyPass(PR);
  initializeX86TileConfigLegacyPass(PR);
  initializeX86FastPreTileConfigLegacyPass(PR);
  initializeX86FastTileConfigLegacyPass(PR);
  initializeKCFIPass(PR);
  initializeX86LowerTileCopyLegacyPass(PR);
  initializeX86ExpandPseudoLegacyPass(PR);
  initializeX86ExecutionDomainFixPass(PR);
  initializeX86DomainReassignmentLegacyPass(PR);
  initializeX86AvoidSFBLegacyPass(PR);
  initializeX86AvoidTrailingCallLegacyPassPass(PR);
  initializeX86SpeculativeLoadHardeningPassPass(PR);
  initializeX86SpeculativeExecutionSideEffectSuppressionPass(PR);
  initializeX86FlagsCopyLoweringLegacyPass(PR);
  initializeX86LoadValueInjectionLoadHardeningPassPass(PR);
  initializeX86LoadValueInjectionRetHardeningPassPass(PR);
  initializeX86OptimizeLEAsLegacyPass(PR);
  initializeX86PartialReductionLegacyPass(PR);
  initializePseudoProbeInserterPass(PR);
  initializeX86ReturnThunksPass(PR);
  initializeX86DAGToDAGISelLegacyPass(PR);
  initializeX86ArgumentStackSlotPassPass(PR);
  initializeX86AsmPrinterPass(PR);
  initializeX86FixupInstTuningLegacyPass(PR);
  initializeX86FixupVectorConstantsLegacyPass(PR);
  initializeX86DynAllocaExpanderLegacyPass(PR);
  initializeX86SuppressAPXForRelocationLegacyPass(PR);
  initializeX86WinEHUnwindV2Pass(PR);
  initializeX86PreLegalizerCombinerPass(PR);

  // c2go #298: register the X86 leaf C2GoABIInternal pass (port of
  // AArch64C2GoLeafABI). The legacy ModulePass is wired into
  // `X86PassConfig::addIRPasses`; the NewPM port is reachable via
  // `opt -passes=x86-c2go-leaf-abi`. POWERED ON: both are gated only by
  // the `c2go.goabi` module flag (mirror of AArch64), and no-ops on
  // modules without it. Emergency off-switch: `-c2go-disable=leaf-abi`.
  initializeX86C2GoLeafABIPass(PR);

  // c2go #298 / Wave AA Track B: register the X86 staged-meta producer
  // (`x86-c2go-frame-meta-stager`) so the AArch64 `C2GoFrameEmitter`
  // counterpart on X86 actually stages a baseline Plan-9 metadata aggregate
  // onto X86MachineFunctionInfo for every Wave V-flipped strict-leaf. The
  // legacy MachineFunctionPass is wired into
  // `X86PassConfig::addPreEmitPass2`. Self-gated on the `c2go.goabi` module
  // flag — non-c2go builds skip the body on the first MF.
  initializeX86C2GoFrameMetaStagerPass(PR);
}

static std::unique_ptr<TargetLoweringObjectFile> createTLOF(const Triple &TT) {
  if (TT.isOSBinFormatMachO()) {
    if (TT.isX86_64())
      return std::make_unique<X86_64MachoTargetObjectFile>();
    return std::make_unique<TargetLoweringObjectFileMachO>();
  }

  if (TT.isOSBinFormatCOFF())
    return std::make_unique<TargetLoweringObjectFileCOFF>();

  if (TT.isX86_64())
    return std::make_unique<X86_64ELFTargetObjectFile>();
  return std::make_unique<X86ELFTargetObjectFile>();
}

static Reloc::Model getEffectiveRelocModel(const Triple &TT, bool JIT,
                                           std::optional<Reloc::Model> RM) {
  bool is64Bit = TT.isX86_64();
  if (!RM) {
    // JIT codegen should use static relocations by default, since it's
    // typically executed in process and not relocatable.
    if (JIT)
      return Reloc::Static;

    // Darwin defaults to PIC in 64 bit mode and dynamic-no-pic in 32 bit mode.
    // Win64 requires rip-rel addressing, thus we force it to PIC. Otherwise we
    // use static relocation model by default.
    if (TT.isOSDarwin()) {
      if (is64Bit)
        return Reloc::PIC_;
      return Reloc::DynamicNoPIC;
    }
    if (TT.isOSWindows() && is64Bit)
      return Reloc::PIC_;
    return Reloc::Static;
  }

  // ELF and X86-64 don't have a distinct DynamicNoPIC model.  DynamicNoPIC
  // is defined as a model for code which may be used in static or dynamic
  // executables but not necessarily a shared library. On X86-32 we just
  // compile in -static mode, in x86-64 we use PIC.
  if (*RM == Reloc::DynamicNoPIC) {
    if (is64Bit)
      return Reloc::PIC_;
    if (!TT.isOSDarwin())
      return Reloc::Static;
  }

  // If we are on Darwin, disallow static relocation model in X86-64 mode, since
  // the Mach-O file format doesn't support it.
  if (*RM == Reloc::Static && TT.isOSDarwin() && is64Bit)
    return Reloc::PIC_;

  return *RM;
}

static CodeModel::Model
getEffectiveX86CodeModel(const Triple &TT, std::optional<CodeModel::Model> CM,
                         bool JIT) {
  bool Is64Bit = TT.isX86_64();
  if (CM) {
    if (*CM == CodeModel::Tiny)
      reportFatalUsageError("target does not support the tiny CodeModel");
    return *CM;
  }
  if (JIT)
    return Is64Bit ? CodeModel::Large : CodeModel::Small;
  return CodeModel::Small;
}

/// Create an X86 target.
///
X86TargetMachine::X86TargetMachine(const Target &T, const Triple &TT,
                                   StringRef CPU, StringRef FS,
                                   const TargetOptions &Options,
                                   std::optional<Reloc::Model> RM,
                                   std::optional<CodeModel::Model> CM,
                                   CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, TT.computeDataLayout(), TT, CPU, FS, Options,
                               getEffectiveRelocModel(TT, JIT, RM),
                               getEffectiveX86CodeModel(TT, CM, JIT), OL),
      TLOF(createTLOF(getTargetTriple())), IsJIT(JIT) {
  // On PS4/PS5, the "return address" of a 'noreturn' call must still be within
  // the calling function. Note that this also includes __stack_chk_fail,
  // so there was some target-specific logic in the instruction selectors
  // to handle that. That code has since been generalized, so the only thing
  // needed is to set TrapUnreachable here.
  if (TT.isPS() || TT.isOSBinFormatMachO()) {
    this->Options.TrapUnreachable = true;
    this->Options.NoTrapAfterNoreturn = TT.isOSBinFormatMachO();
  }

  setMachineOutliner(true);

  // x86 supports the debug entry values.
  setSupportsDebugEntryValues(true);

  initAsmInfo();
}

X86TargetMachine::~X86TargetMachine() = default;

const X86Subtarget *
X86TargetMachine::getSubtargetImpl(const Function &F) const {
  Attribute CPUAttr = F.getFnAttribute("target-cpu");
  Attribute TuneAttr = F.getFnAttribute("tune-cpu");
  Attribute FSAttr = F.getFnAttribute("target-features");

  StringRef CPU =
      CPUAttr.isValid() ? CPUAttr.getValueAsString() : (StringRef)TargetCPU;
  // "x86-64" is a default target setting for many front ends. In these cases,
  // they actually request for "generic" tuning unless the "tune-cpu" was
  // specified.
  StringRef TuneCPU = TuneAttr.isValid() ? TuneAttr.getValueAsString()
                      : CPU == "x86-64"  ? "generic"
                                         : (StringRef)CPU;
  StringRef FS =
      FSAttr.isValid() ? FSAttr.getValueAsString() : (StringRef)TargetFS;

  SmallString<512> Key;
  // The additions here are ordered so that the definitely short strings are
  // added first so we won't exceed the small size. We append the
  // much longer FS string at the end so that we only heap allocate at most
  // one time.

  // Extract prefer-vector-width attribute.
  unsigned PreferVectorWidthOverride = 0;
  Attribute PreferVecWidthAttr = F.getFnAttribute("prefer-vector-width");
  if (PreferVecWidthAttr.isValid()) {
    StringRef Val = PreferVecWidthAttr.getValueAsString();
    unsigned Width;
    if (!Val.getAsInteger(0, Width)) {
      Key += 'p';
      Key += Val;
      PreferVectorWidthOverride = Width;
    }
  }

  // Extract min-legal-vector-width attribute.
  unsigned RequiredVectorWidth = UINT32_MAX;
  Attribute MinLegalVecWidthAttr = F.getFnAttribute("min-legal-vector-width");
  if (MinLegalVecWidthAttr.isValid()) {
    StringRef Val = MinLegalVecWidthAttr.getValueAsString();
    unsigned Width;
    if (!Val.getAsInteger(0, Width)) {
      Key += 'm';
      Key += Val;
      RequiredVectorWidth = Width;
    }
  }

  // Add CPU to the Key.
  Key += CPU;

  // Add tune CPU to the Key.
  Key += TuneCPU;

  // Keep track of the start of the feature portion of the string.
  unsigned FSStart = Key.size();

  // FIXME: This is related to the code below to reset the target options,
  // we need to know whether or not the soft float flag is set on the
  // function before we can generate a subtarget. We also need to use
  // it as a key for the subtarget since that can be the only difference
  // between two functions.
  bool SoftFloat = F.getFnAttribute("use-soft-float").getValueAsBool();
  // If the soft float attribute is set on the function turn on the soft float
  // subtarget feature.
  if (SoftFloat)
    Key += FS.empty() ? "+soft-float" : "+soft-float,";

  Key += FS;

  // We may have added +soft-float to the features so move the StringRef to
  // point to the full string in the Key.
  FS = Key.substr(FSStart);

  auto &I = SubtargetMap[Key];
  if (!I) {
    // This needs to be done before we create a new subtarget since any
    // creation will depend on the TM and the code generation flags on the
    // function that reside in TargetOptions.
    resetTargetOptions(F);
    I = std::make_unique<X86Subtarget>(
        TargetTriple, CPU, TuneCPU, FS, *this,
        MaybeAlign(F.getParent()->getOverrideStackAlignment()),
        PreferVectorWidthOverride, RequiredVectorWidth);
  }
  return I.get();
}

yaml::MachineFunctionInfo *X86TargetMachine::createDefaultFuncInfoYAML() const {
  return new yaml::X86MachineFunctionInfo();
}

yaml::MachineFunctionInfo *
X86TargetMachine::convertFuncInfoToYAML(const MachineFunction &MF) const {
  const auto *MFI = MF.getInfo<X86MachineFunctionInfo>();
  return new yaml::X86MachineFunctionInfo(*MFI);
}

bool X86TargetMachine::parseMachineFunctionInfo(
    const yaml::MachineFunctionInfo &MFI, PerFunctionMIParsingState &PFS,
    SMDiagnostic &Error, SMRange &SourceRange) const {
  const auto &YamlMFI = static_cast<const yaml::X86MachineFunctionInfo &>(MFI);
  PFS.MF.getInfo<X86MachineFunctionInfo>()->initializeBaseYamlFields(YamlMFI);
  return false;
}

bool X86TargetMachine::isNoopAddrSpaceCast(unsigned SrcAS,
                                           unsigned DestAS) const {
  assert(SrcAS != DestAS && "Expected different address spaces!");
  if (getPointerSize(SrcAS) != getPointerSize(DestAS))
    return false;
  return SrcAS < 256 && DestAS < 256;
}

void X86TargetMachine::reset() { SubtargetMap.clear(); }

ScheduleDAGInstrs *
X86TargetMachine::createMachineScheduler(MachineSchedContext *C) const {
  ScheduleDAGMILive *DAG = createSchedLive(C);
  DAG->addMutation(createX86MacroFusionDAGMutation());
  return DAG;
}

ScheduleDAGInstrs *
X86TargetMachine::createPostMachineScheduler(MachineSchedContext *C) const {
  ScheduleDAGMI *DAG = createSchedPostRA(C);
  DAG->addMutation(createX86MacroFusionDAGMutation());
  return DAG;
}

//===----------------------------------------------------------------------===//
// X86 TTI query.
//===----------------------------------------------------------------------===//

TargetTransformInfo
X86TargetMachine::getTargetTransformInfo(const Function &F) const {
  return TargetTransformInfo(std::make_unique<X86TTIImpl>(this, F));
}

//===----------------------------------------------------------------------===//
// Pass Pipeline Configuration
//===----------------------------------------------------------------------===//

namespace {

/// X86 Code Generator Pass Configuration Options.
class X86PassConfig : public TargetPassConfig {
public:
  X86PassConfig(X86TargetMachine &TM, PassManagerBase &PM)
    : TargetPassConfig(TM, PM) {
    // c2go #298 / Wave W Track B (#310 mirror): same correctness hazard
    // root-caused on AArch64 (CSR_AArch64_NoRegs + go-asm-owned prologue
    // vs RegisterCoalescer pre-RA/PEI merging copies inconsistently with
    // the frame contract). The Wave V abitest_amd64 baseline (#485 /
    // project_298_abitest_amd64_baseline_2026_06_07) confirmed the same
    // shape on X86. Flag lives on the Plan 9-codegen TM only; the
    // (unlinked) .o pipeline and all non-c2go compiles use a different TM
    // with the flag unset.
    if (TM.C2GoDisableRegisterCoalescing)
      disablePass(&RegisterCoalescerID);
  }

  X86TargetMachine &getX86TargetMachine() const {
    return getTM<X86TargetMachine>();
  }

  void addIRPasses() override;
  bool addInstSelector() override;
  bool addIRTranslator() override;
  bool addLegalizeMachineIR() override;
  bool addRegBankSelect() override;
  bool addGlobalInstructionSelect() override;
  void addPreLegalizeMachineIR() override;
  bool addILPOpts() override;
  bool addPreISel() override;
  void addMachineSSAOptimization() override;
  void addPreRegAlloc() override;
  bool addPostFastRegAllocRewrite() override;
  void addPostRegAlloc() override;
  void addPreEmitPass() override;
  void addPreEmitPass2() override;
  void addPreSched2() override;
  bool addRegAssignAndRewriteOptimized() override;

  std::unique_ptr<CSEConfigBase> getCSEConfig() const override;
};

class X86ExecutionDomainFix : public ExecutionDomainFix {
public:
  static char ID;
  X86ExecutionDomainFix() : ExecutionDomainFix(ID, X86::VR128XRegClass) {}
  StringRef getPassName() const override {
    return "X86 Execution Dependency Fix";
  }
};
char X86ExecutionDomainFix::ID;

} // end anonymous namespace

INITIALIZE_PASS_BEGIN(X86ExecutionDomainFix, "x86-execution-domain-fix",
  "X86 Execution Domain Fix", false, false)
INITIALIZE_PASS_DEPENDENCY(ReachingDefInfoWrapperPass)
INITIALIZE_PASS_END(X86ExecutionDomainFix, "x86-execution-domain-fix",
  "X86 Execution Domain Fix", false, false)

TargetPassConfig *X86TargetMachine::createPassConfig(PassManagerBase &PM) {
  return new X86PassConfig(*this, PM);
}

MachineFunctionInfo *X86TargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return X86MachineFunctionInfo::create<X86MachineFunctionInfo>(Allocator, F,
                                                                STI);
}

void X86PassConfig::addIRPasses() {
  addPass(createAtomicExpandLegacyPass());

  // We add both pass anyway and when these two passes run, we skip the pass
  // based on the option level and option attribute.
  addPass(createX86LowerAMXIntrinsicsLegacyPass());
  addPass(createX86LowerAMXTypeLegacyPass());

  TargetPassConfig::addIRPasses();

  if (TM->getOptLevel() != CodeGenOptLevel::None) {
    addPass(createInterleavedAccessPass());
    addPass(createX86PartialReductionLegacyPass());
  }

  // Add passes that handle indirect branch removal and insertion of a retpoline
  // thunk. These will be a no-op unless a function subtarget has the retpoline
  // feature enabled.
  addPass(createIndirectBrExpandPass());

  // Add Control Flow Guard checks.
  const Triple &TT = TM->getTargetTriple();
  if (TT.isOSWindows()) {
    if (TT.isX86_64()) {
      addPass(createCFGuardDispatchPass());
    } else {
      addPass(createCFGuardCheckPass());
    }
  }

  if (TM->Options.JMCInstrument)
    addPass(createJMCInstrumenterPass());

  // c2go #298: opportunistically flip NOSPLIT-eligible internal leaf /
  // near-leaf functions to the private C2GoABIInternal register-passing
  // convention. Mirrors the AArch64 wiring; POWERED ON and self-gates
  // inside runOnModule on:
  //   1. `c2go-leaf-abi` emergency-disable flag (default enabled),
  //   2. `c2go.goabi` module flag (set by clang for -fc2go).
  // Skipped at -O0 to mirror AArch64's "savings don't justify the path
  // when alloca clutter dominates" reasoning.
  if (TM->getOptLevel() != CodeGenOptLevel::None)
    addPass(createX86C2GoLeafABIPass());
}

bool X86PassConfig::addInstSelector() {
  // Install an instruction selector.
  addPass(createX86ISelDag(getX86TargetMachine(), getOptLevel()));

  // For ELF, cleanup any local-dynamic TLS accesses.
  if (TM->getTargetTriple().isOSBinFormatELF() &&
      getOptLevel() != CodeGenOptLevel::None)
    addPass(createCleanupLocalDynamicTLSPass());

  addPass(createX86GlobalBaseRegPass());
  addPass(createX86ArgumentStackSlotPass());
  return false;
}

bool X86PassConfig::addIRTranslator() {
  addPass(new IRTranslator(getOptLevel()));
  return false;
}

bool X86PassConfig::addLegalizeMachineIR() {
  addPass(new Legalizer());
  return false;
}

bool X86PassConfig::addRegBankSelect() {
  addPass(new RegBankSelect());
  return false;
}

bool X86PassConfig::addGlobalInstructionSelect() {
  addPass(new InstructionSelect(getOptLevel()));
  // Add GlobalBaseReg in case there is no SelectionDAG passes afterwards
  if (isGlobalISelAbortEnabled())
    addPass(createX86GlobalBaseRegPass());
  return false;
}

void X86PassConfig::addPreLegalizeMachineIR() {
  if (getOptLevel() != CodeGenOptLevel::None) {
    addPass(createX86PreLegalizerCombiner());
  }
}

bool X86PassConfig::addILPOpts() {
  addPass(&EarlyIfConverterLegacyID);
  if (EnableMachineCombinerPass)
    addPass(&MachineCombinerID);
  addPass(createX86CmovConversionLegacyPass());
  return true;
}

bool X86PassConfig::addPreISel() {
  // Only add this pass for 32-bit x86 Windows.
  const Triple &TT = TM->getTargetTriple();
  if (TT.isOSWindows() && TT.isX86_32())
    addPass(createX86WinEHStatePass());
  return true;
}

void X86PassConfig::addPreRegAlloc() {
  if (getOptLevel() != CodeGenOptLevel::None) {
    addPass(&LiveRangeShrinkID);
    addPass(createX86FixupSetCCLegacyPass());
    addPass(createX86OptimizeLEAsLegacyPass());
    addPass(createX86CallFrameOptimizationLegacyPass());
    addPass(createX86AvoidStoreForwardingBlocksLegacyPass());
  }

  addPass(createX86SuppressAPXForRelocationLegacyPass());

  addPass(createX86SpeculativeLoadHardeningPass());
  addPass(createX86FlagsCopyLoweringLegacyPass());
  addPass(createX86DynAllocaExpanderLegacyPass());

  if (getOptLevel() != CodeGenOptLevel::None)
    addPass(createX86PreTileConfigLegacyPass());
  else
    addPass(createX86FastPreTileConfigLegacyPass());
}

void X86PassConfig::addMachineSSAOptimization() {
  addPass(createX86DomainReassignmentLegacyPass());
  TargetPassConfig::addMachineSSAOptimization();
}

void X86PassConfig::addPostRegAlloc() {
  addPass(createX86LowerTileCopyLegacyPass());
  addPass(createX86FPStackifierLegacyPass());
  // When -O0 is enabled, the Load Value Injection Hardening pass will fall back
  // to using the Speculative Execution Side Effect Suppression pass for
  // mitigation. This is to prevent slow downs due to
  // analyses needed by the LVIHardening pass when compiling at -O0.
  if (getOptLevel() != CodeGenOptLevel::None)
    addPass(createX86LoadValueInjectionLoadHardeningPass());
}

void X86PassConfig::addPreSched2() {
  addPass(createX86ExpandPseudoLegacyPass());
  addPass(createKCFIPass());
}

void X86PassConfig::addPreEmitPass() {
  if (getOptLevel() != CodeGenOptLevel::None) {
    addPass(new X86ExecutionDomainFix());
    addPass(createBreakFalseDeps());
  }

  addPass(createX86IndirectBranchTrackingPass());

  addPass(createX86IssueVZeroUpperPass());

  if (getOptLevel() != CodeGenOptLevel::None) {
    addPass(createX86FixupBWInstsLegacyPass());
    addPass(createX86PadShortFunctions());
    addPass(createX86FixupLEAsLegacyPass());
    addPass(createX86FixupInstTuningLegacyPass());
    addPass(createX86FixupVectorConstantsLegacyPass());
  }
  addPass(createX86CompressEVEXLegacyPass());
  addPass(createX86InsertX87waitPass());
}

void X86PassConfig::addPreEmitPass2() {
  const Triple &TT = TM->getTargetTriple();
  const MCAsmInfo *MAI = TM->getMCAsmInfo();

  // The X86 Speculative Execution Pass must run after all control
  // flow graph modifying passes. As a result it was listed to run right before
  // the X86 Retpoline Thunks pass. The reason it must run after control flow
  // graph modifications is that the model of LFENCE in LLVM has to be updated
  // (FIXME: https://bugs.llvm.org/show_bug.cgi?id=45167). Currently the
  // placement of this pass was hand checked to ensure that the subsequent
  // passes don't move the code around the LFENCEs in a way that will hurt the
  // correctness of this pass. This placement has been shown to work based on
  // hand inspection of the codegen output.
  addPass(createX86SpeculativeExecutionSideEffectSuppression());
  addPass(createX86IndirectThunksPass());
  addPass(createX86ReturnThunksPass());

  // Insert extra int3 instructions after trailing call instructions to avoid
  // issues in the unwinder.
  if (TT.isOSWindows() && TT.isX86_64())
    addPass(createX86AvoidTrailingCallLegacyPass());

  // Verify basic block incoming and outgoing cfa offset and register values and
  // correct CFA calculation rule where needed by inserting appropriate CFI
  // instructions.
  if (!TT.isOSDarwin() &&
      (!TT.isOSWindows() ||
       MAI->getExceptionHandlingType() == ExceptionHandling::DwarfCFI))
    addPass(createCFIInstrInserter());

  if (TT.isOSWindows()) {
    // Identify valid longjmp targets for Windows Control Flow Guard.
    addPass(createCFGuardLongjmpPass());
    // Identify valid eh continuation targets for Windows EHCont Guard.
    addPass(createEHContGuardTargetsPass());
  }
  addPass(createX86LoadValueInjectionRetHardeningPass());

  // Insert pseudo probe annotation for callsite profiling
  addPass(createPseudoProbeInserter());

  // KCFI indirect call checks are lowered to a bundle, and on Darwin platforms,
  // also CALL_RVMARKER.
  addPass(createUnpackMachineBundles([&TT](const MachineFunction &MF) {
    // Only run bundle expansion if the module uses kcfi, or there are relevant
    // ObjC runtime functions present in the module.
    const Function &F = MF.getFunction();
    const Module *M = F.getParent();
    return M->getModuleFlag("kcfi") ||
           (TT.isOSDarwin() &&
            (M->getFunction("objc_retainAutoreleasedReturnValue") ||
             M->getFunction("objc_unsafeClaimAutoreleasedReturnValue")));
  }));

  // Analyzes and emits pseudos to support Win x64 Unwind V2. This pass must run
  // after all real instructions have been added to the epilog.
  if (TT.isOSWindows() && TT.isX86_64())
    addPass(createX86WinEHUnwindV2Pass());

  // c2go #298 Wave AA Track B (X86 minimal staged-meta producer). Mirror of
  // the AArch64 `C2GoFrameEmitter` at strict-leaf scope: stages a baseline
  // `C2GoFunctionMetadata` (NoSplit=true, FrameSize=0, SavedLinkSize=0,
  // FrameAlignment=0, ArgSize from the `c2go-argsize` IR fn attribute) onto
  // X86MachineFunctionInfo for every function the Wave V leaf-ABI IR pass
  // flipped to CallingConv::C2GoABIInternal. Without this stage,
  // `X86AsmPrinter::emitFunctionEntryLabel`'s republish (Wave Z Track B,
  // #376) never fires and the Plan-9 streamer falls back to the Stage-4
  // `TEXT name(SB), NOFRAME, $0` directive instead of the intended Stage-1
  // `TEXT name(SB), NOSPLIT|NOFRAME, $0-M`. Self-gates on the c2go.goabi
  // Module flag inside the pass so non-c2go builds are byte-identical.
  addPass(createX86C2GoFrameMetaStagerPass());
}

bool X86PassConfig::addPostFastRegAllocRewrite() {
  addPass(createX86FastTileConfigLegacyPass());
  return true;
}

std::unique_ptr<CSEConfigBase> X86PassConfig::getCSEConfig() const {
  return getStandardCSEConfigForOpt(TM->getOptLevel());
}

static bool onlyAllocateTileRegisters(const TargetRegisterInfo &TRI,
                                      const MachineRegisterInfo &MRI,
                                      const Register Reg) {
  const TargetRegisterClass *RC = MRI.getRegClass(Reg);
  return static_cast<const X86RegisterInfo &>(TRI).isTileRegisterClass(RC);
}

bool X86PassConfig::addRegAssignAndRewriteOptimized() {
  // Don't support tile RA when RA is specified by command line "-regalloc".
  if (!isCustomizedRegAlloc() && EnableTileRAPass) {
    // Allocate tile register first.
    addPass(createGreedyRegisterAllocator(onlyAllocateTileRegisters));
    addPass(createX86TileConfigLegacyPass());
  }
  return TargetPassConfig::addRegAssignAndRewriteOptimized();
}

// ===-- c2go #298 / #435: X86 BackendConfig applier ------------------------===
//
// Mirrors `applyAArch64C2GoConfig` (AArch64TargetMachine.cpp): writes each
// knob into the per-TM field that the X86 backend reads from. The three
// consumers (Wave W Track B):
//
//   * ForceBlockAddressJumpTable: X86TargetLowering::getJumpTableEncoding
//     (X86ISelLoweringCall.cpp) reads C2GoForceBlockAddressJumpTable and
//     returns EK_BlockAddress (8-byte absolute) — Plan-9 .s DATA can
//     represent absolute pointers but not the default PIC label-difference
//     / GOTOFF encoding. Mirror of AArch64's #120 / #375 slice 1.
//
//   * DisableRegisterCoalescing: X86PassConfig ctor reads
//     C2GoDisableRegisterCoalescing and `disablePass(&RegisterCoalescerID)`
//     when set. Same correctness hazard root-caused on AArch64 (#310) and
//     confirmed on X86 by the Wave V abitest_amd64 baseline (#485).
//
//   * DisableGlobalMerge: vacuous-by-design on X86 today (X86PassConfig has
//     no createGlobalMergePass call site, so `_MergedGlobals` is never
//     emitted). Wired through anyway as a forward-compatible gate so any
//     future X86 codegen change that introduces GlobalMerge respects the
//     #397 skip path. The accompanying LIT pins the byte-identical "no
//     _MergedGlobals on X86" baseline so a regression is loud.
//
// The applier is only effective on the Plan-9-codegen TargetMachine: clang
// BackendUtil / c2go-lto set the Cfg booleans ONLY when emitting against
// the OS-neutral ELF amd64 c2go clone. Every non-c2go X86 compile uses a
// separate TM where the fields stay default-false — X86 production
// behavior is byte-identical when c2go is off.
namespace llvm {
namespace c2go {

void applyX86C2GoConfig(TargetMachine *TM, const BackendConfig &Cfg) {
  if (!TM || !TM->getTargetTriple().isX86())
    return;
  auto *X86TM = static_cast<X86TargetMachine *>(TM);
  X86TM->C2GoForceBlockAddressJumpTable = Cfg.ForceBlockAddressJumpTable;
  X86TM->C2GoDisableRegisterCoalescing = Cfg.DisableRegisterCoalescing;
  X86TM->C2GoDisableGlobalMerge = Cfg.DisableGlobalMerge;
}

} // namespace c2go
} // namespace llvm
