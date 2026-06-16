//===- AArch64C2GoPtrSlotLiveness.cpp - c2go GC #330 Milestone 5 ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go GC Approach B (#330) Milestone 5: per-PC liveness of pointer-tagged
// spill slots, mirroring Go's `cmd/compile/internal/liveness/plive.go`
// stack-map computation but at the MIR / frame-index level.
//
// Background. M1 (AArch64InstrInfo::isC2GoPointerDerivedReg) classifies a
// vreg as pointer-derived via its SSA def-chain. M2 (InlineSpiller::spillAll)
// tags every spill slot whose Original vreg was pointer-derived with
// `MachineFrameInfo::setC2GoSpillSlotTag(FI, "ptr")`. Before M5 those tags
// were turned into stackmap bits at EVERY call site (LowerSTATEPOINT and the
// AArch64FrameLowering BODY mask): a coarse all-PCs OR-in. That over-marks:
//
//   * a function whose first call A happens BEFORE the first spill of a
//     ptr-tagged FI: at A the slot holds uninitialised stack bytes — Go's
//     copystack reads those as a pointer and aborts or silently corrupts;
//   * a function whose last reload of a ptr-tagged FI happens at PC X and
//     whose later call B reuses that physical word (spiller snippet
//     mergeable-spill / hoisted-spill at a different SlotIndex, or simply
//     "the slot is now garbage") — copystack again sees a non-pointer at
//     that PC.
//
// M5 fixes this by computing per-call liveness for every ptr-tagged FI:
//
//   * Per FI it scans every memory access. A frame STORE means the slot now
//     holds a value (set "ready"). A frame LOAD touches the slot but does
//     not change ready-ness. The dataflow is reach-defs over the FI:
//         IN(B) =  union over predecessors of OUT(B').
//         OUT(B) = transfer(B, IN(B)).
//         transfer: scan MIs forward; "ready" becomes true after a store to
//         FI; the load itself does not flip ready false (it consumes the
//         value but the bit pattern is still there next time).
//
//   * Live-use direction: "ready" alone is not enough — a slot whose
//     pointer was loaded for the last time before the call is dead, even
//     though the bits are still there. Mirror Go's value-level liveness:
//     compute BACKWARD reach-uses (the slot has a USE after the call).
//         USE_OUT(B) = union over successors S of USE_IN(S).
//         USE_IN(B)  = transfer_back(B, USE_OUT(B)).
//         transfer_back: scan MIs backward; a load sets has-use-after to
//         true; nothing in this backward direction clears it (a later
//         store DOES end the value, but for stack-map soundness we want
//         "slot is live across this call" = "value will be read again
//         before the next write or end-of-function", which is exactly
//         live-out at the call PC).
//
//   * A slot is LIVE at a call iff (ready at the call) AND (has-use after
//     the call). The set of (Call MI, FIs) is recorded on the
//     AArch64FunctionInfo via `setC2GoLiveSpillSlotsAtCall`. The AsmPrinter
//     replaces its old "OR every ptr-tagged slot" loop with a lookup into
//     this set, falling back to the conservative loop only if
//     `isC2GoPtrSlotLivenessValid()` is false (pass disabled or no c2go).
//
// Pass placement. We schedule at `addPreEmitPass2` — the latest hook before
// AsmPrinter — so MI pointers are stable (post outliner, post BB sections,
// post unpack-bundles). We require PEI to have run (spill MIs have
// FixedStackPSV in their MachineMemOperands) which is true at this point.
//
// Soundness invariant (finding 3: path-conditional typing).
//
// M5's `Live = Ready ∧ UseAfter` is a one-bit-per-FI static encoding; it
// cannot represent path-conditional typing of a single slot ("on path A the
// slot holds a ptr, on path B it holds an i64"). For M5 to be sound, the
// FRAME-INDEX-LEVEL invariant "FI is tagged ptr ⇒ every store to FI on
// every reaching path stores a managed pointer" must hold. Two upstream
// guarantees together establish it:
//
//   1. M2 (InlineSpiller::spillAll) tags an FI "ptr" iff the Original vreg
//      being spilled was classified pointer-derived by M1. A single FI is
//      owned by exactly one Original vreg's spill sequence at this stage;
//      every store/load of that FI corresponds to a def/use of the same
//      pointer-derived SSA value. Tag is therefore consistent across all
//      paths reaching any load of FI.
//
//   2. StackSlotColoring may MERGE two FIs onto a single physical color
//      after M2, which could in principle silently combine a ptr-tagged
//      slot and an untagged-i64 slot. The `TagsCompatible` predicate
//      blocks that merge (finding 1 also propagates the tag onto the
//      fresh-color path, so the merged slot always carries a single
//      consistent tag). Result: a "ptr"-tagged physical slot is only
//      written by ptr-derived vregs on every reaching path.
//
// Under invariants (1) and (2), at any safepoint where M5 marks a "ptr" FI
// as live, every reaching path stored a managed pointer to it. Path-
// conditional typing cannot arise. If a future change loosens either
// invariant, M5 must be re-examined (and likely augmented with an
// entry-null-init for ptr-tagged spill slots, mirroring the alloca
// null-init in C2GoFoldAllocaRelocatesPass).
//
//===----------------------------------------------------------------------===//

