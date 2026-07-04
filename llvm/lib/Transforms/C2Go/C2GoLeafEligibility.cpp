//===- C2GoLeafEligibility.cpp - c2go NOSPLIT leaf eligibility --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implementation of the cross-target c2go NOSPLIT leaf / near-leaf eligibility
// analysis + CC-flip worker (Wave W Track A / #298).
//
// See C2GoLeafEligibility.h and clang/docs/c2go_design.md §4.2.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoLeafEligibility.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/C2GoEmergencyFlag.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"
#include <optional>

#define DEBUG_TYPE "c2go-leaf-eligibility"

using namespace llvm;
using namespace llvm::c2go;

STATISTIC(NumC2GoLeafFlipped,
          "Number of c2go functions flipped to C2GoABIInternal");

//===----------------------------------------------------------------------===//
// NOSPLIT budget (mirrors Go cmd/link/internal/ld/stackcheck.go + objabi).
//===----------------------------------------------------------------------===//

unsigned llvm::c2go::getNosplitBudget(StringRef ArchName, StringRef OSName) {
  // objabi.StackNosplitBase = 800; mult = 1, +1 for aix/openbsd, +1 for race.
  // We assume race=false (conservative: race only enlarges the budget).
  static constexpr unsigned StackNosplitBase = 800;
  unsigned Mult = 1;
  if (OSName == "aix" || OSName == "openbsd")
    Mult += 1;
  unsigned Base = StackNosplitBase * Mult;

  // callSize = Arch.RegSize on non-LR machines (amd64 = 8, i386 = 4); 0 on
  // LR machines (arm64 has HasLR).
  unsigned CallSize;
  if (ArchName == "arm64" || ArchName == "aarch64")
    CallSize = 0u;
  else if (ArchName == "amd64" || ArchName == "x86_64")
    CallSize = 8u;
  else if (ArchName == "386" || ArchName == "i386" || ArchName == "x86")
    CallSize = 4u;
  else
    CallSize = 8u; // conservative default for unknown 64-bit arches.

  unsigned Limit = Base - CallSize;

  // arm64 reserves an extra 8 bytes for the saved frame pointer.
  if (ArchName == "arm64" || ArchName == "aarch64")
    Limit -= 8;

  return Limit;
}

unsigned llvm::c2go::getNosplitBudgetForTriple(StringRef TripleStr) {
  Triple T(TripleStr);
  // Map LLVM arch/OS to Go GOARCH/GOOS spellings used by getNosplitBudget.
  StringRef Arch;
  switch (T.getArch()) {
  case Triple::aarch64:
  case Triple::aarch64_be:
    Arch = "arm64";
    break;
  case Triple::x86_64:
    Arch = "amd64";
    break;
  case Triple::x86:
    Arch = "386";
    break;
  default:
    Arch = T.getArchName();
    break;
  }
  StringRef OS;
  switch (T.getOS()) {
  case Triple::Darwin:
  case Triple::MacOSX:
  case Triple::IOS:
    OS = "darwin";
    break;
  case Triple::Linux:
    OS = "linux";
    break;
  case Triple::OpenBSD:
    OS = "openbsd";
    break;
  case Triple::AIX:
    OS = "aix";
    break;
  default:
    OS = T.getOSName();
    break;
  }
  return getNosplitBudget(Arch, OS);
}

//===----------------------------------------------------------------------===//
// Frame estimate + unanalyzable-call/alloca check (target-agnostic).
//===----------------------------------------------------------------------===//

uint64_t llvm::c2go::estimateLeafFrameBytes(const Function &F,
                                            uint64_t LinkageCushionBytes) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  // Per-target linkage cushion: arm64 = saved-LR + saved-FP (16B);
  // amd64 = pushed RA + saved-BP (16B); i386 = pushed RA + saved-BP (8B,
  // but callers pass 16 as a conservative cushion). Backends pass the value
  // explicitly so this TU stays target-neutral.
  uint64_t Bytes = LinkageCushionBytes;
  for (const Instruction &I : instructions(F)) {
    if (const auto *AI = dyn_cast<AllocaInst>(&I)) {
      if (!AI->isStaticAlloca())
        continue; // dynamic alloca handled by hasUnanalyzableCallOrAlloca
      if (auto Size = AI->getAllocationSize(DL))
        Bytes += alignTo(Size->getFixedValue(), 8);
      else
        Bytes += 8;
    }
  }
  // Reserve a generous outgoing-arg / spill cushion. 16 callee-arg slots * 8B.
  Bytes += 128;
  return alignTo(Bytes, 16);
}

