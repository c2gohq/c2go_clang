//===-- X86TargetMachine.h - Define TargetMachine for the X86 ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the X86 specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86TARGETMACHINE_H
#define LLVM_LIB_TARGET_X86_X86TARGETMACHINE_H

#include "X86Subtarget.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/CodeGen/CodeGenTargetMachineImpl.h"
#include "llvm/Support/CodeGen.h"
#include <memory>
#include <optional>

namespace llvm {

class StringRef;
class TargetTransformInfo;

class X86TargetMachine final : public CodeGenTargetMachineImpl {
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
  mutable StringMap<std::unique_ptr<X86Subtarget>> SubtargetMap;
  // True if this is used in JIT.
  bool IsJIT;

  /// Reset internal state.
  void reset() override;

public:
  X86TargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                   StringRef FS, const TargetOptions &Options,
                   std::optional<Reloc::Model> RM,
                   std::optional<CodeModel::Model> CM, CodeGenOptLevel OL,
                   bool JIT);
  ~X86TargetMachine() override;

  const X86Subtarget *getSubtargetImpl(const Function &F) const override;
  // DO NOT IMPLEMENT: There is no such thing as a valid default subtarget,
  // subtargets are per-function entities based on the target-specific
  // attributes of each function.
  const X86Subtarget *getSubtargetImpl() const = delete;

  TargetTransformInfo getTargetTransformInfo(const Function &F) const override;

  // Set up the pass pipeline.
  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;

  TargetLoweringObjectFile *getObjFileLowering() const override {
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

  void registerPassBuilderCallbacks(PassBuilder &PB) override;

  Error buildCodeGenPipeline(ModulePassManager &, raw_pwrite_stream &,
                             raw_pwrite_stream *, CodeGenFileType,
                             const CGPassBuilderOption &,
                             PassInstrumentationCallbacks *) override;

  bool isJIT() const { return IsJIT; }

  // c2go #298 / Wave W Track B: per-TM Plan-9-codegen flags. Mirrors the
  // AArch64TargetMachine layout (#375 slice 1) so the registry shim
  // `applyX86C2GoConfig` can flip real X86 backend state instead of being
  // a dispatch-only no-op. Default false for every X86 TM; clang's
  // BackendUtil and the c2go-lto tool set them ONLY on the Plan 9-codegen
  // TargetMachine (an OS-neutral ELF amd64 clone). Reached from clang via
  // the free-function shim in `llvm/Target/C2GoBackendKnobs.h`.
  //
  // C2GoForceBlockAddressJumpTable: X86TargetLowering's getJumpTableEncoding
  //   reads this to force EK_BlockAddress (8-byte absolute) instead of the
  //   PIC label-difference / GOTOFF encoding that Plan 9 .s DATA cannot
  //   represent. (#120, mirrored from AArch64.)
  //
  // C2GoDisableRegisterCoalescing: X86PassConfig ctor reads this and
  //   disables the RegisterCoalescer. The c2go pipeline uses an
  //   amd64 ABI0 frame contract with a go-asm-owned prologue over large
  //   splittable frames; the coalescer (pre-RA/PEI) merges copies
  //   inconsistently with that contract — same hazard root-caused on
  //   AArch64 (#310). Wave V abitest_amd64 baseline confirmed the same
  //   shape applies to X86 (project_298_abitest_amd64_baseline_2026_06_07).
  //
  // C2GoDisableGlobalMerge: defensive gate. X86PassConfig currently has
  //   no createGlobalMergePass call site, so this knob is vacuous on the
  //   production path today (no `_MergedGlobals` is ever emitted). It is
  //   wired through anyway so any future X86 codegen change that adds
  //   GlobalMerge to the X86 pipeline must respect the c2go skip path —
  //   the same #397 root applies: GlobalMerge coalesces multiple internal
  //   GVs into a single `_MergedGlobals` aggregate, racing the
  //   MCPlan9AsmStreamer go-owned per-GV filter, shipping an unsound
  //   NOPTR layout. Mirrors AArch64TargetMachine.
  bool C2GoForceBlockAddressJumpTable = false;
  bool C2GoDisableRegisterCoalescing = false;
  bool C2GoDisableGlobalMerge = false;

  bool isNoopAddrSpaceCast(unsigned SrcAS, unsigned DestAS) const override;
  ScheduleDAGInstrs *
  createMachineScheduler(MachineSchedContext *C) const override;
  ScheduleDAGInstrs *
  createPostMachineScheduler(MachineSchedContext *C) const override;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_X86_X86TARGETMACHINE_H
