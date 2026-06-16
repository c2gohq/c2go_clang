//=== AArch64CallingConvention.cpp - AArch64 CC impl ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the table-generated and custom routines for the AArch64
// Calling Convention.
//
//===----------------------------------------------------------------------===//

#include "AArch64CallingConvention.h"
#include "AArch64.h"
#include "AArch64InstrInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
using namespace llvm;

static const MCPhysReg XRegList[] = {AArch64::X0, AArch64::X1, AArch64::X2,
                                     AArch64::X3, AArch64::X4, AArch64::X5,
                                     AArch64::X6, AArch64::X7};
static const MCPhysReg HRegList[] = {AArch64::H0, AArch64::H1, AArch64::H2,
                                     AArch64::H3, AArch64::H4, AArch64::H5,
                                     AArch64::H6, AArch64::H7};
static const MCPhysReg SRegList[] = {AArch64::S0, AArch64::S1, AArch64::S2,
                                     AArch64::S3, AArch64::S4, AArch64::S5,
                                     AArch64::S6, AArch64::S7};
static const MCPhysReg DRegList[] = {AArch64::D0, AArch64::D1, AArch64::D2,
                                     AArch64::D3, AArch64::D4, AArch64::D5,
                                     AArch64::D6, AArch64::D7};
static const MCPhysReg QRegList[] = {AArch64::Q0, AArch64::Q1, AArch64::Q2,
                                     AArch64::Q3, AArch64::Q4, AArch64::Q5,
                                     AArch64::Q6, AArch64::Q7};
static const MCPhysReg ZRegList[] = {AArch64::Z0, AArch64::Z1, AArch64::Z2,
                                     AArch64::Z3, AArch64::Z4, AArch64::Z5,
                                     AArch64::Z6, AArch64::Z7};
static const MCPhysReg PRegList[] = {AArch64::P0, AArch64::P1, AArch64::P2,
                                     AArch64::P3};

static bool finishStackBlock(SmallVectorImpl<CCValAssign> &PendingMembers,
                             MVT LocVT, ISD::ArgFlagsTy &ArgFlags,
                             CCState &State, Align SlotAlign) {
  if (LocVT.isScalableVector()) {
    const AArch64Subtarget &Subtarget = static_cast<const AArch64Subtarget &>(
        State.getMachineFunction().getSubtarget());
    const AArch64TargetLowering *TLI = Subtarget.getTargetLowering();

    // We are about to reinvoke the CCAssignFn auto-generated handler. If we
    // don't unset these flags we will get stuck in an infinite loop forever
    // invoking the custom handler.
    ArgFlags.setInConsecutiveRegs(false);
    ArgFlags.setInConsecutiveRegsLast(false);

    // The calling convention for passing SVE tuples states that in the event
    // we cannot allocate enough registers for the tuple we should still leave
    // any remaining registers unallocated. However, when we call the
    // CCAssignFn again we want it to behave as if all remaining registers are
    // allocated. This will force the code to pass the tuple indirectly in
    // accordance with the PCS.
    bool ZRegsAllocated[8];
    for (int I = 0; I < 8; I++) {
      ZRegsAllocated[I] = State.isAllocated(ZRegList[I]);
      State.AllocateReg(ZRegList[I]);
    }
    // The same applies to P registers.
    bool PRegsAllocated[4];
    for (int I = 0; I < 4; I++) {
      PRegsAllocated[I] = State.isAllocated(PRegList[I]);
      State.AllocateReg(PRegList[I]);
    }

    auto &It = PendingMembers[0];
    CCAssignFn *AssignFn =
        TLI->CCAssignFnForCall(State.getCallingConv(), /*IsVarArg=*/false);
    // FIXME: Get the correct original type.
    Type *OrigTy = EVT(It.getValVT()).getTypeForEVT(State.getContext());
    if (AssignFn(It.getValNo(), It.getValVT(), It.getValVT(), CCValAssign::Full,
                 ArgFlags, OrigTy, State))
      llvm_unreachable("Call operand has unhandled type");

    // Return the flags to how they were before.
    ArgFlags.setInConsecutiveRegs(true);
    ArgFlags.setInConsecutiveRegsLast(true);

    // Return the register state back to how it was before, leaving any
    // unallocated registers available for other smaller types.
    for (int I = 0; I < 8; I++)
      if (!ZRegsAllocated[I])
        State.DeallocateReg(ZRegList[I]);
    for (int I = 0; I < 4; I++)
      if (!PRegsAllocated[I])
        State.DeallocateReg(PRegList[I]);

    // All pending members have now been allocated
    PendingMembers.clear();
    return true;
  }

  unsigned Size = LocVT.getSizeInBits() / 8;
  for (auto &It : PendingMembers) {
    It.convertToMem(State.AllocateStack(Size, SlotAlign));
    State.addLoc(It);
    SlotAlign = Align(1);
  }

  // All pending members have now been allocated
  PendingMembers.clear();
  return true;
}