bool llvm::c2go::hasUnanalyzableCallOrAlloca(const Function &F) {
  for (const Instruction &I : instructions(F)) {
    if (const auto *AI = dyn_cast<AllocaInst>(&I)) {
      if (!AI->isStaticAlloca())
        return true; // VLA / alloca() -> dynamic SP movement.
      continue;
    }
    const auto *CB = dyn_cast<CallBase>(&I);
    if (!CB)
      continue;
    // Statepoint-aware: a gc.statepoint itself reports isIndirectCall()==false
    // (its operand 0 is the intrinsic), so handle it explicitly. A null actual
    // callee means a true indirect call wrapped by the statepoint -> not
    // analyzable; a non-null callee is a direct wrapped call whose subtree is
    // walked via collectDirectCallees.
    if (const auto *SP = dyn_cast<GCStatepointInst>(CB)) {
      if (SP->getActualCalledFunction() == nullptr)
        return true;
      continue;
    }
    // Intrinsics that lower to no call (lifetime/dbg/etc.) are harmless; a
    // called intrinsic that is a real libcall would show as an external decl
    // below. Indirect calls cannot be resolved within the TU.
    if (CB->isIndirectCall())
      return true;
    if (CB->isInlineAsm())
      return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Eligibility analysis.
//===----------------------------------------------------------------------===//

namespace {

/// Statepoint-aware "is this function's address taken?" check.
///
/// Under -O2, RewriteStatepointsForGC rewrites `call @foo` into
/// `call @llvm.experimental.gc.statepoint(..., @foo, ...)`, moving the real
/// callee into the statepoint's CalledFunctionPos operand. A naive
/// Function::hasAddressTaken() then reports every such direct call as an
/// address-taken use, which would reject ALL eligible leaves post-RS4GC. We
/// special-case the single direct-call operand of a GCStatepointInst (and an
/// ordinary CallBase callee operand); every other use — gc-live / deopt bundle
/// operands, plain call arguments, stores, global initializers, bitcasts — is
/// treated as address-taken (fail-closed).
bool isC2GoAddressTaken(const Function &F) {
  for (const Use &U : F.uses()) {
    const User *FU = U.getUser();
    if (const auto *CB = dyn_cast<CallBase>(FU)) {
      if (const auto *SP = dyn_cast<GCStatepointInst>(CB)) {
        if (&U == &SP->getOperandUse(GCStatepointInst::CalledFunctionPos) &&
            SP->getActualCalledFunction() == &F)
          continue; // direct call wrapped by a statepoint
        return true; // gc-live / deopt / arg operand => address-taken
      }
      if (CB->isCallee(&U))
        continue;  // ordinary direct call
      return true; // passed as a call argument => address-taken
    }
    return true; // stored / global init / cast => address-taken
  }
  return false;
}

/// Direct, in-TU, defined callees of \p F (deduplicated, order-stable).
void collectDirectCallees(const Function &F,
                          SmallVectorImpl<const Function *> &Out) {
  SmallPtrSet<const Function *, 8> Seen;
  for (const Instruction &I : instructions(F)) {
    const auto *CB = dyn_cast<CallBase>(&I);
    if (!CB)
      continue;
    // Statepoint-aware: under -O2 the real callee lives in the statepoint's
    // CalledFunctionPos operand; getCalledFunction() would return the
    // gc.statepoint intrinsic instead. A null actual callee means a true
    // indirect call (handled by hasUnanalyzableCallOrAlloca).
    const Function *Callee;
    if (const auto *SP = dyn_cast<GCStatepointInst>(CB))
      Callee = SP->getActualCalledFunction();
    else
      Callee = CB->getCalledFunction();
    if (!Callee)
      continue;
    if (Callee->isIntrinsic())
      continue;
    if (Seen.insert(Callee).second)
      Out.push_back(Callee);
  }
}

/// Recursive subtree analysis. Returns the max accumulated stack bytes over
/// F's downstream subtree, or std::nullopt if the subtree is not fully
/// analyzable within this TU (external/declared callee, indirect call,
/// recursion, dynamic alloca). \p OnStack tracks the current DFS path to
/// detect recursion (direct or mutual).
struct SubtreeAnalyzer {
  std::function<uint64_t(const Function &)> FrameOf;
  // Memoized subtree size for fully-analyzable functions.
  DenseMap<const Function *, uint64_t> Memo;
  // Functions known to be unanalyzable (caches negative results).
  SmallPtrSet<const Function *, 16> Bad;

  // Explicit ctor so Memo/Bad default-construct (DenseMap/SmallPtrSet have
  // explicit default ctors, which aggregate-init `{}` cannot use).
  SubtreeAnalyzer(std::function<uint64_t(const Function &)> Fn)
      : FrameOf(std::move(Fn)) {}

  std::optional<uint64_t>
  subtreeBytes(const Function &F,
               SmallPtrSetImpl<const Function *> &OnStack) {
    if (auto It = Memo.find(&F); It != Memo.end())
      return It->second;
    if (Bad.count(&F))
      return std::nullopt;

    // A declaration (no body) is external to this TU -> unanalyzable.
    if (F.isDeclaration()) {
      Bad.insert(&F);
      return std::nullopt;
    }
    // Recursion (this function already on the current DFS path).
    if (!OnStack.insert(&F).second) {
      Bad.insert(&F);
      return std::nullopt;
    }

    std::optional<uint64_t> Result;
    if (hasUnanalyzableCallOrAlloca(F)) {
      Result = std::nullopt;
    } else {
      uint64_t Own = FrameOf(F);
      uint64_t MaxChild = 0;
      bool Ok = true;
      SmallVector<const Function *, 8> Callees;
      collectDirectCallees(F, Callees);
      for (const Function *C : Callees) {
        auto Child = subtreeBytes(*C, OnStack);
        if (!Child) {
          Ok = false;
          break;
        }
        MaxChild = std::max(MaxChild, *Child);
      }
      Result = Ok ? std::optional<uint64_t>(Own + MaxChild) : std::nullopt;
    }

    OnStack.erase(&F);
    if (Result)
      Memo[&F] = *Result;
    else
      Bad.insert(&F);
    return Result;
  }
};

} // namespace

DenseMap<const Function *, LeafEligibility>
llvm::c2go::analyzeC2GoLeafEligibility(
    const Module &M, const LeafEligibilityTargetHooks &Hooks,
    const DenseMap<const Function *, uint64_t> *PerFuncFrameBytes,
    bool IgnoreAddressTaken) {
  const unsigned Budget = Hooks.Budget;

  auto FrameOf = [&](const Function &F) -> uint64_t {
    if (PerFuncFrameBytes) {
      if (auto It = PerFuncFrameBytes->find(&F); It != PerFuncFrameBytes->end())
        return It->second;
    }
    return Hooks.FrameOf(F);
  };

  SubtreeAnalyzer SA{FrameOf};

  DenseMap<const Function *, LeafEligibility> Result;
  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;

    LeafEligibility E;

    // (a) Must not be a c2go_extern / GoABI0-boundary symbol. Those are called
    // by Go through the declared ABI0 stack layout, so the private register
    // convention cannot apply. In c2go-mode IR, BOTH internal and boundary
    // functions carry CallingConv::GoABI0; clang distinguishes them with the
    // `c2go-reg-return` fn-attr (present on internal functions only — see
    // §2.0.2 / CodeGenModule.cpp:2715). Absence of the attr => boundary symbol.
    if (!F.hasFnAttribute("c2go-reg-return")) {
      E.IneligibleReason = "c2go_extern / GoABI0 boundary (no c2go-reg-return)";
      Result[&F] = E;
      continue;
    }

    // (a.5) A c2go void** tagged-argument-pack variadic function must keep
    // GoABI0 stack passing: its argsize reserves the trailing void** slot and
    // va_arg walks the packed array as a stack object, so the private register
    // ABI cannot apply. clang tags it with c2go-void-vararg (the #495 wrapper
    // skips variadic functions likewise); never register-flip one.
    if (F.hasFnAttribute("c2go-void-vararg")) {
      E.IneligibleReason = "c2go void** variadic (keeps GoABI0 stack passing)";
      Result[&F] = E;
      continue;
    }

    // (b) Internal linkage only (C `static`). A non-internal function may have
    // cross-TU callers that, lacking out-of-band ABI metadata, would marshal
    // via the default ABI0 -> mismatch. (Cross-TU near-leaf is deferred to the
    // c2go-lto two-pass build, §4.2.1.)
    if (!F.hasLocalLinkage()) {
      E.IneligibleReason = "not static (cross-TU callers)";
      Result[&F] = E;
      continue;
    }

    // Address-taken functions can be reached via indirect calls, which must
    // use one uniform ABI; exclude them (§4.2.1). Statepoint-aware: a direct
    // call wrapped by gc.statepoint (the real callee in CalledFunctionPos) is
    // NOT an address-taken use (see isC2GoAddressTaken). When IgnoreAddressTaken
    // is set (clang #495 wrapper pre-check) this gate is skipped so the caller
    // can ask "would F be eligible if it were NOT address-taken?".
    if (!IgnoreAddressTaken && isC2GoAddressTaken(F)) {
      E.IneligibleReason = "address-taken (reachable indirectly)";
      Result[&F] = E;
      continue;
    }

    // (c) Whole downstream subtree analyzable in this TU + within budget.
    SmallVector<const Function *, 8> Callees;
    collectDirectCallees(F, Callees);
    E.IsStrictLeaf = Callees.empty() && !hasUnanalyzableCallOrAlloca(F);

    SmallPtrSet<const Function *, 16> OnStack;
    auto Subtree = SA.subtreeBytes(F, OnStack);
    if (!Subtree) {
      E.IneligibleReason =
          "downstream subtree not statically analyzable in this TU "
          "(external/indirect call, recursion, or dynamic alloca)";
      Result[&F] = E;
      continue;
    }

    E.SubtreeStackBytes = *Subtree;
    if (*Subtree > Budget) {
      E.IneligibleReason = "subtree stack exceeds NOSPLIT budget";
      Result[&F] = E;
      continue;
    }

    E.Eligible = true;
    Result[&F] = E;
  }

  return Result;
}

//===----------------------------------------------------------------------===//
// CC-flip worker shared by AArch64 + X86 backend passes.
//===----------------------------------------------------------------------===//

bool llvm::c2go::runC2GoLeafCCFlip(Module &M,
                                    const LeafEligibilityTargetHooks &Hooks,
                                    StringRef ExtraGate) {
  // Default ON; only disabled when the emergency flag lists "leaf-abi".
  if (llvm::c2go::isC2GoDisabled("leaf-abi"))
    return false;
  // Only act on c2go-mode modules (the GoABI0 / Plan 9 pipeline). For any
  // other module the convention is meaningless and must not be applied.
  if (M.getModuleFlag(llvm::c2go::kGoabiModuleFlag) == nullptr)
    return false;
  // Optional second gate: a target may pass a non-empty ExtraGate module-flag
  // name to require it in addition to c2go.goabi. (X86's leaf flip no longer
  // uses a second gate — it passes ExtraGate="" — now that #298 landed the
  // real CC_X86_64_C2GoABIInternal lowering.)
  if (!ExtraGate.empty() && M.getModuleFlag(ExtraGate) == nullptr)
    return false;

  auto Elig = analyzeC2GoLeafEligibility(M, Hooks);

  bool Changed = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    auto It = Elig.find(&F);
    if (It == Elig.end() || !It->second.Eligible)
      continue;

    LLVM_DEBUG(dbgs() << "c2go-leaf-abi: " << F.getName()
                      << " -> C2GoABIInternal (subtree "
                      << It->second.SubtreeStackBytes << "B, strictLeaf="
                      << It->second.IsStrictLeaf << ")\n");

    F.setCallingConv(CallingConv::C2GoABIInternal);
    ++NumC2GoLeafFlipped;
    Changed = true;
  }

  if (!Changed)
    return false;

  // Second pass: update every DIRECT call site whose callee was flipped, so
  // caller and callee agree on the register convention. Eligible callees are
  // never address-taken (rejected in the analysis), so all reachable calls to
  // them are direct CallBases we can see and rewrite here.
  for (Function &F : M) {
    for (Instruction &I : instructions(F)) {
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      // Statepoint-aware: under -O2 the direct callee is the statepoint's
      // CalledFunctionPos operand; setting the CC on the statepoint
      // instruction itself makes the register convention reach the call site.
      Function *Callee;
      if (auto *SP = dyn_cast<GCStatepointInst>(CB))
        Callee = SP->getActualCalledFunction();
      else
        Callee = CB->getCalledFunction();
      if (!Callee)
        continue;
      if (Callee->getCallingConv() == CallingConv::C2GoABIInternal)
        CB->setCallingConv(CallingConv::C2GoABIInternal);
    }
  }

  return true;
}
