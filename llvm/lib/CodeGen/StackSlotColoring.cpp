//===- StackSlotColoring.cpp - Stack slot coloring pass. ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the stack slot coloring pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/StackSlotColoring.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveDebugVariables.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveIntervalUnion.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/CodeGen/PseudoSourceValueManager.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <cstdint>
#include <iterator>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "stack-slot-coloring"

static cl::opt<bool>
DisableSharing("no-stack-slot-sharing",
             cl::init(false), cl::Hidden,
             cl::desc("Suppress slot sharing during stack coloring"));

static cl::opt<int> DCELimit("ssc-dce-limit", cl::init(-1), cl::Hidden);

STATISTIC(NumEliminated, "Number of stack slots eliminated due to coloring");
STATISTIC(NumDead,       "Number of trivially dead stack accesses eliminated");

namespace {

class StackSlotColoring {
  MachineFunction *MF = nullptr;
  MachineFrameInfo *MFI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  LiveStacks *LS = nullptr;
  const MachineBlockFrequencyInfo *MBFI = nullptr;
  SlotIndexes *Indexes = nullptr;

  // SSIntervals - Spill slot intervals.
  std::vector<LiveInterval *> SSIntervals;

  // SSRefs - Keep a list of MachineMemOperands for each spill slot.
  // MachineMemOperands can be shared between instructions, so we need
  // to be careful that renames like [FI0, FI1] -> [FI1, FI2] do not
  // become FI0 -> FI1 -> FI2.
  SmallVector<SmallVector<MachineMemOperand *, 8>, 16> SSRefs;

  // OrigAlignments - Alignments of stack objects before coloring.
  SmallVector<Align, 16> OrigAlignments;

  // OrigSizes - Sizes of stack objects before coloring.
  SmallVector<unsigned, 16> OrigSizes;

  // AllColors - If index is set, it's a spill slot, i.e. color.
  // FIXME: This assumes PEI locate spill slot with smaller indices
  // closest to stack pointer / frame pointer. Therefore, smaller
  // index == better color. This is per stack ID.
  SmallVector<BitVector, 2> AllColors;

  // NextColor - Next "color" that's not yet used. This is per stack ID.
  SmallVector<int, 2> NextColors = {-1};

  // UsedColors - "Colors" that have been assigned. This is per stack ID
  SmallVector<BitVector, 2> UsedColors;

  // Join all intervals sharing one color into a single LiveIntervalUnion to
  // speedup range overlap test.
  class ColorAssignmentInfo {
    // Single liverange (used to avoid creation of LiveIntervalUnion).
    LiveInterval *SingleLI = nullptr;
    // LiveIntervalUnion to perform overlap test.
    LiveIntervalUnion *LIU = nullptr;
    // LiveIntervalUnion has a parameter in its constructor so doing this
    // dirty magic.
    uint8_t LIUPad[sizeof(LiveIntervalUnion)];

  public:
    ~ColorAssignmentInfo() {
      if (LIU)
        LIU->~LiveIntervalUnion(); // Dirty magic again.
    }

    // Return true if LiveInterval overlaps with any
    // intervals that have already been assigned to this color.
    bool overlaps(LiveInterval *LI) const {
      if (LIU)
        return LiveIntervalUnion::Query(*LI, *LIU).checkInterference();
      return SingleLI ? SingleLI->overlaps(*LI) : false;
    }

    // Add new LiveInterval to this color.
    void add(LiveInterval *LI, LiveIntervalUnion::Allocator &Alloc) {
      assert(!overlaps(LI));
      if (LIU) {
        LIU->unify(*LI, *LI);
      } else if (SingleLI) {
        LIU = new (LIUPad) LiveIntervalUnion(Alloc);
        LIU->unify(*SingleLI, *SingleLI);
        LIU->unify(*LI, *LI);
        SingleLI = nullptr;
      } else
        SingleLI = LI;
    }
  };

  LiveIntervalUnion::Allocator LIUAlloc;

  // Assignments - Color to intervals mapping.
  SmallVector<ColorAssignmentInfo, 16> Assignments;

