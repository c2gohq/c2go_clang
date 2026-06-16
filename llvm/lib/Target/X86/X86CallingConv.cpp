//=== X86CallingConv.cpp - X86 Custom Calling Convention Impl   -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the implementation of custom routines for the X86
// Calling Convention that aren't done by tablegen.
//
//===----------------------------------------------------------------------===//

#include "X86CallingConv.h"
#include "X86Subtarget.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

/// When regcall calling convention compiled to 32 bit arch, special treatment
/// is required for 64 bit masks.
/// The value should be assigned to two GPRs.
/// \return true if registers were allocated and false otherwise.
static bool CC_X86_32_RegCall_Assign2Regs(unsigned &ValNo, MVT &ValVT,
                                          MVT &LocVT,
                                          CCValAssign::LocInfo &LocInfo,
                                          ISD::ArgFlagsTy &ArgFlags,
                                          CCState &State) {
  // List of GPR registers that are available to store values in regcall
  // calling convention.
  static const MCPhysReg RegList[] = {X86::EAX, X86::ECX, X86::EDX, X86::EDI,
                                      X86::ESI};

  // The vector will save all the available registers for allocation.
  SmallVector<unsigned, 5> AvailableRegs;

  // searching for the available registers.
  for (auto Reg : RegList) {
    if (!State.isAllocated(Reg))
      AvailableRegs.push_back(Reg);
  }

  const size_t RequiredGprsUponSplit = 2;
  if (AvailableRegs.size() < RequiredGprsUponSplit)
    return false; // Not enough free registers - continue the search.

  // Allocating the available registers.
  for (unsigned I = 0; I < RequiredGprsUponSplit; I++) {

    // Marking the register as located.
    MCRegister Reg = State.AllocateReg(AvailableRegs[I]);

    // Since we previously made sure that 2 registers are available
    // we expect that a real register number will be returned.
    assert(Reg && "Expecting a register will be available");

    // Assign the value to the allocated register
    State.addLoc(CCValAssign::getCustomReg(ValNo, ValVT, Reg, LocVT, LocInfo));
  }

  // Successful in allocating registers - stop scanning next rules.
  return true;
}

static ArrayRef<MCPhysReg> CC_X86_VectorCallGetSSEs(const MVT &ValVT) {
  if (ValVT.is512BitVector()) {
    static const MCPhysReg RegListZMM[] = {X86::ZMM0, X86::ZMM1, X86::ZMM2,
                                           X86::ZMM3, X86::ZMM4, X86::ZMM5};
    return RegListZMM;
  }

  if (ValVT.is256BitVector()) {
    static const MCPhysReg RegListYMM[] = {X86::YMM0, X86::YMM1, X86::YMM2,
                                           X86::YMM3, X86::YMM4, X86::YMM5};
    return RegListYMM;
  }

  static const MCPhysReg RegListXMM[] = {X86::XMM0, X86::XMM1, X86::XMM2,
                                         X86::XMM3, X86::XMM4, X86::XMM5};
  return RegListXMM;
}

static ArrayRef<MCPhysReg> CC_X86_64_VectorCallGetGPRs() {
  static const MCPhysReg RegListGPR[] = {X86::RCX, X86::RDX, X86::R8, X86::R9};
  return RegListGPR;
}