/// The Darwin variadic PCS places anonymous arguments in 8-byte stack slots. An
/// [N x Ty] type must still be contiguous in memory though.
static bool CC_AArch64_Custom_Stack_Block(
      unsigned &ValNo, MVT &ValVT, MVT &LocVT, CCValAssign::LocInfo &LocInfo,
      ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  SmallVectorImpl<CCValAssign> &PendingMembers = State.getPendingLocs();

  // Add the argument to the list to be allocated once we know the size of the
  // block.
  PendingMembers.push_back(
      CCValAssign::getPending(ValNo, ValVT, LocVT, LocInfo));

  if (!ArgFlags.isInConsecutiveRegsLast())
    return true;

  return finishStackBlock(PendingMembers, LocVT, ArgFlags, State, Align(8));
}

/// Given an [N x Ty] block, it should be passed in a consecutive sequence of
/// registers. If no such sequence is available, mark the rest of the registers
/// of that type as used and place the argument on the stack.
static bool CC_AArch64_Custom_Block(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                                    CCValAssign::LocInfo &LocInfo,
                                    ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  const AArch64Subtarget &Subtarget = static_cast<const AArch64Subtarget &>(
      State.getMachineFunction().getSubtarget());
  bool IsDarwinILP32 = Subtarget.isTargetILP32() && Subtarget.isTargetMachO();

  // Try to allocate a contiguous block of registers, each of the correct
  // size to hold one member.
  ArrayRef<MCPhysReg> RegList;
  if (LocVT.SimpleTy == MVT::i64 || (IsDarwinILP32 && LocVT.SimpleTy == MVT::i32))
    RegList = XRegList;
  else if (LocVT.SimpleTy == MVT::f16 || LocVT.SimpleTy == MVT::bf16)
    RegList = HRegList;
  else if (LocVT.SimpleTy == MVT::f32 || LocVT.is32BitVector())
    RegList = SRegList;
  else if (LocVT.SimpleTy == MVT::f64 || LocVT.is64BitVector())
    RegList = DRegList;
  else if (LocVT.SimpleTy == MVT::f128 || LocVT.is128BitVector())
    RegList = QRegList;
  else if (LocVT.isScalableVector()) {
    // Scalable masks should be pass by Predicate registers.
    if (LocVT == MVT::nxv1i1 || LocVT == MVT::nxv2i1 || LocVT == MVT::nxv4i1 ||
        LocVT == MVT::nxv8i1 || LocVT == MVT::nxv16i1 ||
        LocVT == MVT::aarch64svcount)
      RegList = PRegList;
    else
      RegList = ZRegList;
  } else {
    // Not an array we want to split up after all.
    return false;
  }

  SmallVectorImpl<CCValAssign> &PendingMembers = State.getPendingLocs();

  // Add the argument to the list to be allocated once we know the size of the
  // block.
  PendingMembers.push_back(
      CCValAssign::getPending(ValNo, ValVT, LocVT, LocInfo));

  if (!ArgFlags.isInConsecutiveRegsLast())
    return true;

  // [N x i32] arguments get packed into x-registers on Darwin's arm64_32
  // because that's how the armv7k Clang front-end emits small structs.
  unsigned EltsPerReg = (IsDarwinILP32 && LocVT.SimpleTy == MVT::i32) ? 2 : 1;
  ArrayRef<MCPhysReg> RegResult = State.AllocateRegBlock(
      RegList, alignTo(PendingMembers.size(), EltsPerReg) / EltsPerReg);
  if (!RegResult.empty() && EltsPerReg == 1) {
    for (const auto &[It, Reg] : zip(PendingMembers, RegResult)) {
      It.convertToReg(Reg);
      State.addLoc(It);
    }
    PendingMembers.clear();
    return true;
  } else if (!RegResult.empty()) {
    assert(EltsPerReg == 2 && "unexpected ABI");
    bool UseHigh = false;
    CCValAssign::LocInfo Info;
    unsigned RegIdx = 0;
    for (auto &It : PendingMembers) {
      Info = UseHigh ? CCValAssign::AExtUpper : CCValAssign::ZExt;
      State.addLoc(CCValAssign::getReg(It.getValNo(), MVT::i32,
                                       RegResult[RegIdx], MVT::i64, Info));
      UseHigh = !UseHigh;
      if (!UseHigh)
        ++RegIdx;
    }
    PendingMembers.clear();
    return true;
  }

  if (!LocVT.isScalableVector()) {
    // Mark all regs in the class as unavailable
    for (auto Reg : RegList)
      State.AllocateReg(Reg);
  }

  const MaybeAlign StackAlign =
      State.getMachineFunction().getDataLayout().getStackAlignment();
  assert(StackAlign && "data layout string is missing stack alignment");
  const Align MemAlign = ArgFlags.getNonZeroMemAlign();
  Align SlotAlign = std::min(MemAlign, *StackAlign);
  if (!Subtarget.isTargetDarwin())
    SlotAlign = std::max(SlotAlign, Align(8));

  return finishStackBlock(PendingMembers, LocVT, ArgFlags, State, SlotAlign);
}