  // c2go (#492): per-color LOGICAL c2go type tag = the value class actually
  // assigned to each color so far. Seeded from the physical M2 spill tags,
  // overwritten on each fresh-color assignment in ColorSlot. ColorSlot's
  // TagsCompatible consults THIS instead of the physical-slot tag, which can be
  // a STALE "ptr" left by a ptr interval that got recolored elsewhere — a scalar
  // fresh-color would otherwise leave that stale tag and let a later ptr FI share
  // the color (TagsCompatible seeing "ptr"), over-marking the scalar. Empty for
  // non-c2go targets (the TII tag hooks are no-ops), so ColorSlot is unchanged.
  SmallVector<std::string, 16> C2GoColorTags;

  // c2go (#492): STABLE snapshot of each FI's ORIGINAL c2go type tag, taken
  // before the coloring loop and NEVER mutated. ColorSlot reads its current
  // FI's tag (LiTag) and seeds C2GoColorTags from here, so neither the share
  // decision nor the fresh-color logical-tag set ever reads the mutable
  // physical tag map (which an earlier interval could contaminate by picking a
  // not-yet-processed FI's index as its color). The end-of-coloring recompute
  // also reads this. Empty for non-c2go targets.
  SmallVector<std::string, 16> C2GoOrigTags;

public:
  StackSlotColoring(MachineFunction &MF, LiveStacks *LS,
                    MachineBlockFrequencyInfo *MBFI, SlotIndexes *Indexes)
      : MF(&MF), MFI(&MF.getFrameInfo()),
        TII(MF.getSubtarget().getInstrInfo()), LS(LS), MBFI(MBFI),
        Indexes(Indexes) {}
  bool run(MachineFunction &MF);

private:
  void InitializeSlots();
  void ScanForSpillSlotRefs(MachineFunction &MF);
  int ColorSlot(LiveInterval *li);
  bool ColorSlots(MachineFunction &MF);
  void RewriteInstruction(MachineInstr &MI, SmallVectorImpl<int> &SlotMapping,
                          MachineFunction &MF);
  bool RemoveDeadStores(MachineBasicBlock *MBB);
};

class StackSlotColoringLegacy : public MachineFunctionPass {
public:
  static char ID; // Pass identification