#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/C2GoEmergencyFlag.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

using namespace llvm;

#define DEBUG_TYPE "aarch64-c2go-ptrslot-liveness"
#define PASS_NAME "AArch64 c2go GC #330 M5: per-PC pointer-slot liveness"

// #377: was `-c2go-gc-ptrslot-liveness` cl::opt (default ON). M5 per-PC
// liveness for pointer-tagged spill slots is mature (#330 SQLite 7×150
// soak). Operators can disable via `-mllvm -c2go-disable=ptrslot-liveness`
// if a regression is reported; without M5, AsmPrinter falls back to the
// all-PCs OR-in (over-mark) path.

namespace {

class AArch64C2GoPtrSlotLiveness : public MachineFunctionPass {
public:
  static char ID;
  AArch64C2GoPtrSlotLiveness() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return PASS_NAME; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // namespace

char AArch64C2GoPtrSlotLiveness::ID = 0;

INITIALIZE_PASS(AArch64C2GoPtrSlotLiveness, "aarch64-c2go-ptrslot-liveness",
                PASS_NAME, false, false)

FunctionPass *llvm::createAArch64C2GoPtrSlotLivenessPass() {
  return new AArch64C2GoPtrSlotLiveness();
}

namespace {

// Collect every (FI, store-or-load) memory access on \p MI. Uses MachineMemO
// FixedStackPseudoSourceValue, the only reliable channel post-PEI (the
// original MO.isFI() operand has been replaced by [SP, #imm] by then).
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

bool AArch64C2GoPtrSlotLiveness::runOnMachineFunction(MachineFunction &MF) {
  if (llvm::c2go::isC2GoDisabled("ptrslot-liveness"))
    return false;
  // c2go-mode gate. Non-c2go translation units must be byte-identical.
  if (!MF.getFunction().getParent()->getModuleFlag(llvm::c2go::kGoabiModuleFlag))
    return false;

  MachineFrameInfo &MFI = MF.getFrameInfo();
  AArch64FunctionInfo &AFI = *MF.getInfo<AArch64FunctionInfo>();

  // Collect the ptr-tagged FIs. If empty, mark valid (no over-mark possible)
  // and return; AsmPrinter will see an empty live-set at every call.
  // #375 slice 3: spill-slot tags now live on AArch64FunctionInfo.
  SmallVector<int, 8> PtrFIs;
  for (const auto &KV : AFI.getC2GoSpillSlotTags()) {
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
  // crucial behavioural switch over pre-M5 default (which OR'd everything).
  AFI.setC2GoPtrSlotLivenessValid(true);
  if (PtrFIs.empty())
    return false;

  // Number of FIs / bitvector width.
  unsigned N = PtrFIs.size();
  DenseMap<int, unsigned> FIToBit;
  for (unsigned I = 0; I < N; ++I)
    FIToBit[PtrFIs[I]] = I;

  // -------- Forward pass: "ready" (must-reach-defs / available expressions).
  // A bit is set at the entry to a program point iff EVERY path from
  // function entry to that point passes through a store to the FI. This is
  // strictly stronger than may-reach: at a call PC the slot's bytes are
  // guaranteed to be the result of some store (not unwritten stack garbage),
  // so it is safe for copystack to interpret them as a pointer.
  //
  // Lattice direction: forward; meet = INTERSECTION (must-reach).
  // ReadyIn[entry]  = 0  (nothing stored on entry).
  // ReadyIn[B]      = intersection over predecessors P of ReadyOut[P] for
  //                   non-entry blocks. With no predecessors and not entry
  //                   (unreachable), seed with "all ones" so the meet is
  //                   non-restrictive (the block contributes only what its
  //                   own stores establish).
  // ReadyOut[B]     = transfer(B, ReadyIn[B]).
  // transfer:       scan MIs forward; a store-to-FI sets ready[FI] = 1.
  //                 Loads do not change ready (slot still holds the value).

  unsigned NumBlocks = MF.getNumBlockIDs();
  // Pre-compute the entry-reachable block set. A dead MBB (no path from
  // entry) left over by earlier passes still owns a numeric slot in the
  // function's block numbering, so its ReadyOut initial value would
  // otherwise be the "all ones" intersection identity and silently
  // contaminate any join MBB it happens to be a predecessor of —
  // over-marking slots that have no actual store on any live path.
  // BFS from MF.front(), then below treat unreachable MBBs as zero-Out
  // and skip them as predecessors. (Finding s9.)
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
  // Use "all ones" sentinel for unvisited reachable blocks so meet
  // (intersection) is correct on the first sweep. Unreachable blocks get
  // an all-zeros Out so they cannot contaminate any join via &=.
  std::vector<BitVector> ReadyIn(NumBlocks, BitVector(N, true));
  std::vector<BitVector> ReadyOut(NumBlocks, BitVector(N, true));
  for (unsigned I = 0; I < NumBlocks; ++I)
    if (!Reachable.test(I))
      ReadyOut[I].reset();
  if (auto *Entry = &MF.front()) {
    ReadyIn[Entry->getNumber()].reset();
    // Seed entry's Out by transferring through it from a zero IN.
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

  auto transferForward = [&](const MachineBasicBlock &MBB,
                             BitVector &State) {
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

  // Worklist forward propagation (must-reach / intersection meet).
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (const MachineBasicBlock &MBB : MF) {
      unsigned Idx = MBB.getNumber();
      // Entry block is fixed (handled above); only recompute Out.
      if (&MBB == &MF.front()) {
        BitVector NewOut = ReadyIn[Idx];
        transferForward(MBB, NewOut);
        if (NewOut != ReadyOut[Idx]) {
          ReadyOut[Idx] = NewOut;
          Changed = true;
        }
        continue;
      }
      // Skip blocks that themselves are not entry-reachable: they cannot
      // contribute to a sound must-reach result and their state is fixed
      // at all-zeros above.
      if (!Reachable.test(Idx))
        continue;
      BitVector NewIn(N, true); // identity for intersection
      bool HasPred = false;
      for (const MachineBasicBlock *P : MBB.predecessors()) {
        // An unreachable pred carries no information; meeting against its
        // all-zeros Out would wipe the join, and meeting against any
        // initial all-ones would falsely keep bits — finding s9.
        if (!Reachable.test(P->getNumber()))
          continue;
        NewIn &= ReadyOut[P->getNumber()];
        HasPred = true;
      }
      if (!HasPred)
        NewIn.reset(); // unreachable (no reachable preds and not entry)
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

  // -------- Backward pass: classic reach-uses ("live-value liveness").
  // UseOut[B] = union over successors S of UseIn[S].
  // UseIn[B]  = transfer_back(B, UseOut[B]).
  // transfer_back: scan MIs in reverse;
  //   * a STORE to FI KILLS the bit (the prior contents are overwritten —
  //     anything reachable from PCs above this store sees a NEW value, so
  //     this store decouples "before" and "after");
  //   * a LOAD from FI SETs the bit (the value at this PC is used; reach
  //     backward across the load).
  // This is the standard backward dataflow you get for treating FI as a
  // memory location with single-word semantics. It correctly models slot
  // reuse: if a slot was stored a ptr, used, then re-stored an int and
  // re-used as an int, the ptr-store is killed by the int-store and the
  // int-use does not back-propagate past the int-store.

  std::vector<BitVector> UseIn(NumBlocks, BitVector(N, false));
  std::vector<BitVector> UseOut(NumBlocks, BitVector(N, false));

  auto transferBackward = [&](const MachineBasicBlock &MBB,
                              BitVector &State) {
    // Reverse-walk MIs.
    for (auto It = MBB.rbegin(), E = MBB.rend(); It != E; ++It) {
      const MachineInstr &MI = *It;
      // Finding 4: a single MI can have BOTH a load and a store MMO for the
      // same FI (rare). The semantic we want — store-kill BEFORE load-set so
      // a load-store pair on the same MI leaves the bit set — must be
      // independent of MMO operand order. `collectFIAccesses` returns
      // accesses in MMO order; iterating naively `if (IsStore) reset; if
      // (IsLoad) set;` on the SEQUENCE `[load, store]` would produce
      // load->set followed by store->reset = bit reset (WRONG). Group per
      // MI by FI first, then apply store-kill before load-set.
      SmallDenseMap<int, std::pair<bool, bool>, 2> PerFI; // FI -> (hasStore, hasLoad)
      for (const MemFIAccess &A : collectFIAccesses(MI)) {
        if (!FIToBit.contains(A.FI))
          continue;
        auto &E = PerFI[A.FI];
        if (A.IsStore)
          E.first = true;
        if (A.IsLoad)
          E.second = true;
      }
      for (const auto &KV : PerFI) {
        unsigned Bit = FIToBit[KV.first];
        bool HasStore = KV.second.first;
        bool HasLoad = KV.second.second;
        if (HasStore)
          State.reset(Bit);
        if (HasLoad)
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
  // "Ready at call" = ready-state immediately BEFORE the MI executes (i.e.
  // every prior MI in the block has been transferred). "UseAfter at call" =
  // use-state immediately AFTER the MI (i.e. start from the block's UseOut
  // and reverse-walk back to just past this MI).

  unsigned NumSites = 0;
  for (MachineBasicBlock &MBB : MF) {
    unsigned Idx = MBB.getNumber();
    // Forward sweep state, restarting from block's ReadyIn.
    BitVector Ready = ReadyIn[Idx];
    // We will compute UseAfter[MI] on demand by reverse-walking. To do this
    // efficiently in one pass, build a per-MI position list once.
    SmallVector<const MachineInstr *, 16> CallMIs;
    // First sub-pass forward: collect call positions and snapshot Ready
    // BEFORE each call.
    SmallVector<BitVector, 8> ReadyAtCall;
    for (MachineInstr &MI : MBB) {
      if (MI.isCall()) {
        ReadyAtCall.push_back(Ready);
        CallMIs.push_back(&MI);
      }
      // Apply transfer for this MI to Ready.
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

    // Second sub-pass backward: collect UseAfter for each call MI by
    // reverse-walking from UseOut[block] and applying loads BACKWARDS until
    // we pass the MI. UseAfter[MI] is the state AFTER the MI executes —
    // i.e. AFTER all later MIs have been backward-transferred (we are
    // walking from the block tail forward in reverse). Equivalently, when
    // the reverse cursor is at MI, the bits accumulated represent the
    // "live use" set seen from the MIs strictly after MI (and from
    // successor blocks via UseOut), which is exactly UseAfter[MI].
    BitVector UseAfter = UseOut[Idx];
    SmallVector<BitVector, 8> UseAfterAtCall(CallMIs.size(), BitVector(N, false));
    size_t CallIdxFromEnd = CallMIs.size();
    for (auto It = MBB.rbegin(), E = MBB.rend(); It != E; ++It) {
      const MachineInstr &MI = *It;
      if (MI.isCall()) {
        // The call is AT this PC; UseAfter is the state strictly AFTER MI,
        // which is what we currently have (we have NOT yet applied this
        // MI's loads — loads at the call MI itself, if any, would belong
        // to the "before" view anyway since the call uses them).
        --CallIdxFromEnd;
        UseAfterAtCall[CallIdxFromEnd] = UseAfter;
      }
      // Apply backward transfer for this MI: store-kill BEFORE load-set so
      // a load+store on the same MI leaves the bit set (cf. transferBackward).
      // Finding 4: group accesses per-FI before applying so the semantics
      // are independent of MMO operand order within MI.
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
        bool HasStore = KV.second.first;
        bool HasLoad = KV.second.second;
        if (HasStore)
          UseAfter.reset(Bit);
        if (HasLoad)
          UseAfter.set(Bit);
      }
    }
    assert(CallIdxFromEnd == 0 && "Unbalanced call-position sub-passes");

    // Combine and emit per-call live set.
    for (size_t I = 0, E = CallMIs.size(); I < E; ++I) {
      BitVector Live = ReadyAtCall[I];
      Live &= UseAfterAtCall[I];
      if (Live.none())
        continue;
      SmallVector<int, 4> LiveFIs;
      LiveFIs.reserve(Live.count());
      for (int Bit = Live.find_first(); Bit >= 0;
           Bit = Live.find_next(Bit)) {
        LiveFIs.push_back(PtrFIs[Bit]);
      }
      AFI.setC2GoLiveSpillSlotsAtCall(CallMIs[I], LiveFIs);
      ++NumSites;
    }
  }

  LLVM_DEBUG(dbgs() << "c2go-gc-B M5: " << MF.getName() << ": " << PtrFIs.size()
                    << " ptr-tagged FIs, " << NumSites
                    << " call-sites with live ptr-slots\n");
  return false; // analysis only — no MIR changes
}