//===----------------------------------------------------------------------===//
// c2go-ABIInternal: private register-passing convention (optimization layer)
//===----------------------------------------------------------------------===//
//
// Implements Go's ABIInternal register-assignment for arm64 as a hand-written
// CCAssignFn. Per abi-internal.md (§"arm64 architecture"):
//   - integer / pointer args+results : R0 .. R15  (16 integer registers)
//   - floating-point args+results    : F0 .. F15  (16 FP registers)
//   - integer and FP register indices advance independently.
//
// This convention is ONLY applied to functions that c2go has proven
// NOSPLIT-eligible (leaf / TU-internal near-leaf, static, non-c2go_extern,
// downstream subtree <= NOSPLIT budget). NOSPLIT => no entry morestack =>
// register-passed arguments are never clobbered by a stack copy. The symbol
// is still emitted as ABI0 to Go; this register convention is invisible to
// the Go ABI (verified by abitest T2/T7). See clang/docs/c2go_design.md
// §2.0.1 / §4.2.
//
// Spill space: abi-internal.md requires the CALLER to reserve (uninitialized)
// spill space for each register-assigned argument. For c2go that reservation
// is part of the caller's fixed frame and is materialized by the c2go
// FrameLowering path (owned by the foundation agent); the CC assignment here
// only places values into registers. Because eligible callees are NOSPLIT,
// the spill space is never actually populated at runtime (no morestack), so
// it is a frame-size reservation only.
//
// Implementation note on aggregates: by the time a CCAssignFn runs, LLVM has
// already legalized IR aggregates into a sequence of scalar value pieces
// (one ValNo per piece), and composites flagged InConsecutiveRegs arrive as a
// pending block. We honor Go's "an argument containing a non-trivial array,
// or that does not fit entirely in the remaining registers, is passed on the
// stack" rule for the consecutive-reg block case via finishStackBlock; plain
// scalar pieces register-assign individually with stack overflow. v0 keeps
// SIMD/vectors and oversized composites simple (block-or-stack), matching the
// design's "simplest correct subset" scope.

// 16 integer argument/result registers: R0-R15 (X = 64-bit view).
static const MCPhysReg C2GoXRegList[] = {
    AArch64::X0,  AArch64::X1,  AArch64::X2,  AArch64::X3,
    AArch64::X4,  AArch64::X5,  AArch64::X6,  AArch64::X7,
    AArch64::X8,  AArch64::X9,  AArch64::X10, AArch64::X11,
    AArch64::X12, AArch64::X13, AArch64::X14, AArch64::X15};