  StackSlotColoringLegacy() : MachineFunctionPass(ID) {
    initializeStackSlotColoringLegacyPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<SlotIndexesWrapperPass>();
    AU.addPreserved<SlotIndexesWrapperPass>();
    AU.addRequired<LiveStacksWrapperLegacy>();
    AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
    AU.addPreserved<MachineBlockFrequencyInfoWrapperPass>();
    AU.addPreservedID(MachineDominatorsID);

    // In some Target's pipeline, register allocation (RA) might be
    // split into multiple phases based on register class. So, this pass
    // may be invoked multiple times requiring it to save these analyses to be
    // used by RA later.
    AU.addPreserved<LiveIntervalsWrapperPass>();
    AU.addPreserved<LiveDebugVariablesWrapperLegacy>();

    MachineFunctionPass::getAnalysisUsage(AU);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // end anonymous namespace

char StackSlotColoringLegacy::ID = 0;

char &llvm::StackSlotColoringID = StackSlotColoringLegacy::ID;

INITIALIZE_PASS_BEGIN(StackSlotColoringLegacy, DEBUG_TYPE,
                      "Stack Slot Coloring", false, false)
INITIALIZE_PASS_DEPENDENCY(SlotIndexesWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LiveStacksWrapperLegacy)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(StackSlotColoringLegacy, DEBUG_TYPE, "Stack Slot Coloring",
                    false, false)

namespace {

// IntervalSorter - Comparison predicate that sort live intervals by
// their weight.
struct IntervalSorter {
  bool operator()(LiveInterval* LHS, LiveInterval* RHS) const {
    return LHS->weight() > RHS->weight();
  }
};

} // end anonymous namespace

/// ScanForSpillSlotRefs - Scan all the machine instructions for spill slot
/// references and update spill slot weights.
void StackSlotColoring::ScanForSpillSlotRefs(MachineFunction &MF) {
  SSRefs.resize(MFI->getObjectIndexEnd());

  // FIXME: Need the equivalent of MachineRegisterInfo for frameindex operands.
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB) {
      for (const MachineOperand &MO : MI.operands()) {
        if (!MO.isFI())
          continue;
        int FI = MO.getIndex();
        if (FI < 0)
          continue;
        if (!LS->hasInterval(FI))
          continue;
        LiveInterval &li = LS->getInterval(FI);
        if (!MI.isDebugInstr())
          li.incrementWeight(
              LiveIntervals::getSpillWeight(false, true, MBFI, MI));
      }
      for (MachineMemOperand *MMO : MI.memoperands()) {
        if (const FixedStackPseudoSourceValue *FSV =
                dyn_cast_or_null<FixedStackPseudoSourceValue>(
                    MMO->getPseudoValue())) {
          int FI = FSV->getFrameIndex();
          if (FI >= 0)
            SSRefs[FI].push_back(MMO);
        }
      }
    }
  }
}

/// InitializeSlots - Process all spill stack slot liveintervals and add them
/// to a sorted (by weight) list.
void StackSlotColoring::InitializeSlots() {
  int LastFI = MFI->getObjectIndexEnd();

  // There is always at least one stack ID.
  AllColors.resize(1);
  UsedColors.resize(1);

  OrigAlignments.resize(LastFI);
  OrigSizes.resize(LastFI);
  AllColors[0].resize(LastFI);
  UsedColors[0].resize(LastFI);
  Assignments.resize(LastFI);

  using Pair = std::iterator_traits<LiveStacks::iterator>::value_type;

  SmallVector<Pair *, 16> Intervals;

  Intervals.reserve(LS->getNumIntervals());
  for (auto &I : *LS)
    Intervals.push_back(&I);
  llvm::sort(Intervals,
             [](Pair *LHS, Pair *RHS) { return LHS->first < RHS->first; });

  // Gather all spill slots into a list.
  LLVM_DEBUG(dbgs() << "Spill slot intervals:\n");
  for (auto *I : Intervals) {
    LiveInterval &li = I->second;
    LLVM_DEBUG(li.dump());
    int FI = li.reg().stackSlotIndex();
    if (MFI->isDeadObjectIndex(FI))
      continue;

    SSIntervals.push_back(&li);
    OrigAlignments[FI] = MFI->getObjectAlign(FI);
    OrigSizes[FI]      = MFI->getObjectSize(FI);

    auto StackID = MFI->getStackID(FI);
    if (StackID != 0) {
      if (StackID >= AllColors.size()) {
        AllColors.resize(StackID + 1);
        UsedColors.resize(StackID + 1);
      }
      AllColors[StackID].resize(LastFI);
      UsedColors[StackID].resize(LastFI);
    }

    AllColors[StackID].set(FI);
  }
  LLVM_DEBUG(dbgs() << '\n');

  // Sort them by weight.
  llvm::stable_sort(SSIntervals, IntervalSorter());

  NextColors.resize(AllColors.size());

  // Get first "color".
  for (unsigned I = 0, E = AllColors.size(); I != E; ++I)
    NextColors[I] = AllColors[I].find_first();
}

/// ColorSlot - Assign a "color" (stack slot) to the specified stack slot.
int StackSlotColoring::ColorSlot(LiveInterval *li) {
  int Color = -1;
  bool Share = false;
  int FI = li->reg().stackSlotIndex();
  uint8_t StackID = MFI->getStackID(FI);

  if (!DisableSharing) {
    // c2go (#330): never merge a pointer-tagged spill slot with an untagged
    // one. The per-PC Go locals pointer map can only describe ONE typing per
    // physical slot — merging a "ptr" slot with a plain i64 spill leaves
    // copystack rewriting an integer at the PC where the untagged value is
    // live (observed crash in SQLite -O2 `deep`/`all`: `runtime: bad pointer
    // in frame sqlitepkg.resolveExprStep ... 0x3 / 0x5 / 0x80`). Two "ptr"
    // tags can still share — each PC's content is some live pointer, which
    // copystack handles uniformly. Gated to c2go-mode (any tag present
    // implies c2go).
    // #375 slice 3: spill-slot tags went to AArch64FunctionInfo behind a TII
    // virtual; default is empty StringRef for non-c2go targets.
    // #426: virtual renamed to generic getStackSlotTypeTag.
    // c2go (#492): the current FI's tag for the share decision must be its STABLE
    // original (C2GoOrigTags), NOT the mutable physical map — an earlier interval
    // (processed by weight, not FI order) could have fresh-picked THIS FI's index
    // as its color and written "ptr" into the physical map, contaminating the read
    // and misclassifying a scalar as a pointer.
    StringRef LiTag = StringRef(C2GoOrigTags[FI]);
    auto TagsCompatible = [&](int OtherFI) -> bool {
      // c2go (#492): compare the color's LOGICAL tag (C2GoColorTags = the value
      // class actually assigned to it so far), NOT the physical-slot tag which
      // may be a STALE "ptr" left by a ptr interval that was recolored elsewhere.
      // Trusting the stale physical tag here would let a scalar FI and a ptr FI
      // share one color (over-marking the scalar). Non-c2go: C2GoColorTags is
      // all-empty, so this is identical to the old physical-tag comparison.
      return StringRef(C2GoColorTags[OtherFI]) == LiTag;
    };

    // Check if it's possible to reuse any of the used colors.
    Color = UsedColors[StackID].find_first();
    while (Color != -1) {
      if (!Assignments[Color].overlaps(li) && TagsCompatible(Color)) {
        Share = true;
        ++NumEliminated;
        break;
      }
      Color = UsedColors[StackID].find_next(Color);
    }
  }

  if (Color != -1 && MFI->getStackID(Color) != MFI->getStackID(FI)) {
    LLVM_DEBUG(dbgs() << "cannot share FIs with different stack IDs\n");
    Share = false;
  }

  // Assign it to the first available color (assumed to be the best) if it's
  // not possible to share a used color with other objects.
  if (!Share) {
    assert(NextColors[StackID] != -1 && "No more spill slots?");
    Color = NextColors[StackID];
    UsedColors[StackID].set(Color);
    NextColors[StackID] = AllColors[StackID].find_next(NextColors[StackID]);

    // c2go (#492): record this FI's STABLE original tag (C2GoOrigTags) as the
    // color's LOGICAL tag, overwriting any stale tag inherited from a ptr
    // interval that was recolored away — so a scalar taking such a color resets
    // it to "" and a later ptr FI's TagsCompatible won't wrongly share it.
    //
    // We deliberately do NOT write the physical tag map here. The previous
    // `setStackSlotTypeTag(Color, LiTag)` (#330 finding 1: propagate the M2 tag
    // onto the fresh color so M5's per-PC liveness sees it) is REMOVED — the
    // end-of-coloring recompute from the final SlotMapping (in ColorSlots) is now
    // the authoritative physical write, and a mid-loop physical write would
    // contaminate the original tag a not-yet-processed FI later reads as its own
    // LiTag (round-2 finding). #375 slice 3 / #426: tag channel via TII.
    C2GoColorTags[Color] = C2GoOrigTags[FI];
  }

  assert(MFI->getStackID(Color) == MFI->getStackID(FI));

  // Record the assignment.
  Assignments[Color].add(li, LIUAlloc);
  LLVM_DEBUG(dbgs() << "Assigning fi#" << FI << " to fi#" << Color << "\n");

  // Change size and alignment of the allocated slot. If there are multiple
  // objects sharing the same slot, then make sure the size and alignment
  // are large enough for all.
  Align Alignment = OrigAlignments[FI];
  if (!Share || Alignment > MFI->getObjectAlign(Color))
    MFI->setObjectAlignment(Color, Alignment);
  int64_t Size = OrigSizes[FI];
  if (!Share || Size > MFI->getObjectSize(Color))
    MFI->setObjectSize(Color, Size);
  return Color;
}

/// Colorslots - Color all spill stack slots and rewrite all frameindex machine
/// operands in the function.
bool StackSlotColoring::ColorSlots(MachineFunction &MF) {
  unsigned NumObjs = MFI->getObjectIndexEnd();
  SmallVector<int, 16> SlotMapping(NumObjs, -1);
  SmallVector<float, 16> SlotWeights(NumObjs, 0.0);
  SmallVector<SmallVector<int, 4>, 16> RevMap(NumObjs);
  BitVector UsedColors(NumObjs);

  // c2go (#492): snapshot the ORIGINAL per-FI c2go type tags (set by M2 /
  // AArch64InstrInfo::storeRegToStackSlot before this pass) BEFORE ColorSlot
  // mutates the tag map via fresh-color propagation. We re-derive each
  // physical color's tag from the final SlotMapping below so that a slot's
  // tag matches the value class that actually ends up living in it — neither
  // over- nor under-marked. Empty for non-c2go targets (the TII tag hooks are
  // no-ops there), so the recompute loop is a no-op and codegen is identical.
  C2GoOrigTags.assign(NumObjs, std::string());
  for (unsigned FI = 0; FI < NumObjs; ++FI)
    C2GoOrigTags[FI] = std::string(TII->getStackSlotTypeTag(MF, FI));
  // c2go (#492): seed the per-color logical-tag map from the true physical tags
  // so ColorSlot's TagsCompatible starts correct; ColorSlot overwrites each
  // entry on fresh-color assignment as intervals are colored below.
  C2GoColorTags = C2GoOrigTags;

  LLVM_DEBUG(dbgs() << "Color spill slot intervals:\n");
  bool Changed = false;
  for (LiveInterval *li : SSIntervals) {
    int SS = li->reg().stackSlotIndex();
    int NewSS = ColorSlot(li);
    assert(NewSS >= 0 && "Stack coloring failed?");
    SlotMapping[SS] = NewSS;
    RevMap[NewSS].push_back(SS);
    SlotWeights[NewSS] += li->weight();
    UsedColors.set(NewSS);
    Changed |= (SS != NewSS);
  }

  // c2go (#492): recompute every physical color's c2go type tag from the
  // FINAL SlotMapping. A spill FI's tag must describe the value class that
  // actually lives in its physical slot post-coloring. The fresh-color path
  // (ColorSlot) could leave a STALE "ptr" tag on a color number whose
  // original ptr interval was relocated elsewhere while a scalar spill now
  // occupies that physical slot — M5/LowerSTATEPOINT would then mark the
  // scalar as a pointer in the Go locals map and copystack would relocate it
  // ("runtime: bad pointer in frame ..." — SQLite -O2 selectExpander offset
  // 96 held the Expr constant 0x18=24 at a safepoint). A naive unconditional
  // clear, conversely, would DROP a still-live ptr slot's tag (under-mark).
  //
  // The exact rule: a physical color's tag is the union (OR) of the ORIGINAL
  // tags of every FI that maps onto it. FIs sharing a color went through the
  // TagsCompatible-gated REUSE path, so their original tags already agree;
  // the OR is the safe, order-independent way to combine them. We first clear
  // each touched color, then OR in each mapped FI's snapshot tag, so a color
  // whose original ptr interval moved away (no ptr FI maps back) ends up
  // untagged, while a color a ptr value still lands on stays "ptr".
  //
  // Gated to c2go: C2GoOrigTags is all-empty for non-c2go targets (the TII tag
  // hooks are no-ops there), so every clear/set is empty-over-empty — codegen
  // is byte-identical.
  //
  // Scope: this canonicalizes the tag of every color a spill FI MAPS ONTO — the
  // live colors M5 / the AsmPrinter locals map actually consult. A ptr-tagged FI
  // that maps away and is not itself a final color may retain a stale physical
  // tag, but such FIs are dead / allocation-filtered by those consumers and
  // never reach the emitted locals pointer map.
  for (unsigned FI = 0; FI < NumObjs; ++FI)
    if (SlotMapping[FI] >= 0)
      TII->setStackSlotTypeTag(MF, SlotMapping[FI], "");
  for (unsigned FI = 0; FI < NumObjs; ++FI) {
    int NewFI = SlotMapping[FI];
    if (NewFI >= 0 && !C2GoOrigTags[FI].empty())
      TII->setStackSlotTypeTag(MF, NewFI, C2GoOrigTags[FI]);
  }

  LLVM_DEBUG(dbgs() << "\nSpill slots after coloring:\n");
  for (LiveInterval *li : SSIntervals) {
    int SS = li->reg().stackSlotIndex();
    li->setWeight(SlotWeights[SS]);
  }
  // Sort them by new weight.
  llvm::stable_sort(SSIntervals, IntervalSorter());

#ifndef NDEBUG
  for (LiveInterval *li : SSIntervals)
    LLVM_DEBUG(li->dump());
  LLVM_DEBUG(dbgs() << '\n');
#endif

  if (!Changed)
    return false;

  // Rewrite all MachineMemOperands.
  for (unsigned SS = 0, SE = SSRefs.size(); SS != SE; ++SS) {
    int NewFI = SlotMapping[SS];
    if (NewFI == -1 || (NewFI == (int)SS))
      continue;

    const PseudoSourceValue *NewSV = MF.getPSVManager().getFixedStack(NewFI);
    SmallVectorImpl<MachineMemOperand *> &RefMMOs = SSRefs[SS];
    for (MachineMemOperand *MMO : RefMMOs)
      MMO->setValue(NewSV);
  }

  // Rewrite all MO_FrameIndex operands.  Look for dead stores.
  for (MachineBasicBlock &MBB : MF) {
    for (MachineInstr &MI : MBB)
      RewriteInstruction(MI, SlotMapping, MF);
    RemoveDeadStores(&MBB);
  }

  // Delete unused stack slots.
  for (int StackID = 0, E = AllColors.size(); StackID != E; ++StackID) {
    int NextColor = NextColors[StackID];
    while (NextColor != -1) {
      LLVM_DEBUG(dbgs() << "Removing unused stack object fi#" << NextColor << "\n");
      MFI->RemoveStackObject(NextColor);
      NextColor = AllColors[StackID].find_next(NextColor);
    }
  }

  return true;
}

/// RewriteInstruction - Rewrite specified instruction by replacing references
/// to old frame index with new one.
void StackSlotColoring::RewriteInstruction(MachineInstr &MI,
                                           SmallVectorImpl<int> &SlotMapping,
                                           MachineFunction &MF) {
  // Update the operands.
  for (MachineOperand &MO : MI.operands()) {
    if (!MO.isFI())
      continue;
    int OldFI = MO.getIndex();
    if (OldFI < 0)
      continue;
    int NewFI = SlotMapping[OldFI];
    if (NewFI == -1 || NewFI == OldFI)
      continue;

    assert(MFI->getStackID(OldFI) == MFI->getStackID(NewFI));
    MO.setIndex(NewFI);
  }

  // The MachineMemOperands have already been updated.
}

/// RemoveDeadStores - Scan through a basic block and look for loads followed
/// by stores.  If they're both using the same stack slot, then the store is
/// definitely dead.  This could obviously be much more aggressive (consider
/// pairs with instructions between them), but such extensions might have a
/// considerable compile time impact.
bool StackSlotColoring::RemoveDeadStores(MachineBasicBlock* MBB) {
  // FIXME: This could be much more aggressive, but we need to investigate
  // the compile time impact of doing so.
  bool changed = false;

  SmallVector<MachineInstr*, 4> toErase;

  for (MachineBasicBlock::iterator I = MBB->begin(), E = MBB->end();
       I != E; ++I) {
    if (DCELimit != -1 && (int)NumDead >= DCELimit)
      break;
    int FirstSS, SecondSS;
    if (TII->isStackSlotCopy(*I, FirstSS, SecondSS) && FirstSS == SecondSS &&
        FirstSS != -1) {
      ++NumDead;
      changed = true;
      toErase.push_back(&*I);
      continue;
    }

    MachineBasicBlock::iterator NextMI = std::next(I);
    MachineBasicBlock::iterator ProbableLoadMI = I;

    Register LoadReg;
    Register StoreReg;
    TypeSize LoadSize = TypeSize::getZero();
    TypeSize StoreSize = TypeSize::getZero();
    if (!(LoadReg = TII->isLoadFromStackSlot(*I, FirstSS, LoadSize)))
      continue;
    // Skip the ...pseudo debugging... instructions between a load and store.
    while ((NextMI != E) && NextMI->isDebugInstr()) {
      ++NextMI;
      ++I;
    }
    if (NextMI == E) continue;
    if (!(StoreReg = TII->isStoreToStackSlot(*NextMI, SecondSS, StoreSize)))
      continue;
    if (FirstSS != SecondSS || LoadReg != StoreReg || FirstSS == -1 ||
        LoadSize != StoreSize || !MFI->isSpillSlotObjectIndex(FirstSS))
      continue;

    ++NumDead;
    changed = true;

    if (NextMI->findRegisterUseOperandIdx(LoadReg, /*TRI=*/nullptr, true) !=
        -1) {
      ++NumDead;
      toErase.push_back(&*ProbableLoadMI);
    }

    toErase.push_back(&*NextMI);
    ++I;
  }

  for (MachineInstr *MI : toErase) {
    if (Indexes)
      Indexes->removeMachineInstrFromMaps(*MI);
    MI->eraseFromParent();
  }

  return changed;
}

bool StackSlotColoring::run(MachineFunction &MF) {
  LLVM_DEBUG({
    dbgs() << "********** Stack Slot Coloring **********\n"
           << "********** Function: " << MF.getName() << '\n';
  });

  bool Changed = false;

  unsigned NumSlots = LS->getNumIntervals();
  if (NumSlots == 0)
    // Nothing to do!
    return false;

  // If there are calls to setjmp or sigsetjmp, don't perform stack slot
  // coloring. The stack could be modified before the longjmp is executed,
  // resulting in the wrong value being used afterwards.
  if (MF.exposesReturnsTwice())
    return false;

  // Gather spill slot references
  ScanForSpillSlotRefs(MF);
  InitializeSlots();
  Changed = ColorSlots(MF);

  for (int &Next : NextColors)
    Next = -1;

  SSIntervals.clear();
  for (auto &RefMMOs : SSRefs)
    RefMMOs.clear();
  SSRefs.clear();
  OrigAlignments.clear();
  OrigSizes.clear();
  AllColors.clear();
  UsedColors.clear();
  Assignments.clear();

  return Changed;
}

bool StackSlotColoringLegacy::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;

