//===- C2GoFrameAddrRemat.cpp - rematerialize frame-address vregs --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go: a virtual register holding a materialized FRAME ADDRESS (e.g. AArch64
// `%v = ADDXri %stack.N, imm` or X86 `%v = LEA64r %stack.N, ...`) must never
// live across a call in a goroutine-stack (goabi) function. Every call may
// reach morestack and relocate the entire stack (copystack); copystack adjusts
// stack-resident POINTER SLOTS it knows about (tracked allocas, aggregate
// pointer fields, tagged GC-pointer spills), but a register-allocator spill of
// a compiler-materialized frame address lands in an ANONYMOUS slot no
// stackmap covers — after the move the reloaded base still points into the
// freed old stack segment and every access through it reads the dead pre-copy
// frame image. FastRA (-O0) spills every cross-call vreg and never
// rematerializes, so plain C like musl's vfprintf (`f` re-read through a
// spilled base after printf_core returns) breaks under stack growth.
//
// The Go compiler never lets a raw frame address survive a safepoint in a
// register or an untracked slot — it recomputes such addresses at each use.
// This pass mirrors that: for every vreg whose unique definition is a
// trivially-rematerializable instruction with a FrameIndex operand, each use
// that is (a) in another basic block, (b) a PHI incoming, or (c) separated
// from the definition by a call, is rewritten to a fresh clone of the
// definition inserted immediately before the use. FrameIndex references are
// resolved by PEI at the clone's location, so the recomputed address is
// correct on whatever stack the function is executing on. Uses adjacent to
// the definition (same block, no intervening call) keep the original vreg, so
// optimized code quality is unaffected where there is no safepoint to cross.
//
// Runs from addPreRegAlloc on both the FastRA and greedy pipelines (the hook
// precedes the regalloc mode split) and self-gates on the `c2go.goabi`
// module flag, matching the other c2go backend passes.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/Pass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

using namespace llvm;

#define DEBUG_TYPE "c2go-frame-addr-remat"

namespace {

class C2GoFrameAddrRemat : public MachineFunctionPass {
public:
  static char ID;