// 32-bit (W) view of the same 16 integer registers, by index.
static const MCPhysReg C2GoWRegList[] = {
    AArch64::W0,  AArch64::W1,  AArch64::W2,  AArch64::W3,
    AArch64::W4,  AArch64::W5,  AArch64::W6,  AArch64::W7,
    AArch64::W8,  AArch64::W9,  AArch64::W10, AArch64::W11,
    AArch64::W12, AArch64::W13, AArch64::W14, AArch64::W15};
// 16 floating-point registers, sized views.
static const MCPhysReg C2GoHRegList[] = {
    AArch64::H0,  AArch64::H1,  AArch64::H2,  AArch64::H3,
    AArch64::H4,  AArch64::H5,  AArch64::H6,  AArch64::H7,
    AArch64::H8,  AArch64::H9,  AArch64::H10, AArch64::H11,
    AArch64::H12, AArch64::H13, AArch64::H14, AArch64::H15};
static const MCPhysReg C2GoSRegList[] = {
    AArch64::S0,  AArch64::S1,  AArch64::S2,  AArch64::S3,
    AArch64::S4,  AArch64::S5,  AArch64::S6,  AArch64::S7,
    AArch64::S8,  AArch64::S9,  AArch64::S10, AArch64::S11,
    AArch64::S12, AArch64::S13, AArch64::S14, AArch64::S15};
static const MCPhysReg C2GoDRegList[] = {
    AArch64::D0,  AArch64::D1,  AArch64::D2,  AArch64::D3,
    AArch64::D4,  AArch64::D5,  AArch64::D6,  AArch64::D7,
    AArch64::D8,  AArch64::D9,  AArch64::D10, AArch64::D11,
    AArch64::D12, AArch64::D13, AArch64::D14, AArch64::D15};
static const MCPhysReg C2GoQRegList[] = {
    AArch64::Q0,  AArch64::Q1,  AArch64::Q2,  AArch64::Q3,
    AArch64::Q4,  AArch64::Q5,  AArch64::Q6,  AArch64::Q7,
    AArch64::Q8,  AArch64::Q9,  AArch64::Q10, AArch64::Q11,
    AArch64::Q12, AArch64::Q13, AArch64::Q14, AArch64::Q15};