  LiveStacks *LS = &getAnalysis<LiveStacksWrapperLegacy>().getLS();
  MachineBlockFrequencyInfo *MBFI =
      &getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
  SlotIndexes *Indexes = &getAnalysis<SlotIndexesWrapperPass>().getSI();
  StackSlotColoring Impl(MF, LS, MBFI, Indexes);
  return Impl.run(MF);
}

PreservedAnalyses
StackSlotColoringPass::run(MachineFunction &MF,
                           MachineFunctionAnalysisManager &MFAM) {
  LiveStacks *LS = &MFAM.getResult<LiveStacksAnalysis>(MF);
  MachineBlockFrequencyInfo *MBFI =
      &MFAM.getResult<MachineBlockFrequencyAnalysis>(MF);
  SlotIndexes *Indexes = &MFAM.getResult<SlotIndexesAnalysis>(MF);
  StackSlotColoring Impl(MF, LS, MBFI, Indexes);
  bool Changed = Impl.run(MF);
  if (!Changed)
    return PreservedAnalyses::all();

  auto PA = getMachineFunctionPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  PA.preserve<SlotIndexesAnalysis>();
  PA.preserve<MachineBlockFrequencyAnalysis>();
  PA.preserve<MachineDominatorTreeAnalysis>();
  PA.preserve<LiveIntervalsAnalysis>();
  PA.preserve<LiveDebugVariablesAnalysis>();
  return PA;
}
