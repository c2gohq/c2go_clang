//===- AArch64C2GoLeafABI.h - c2go NOSPLIT leaf eligibility -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// AArch64 backend hook into the cross-target c2go NOSPLIT leaf eligibility
// analysis (Wave W Track A / #298). The reusable algorithm lives in
// `llvm/Transforms/C2Go/C2GoLeafEligibility.{h,cpp}`; this TU only supplies
// the AArch64-specific frame estimate + NOSPLIT budget hooks and registers
// the legacy ModulePass + NewPM pass for the AArch64 codegen pipeline.
//
// Design reference: clang/docs/c2go_design.md §2.0.1 / §4.2 / §4.2.1 and
// ABI_TEST_FINDINGS_2026-05-25.md §一/§六. The shared eligibility rule (v0)
// is documented in `C2GoLeafEligibility.h`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_AARCH64C2GOLEAFABI_H
#define LLVM_LIB_TARGET_AARCH64_AARCH64C2GOLEAFABI_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class Module;

/// NewPM port of the legacy `AArch64C2GoLeafABI` ModulePass (#379). The two
/// entry points share a worker that calls `c2go::runC2GoLeafCCFlip` with the
/// AArch64 target hooks, so the eligibility analysis + CC-flip behavior is
/// byte-identical regardless of which PM creates the pass. The legacy entry
/// stays the canonical worker for the AArch64 legacy codegen pipeline
/// (clang BackendUtil + c2go-lto both call `addPassesToEmitFile` which runs
/// the legacy `addIRPasses` hook); this NewPM wrapper exists to make the
/// pass name resolvable via `opt -passes=aarch64-c2go-leaf-abi` and to give
/// future NewPM CodeGenPassBuilder ports a hook.
class AArch64C2GoLeafABIPass : public PassInfoMixin<AArch64C2GoLeafABIPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_LIB_TARGET_AARCH64_AARCH64C2GOLEAFABI_H
