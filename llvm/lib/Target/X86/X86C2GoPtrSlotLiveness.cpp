//===- X86C2GoPtrSlotLiveness.cpp - c2go GC #330 Milestone 5 (X86) --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go GC Approach B (#330) Milestone 5, X86 port (#298): per-PC liveness of
// pointer-tagged spill slots. FAITHFUL MIRROR of
// AArch64C2GoPtrSlotLiveness.cpp — the dataflow is target-NEUTRAL (it operates
// on FrameIndices and MachineMemOperand FixedStackPseudoSourceValues, which are
// target-independent). The only X86-specific difference is the storage type:
// the M2 spill-slot tags + the M5 result set live on X86MachineFunctionInfo
// (the tags were relocated off MachineFrameInfo onto each target's MFI; see
// AArch64FunctionInfo's #375/#426 history). See the AArch64 file's header for
// the full algorithm rationale and soundness invariant; the comment density
// here is reduced to avoid duplicating that prose verbatim.
//
// Summary of the algorithm (identical to AArch64):
//
//   * M2 (X86InstrInfo::storeRegToStackSlot) tags every spill slot whose
//     spilled vreg was classified pointer-derived with
//     `X86MachineFunctionInfo::setC2GoSpillSlotTag(FI, "ptr")`.
//   * Forward pass computes "ready" = must-reach-defs (every path from entry to
//     the PC passes a store to FI), so a call PC reading the slot sees real
//     stored bytes (not unwritten stack garbage). Meet = INTERSECTION.
//   * Backward pass computes reach-uses (the slot has a use after the call).
//     Meet = UNION; a store KILLS the bit, a load SETs it.
//   * A slot is LIVE at a call iff (ready) AND (used-after). The per-call set
//     is recorded via `setC2GoLiveSpillSlotsAtCall`; the AsmPrinter
//     (X86MCInstLower) replaces its conservative "OR every ptr-tagged slot"
//     loop with a lookup, falling back only if `isC2GoPtrSlotLivenessValid()`
//     is false.
//
// Pass placement: `addPreEmitPass2` (latest hook before AsmPrinter — MI
// pointers stable, spill MIs carry FixedStackPSV in their MMOs).
//
// Soundness invariant (path-conditional typing): identical to AArch64. M5's
// one-bit-per-FI encoding is sound because (1) M2 tags an FI "ptr" iff the
// Original vreg was pointer-derived, and (2) StackSlotColoring's `TagsCompatible`
// merge-guard (consumed through TII::getStackSlotTypeTag, which X86 now
// overrides to route through X86MachineFunctionInfo) blocks merging a ptr slot
// with an untagged-i64 slot. Under (1)+(2) a "ptr"-tagged slot is only written
// by ptr-derived vregs on every reaching path.
//
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86MachineFunctionInfo.h"
#include "X86Subtarget.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/C2GoEmergencyFlag.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

using namespace llvm;

#define DEBUG_TYPE "x86-c2go-ptrslot-liveness"
#define PASS_NAME "X86 c2go GC #330 M5: per-PC pointer-slot liveness"

namespace {

class X86C2GoPtrSlotLiveness : public MachineFunctionPass {
public:
  static char ID;
  X86C2GoPtrSlotLiveness() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return PASS_NAME; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  bool runOnMachineFunction(MachineFunction &MF) override;
};

// Collect every (FI, store-or-load) memory access on \p MI via the
// FixedStackPseudoSourceValue on its MachineMemOperands — the only reliable
// channel post-PEI (the original MO.isFI() operand has been replaced by a
// [SP/BP, #imm] addressing mode by then). Target-neutral, mirror of AArch64.
struct MemFIAccess {
  int FI = -1;
  bool IsLoad = false;
  bool IsStore = false;
};
static SmallVector<MemFIAccess, 2> collectFIAccesses(const MachineInstr &MI) {
  SmallVector<MemFIAccess, 2> Out;
  for (const MachineMemOperand *MMO : MI.memoperands()) {
    if (auto *PSV = dyn_cast_or_null<FixedStackPseudoSourceValue>(
            MMO->getPseudoValue())) {
      MemFIAccess A;
      A.FI = PSV->getFrameIndex();
      A.IsLoad = MMO->isLoad();
      A.IsStore = MMO->isStore();
      Out.push_back(A);
    }
  }
  return Out;
}

} // namespace

char X86C2GoPtrSlotLiveness::ID = 0;

INITIALIZE_PASS(X86C2GoPtrSlotLiveness, "x86-c2go-ptrslot-liveness", PASS_NAME,
                false, false)

FunctionPass *llvm::createX86C2GoPtrSlotLivenessPass() {
  return new X86C2GoPtrSlotLiveness();
}