static bool CC_X86_VectorCallAssignRegister(unsigned &ValNo, MVT &ValVT,
                                            MVT &LocVT,
                                            CCValAssign::LocInfo &LocInfo,
                                            ISD::ArgFlagsTy &ArgFlags,
                                            CCState &State) {

  ArrayRef<MCPhysReg> RegList = CC_X86_VectorCallGetSSEs(ValVT);
  bool Is64bit = static_cast<const X86Subtarget &>(
                     State.getMachineFunction().getSubtarget())
                     .is64Bit();

  for (auto Reg : RegList) {
    // If the register is not marked as allocated - assign to it.
    if (!State.isAllocated(Reg)) {
      MCRegister AssigedReg = State.AllocateReg(Reg);
      assert(AssigedReg == Reg && "Expecting a valid register allocation");
      State.addLoc(
          CCValAssign::getReg(ValNo, ValVT, AssigedReg, LocVT, LocInfo));
      return true;
    }
    // If the register is marked as shadow allocated - assign to it.
    if (Is64bit && State.IsShadowAllocatedReg(Reg)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
  }

  llvm_unreachable("Clang should ensure that hva marked vectors will have "
                   "an available register.");
  return false;
}

/// Vectorcall calling convention has special handling for vector types or
/// HVA for 64 bit arch.
/// For HVAs shadow registers might be allocated on the first pass
/// and actual XMM registers are allocated on the second pass.
/// For vector types, actual XMM registers are allocated on the first pass.
/// \return true if registers were allocated and false otherwise.
static bool CC_X86_64_VectorCall(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                                 CCValAssign::LocInfo &LocInfo,
                                 ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  // On the second pass, go through the HVAs only.
  if (ArgFlags.isSecArgPass()) {
    if (ArgFlags.isHva())
      return CC_X86_VectorCallAssignRegister(ValNo, ValVT, LocVT, LocInfo,
                                             ArgFlags, State);
    return true;
  }

  // Process only vector types as defined by vectorcall spec:
  // "A vector type is either a floating-point type, for example,
  //  a float or double, or an SIMD vector type, for example, __m128 or __m256".
  if (!(ValVT.isFloatingPoint() ||
        (ValVT.isVector() && ValVT.getSizeInBits() >= 128))) {
    // If R9 was already assigned it means that we are after the fourth element
    // and because this is not an HVA / Vector type, we need to allocate
    // shadow XMM register.
    if (State.isAllocated(X86::R9)) {
      // Assign shadow XMM register.
      (void)State.AllocateReg(CC_X86_VectorCallGetSSEs(ValVT));
    }

    return false;
  }

  if (!ArgFlags.isHva() || ArgFlags.isHvaStart()) {
    // Assign shadow GPR register.
    (void)State.AllocateReg(CC_X86_64_VectorCallGetGPRs());

    // Assign XMM register - (shadow for HVA and non-shadow for non HVA).
    if (MCRegister Reg = State.AllocateReg(CC_X86_VectorCallGetSSEs(ValVT))) {
      // In Vectorcall Calling convention, additional shadow stack can be
      // created on top of the basic 32 bytes of win64.
      // It can happen if the fifth or sixth argument is vector type or HVA.
      // At that case for each argument a shadow stack of 8 bytes is allocated.
      const TargetRegisterInfo *TRI =
          State.getMachineFunction().getSubtarget().getRegisterInfo();
      if (TRI->regsOverlap(Reg, X86::XMM4) ||
          TRI->regsOverlap(Reg, X86::XMM5))
        State.AllocateStack(8, Align(8));

      if (!ArgFlags.isHva()) {
        State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
        return true; // Allocated a register - Stop the search.
      }
    }
  }

  // If this is an HVA - Stop the search,
  // otherwise continue the search.
  return ArgFlags.isHva();
}

/// Vectorcall calling convention has special handling for vector types or
/// HVA for 32 bit arch.
/// For HVAs actual XMM registers are allocated on the second pass.
/// For vector types, actual XMM registers are allocated on the first pass.
/// \return true if registers were allocated and false otherwise.
static bool CC_X86_32_VectorCall(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                                 CCValAssign::LocInfo &LocInfo,
                                 ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  // On the second pass, go through the HVAs only.
  if (ArgFlags.isSecArgPass()) {
    if (ArgFlags.isHva())
      return CC_X86_VectorCallAssignRegister(ValNo, ValVT, LocVT, LocInfo,
                                             ArgFlags, State);
    return true;
  }

  // Process only vector types as defined by vectorcall spec:
  // "A vector type is either a floating point type, for example,
  //  a float or double, or an SIMD vector type, for example, __m128 or __m256".
  if (!(ValVT.isFloatingPoint() ||
        (ValVT.isVector() && ValVT.getSizeInBits() >= 128))) {
    return false;
  }

  if (ArgFlags.isHva())
    return true; // If this is an HVA - Stop the search.

  // Assign XMM register.
  if (MCRegister Reg = State.AllocateReg(CC_X86_VectorCallGetSSEs(ValVT))) {
    State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
    return true;
  }

  // In case we did not find an available XMM register for a vector -
  // pass it indirectly.
  // It is similar to CCPassIndirect, with the addition of inreg.
  if (!ValVT.isFloatingPoint()) {
    LocVT = MVT::i32;
    LocInfo = CCValAssign::Indirect;
    ArgFlags.setInReg();
  }

  return false; // No register was assigned - Continue the search.
}

static bool CC_X86_AnyReg_Error(unsigned &, MVT &, MVT &,
                                CCValAssign::LocInfo &, ISD::ArgFlagsTy &,
                                CCState &) {
  llvm_unreachable("The AnyReg calling convention is only supported by the "
                   "stackmap and patchpoint intrinsics.");
  // gracefully fallback to X86 C calling convention on Release builds.
  return false;
}

static bool CC_X86_32_MCUInReg(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                               CCValAssign::LocInfo &LocInfo,
                               ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  // This is similar to CCAssignToReg<[EAX, EDX, ECX]>, but makes sure
  // not to split i64 and double between a register and stack
  static const MCPhysReg RegList[] = {X86::EAX, X86::EDX, X86::ECX};
  static const unsigned NumRegs = std::size(RegList);

  SmallVectorImpl<CCValAssign> &PendingMembers = State.getPendingLocs();

  // If this is the first part of an double/i64/i128, or if we're already
  // in the middle of a split, add to the pending list. If this is not
  // the end of the split, return, otherwise go on to process the pending
  // list
  if (ArgFlags.isSplit() || !PendingMembers.empty()) {
    PendingMembers.push_back(
        CCValAssign::getPending(ValNo, ValVT, LocVT, LocInfo));
    if (!ArgFlags.isSplitEnd())
      return true;
  }

  // If there are no pending members, we are not in the middle of a split,
  // so do the usual inreg stuff.
  if (PendingMembers.empty()) {
    if (MCRegister Reg = State.AllocateReg(RegList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
    return false;
  }

  assert(ArgFlags.isSplitEnd());

  // We now have the entire original argument in PendingMembers, so decide
  // whether to use registers or the stack.
  // Per the MCU ABI:
  // a) To use registers, we need to have enough of them free to contain
  // the entire argument.
  // b) We never want to use more than 2 registers for a single argument.

  unsigned FirstFree = State.getFirstUnallocated(RegList);
  bool UseRegs = PendingMembers.size() <= std::min(2U, NumRegs - FirstFree);

  for (auto &It : PendingMembers) {
    if (UseRegs)
      It.convertToReg(State.AllocateReg(RegList[FirstFree++]));
    else
      It.convertToMem(State.AllocateStack(4, Align(4)));
    State.addLoc(It);
  }

  PendingMembers.clear();

  return true;
}

/// X86 interrupt handlers can only take one or two stack arguments, but if
/// there are two arguments, they are in the opposite order from the standard
/// convention. Therefore, we have to look at the argument count up front before
/// allocating stack for each argument.
static bool CC_X86_Intr(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                        CCValAssign::LocInfo &LocInfo,
                        ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  const MachineFunction &MF = State.getMachineFunction();
  size_t ArgCount = State.getMachineFunction().getFunction().arg_size();
  bool Is64Bit = MF.getSubtarget<X86Subtarget>().is64Bit();
  unsigned SlotSize = Is64Bit ? 8 : 4;
  unsigned Offset;
  if (ArgCount == 1 && ValNo == 0) {
    // If we have one argument, the argument is five stack slots big, at fixed
    // offset zero.
    Offset = State.AllocateStack(5 * SlotSize, Align(4));
  } else if (ArgCount == 2 && ValNo == 0) {
    // If we have two arguments, the stack slot is *after* the error code
    // argument. Pretend it doesn't consume stack space, and account for it when
    // we assign the second argument.
    Offset = SlotSize;
  } else if (ArgCount == 2 && ValNo == 1) {
    // If this is the second of two arguments, it must be the error code. It
    // appears first on the stack, and is then followed by the five slot
    // interrupt struct.
    Offset = 0;
    (void)State.AllocateStack(6 * SlotSize, Align(4));
  } else {
    report_fatal_error("unsupported x86 interrupt prototype");
  }

  // FIXME: This should be accounted for in
  // X86FrameLowering::getFrameIndexReference, not here.
  if (Is64Bit && ArgCount == 2)
    Offset += SlotSize;

  State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
  return true;
}

static bool CC_X86_64_Pointer(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                              CCValAssign::LocInfo &LocInfo,
                              ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  if (LocVT != MVT::i64) {
    LocVT = MVT::i64;
    LocInfo = CCValAssign::ZExt;
  }
  return false;
}

/// Special handling for i128: Either allocate the value to two consecutive
/// i64 registers, or to the stack. Do not partially allocate in registers,
/// and do not reserve any registers when allocating to the stack.
static bool CC_X86_64_I128(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                           CCValAssign::LocInfo &LocInfo,
                           ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  assert(ValVT == MVT::i64 && "Should have i64 parts");
  SmallVectorImpl<CCValAssign> &PendingMembers = State.getPendingLocs();
  PendingMembers.push_back(
      CCValAssign::getPending(ValNo, ValVT, LocVT, LocInfo));

  if (!ArgFlags.isInConsecutiveRegsLast())
    return true;

  unsigned NumRegs = PendingMembers.size();
  assert(NumRegs == 2 && "Should have two parts");

  static const MCPhysReg Regs[] = {X86::RDI, X86::RSI, X86::RDX,
                                   X86::RCX, X86::R8,  X86::R9};
  ArrayRef<MCPhysReg> Allocated = State.AllocateRegBlock(Regs, NumRegs);
  if (!Allocated.empty()) {
    PendingMembers[0].convertToReg(Allocated[0]);
    PendingMembers[1].convertToReg(Allocated[1]);
  } else {
    int64_t Offset = State.AllocateStack(16, Align(16));
    PendingMembers[0].convertToMem(Offset);
    PendingMembers[1].convertToMem(Offset + 8);
  }
  State.addLoc(PendingMembers[0]);
  State.addLoc(PendingMembers[1]);
  PendingMembers.clear();
  return true;
}

/// Special handling for i128 and fp128: on x86-32, i128 and fp128 get legalized
/// as four i32s, but fp128 must be passed on the stack with 16-byte alignment.
/// Technically only fp128 has a specified ABI, but it makes sense to handle
/// i128 the same until we hear differently.
static bool CC_X86_32_I128_FP128(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                                 CCValAssign::LocInfo &LocInfo,
                                 ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  assert(ValVT == MVT::i32 && "Should have i32 parts");
  SmallVectorImpl<CCValAssign> &PendingMembers = State.getPendingLocs();
  PendingMembers.push_back(
      CCValAssign::getPending(ValNo, ValVT, LocVT, LocInfo));

  if (!ArgFlags.isInConsecutiveRegsLast())
    return true;

  assert(PendingMembers.size() == 4 && "Should have four parts");

  int64_t Offset = State.AllocateStack(16, Align(16));
  PendingMembers[0].convertToMem(Offset);
  PendingMembers[1].convertToMem(Offset + 4);
  PendingMembers[2].convertToMem(Offset + 8);
  PendingMembers[3].convertToMem(Offset + 12);

  State.addLoc(PendingMembers[0]);
  State.addLoc(PendingMembers[1]);
  State.addLoc(PendingMembers[2]);
  State.addLoc(PendingMembers[3]);
  PendingMembers.clear();
  return true;
}

//===----------------------------------------------------------------------===//
// c2go #298 / Wave X Track A: x86-64 C2GoABIInternal CC table.
//
// Mirrors `CC_AArch64_C2GoABIInternal` in
// llvm/lib/Target/AArch64/AArch64CallingConvention.cpp — same all-or-nothing
// aggregate semantics, same integer / FP register lists order (8 GPR + 15
// XMM, matching Go runtime's amd64 ABIInternal), same stack overflow rule.
//
// Register reservations (matching Go runtime ABIInternal / abi-internal.md):
//   integer args/results: RAX, RBX, RCX, RDI, RSI, R8, R9, R10, R11   (9)
//   floating args/results: XMM0..XMM14                                 (15)
//   RDX:  reserved as the closure context pointer (NOT a candidate)
//   R15:  reserved as the zero register (NOT a candidate)
//   RSP / RBP / R12 / R13 / R14: callee-saved / fixed-purpose, not used here
//
// The CC is reachable when the `c2go.goabi` module flag is set AND
// X86C2GoLeafABI flips an eligible near-leaf's CC (#298 power-on; gated
// only by `c2go.goabi`, mirror of AArch64). Modules without `c2go.goabi`
// never see this CC and keep their existing CC table behavior byte-for-byte.
//
// The hand-written implementation rather than .td matches AArch64's decision
// for the same reason: the "consecutive-regs: all-in-regs or all-on-stack"
// aggregate rule cannot be expressed in CCIfConsecutiveRegs alone — once a
// register block does not fit, all registers in the class must be burnt to
// preserve Go's no-back-fill invariant. See AArch64CallingConvention.cpp
// (CC_AArch64_C2GoABIInternal_Common) for the original.
//===----------------------------------------------------------------------===//

// Integer register lists, sized views by index. Order matches Go runtime
// ABIInternal `intArgRegs` (see go/src/cmd/compile/internal/amd64/ssa.go and
// runtime/asm_amd64.s). Sub-i32 promote to i32 (E-view) and integer pointers
// flow through the i64 path naturally (see CC_X86_64_C2GoABIInternal_Common).
static const MCPhysReg C2GoIntRegList64[] = {
    X86::RAX, X86::RBX, X86::RCX, X86::RDI,
    X86::RSI, X86::R8,  X86::R9,  X86::R10, X86::R11};
static const MCPhysReg C2GoIntRegList32[] = {
    X86::EAX, X86::EBX, X86::ECX, X86::EDI,
    X86::ESI, X86::R8D, X86::R9D, X86::R10D, X86::R11D};
static const MCPhysReg C2GoIntRegList16[] = {
    X86::AX,  X86::BX,  X86::CX,  X86::DI,
    X86::SI,  X86::R8W, X86::R9W, X86::R10W, X86::R11W};
static const MCPhysReg C2GoIntRegList8[] = {
    X86::AL,  X86::BL,  X86::CL,  X86::DIL,
    X86::SIL, X86::R8B, X86::R9B, X86::R10B, X86::R11B};

// FP / 128-bit SIMD register list: XMM0..XMM14 (15 regs). XMM15 is reserved
// as the FP zero register in Go's ABIInternal (not in the candidate set).
static const MCPhysReg C2GoXmmRegList[] = {
    X86::XMM0,  X86::XMM1,  X86::XMM2,  X86::XMM3,  X86::XMM4,
    X86::XMM5,  X86::XMM6,  X86::XMM7,  X86::XMM8,  X86::XMM9,
    X86::XMM10, X86::XMM11, X86::XMM12, X86::XMM13, X86::XMM14};

// Stack block finisher for an aggregate that does not fit entirely in
// registers — burn the rest of the class and lay all pending members on the
// stack at the requested SlotAlign. Mirrors AArch64 finishStackBlock fast
// path (no SVE / scalable-vector branch for X86; those are out of v0 scope).
static bool c2goFinishStackBlock(SmallVectorImpl<CCValAssign> &PendingMembers,
                                 MVT LocVT, CCState &State, Align SlotAlign) {
  unsigned Size = LocVT.getSizeInBits() / 8;
  for (auto &It : PendingMembers) {
    It.convertToMem(State.AllocateStack(Size, SlotAlign));
    State.addLoc(It);
    SlotAlign = Align(1);
  }
  PendingMembers.clear();
  return true;
}

// Core shared call+return path. Returns true when fully handled, false when
// the value type is out of scope (caller should fall through to a generic
// path / abort). Matches AArch64's _Common helper signature semantics.
static bool CC_X86_64_C2GoABIInternal_Common(unsigned &ValNo, MVT &ValVT,
                                             MVT &LocVT,
                                             CCValAssign::LocInfo &LocInfo,
                                             ISD::ArgFlagsTy &ArgFlags,
                                             CCState &State) {
  // Aggregate / consecutive-regs block: gather pending members and place them
  // all-in-registers or all-on-stack — Go's "fits entirely or goes to the
  // stack" invariant (mirror of AArch64 CC_AArch64_C2GoABIInternal_Common).
  if (ArgFlags.isInConsecutiveRegs() || !State.getPendingLocs().empty()) {
    ArrayRef<MCPhysReg> RegList;
    if (LocVT.SimpleTy == MVT::i64)
      RegList = C2GoIntRegList64;
    else if (LocVT.SimpleTy == MVT::i32)
      RegList = C2GoIntRegList32;
    else if (LocVT.SimpleTy == MVT::f32 || LocVT.SimpleTy == MVT::f64 ||
             LocVT.is64BitVector() || LocVT.is32BitVector() ||
             LocVT.is128BitVector())
      RegList = C2GoXmmRegList;
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
    // Did not fit — burn the whole register file for this class (Go never
    // back-fills once an aggregate spills) and place the block on the stack
    // at 8-byte alignment (Plan 9 / Go word size on amd64).
    for (auto Reg : RegList)
      State.AllocateReg(Reg);
    return c2goFinishStackBlock(PendingMembers, LocVT, State, Align(8));
  }

  // Plain integer / pointer scalar (i1/i8/i16 promote to i32 E-view, i64/ptr
  // use the R-view). Stack overflow uses the natural type slot size.
  if (LocVT == MVT::i1 || LocVT == MVT::i8 || LocVT == MVT::i16 ||
      LocVT == MVT::i32) {
    if (unsigned Reg = State.AllocateReg(C2GoIntRegList32)) {
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
    if (unsigned Reg = State.AllocateReg(C2GoIntRegList64)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(8, Align(8)), LocVT, LocInfo));
    return true;
  }

  // Floating-point scalar — f32/f64 share the XMM list (Go's ABIInternal
  // does not distinguish single vs. double for register selection).
  if (LocVT == MVT::f32) {
    if (unsigned Reg = State.AllocateReg(C2GoXmmRegList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(4, Align(4)), LocVT, LocInfo));
    return true;
  }
  if (LocVT == MVT::f64) {
    if (unsigned Reg = State.AllocateReg(C2GoXmmRegList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(8, Align(8)), LocVT, LocInfo));
    return true;
  }

  // 128-bit SIMD / vector scalar (v0: same XMM list, 16B stack slot on
  // overflow). Wider vectors (256/512-bit YMM/ZMM) are deferred — they would
  // need their own size-class list and aggregate rule; current Go runtime
  // ABIInternal also does not assign them in registers.
  if (LocVT == MVT::f128 || LocVT.is128BitVector()) {
    if (unsigned Reg = State.AllocateReg(C2GoXmmRegList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return true;
    }
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(16, Align(16)), LocVT, LocInfo));
    return true;
  }

  (void)C2GoIntRegList16;
  (void)C2GoIntRegList8;
  // Anything else (i128, scalable vectors, x86 mask regs, YMM / ZMM, exotic
  // types) is out of v0 scope. We MUST NOT return `false` here: the td entry
  // (`CC_X86` in X86CallingConv.td) routes `CallingConv::C2GoABIInternal`
  // through `CCCustom<"CC_X86_64_C2GoABIInternal_TD">` followed by
  // `CCDelegateTo<CC_X86_64>` as the fall-through tail. Returning `false`
  // would silently land the value in the generic SysV register file (RDI /
  // RSI / RDX / ...) while the function itself has already been flipped to
  // `c2goabiinternalcc` by `X86C2GoLeafABI` — the caller and the callee
  // would then disagree on the register layout and produce silent memory
  // corruption at the call boundary. Hard-fail loudly instead so any
  // regression that lets an unsupported type reach this path is caught at
  // build / test time. Extended type support (i128, YMM, ZMM, scalable,
  // mask) is tracked under Wave Y / Z of #298.
  std::string Msg;
  raw_string_ostream OS(Msg);
  OS << "c2go ABIInternal (x86_64): unsupported arg/result type ";
  LocVT.print(OS);
  OS << " — caller has been flipped to `c2goabiinternalcc` but the "
        "backend has no register-pass rule for this MVT (i128 / YMM / "
        "ZMM / scalable / x86 mask are deferred to Wave Y/Z #298). "
        "Falling through to the generic CC_X86_64 SysV path would "
        "silently corrupt the call ABI. Refusing to lower.";
  report_fatal_error(StringRef(Msg));
}

// CCCustom entry points referenced from X86CallingConv.td via
// `CCIfCC<"CallingConv::C2GoABIInternal", CCCustom<"...">>` in CC_X86 /
// RetCC_X86. Per CCCustom contract, a return value of `true` means "value
// fully handled, stop further dispatch"; `false` means "fall through to the
// next CCIf in the td". For C2GoABIInternal we MUST terminate dispatch for
// every type — otherwise the generic CC_X86_64 path would assign the value
// into the SysV register file and silently corrupt the call. The shared
// helper now `report_fatal_error`s on out-of-scope types (see the
// `CC_X86_64_C2GoABIInternal_Common` tail) so it never returns `false`; it
// either returns `true` on success or aborts. We forward its result as-is
// and `return true` is therefore the only value that actually leaves these
// thunks.
bool llvm::CC_X86_64_C2GoABIInternal_TD(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                                        CCValAssign::LocInfo &LocInfo,
                                        ISD::ArgFlagsTy &ArgFlags,
                                        CCState &State) {
  return CC_X86_64_C2GoABIInternal_Common(ValNo, ValVT, LocVT, LocInfo,
                                          ArgFlags, State);
}

bool llvm::RetCC_X86_64_C2GoABIInternal_TD(unsigned &ValNo, MVT &ValVT,
                                           MVT &LocVT,
                                           CCValAssign::LocInfo &LocInfo,
                                           ISD::ArgFlagsTy &ArgFlags,
                                           CCState &State) {
  // Per abi-internal.md, results use the SAME register sequences as args
  // with both the integer and FP indices reset to 0. A fresh CCState is
  // used on the return path (see X86ISelLoweringCall.cpp's CheckReturn /
  // AnalyzeReturn / AnalyzeCallResult call sites), so the counters start
  // at zero naturally here.
  return CC_X86_64_C2GoABIInternal_Common(ValNo, ValVT, LocVT, LocInfo,
                                          ArgFlags, State);
}

//===----------------------------------------------------------------------===//
// c2go #298 Wave AH.2 — x86-64 GoABI0 stack-only CC table.
//
// Mirror of `CC_AArch64_GoABI0` (AArch64CallingConvention.td:378-387) at
// the amd64 outgoing-arg contract. The baseline implementation walked in
// project_298_abitest_amd64_baseline_2026_06_07 (#485 T3 CallStackArg
// PASS) anchors the slot rules used here:
//
//   * outgoing-args block starts at SP+0 (the amd64 contract; aarch64's
//     SP+8 saved-LR slot has no amd64 equivalent because `CALL` pushes
//     retPC into the caller's frame, OUTSIDE the callee's declared
//     `$framesize`).
//   * type→slot map matches the AArch64 GoABI0 td block exactly:
//       i1/i8/i16/i32  → 4 bytes / 4-byte align (Go zero-extend rule).
//       i64 / pointer  → 8 bytes / 8-byte align.
//       f32            → 4 bytes / 4-byte align.
//       f64            → 8 bytes / 8-byte align.
//       128-bit SIMD   → 16 bytes / 16-byte align.
//   * No register branch — every arg / result lives on the stack. This
//     mirrors the Go boundary `cmd/asm` produces for ABI0 entries.
//
// All-or-nothing aggregate consecutive-regs handling is unused here
// because GoABI0 has no register branch — every member of an aggregate
// just gets its own stack slot in declaration order. The CCCustom hook
// is still preferred over a pure-td CCIfType chain for symmetry with
// the C2GoABIInternal sibling and to keep the slot-rule single source
// of truth in C++ (the td block above only routes to here).
//===----------------------------------------------------------------------===//

static bool CC_X86_64_GoABI0_Common(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                                    CCValAssign::LocInfo &LocInfo,
                                    ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  // 4-byte slot types (Go zero-extends sub-i32 on the stack).
  if (LocVT == MVT::i1 || LocVT == MVT::i8 || LocVT == MVT::i16 ||
      LocVT == MVT::i32 || LocVT == MVT::f32) {
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(4, Align(4)), LocVT, LocInfo));
    return true;
  }
  // 8-byte slot types (pointers, i64, f64).
  if (LocVT == MVT::i64 || LocVT == MVT::f64) {
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(8, Align(8)), LocVT, LocInfo));
    return true;
  }
  // 128-bit SIMD / f128 — direct 16-byte slot.
  if (LocVT == MVT::f128 || LocVT.is128BitVector()) {
    State.addLoc(CCValAssign::getMem(
        ValNo, ValVT, State.AllocateStack(16, Align(16)), LocVT, LocInfo));
    return true;
  }

  // Anything else is out of v0 scope (i128, scalable vectors, x86 mask
  // regs, YMM/ZMM, etc.). The C2GoABIInternal sibling above documents
  // why returning `false` would silently fall through to the generic
  // SysV path and corrupt the call — same logic applies here. Hard-fail
  // so a regression that lets an unsupported type reach this CC is
  // caught at build / test time. Extended type support is tracked under
  // Wave Y/Z of #298.
  std::string Msg;
  raw_string_ostream OS(Msg);
  OS << "c2go GoABI0 (x86_64): unsupported arg/result type ";
  LocVT.print(OS);
  OS << " — caller is in CallingConv::GoABI0 but the backend has no "
        "stack-slot rule for this MVT. Falling through to CC_X86_64 SysV "
        "would silently corrupt the GoABI0 stack frame contract. "
        "Refusing to lower.";
  report_fatal_error(StringRef(Msg));
}

bool llvm::CC_X86_64_GoABI0_TD(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                               CCValAssign::LocInfo &LocInfo,
                               ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  return CC_X86_64_GoABI0_Common(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State);
}

bool llvm::RetCC_X86_64_GoABI0_TD(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                                  CCValAssign::LocInfo &LocInfo,
                                  ISD::ArgFlagsTy &ArgFlags, CCState &State) {
  // GoABI0 results sit on the caller's stack after the incoming-args
  // block (per Go ABI0 contract). A fresh CCState is used on the return
  // path so the slot offsets restart at 0 — matching the AArch64
  // RetCC_AArch64_GoABI0 mirror semantically.
  return CC_X86_64_GoABI0_Common(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State);
}

// Provides entry points of CC_X86 and RetCC_X86.
#include "X86GenCallingConv.inc"