  C2GoFrameAddrRemat() : MachineFunctionPass(ID) {
    initializeC2GoFrameAddrRematPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override {
    return "C2Go Frame Address Rematerialization";
  }

  // Only clones/rewrites instructions inside existing blocks — the CFG (and
  // every CFG-derived analysis) survives untouched. Without this the pass
  // manager re-runs MachineDominatorTree/MachineLoopInfo for downstream
  // consumers on every function.
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  // Lazy per-block instruction numbering + sorted call positions, used to
  // decide "is there a call between Def and Use" without an O(n^2) walk in
  // huge blocks (printf_core-sized functions at -O0).
  DenseMap<const MachineBasicBlock *, DenseMap<const MachineInstr *, unsigned>>
      OrderCache;
  DenseMap<const MachineBasicBlock *, SmallVector<unsigned, 8>> CallsCache;

  void numberBlock(const MachineBasicBlock &MBB);
  bool hasCallBetween(const MachineInstr &Def, const MachineInstr &Use);
};

} // end anonymous namespace

char C2GoFrameAddrRemat::ID = 0;

char &llvm::C2GoFrameAddrRematID = C2GoFrameAddrRemat::ID;

INITIALIZE_PASS(C2GoFrameAddrRemat, DEBUG_TYPE,
                "C2Go Frame Address Rematerialization", false, false)

FunctionPass *llvm::createC2GoFrameAddrRematPass() {
  return new C2GoFrameAddrRemat();
}

void C2GoFrameAddrRemat::numberBlock(const MachineBasicBlock &MBB) {
  auto &Order = OrderCache[&MBB];
  auto &Calls = CallsCache[&MBB];
  unsigned Idx = 0;
  for (const MachineInstr &MI : MBB) {
    Order[&MI] = Idx;
    if (MI.isCall())
      Calls.push_back(Idx);
    ++Idx;
  }
}

bool C2GoFrameAddrRemat::hasCallBetween(const MachineInstr &Def,
                                        const MachineInstr &Use) {
  const MachineBasicBlock *MBB = Def.getParent();
  assert(Use.getParent() == MBB && "same-block query only");
  if (!OrderCache.count(MBB))
    numberBlock(*MBB);
  const auto &Order = OrderCache[MBB];
  const auto &Calls = CallsCache[MBB];
  unsigned DefIdx = Order.lookup(&Def), UseIdx = Order.lookup(&Use);
  // First call index strictly after Def; is it strictly before Use? (A use
  // that itself is a call consumes the register before the callee's prologue
  // can reach morestack, so the interval is exclusive on both ends.)
  auto It = std::upper_bound(Calls.begin(), Calls.end(), DefIdx);
  return It != Calls.end() && *It < UseIdx;
}

// A frame-address materialization: single explicit def of a virtual register,
// at least one FrameIndex operand, and safe to duplicate anywhere
// (isTriviallyReMaterializable). The FI requirement filters this down to
// address computations (constant materializations etc. have no FI operand and
// are no copystack hazard).
static bool isFrameAddrMaterialization(const MachineInstr &MI,
                                       const TargetInstrInfo &TII) {
  if (MI.getDesc().getNumDefs() != 1)
    return false;
  const MachineOperand &DefOp = MI.getOperand(0);
  if (!DefOp.isReg() || !DefOp.getReg().isVirtual() || DefOp.getSubReg())
    return false;
  bool HasFI = false;
  for (const MachineOperand &MO : MI.operands())
    if (MO.isFI())
      HasFI = true;
  if (!HasFI)
    return false;
  return TII.isTriviallyReMaterializable(MI);
}

bool C2GoFrameAddrRemat::runOnMachineFunction(MachineFunction &MF) {
  // Self-gate: only goabi (goroutine-stack) modules have moving stacks.
  if (!MF.getFunction().getParent()->getModuleFlag(
          llvm::c2go::kGoabiModuleFlag))
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  if (!MRI.isSSA())
    return false; // pre-RA only

  const TargetInstrInfo &TII = *MF.getSubtarget().getInstrInfo();
  OrderCache.clear();
  CallsCache.clear();

  bool Changed = false;
  for (unsigned I = 0, E = MRI.getNumVirtRegs(); I != E; ++I) {
    Register Reg = Register::index2VirtReg(I);
    MachineInstr *Def = MRI.getUniqueVRegDef(Reg);
    if (!Def || !isFrameAddrMaterialization(*Def, TII))
      continue;

    // Collect the uses to rewrite. PHI incomings are handled per (reg, pred)
    // operand pair (the clone must sit in the predecessor); ordinary users
    // are handled per instruction (one clone rewrites every operand).
    SmallVector<MachineInstr *, 8> InstrUses;
    SmallVector<std::pair<MachineInstr *, unsigned>, 4> PHIUses;
    for (MachineOperand &MO : MRI.use_nodbg_operands(Reg)) {
      MachineInstr *UseMI = MO.getParent();
      if (UseMI == Def)
        continue;
      if (UseMI->isPHI()) {
        PHIUses.push_back({UseMI, UseMI->getOperandNo(&MO)});
        continue;
      }
      if (UseMI->getParent() != Def->getParent() ||
          hasCallBetween(*Def, *UseMI)) {
        if (InstrUses.empty() || InstrUses.back() != UseMI)
          InstrUses.push_back(UseMI);
      }
    }
    // Dedup instruction users (an MI may use Reg in several operands, and
    // use_nodbg_operands does not group them).
    llvm::sort(InstrUses);
    InstrUses.erase(std::unique(InstrUses.begin(), InstrUses.end()),
                    InstrUses.end());

    if (InstrUses.empty() && PHIUses.empty())
      continue;

    for (MachineInstr *UseMI : InstrUses) {
      Register NewReg = MRI.cloneVirtualRegister(Reg);
      MachineInstr *Clone = MF.CloneMachineInstr(Def);
      Clone->getOperand(0).setReg(NewReg);
      UseMI->getParent()->insert(UseMI->getIterator(), Clone);
      for (MachineOperand &MO : UseMI->operands())
        if (MO.isReg() && MO.isUse() && MO.getReg() == Reg)
          MO.setReg(NewReg);
      Changed = true;
    }
    for (auto [PHI, OpNo] : PHIUses) {
      // PHI operands come in (value, pred-block) pairs; the clone belongs at
      // the end of that predecessor, before its terminators.
      MachineBasicBlock *Pred = PHI->getOperand(OpNo + 1).getMBB();
      Register NewReg = MRI.cloneVirtualRegister(Reg);
      MachineInstr *Clone = MF.CloneMachineInstr(Def);
      Clone->getOperand(0).setReg(NewReg);
      Pred->insert(Pred->getFirstTerminator(), Clone);
      PHI->getOperand(OpNo).setReg(NewReg);
      // The block gained an instruction; drop its stale numbering.
      OrderCache.erase(Pred);
      CallsCache.erase(Pred);
      Changed = true;
    }

    // If nothing non-debug reads the original any more, retire it.
    if (MRI.use_nodbg_empty(Reg)) {
      MRI.markUsesInDebugValueAsUndef(Reg);
      MachineBasicBlock *DefBB = Def->getParent();
      Def->eraseFromParent();
      OrderCache.erase(DefBB);
      CallsCache.erase(DefBB);
    }
  }
  return Changed;
}