bool X86C2GoPtrSlotLiveness::runOnMachineFunction(MachineFunction &MF) {
  if (llvm::c2go::isC2GoDisabled("ptrslot-liveness"))
    return false;
  // c2go-mode gate. Non-c2go translation units must be byte-identical.
  if (!MF.getFunction().getParent()->getModuleFlag(
          llvm::c2go::kGoabiModuleFlag))
    return false;

  MachineFrameInfo &MFI = MF.getFrameInfo();
  X86MachineFunctionInfo &X86FI = *MF.getInfo<X86MachineFunctionInfo>();

  // Collect the ptr-tagged FIs. If empty, mark valid (no over-mark possible)
  // and return; AsmPrinter will see an empty live-set at every call.
  SmallVector<int, 8> PtrFIs;
  for (const auto &KV : X86FI.getC2GoSpillSlotTags()) {
    if (KV.second != "ptr")
      continue;
    int FI = KV.first;
    if (FI < MFI.getObjectIndexBegin() || FI >= MFI.getObjectIndexEnd() ||
        MFI.isDeadObjectIndex(FI) || MFI.getObjectAllocation(FI))
      continue;
    PtrFIs.push_back(FI);
  }
  // Mark valid unconditionally — even when PtrFIs is empty, the contract is
  // "M5 has spoken; trust the per-PC map (which is empty)". This is the
  // behavioural switch over the conservative all-PCs OR-in fallback.
  X86FI.setC2GoPtrSlotLivenessValid(true);
  if (PtrFIs.empty())
    return false;

  unsigned N = PtrFIs.size();
  DenseMap<int, unsigned> FIToBit;
  for (unsigned I = 0; I < N; ++I)
    FIToBit[PtrFIs[I]] = I;

  // -------- Forward pass: "ready" (must-reach-defs). Meet = INTERSECTION.
  unsigned NumBlocks = MF.getNumBlockIDs();

  // Entry-reachable block set (finding s9): a dead MBB left by earlier passes
  // owns a numeric slot whose all-ones intersection identity would contaminate
  // a join it happens to predecess. BFS from entry; treat unreachable MBBs as
  // zero-Out and skip them as predecessors.
  BitVector Reachable(NumBlocks, false);
  if (auto *Entry = &MF.front()) {
    SmallVector<const MachineBasicBlock *, 16> Worklist;
    Worklist.push_back(Entry);
    Reachable.set(Entry->getNumber());
    while (!Worklist.empty()) {
      const MachineBasicBlock *Cur = Worklist.pop_back_val();
      for (const MachineBasicBlock *S : Cur->successors())
        if (!Reachable.test(S->getNumber())) {
          Reachable.set(S->getNumber());
          Worklist.push_back(S);
        }
    }
  }

  std::vector<BitVector> ReadyIn(NumBlocks, BitVector(N, true));
  std::vector<BitVector> ReadyOut(NumBlocks, BitVector(N, true));
  for (unsigned I = 0; I < NumBlocks; ++I)
    if (!Reachable.test(I))
      ReadyOut[I].reset();
  if (auto *Entry = &MF.front()) {
    ReadyIn[Entry->getNumber()].reset();
    BitVector EntryOut(N, false);
    for (const MachineInstr &MI : *Entry) {
      for (const MemFIAccess &A : collectFIAccesses(MI)) {
        auto It = FIToBit.find(A.FI);
        if (It == FIToBit.end())
          continue;
        if (A.IsStore)
          EntryOut.set(It->second);
      }
    }
    ReadyOut[Entry->getNumber()] = EntryOut;
  }

  auto transferForward = [&](const MachineBasicBlock &MBB, BitVector &State) {
    for (const MachineInstr &MI : MBB) {
      for (const MemFIAccess &A : collectFIAccesses(MI)) {
        auto It = FIToBit.find(A.FI);
        if (It == FIToBit.end())
          continue;
        if (A.IsStore)
          State.set(It->second);
        // Loads do not change ready (the slot still holds the value).
      }
    }
  };

  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (const MachineBasicBlock &MBB : MF) {
      unsigned Idx = MBB.getNumber();
      if (&MBB == &MF.front()) {
        BitVector NewOut = ReadyIn[Idx];
        transferForward(MBB, NewOut);
        if (NewOut != ReadyOut[Idx]) {
          ReadyOut[Idx] = NewOut;
          Changed = true;
        }
        continue;
      }
      if (!Reachable.test(Idx))
        continue;
      BitVector NewIn(N, true); // identity for intersection
      bool HasPred = false;
      for (const MachineBasicBlock *P : MBB.predecessors()) {
        if (!Reachable.test(P->getNumber()))
          continue;
        NewIn &= ReadyOut[P->getNumber()];
        HasPred = true;
      }
      if (!HasPred)
        NewIn.reset();
      if (NewIn != ReadyIn[Idx]) {
        ReadyIn[Idx] = NewIn;
        Changed = true;
      }
      BitVector NewOut = NewIn;
      transferForward(MBB, NewOut);
      if (NewOut != ReadyOut[Idx]) {
        ReadyOut[Idx] = NewOut;
        Changed = true;
      }
    }
  }

  // -------- Backward pass: reach-uses ("live-value liveness").
  // UseOut[B] = union over successors of UseIn[S]; a STORE kills the bit, a
  // LOAD sets it. Models slot reuse correctly.
  std::vector<BitVector> UseIn(NumBlocks, BitVector(N, false));
  std::vector<BitVector> UseOut(NumBlocks, BitVector(N, false));

  auto transferBackward = [&](const MachineBasicBlock &MBB, BitVector &State) {
    for (auto It = MBB.rbegin(), E = MBB.rend(); It != E; ++It) {
      const MachineInstr &MI = *It;
      // Finding 4: group per-FI before applying so store-kill BEFORE load-set
      // is independent of MMO operand order (a load+store on the same MI must
      // leave the bit set).
      SmallDenseMap<int, std::pair<bool, bool>, 2> PerFI;
      for (const MemFIAccess &A : collectFIAccesses(MI)) {
        if (!FIToBit.contains(A.FI))
          continue;
        auto &PE = PerFI[A.FI];
        if (A.IsStore)
          PE.first = true;
        if (A.IsLoad)
          PE.second = true;
      }
      for (const auto &KV : PerFI) {
        unsigned Bit = FIToBit[KV.first];
        if (KV.second.first)
          State.reset(Bit);
        if (KV.second.second)
          State.set(Bit);
      }
    }
  };

  Changed = true;
  while (Changed) {
    Changed = false;
    for (const MachineBasicBlock &MBB : MF) {
      unsigned Idx = MBB.getNumber();
      BitVector NewOut(N, false);
      if (!MBB.succ_empty()) {
        bool First = true;
        for (const MachineBasicBlock *S : MBB.successors()) {
          if (First) {
            NewOut = UseIn[S->getNumber()];
            First = false;
          } else {
            NewOut |= UseIn[S->getNumber()];
          }
        }
      }
      if (NewOut != UseOut[Idx]) {
        UseOut[Idx] = NewOut;
        Changed = true;
      }
      BitVector NewIn = NewOut;
      transferBackward(MBB, NewIn);
      if (NewIn != UseIn[Idx]) {
        UseIn[Idx] = NewIn;
        Changed = true;
      }
    }
  }

  // -------- Walk calls; at each call MI compute LIVE = Ready & UseAfter.
  unsigned NumSites = 0;
  for (MachineBasicBlock &MBB : MF) {
    unsigned Idx = MBB.getNumber();
    BitVector Ready = ReadyIn[Idx];
    SmallVector<const MachineInstr *, 16> CallMIs;
    SmallVector<BitVector, 8> ReadyAtCall;
    for (MachineInstr &MI : MBB) {
      if (MI.isCall()) {
        ReadyAtCall.push_back(Ready);
        CallMIs.push_back(&MI);
      }
      for (const MemFIAccess &A : collectFIAccesses(MI)) {
        auto It = FIToBit.find(A.FI);
        if (It == FIToBit.end())
          continue;
        if (A.IsStore)
          Ready.set(It->second);
      }
    }

    if (CallMIs.empty())
      continue;

    BitVector UseAfter = UseOut[Idx];
    SmallVector<BitVector, 8> UseAfterAtCall(CallMIs.size(),
                                             BitVector(N, false));
    size_t CallIdxFromEnd = CallMIs.size();
    for (auto It = MBB.rbegin(), E = MBB.rend(); It != E; ++It) {
      const MachineInstr &MI = *It;
      if (MI.isCall()) {
        --CallIdxFromEnd;
        UseAfterAtCall[CallIdxFromEnd] = UseAfter;
      }
      SmallDenseMap<int, std::pair<bool, bool>, 2> PerFI;
      for (const MemFIAccess &A : collectFIAccesses(MI)) {
        if (!FIToBit.contains(A.FI))
          continue;
        auto &PE = PerFI[A.FI];
        if (A.IsStore)
          PE.first = true;
        if (A.IsLoad)
          PE.second = true;
      }
      for (const auto &KV : PerFI) {
        unsigned Bit = FIToBit[KV.first];
        if (KV.second.first)
          UseAfter.reset(Bit);
        if (KV.second.second)
          UseAfter.set(Bit);
      }
    }
    assert(CallIdxFromEnd == 0 && "Unbalanced call-position sub-passes");

    for (size_t I = 0, E = CallMIs.size(); I < E; ++I) {
      BitVector Live = ReadyAtCall[I];
      Live &= UseAfterAtCall[I];
      if (Live.none())
        continue;
      SmallVector<int, 4> LiveFIs;
      LiveFIs.reserve(Live.count());
      for (int Bit = Live.find_first(); Bit >= 0; Bit = Live.find_next(Bit))
        LiveFIs.push_back(PtrFIs[Bit]);
      X86FI.setC2GoLiveSpillSlotsAtCall(CallMIs[I], LiveFIs);
      ++NumSites;
    }
  }

  LLVM_DEBUG(dbgs() << "x86-c2go-gc-B M5: " << MF.getName() << ": "
                    << PtrFIs.size() << " ptr-tagged FIs, " << NumSites
                    << " call-sites with live ptr-slots\n");
  return false; // analysis only — no MIR changes
}
