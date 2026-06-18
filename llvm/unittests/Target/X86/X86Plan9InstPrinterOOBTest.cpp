//===- X86Plan9InstPrinterOOBTest.cpp - Short-MCInst OOB guard tests ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MC-level tests for the operand-bounds guards in the X86 Plan 9
// InstPrinter dispatchers (tryPrint*).
//
// These exercise the strict "< N" guard semantics: a tryPrint* dispatcher
// seeing a rare, malformed MCInst with fewer operands than its body's
// deepest getOperand() index demands. That shape is not producible from
// MIR (the MachineVerifier rejects it before AsmPrinter) nor from llvm-mc
// (the asm parser rebuilds the MCInst to the InstrInfo-declared operand
// count), so it can only be reached by constructing the MCInst by hand and
// calling tryPrintInst through the public MCPlan9SymbolicPrinter interface,
// which is what this test does.
//
// Pinned guards:
//   * tryPrintArithReg                   - head guard < 3
//   * tryPrintIndirectCall mem           - branch guard < 5 for CALL{64,32,16}m
//   * tryPrintUnconditionalBranch JMP64r - branch guard < 1
//   * tryPrintUnconditionalBranch JMP64m - branch guard < 5
//
// For each guard we feed an MCInst whose operand count is exactly one below
// the threshold. Expected behaviour:
//   1.  tryPrintInst returns false (the dispatcher refuses the shape).
//   2.  Nothing is written to the output stream.
//   3.  No SmallVector OOB / assertion fires.
//
// Without the guards, (3) would fail via "Assertion failed: (idx < size())"
// in SmallVector::operator[]. The early-return path makes (1) and (2) hold
// without touching out-of-range operand indices.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/X86MCTargetDesc.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCPlan9AsmStreamer.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

class X86Plan9InstPrinterOOBTest : public ::testing::Test {
protected:
  std::unique_ptr<MCRegisterInfo> MRI;
  std::unique_ptr<MCAsmInfo> MAI;
  std::unique_ptr<const MCInstrInfo> MII;
  std::unique_ptr<MCSubtargetInfo> STI;
  std::unique_ptr<MCInstPrinter> Printer;
  // Non-owning; points into Printer's implementation. X86Plan9InstPrinter
  // inherits MCPlan9SymbolicPrinter and exposes it via
  // getPlan9SymbolicPrinter.
  MCPlan9SymbolicPrinter *SymPrinter = nullptr;

  void SetUp() override {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();

    StringRef TripleName = "x86_64-apple-darwin";
    Triple TT(TripleName);
    std::string Err;
    const Target *TheTarget = TargetRegistry::lookupTarget(TT, Err);
    ASSERT_TRUE(TheTarget) << "x86_64 target lookup failed: " << Err;

    MRI.reset(TheTarget->createMCRegInfo(TT));
    MCTargetOptions MCOptions;
    MAI.reset(TheTarget->createMCAsmInfo(*MRI, TT, MCOptions));
    MII.reset(TheTarget->createMCInstrInfo());
    STI.reset(TheTarget->createMCSubtargetInfo(TT, "", ""));
    // Syntax variant 2 = Plan 9, which selects X86Plan9InstPrinter in
    // createX86MCInstPrinter.
    Printer.reset(TheTarget->createMCInstPrinter(TT, /*SyntaxVariant=*/2,
                                                  *MAI, *MII, *MRI));
    ASSERT_TRUE(Printer) << "X86 Plan9 InstPrinter construction failed "
                            "(variant=2 not registered?)";
    SymPrinter = Printer->getPlan9SymbolicPrinter();
    ASSERT_TRUE(SymPrinter)
        << "X86Plan9InstPrinter does not expose MCPlan9SymbolicPrinter "
           "via getPlan9SymbolicPrinter()";
  }

  // Build a malformed MCInst: real X86 opcode, fewer explicit operands than
  // the dispatcher body needs.
  static MCInst makeShortInst(unsigned Opcode, unsigned NumOps,
                              MCRegister Reg) {
    MCInst MI;
    MI.setOpcode(Opcode);
    for (unsigned I = 0; I < NumOps; ++I)
      MI.addOperand(MCOperand::createReg(Reg));
    return MI;
  }

  // Try-print the MCInst and assert the dispatcher refused (returned false)
  // without writing output and without aborting.
  void expectShortInstRefused(const MCInst &MI, const char *Tag) {
    std::string Buf;
    raw_string_ostream OS(Buf);
    bool Printed = SymPrinter->tryPrintInst(&MI, *STI, OS);
    EXPECT_FALSE(Printed) << Tag << ": tryPrintInst rendered a short-operand "
                                    "MCInst it should have refused";
    EXPECT_TRUE(Buf.empty())
        << Tag << ": tryPrintInst wrote output for a refused MCInst: '" << Buf
        << "'";
  }
};

// tryPrintArithReg head guard < 3. CMP64rr's body indexes operand(2).
// Without the head guard, a per-branch < (NumDefs + 2) check collapses to
// < 2 when NumDefs == 0 and admits a 2-operand inst, then operand(2) OOBs.
// The head guard < 3 refuses early.
TEST_F(X86Plan9InstPrinterOOBTest, CMP64rrShortOperandRefused) {
  // 2 explicit operands, one below the head-guard threshold of 3.
  MCInst MI = makeShortInst(X86::CMP64rr, /*NumOps=*/2, X86::RAX);
  expectShortInstRefused(MI, "CMP64rr/2");
}