// Core scalar register-assignment shared by the call and return paths.
// Returns true if it fully handled the value (assigned to reg or stack).
static bool CC_AArch64_C2GoABIInternal_Common(unsigned &ValNo, MVT &ValVT,
                                              MVT &LocVT,
                                              CCValAssign::LocInfo &LocInfo,
                                              ISD::ArgFlagsTy &ArgFlags,
                                              CCState &State) {
  // Composite / vector pieces flagged for consecutive registers: gather them
  // into a pending block and place all-in-registers or all-on-stack, mirroring
  // CC_AArch64_Custom_Block. This preserves Go's "fits entirely or goes to the
  // stack" rule for a single top-level aggregate argument.
  if (ArgFlags.isInConsecutiveRegs() || !State.getPendingLocs().empty()) {
    ArrayRef<MCPhysReg> RegList;
    if (LocVT.SimpleTy == MVT::i64)
      RegList = C2GoXRegList;
    else if (LocVT.SimpleTy == MVT::i32)
      // i32 aggregate fields must take the block path with the W-register list.
      // Without this branch RegList stays empty, the function falls through to
      // the plain scalar path at 344, and an aggregate that mixes i64 + i32
      // fields can end up half-in-regs / half-on-stack — violating Go's
      // "consecutive-regs: all-in-regs or all-on-stack" invariant.
      RegList = C2GoWRegList;
    else if (LocVT.SimpleTy == MVT::f16 || LocVT.SimpleTy == MVT::bf16)
      RegList = C2GoHRegList;
    else if (LocVT.SimpleTy == MVT::f32 || LocVT.is32BitVector())
      RegList = C2GoSRegList;
    else if (LocVT.SimpleTy == MVT::f64 || LocVT.is64BitVector())
      RegList = C2GoDRegList;
    else if (LocVT.SimpleTy == MVT::f128 || LocVT.is128BitVector())
      RegList = C2GoQRegList;
    else
      return false;

    SmallVectorImpl<CCValAssign> &PendingMembers = State.getPendingLocs();
    PendingMembers.push_back(
        CCValAssign::getPending(ValNo, ValVT, LocVT, LocInfo));
    if (!ArgFlags.isInConsecutiveRegsLast())
      return true;

    ArrayRef<MCPhysReg> RegResult =
        State.AllocateRegBlock(RegList, PendingMembers.size());
    if (!RegResult.empty()) {
      for (const auto &[It, Reg] : zip(PendingMembers, RegResult)) {
        It.convertToReg(Reg);
        State.addLoc(It);
      }
      PendingMembers.clear();
      return true;
    }
    // Did not fit: burn the whole register file for this class (Go does not
    // back-fill once an aggregate spills) and place the block on the stack.
    for (auto Reg : RegList)
      State.AllocateReg(Reg);
    return finishStackBlock(PendingMembers, LocVT, ArgFlags, State, Align(8));
  }

  // Plain integer / pointer scalar.
  if (LocVT == MVT::i1 || LocVT == MVT::i8 || LocVT == MVT::i16 ||
      LocVT == MVT::i32) {
    // Promote sub-word integers to a 32-bit register slot (W view).
    if (unsigned Reg = State.AllocateReg(C2GoWRegList)) {
      State.addLoc(
          CCValAssign::getReg(ValNo, ValVT, Reg, MVT::i32, CCValAssign::ZExt));
      return true;
    }
    State.addLoc(
        CCValAssign::getMem(ValNo, ValVT, State.AllocateStack(4, Align(4)),
                            LocVT, LocInfo));
    return true;
  }
  if (LocVT == MVT::i64) {
    if (unsigned Reg = State.AllocateReg(C2GoXRegList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(8, Align(8)), LocVT, LocInfo));
    return true;
  }

  // Floating-point scalar.
  if (LocVT == MVT::f16 || LocVT == MVT::bf16) {
    if (unsigned Reg = State.AllocateReg(C2GoHRegList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(2, Align(2)), LocVT, LocInfo));
    return true;
  }
  if (LocVT == MVT::f32) {
    if (unsigned Reg = State.AllocateReg(C2GoSRegList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(4, Align(4)), LocVT, LocInfo));
    return true;
  }
  if (LocVT == MVT::f64) {
    if (unsigned Reg = State.AllocateReg(C2GoDRegList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(8, Align(8)), LocVT, LocInfo));
    return true;
  }

  // 128-bit SIMD / vector scalar (SIMD args go in V registers — §2.0.1).
  if (LocVT == MVT::f128 || LocVT.is128BitVector()) {
    if (unsigned Reg = State.AllocateReg(C2GoQRegList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(16, Align(16)), LocVT, LocInfo));
    return true;
  }

  // Anything else (scalable vectors, exotic types) is out of v0 scope.
  return false;
}

bool llvm::CC_AArch64_C2GoABIInternal(unsigned ValNo, MVT ValVT, MVT LocVT,
                                      CCValAssign::LocInfo LocInfo,
                                      ISD::ArgFlagsTy ArgFlags, Type *OrigTy,
                                      CCState &State) {
  return !CC_AArch64_C2GoABIInternal_Common(ValNo, ValVT, LocVT, LocInfo,
                                            ArgFlags, State);
}

bool llvm::RetCC_AArch64_C2GoABIInternal(unsigned ValNo, MVT ValVT, MVT LocVT,
                                         CCValAssign::LocInfo LocInfo,
                                         ISD::ArgFlagsTy ArgFlags, Type *OrigTy,
                                         CCState &State) {
  // Per abi-internal.md, results use the SAME register sequences as arguments
  // (integer index and FP index both reset to 0 before results are assigned).
  // A fresh CCState is used for the return path, so the register counters
  // start at 0 here naturally.
  return !CC_AArch64_C2GoABIInternal_Common(ValNo, ValVT, LocVT, LocInfo,
                                            ArgFlags, State);
}

// TableGen provides definitions of the calling convention analysis entry
// points.
#include "AArch64GenCallingConv.inc"
