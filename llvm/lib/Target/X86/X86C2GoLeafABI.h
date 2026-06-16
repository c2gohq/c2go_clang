//===- X86C2GoLeafABI.h - c2go NOSPLIT leaf eligibility (X86) ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go optimization layer (X86 port skeleton, Wave V Track A / #298).
//
// Mirrors `AArch64C2GoLeafABI.h` for the X86/X86_64 backend so the same
// NOSPLIT leaf / TU-internal near-leaf eligibility analysis + CC-flip behavior
// is available when targeting amd64 (and eventually i386). The actual private
// register-passing convention (CallingConv::C2GoABIInternal) on X86 is set up
// here; lowering it to the X86 ABIInternal register list
// (AX,BX,CX,DI,SI,R8-R11 / X0-X14, X15 fixed zero, DX closure ctxt — see
// `ABI_TEST_FINDINGS_2026-05-25.md` + abitest_amd64 baseline #485) is the
// follow-up work tracked by #298 main task and lands in
// X86ISelLoweringCall.cpp / X86CallingConv.{td,cpp}.
//
// SCOPE OF THIS FILE (Wave V Track A landed):
//   * NewPM + legacy entry points (mirror of AArch64).
//   * Eligibility analysis (same v0 rule as AArch64: c2go-reg-return attr +
//     local linkage + not address-taken + analyzable in-TU subtree within
//     NOSPLIT budget).
//   * CC flip to `CallingConv::C2GoABIInternal` (callee + direct call sites
//     in lockstep), gated by the `c2go.goabi` module flag.
//
// DEFERRED to #298 main task:
//   * Real X86 register assignment for C2GoABIInternal — currently the X86
//     backend has NO CCAssign function for that CC; clang/llc will assert if
//     a flipped function is actually code-generated until X86CallingConv.cpp
//     gets a `CC_X86_64_C2GoABIInternal` (and we wire it from
//     X86ISelLoweringCall.cpp).
//   * Plan-9 NOSPLIT .s emission for X86 (the AArch64 path lives in
//     MCPlan9AsmStreamer; the amd64 emit story has a separate task).
//   * Eligibility-analysis dedup with AArch64: today the analysis is
//     duplicated here intentionally so this TU stays free of any
//     llvm/lib/Target/AArch64/ include (cross-target layer violation).
//     Once #298 lands a `llvm/Transforms/C2Go/C2GoLeafEligibility.{h,cpp}`,
//     both AArch64C2GoLeafABI.cpp and this file should forward to it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86C2GOLEAFABI_H
#define LLVM_LIB_TARGET_X86_X86C2GOLEAFABI_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;
class ModulePass;
class PassRegistry;

/// Legacy-PM entry point (mirror of `createAArch64C2GoLeafABIPass`).
/// Wired into `X86PassConfig::addIRPasses` so the production legacy codegen
/// pipeline (clang BackendUtil + c2go-lto's `addPassesToEmitFile`) picks it
/// up automatically; same opt-level gating as AArch64 (>= -O1).
ModulePass *createX86C2GoLeafABIPass();

/// Initialize the legacy ModulePass with the PassRegistry. Called from
/// `LLVMInitializeX86Target` alongside the other X86 pass initializers.
void initializeX86C2GoLeafABIPass(PassRegistry &);

/// NewPM port (mirror of `AArch64C2GoLeafABIPass`). Registered via
/// `X86PassRegistry.def` as `x86-c2go-leaf-abi` so it is resolvable from
/// `opt -passes=x86-c2go-leaf-abi`.
class X86C2GoLeafABIPass : public PassInfoMixin<X86C2GoLeafABIPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_X86C2GOLEAFABI_H
