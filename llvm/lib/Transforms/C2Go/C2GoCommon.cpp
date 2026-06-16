//===- C2GoCommon.cpp - Shared CC/attr enforcement for C2Go passes --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See C2GoCommon.h for the rationale. This file is the single source of
// truth for the GoABI0 declaration retrofit + the call-site CC sweep
// used by C2GoMemcpyTyping, C2GoWriteBarriers, C2GoEscapeCheck and
// C2GoLoopPoll. Wave CD #419 audit Finding #6 (task #429) consolidated
// the previous per-pass duplicates here.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoCommon.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/User.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

bool llvm::c2go::enforceGoABI0AndOptLeaf(Function *F, bool IsLeaf) {
  bool Changed = false;
  if (F->getCallingConv() != CallingConv::GoABI0) {
    F->setCallingConv(CallingConv::GoABI0);
    Changed = true;
  }
  if (IsLeaf) {
    if (!F->hasFnAttribute("gc-leaf-function")) {
      F->addFnAttr("gc-leaf-function");
      Changed = true;
    }
  } else {
    // #415 — STRIP gc-leaf-function on non-leaf reuse: if an upstream
    // pass / IR fixture / future regression pre-decorated this helper
    // with `gc-leaf-function`, the additive-only path would leak a
    // stale attr through reuse → RewriteStatepointsForGC::
    // callsGCLeafFunction would skip the statepoint wrap → managed
    // roots not relocated across the genuinely non-leaf call → use-
    // after-relocate.
    if (F->hasFnAttribute("gc-leaf-function")) {
      F->removeFnAttr("gc-leaf-function");
      Changed = true;
    }
  }
  return Changed;
}

bool llvm::c2go::enforceCallSiteCC(Function *F) {
  if (!F)
    return false;
  bool Changed = false;
  CallingConv::ID Want = F->getCallingConv();
  for (User *U : F->users()) {
    auto *CB = dyn_cast<CallBase>(U);
    if (!CB)
      continue;
    // Only direct calls — indirect calls through a bitcast or a
    // function-pointer load are not what this audit targets.
    if (CB->getCalledFunction() != F)
      continue;
    if (CB->getCallingConv() == Want)
      continue;
    // Fix: rewrite the offending CallInst's CC to match the declared
    // CC. `Function::setCallingConv` only updates the declaration;
    // pre-existing CallInsts (e.g. from an upstream lowering or a
    // hand-written IR fixture) retain whatever CC they were created
    // with, and the IR verifier accepts the mismatch silently. Without
    // the fix here the Go-linker-generated ABI0 entry on the callee
    // would read garbage from the stack at runtime (same failure mode
    // as #229 / #399). Always emit a one-line diagnostic so a
    // regression is loud, even after the rewrite — the message lets
    // a CI / human notice the upstream emitter that produced the
    // wrong CC.
    CallingConv::ID Got = CB->getCallingConv();
    CB->setCallingConv(Want);
    Changed = true;
    errs() << "c2go: WARNING — call site CC mismatch for helper '"
           << F->getName() << "' in function '"
           << CB->getFunction()->getName() << "' (want CC "
           << static_cast<unsigned>(Want) << ", got CC "
           << static_cast<unsigned>(Got) << "); rewrote to match.\n";
    // c2go #437(a): also bump the module-level `c2go.cc.violations` counter
    // so downstream tools (c2go-lto's ship-gate) can refuse to emit a final
    // archive when any in-flight CC mismatch was detected during the
    // per-TU pipeline. The fix+warn here keeps release+assertions LIT
    // green; the gate runs at the end of c2go-lto::main only.
    //
    // #445: must use `setModuleFlag` (not `addModuleFlag`) so the second+
    // mismatch in the same module replaces the existing flag entry rather
    // than appending a duplicate operand. Module flag IDs must be unique
    // per Verifier.cpp:2029-2032; a duplicate entry trips the IR verifier
    // ("module flag identifiers must be unique"). `Module::Max` behaviour
    // is harmless here because every call writes (Cur+1) which is already
    // strictly greater than `Cur` — the running counter is monotonic.
    Module *M = CB->getModule();
    if (M) {
      unsigned Cur = 0;
      if (auto *MD = M->getModuleFlag(c2go::kCCViolationsFlag))
        if (auto *CAM = dyn_cast<ConstantAsMetadata>(MD))
          if (auto *CI = dyn_cast<ConstantInt>(CAM->getValue()))
            Cur = (unsigned)CI->getZExtValue();
      M->setModuleFlag(Module::Max, c2go::kCCViolationsFlag,
                       uint32_t(Cur + 1));
    }
  }
  return Changed;
}
