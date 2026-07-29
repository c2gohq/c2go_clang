//===- C2GoLibCallRouting.h - Route synthesized libc calls ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOLIBCALLROUTING_H
#define LLVM_TRANSFORMS_C2GO_C2GOLIBCALLROUTING_H

#include "llvm/IR/PassManager.h"

namespace llvm {

/// Redirect raw external libc calls synthesized by LLVM back to the direct
/// GoABI0 c2go_linkname targets recorded in !c2go.libcall.routes.
///
/// This pass intentionally runs after the ordinary optimization pipeline (so
/// libc idiom recognition remains available) but before RewriteStatepointsForGC
/// (so each newly-GoABI0 call is classified and wrapped correctly).
class C2GoLibCallRoutingPass : public PassInfoMixin<C2GoLibCallRoutingPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOLIBCALLROUTING_H