// TEST64rr goes through the same ArithReg dispatcher (NumDefs == 0 family),
// so the same head guard applies.
TEST_F(X86Plan9InstPrinterOOBTest, TEST64rrShortOperandRefused) {
  MCInst MI = makeShortInst(X86::TEST64rr, /*NumOps=*/2, X86::RAX);
  expectShortInstRefused(MI, "TEST64rr/2");
}

// tryPrintIndirectCall branch guard < 5 for the mem form (CALL{64,32,16}m).
// The mem operand is a 5-tuple {Base, Scale, Idx, Disp, Seg} and
// printPlan9MemRef indexes OpStart+0..OpStart+3. With only a head guard
// < 1, any inst with 1-4 operands falls into the body and OOBs at
// getOperand(3).
TEST_F(X86Plan9InstPrinterOOBTest, CALL64mShortMemOperandRefused) {
  // 4 explicit operands, one below the mem-branch threshold of 5.
  MCInst MI = makeShortInst(X86::CALL64m, /*NumOps=*/4, X86::RAX);
  expectShortInstRefused(MI, "CALL64m/4");
}

// The 32- and 16-bit mem-indirect call forms named in the header share the
// same 5-tuple mem operand and the same printPlan9MemRef OOB exposure as
// CALL64m, so a 4-operand inst must be refused for them too (the operands are
// never read past the guard, so a filler reg of any class is fine).
TEST_F(X86Plan9InstPrinterOOBTest, CALL32mShortMemOperandRefused) {
  MCInst MI = makeShortInst(X86::CALL32m, /*NumOps=*/4, X86::RAX);
  expectShortInstRefused(MI, "CALL32m/4");
}

TEST_F(X86Plan9InstPrinterOOBTest, CALL16mShortMemOperandRefused) {
  MCInst MI = makeShortInst(X86::CALL16m, /*NumOps=*/4, X86::RAX);
  expectShortInstRefused(MI, "CALL16m/4");
}

// tryPrintUnconditionalBranch JMP64r branch guard < 1. Without it, a
// 0-operand JMP64r OOBs at getOperand(0); the guard refuses the zero-op
// shape.
TEST_F(X86Plan9InstPrinterOOBTest, JMP64rZeroOperandRefused) {
  MCInst MI;
  MI.setOpcode(X86::JMP64r);
  // No operands appended.
  expectShortInstRefused(MI, "JMP64r/0");
}

// tryPrintUnconditionalBranch JMP64m branch guard < 5. The computed-goto /
// jump-table form has the same 5-tuple mem operand as CALL64m and the same
// OOB exposure on the body's printPlan9MemRef call.
TEST_F(X86Plan9InstPrinterOOBTest, JMP64mShortMemOperandRefused) {
  MCInst MI = makeShortInst(X86::JMP64m, /*NumOps=*/4, X86::RAX);
  expectShortInstRefused(MI, "JMP64m/4");
}

// A well-formed but unrecognised MCInst (an opcode the Plan 9 printer does
// not claim) must also return false without crashing. Pins the dispatcher's
// "unknown opcode" fallthrough against accidental claim-and-crash on a
// future opcode.
TEST_F(X86Plan9InstPrinterOOBTest, UnrecognizedOpcodeFallthroughIsClean) {
  // Pick a vector op the Plan 9 dispatcher table does not enumerate.
  MCInst MI;
  MI.setOpcode(X86::VPCMPEQDZ128rr);
  MI.addOperand(MCOperand::createReg(X86::XMM0));
  MI.addOperand(MCOperand::createReg(X86::XMM1));
  MI.addOperand(MCOperand::createReg(X86::XMM2));
  std::string Buf;
  raw_string_ostream OS(Buf);
  bool Printed = SymPrinter->tryPrintInst(&MI, *STI, OS);
  EXPECT_FALSE(Printed) << "Plan 9 dispatcher claimed an unrecognised opcode";
  EXPECT_TRUE(Buf.empty())
      << "Plan 9 dispatcher wrote output for an unclaimed opcode: '" << Buf
      << "'";
}

// A well-formed RET64 (the simplest Plan 9-claimed opcode) must still emit
// cleanly. The OOB guards must never refuse a legitimate inst: behaviour
// stays unchanged for any inst whose operand count meets or exceeds the
// guard threshold. Pins the negative invariant (no false refuses) of the
// operand-count guards.
TEST_F(X86Plan9InstPrinterOOBTest, WellFormedRET64StillEmits) {
  MCInst MI;
  MI.setOpcode(X86::RET64);
  std::string Buf;
  raw_string_ostream OS(Buf);
  bool Printed = SymPrinter->tryPrintInst(&MI, *STI, OS);
  EXPECT_TRUE(Printed) << "Plan 9 dispatcher refused well-formed RET64";
  EXPECT_NE(Buf.find("RET"), std::string::npos)
      << "RET64 did not emit a RET mnemonic; got: '" << Buf << "'";
}

} // namespace
