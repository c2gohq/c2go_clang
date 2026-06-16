//==-- AArch64TargetMachine.h - Define TargetMachine for AArch64 -*- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the AArch64 specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_AARCH64TARGETMACHINE_H
#define LLVM_LIB_TARGET_AARCH64_AARCH64TARGETMACHINE_H

#include "AArch64InstrInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/IR/DataLayout.h"
#include <optional>

namespace llvm {

class AArch64TargetMachine : public CodeGenTargetMachineImpl {
protected:
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
  mutable StringMap<std::unique_ptr<AArch64Subtarget>> SubtargetMap;

  /// Reset internal state.
  void reset() override;

public:
  AArch64TargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                       StringRef FS, const TargetOptions &Options,
                       std::optional<Reloc::Model> RM,
                       std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                       bool JIT, bool IsLittleEndian);

  ~AArch64TargetMachine() override;
  const AArch64Subtarget *getSubtargetImpl(const Function &F) const override;
  // DO NOT IMPLEMENT: There is no such thing as a valid default subtarget,
  // subtargets are per-function entities based on the target-specific
  // attributes of each function.
  const AArch64Subtarget *getSubtargetImpl() const = delete;

  // Pass Pipeline Configuration
  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;

  void registerPassBuilderCallbacks(PassBuilder &PB) override;

  TargetTransformInfo getTargetTransformInfo(const Function &F) const override;

  TargetLoweringObjectFile* getObjFileLowering() const override {
    return TLOF.get();
  }

  MachineFunctionInfo *
  createMachineFunctionInfo(BumpPtrAllocator &Allocator, const Function &F,
                            const TargetSubtargetInfo *STI) const override;

  yaml::MachineFunctionInfo *createDefaultFuncInfoYAML() const override;
  yaml::MachineFunctionInfo *
  convertFuncInfoToYAML(const MachineFunction &MF) const override;
  bool parseMachineFunctionInfo(const yaml::MachineFunctionInfo &,
                                PerFunctionMIParsingState &PFS,
                                SMDiagnostic &Error,
                                SMRange &SourceRange) const override;

  /// Returns true if a cast between SrcAS and DestAS is a noop.
  bool isNoopAddrSpaceCast(unsigned SrcAS, unsigned DestAS) const override {
    return getPointerSize(SrcAS) == getPointerSize(DestAS);
  }
  ScheduleDAGInstrs *
  createMachineScheduler(MachineSchedContext *C) const override;

  ScheduleDAGInstrs *
  createPostMachineScheduler(MachineSchedContext *C) const override;

  size_t clearLinkerOptimizationHints(
      const SmallPtrSetImpl<MachineInstr *> &MIs) const override;

  /// Returns true if the new SME ABI lowering should be used.
  bool useNewSMEABILowering() const { return UseNewSMEABILowering; }

  // c2go #375 slice 1: per-TM Plan-9-codegen flags. Migrated off the base
  // `llvm::TargetMachine` to keep the c2go-specific knobs scoped to the only
  // backend that consumes them. Default false for all AArch64 TMs; clang's
  // BackendUtil and the c2go-lto tool set them ONLY on the Plan 9-codegen
  // TargetMachine (an OS-neutral ELF arm64 clone). Reached from clang via
  // the free-function shim in `llvm/Target/C2GoBackendKnobs.h`.
  //
  // C2GoForceBlockAddressJumpTable: AArch64TargetLowering's
  //   getJumpTableEncoding() reads this to force EK_BlockAddress (8-byte
  //   absolute) instead of label-difference, and
  //   AArch64PassConfig::addPostBBSections skips AArch64CompressJumpTables.
  //   Plan 9 .s DATA can't represent label-difference encodings. (#120)
  //
  // C2GoDisableRegisterCoalescing: AArch64PassConfig ctor reads this and
  //   disables the RegisterCoalescer. The c2go pipeline uses
  //   CSR_AArch64_NoRegs + a go-asm-owned prologue (injected at PEI) over
  //   large splittable frames; the coalescer (pre-RA/PEI) merges copies
  //   inconsistently with that frame contract, miscompiling sqlite3Parser
  //   yytos. Root cause tracked in #310.
  //
  // C2GoDisableGlobalMerge: AArch64PassConfig::addPreISel reads this and
  //   skips the GlobalMergePass. GlobalMerge coalesces multiple internal
  //   globals into a single `_MergedGlobals` aggregate, which races the
  //   MCPlan9AsmStreamer go-owned filter: merged GVs are erased from the
  //   Module before the streamer can classify them per-GV, so go-owned
  //   pointer-bearing globals get folded into a NOPTR blob and the .s
  //   ships an unsound NOPTR layout (#397).
  bool C2GoForceBlockAddressJumpTable = false;
  bool C2GoDisableRegisterCoalescing = false;
  bool C2GoDisableGlobalMerge = false;

private:
  bool isLittle;
  bool UseNewSMEABILowering;
};

// AArch64 little endian target machine.
//
class AArch64leTargetMachine : public AArch64TargetMachine {
  virtual void anchor();

public:
  AArch64leTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                         StringRef FS, const TargetOptions &Options,
                         std::optional<Reloc::Model> RM,
                         std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                         bool JIT);
};

// AArch64 big endian target machine.
//
class AArch64beTargetMachine : public AArch64TargetMachine {
  virtual void anchor();

public:
  AArch64beTargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                         StringRef FS, const TargetOptions &Options,
                         std::optional<Reloc::Model> RM,
                         std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                         bool JIT);
};

} // end namespace llvm

#endif
