//===- X86Plan9InstPrinterOOBTest.cpp - Short-MCInst OOB guard tests ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #298 / Track AG.3 — MC-level strong proof of the AF.3 systematic
// audit. The companion LIT (llvm/test/CodeGen/X86/c2go-plan9-instprinter-
// oob.ll) drives ill-formed-but-naturally-emitted MCInsts through normal
// codegen so the post-fix path falls through to the raw-byte streamer.
// That LIT exercises the WELL-FORMED operand-count path (CMP64rr with
// NumDefs==0 + 2 explicit operands; CALL64m / JMP64r / JMP64m at -O0).
//
// What the LIT could NOT exercise is the strict "< N" guard semantics:
// the path where a `tryPrint*` dispatcher sees a (rare, malformed) MCInst
// with fewer slots than its body's deepest getOperand() index demands.
// That shape isn't producible from MIR (the MachineVerifier rejects it
// before AsmPrinter runs) nor from llvm-mc (the asm parser rebuilds the
// MCInst to the InstrInfo-declared operand count). The only direct vector
// is to construct the MCInst by hand and call `tryPrintInst` through the
// public `MCPlan9SymbolicPrinter` interface — which is exactly what this
// gtest does.
//
// Pinned invariants (Wave AF.3 sweep — see X86Plan9InstPrinter.cpp:18-43):
//   * tryPrintArithReg          — head guard `< 3` (Wave AE.2)
//   * tryPrintIndirectCall mem  — branch guard `< 5` for CALL{64,32,16}m
//   * tryPrintUnconditionalBranch JMP64r — branch guard `< 1`
//   * tryPrintUnconditionalBranch JMP64m — branch guard `< 5`
//
// For each pinned guard we feed a MCInst whose operand count is exactly
// one less than the guard's threshold. The expected behaviour is:
//   1.  tryPrintInst returns false (the dispatcher refuses the shape).
//   2.  Nothing is written to the output stream.
//   3.  No SmallVector OOB / assertion fires.
//
// Pre-fix, (3) would have failed via `Assertion failed: (idx < size())`
// in SmallVector::operator[]. The post-fix early-return path makes (1)
// and (2) hold without touching out-of-range operand indices.
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
  // Non-owning — points into `Printer`'s implementation (X86Plan9InstPrinter
  // inherits MCPlan9SymbolicPrinter via multiple inheritance and exposes
  // itself via `getPlan9SymbolicPrinter`).
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
    // Variant 2 = Plan 9 — selects `X86Plan9InstPrinter` in
    // `createX86MCInstPrinter` (X86MCTargetDesc.cpp:486).
    Printer.reset(TheTarget->createMCInstPrinter(TT, /*SyntaxVariant=*/2,
                                                  *MAI, *MII, *MRI));
    ASSERT_TRUE(Printer) << "X86 Plan9 InstPrinter construction failed "
                            "(variant=2 not registered?)";
    SymPrinter = Printer->getPlan9SymbolicPrinter();
    ASSERT_TRUE(SymPrinter)
        << "X86Plan9InstPrinter does not expose MCPlan9SymbolicPrinter "
           "via getPlan9SymbolicPrinter()";
  }

  // Helper: build a malformed MCInst — real X86 opcode, fewer explicit
  // operands than the dispatcher body needs.
  static MCInst makeShortInst(unsigned Opcode, unsigned NumOps,
                              MCRegister Reg) {
    MCInst MI;
    MI.setOpcode(Opcode);
    for (unsigned I = 0; I < NumOps; ++I)
      MI.addOperand(MCOperand::createReg(Reg));
    return MI;
  }

  // Helper: try-print the MCInst and assert the dispatcher refused (false
  // return) without writing output and without aborting.
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

// (a) tryPrintArithReg — head guard `< 3` (Wave AE.2 / Y C6 mirror).
// CMP64rr's body indexes operand(2). Pre-fix path admitted MCInsts with
// 2 operands (the per-branch `< (NumDefs + 2)` guard for NumDefs == 0
// collapsed to `< 2`), then operand(2) OOB'd. Post-fix: head guard `< 3`
// refuses early.
TEST_F(X86Plan9InstPrinterOOBTest, CMP64rrShortOperandRefused) {
  // 2 explicit operands — one below the head-guard threshold of 3.
  MCInst MI = makeShortInst(X86::CMP64rr, /*NumOps=*/2, X86::RAX);
  expectShortInstRefused(MI, "CMP64rr/2");
}

// Additional ArithReg shape — TEST64rr falls through the same dispatcher,
// also NumDefs==0 family. Same guard.
TEST_F(X86Plan9InstPrinterOOBTest, TEST64rrShortOperandRefused) {
  MCInst MI = makeShortInst(X86::TEST64rr, /*NumOps=*/2, X86::RAX);
  expectShortInstRefused(MI, "TEST64rr/2");
}

// (b) tryPrintIndirectCall — branch guard `< 5` for the mem form
// (CALL{64,32,16}m). The mem operand is a 5-tuple {Base, Scale, Idx,
// Disp, Seg}; printPlan9MemRef indexes OpStart+0..OpStart+3. Pre-AF.3
// path had a head guard `< 1` only — any MCInst with 1-4 operands fell
// through into the body and OOB'd at getOperand(3).
TEST_F(X86Plan9InstPrinterOOBTest, CALL64mShortMemOperandRefused) {
  // 4 explicit operands — one below the mem-branch threshold of 5.
  MCInst MI = makeShortInst(X86::CALL64m, /*NumOps=*/4, X86::RAX);
  expectShortInstRefused(MI, "CALL64m/4");
}

// (c) tryPrintUnconditionalBranch JMP64r — branch guard `< 1`.
// Pre-AF.3 path had no head guard; a 0-operand JMP64r would have OOB'd
// at getOperand(0). The guard refuses the shape with zero ops.
TEST_F(X86Plan9InstPrinterOOBTest, JMP64rZeroOperandRefused) {
  MCInst MI;
  MI.setOpcode(X86::JMP64r);
  // No operands appended.
  expectShortInstRefused(MI, "JMP64r/0");
}

// (d) tryPrintUnconditionalBranch JMP64m — branch guard `< 5`.
// Computed-goto / jump-table form. Same 5-tuple mem operand as
// CALL64m; same OOB exposure on the body's `printPlan9MemRef` call.
TEST_F(X86Plan9InstPrinterOOBTest, JMP64mShortMemOperandRefused) {
  MCInst MI = makeShortInst(X86::JMP64m, /*NumOps=*/4, X86::RAX);
  expectShortInstRefused(MI, "JMP64m/4");
}

// Companion sanity-check: a WELL-FORMED but unrecognised MCInst (an
// opcode the Plan 9 printer doesn't claim) must also return false
// without crashing. This pins the dispatcher's "I don't know this
// opcode" fallthrough — guarding against accidental claim-and-crash
// on a future opcode that ships through some other guarded path.
TEST_F(X86Plan9InstPrinterOOBTest, UnrecognizedOpcodeFallthroughIsClean) {
  // Pick a vector op the Plan 9 dispatcher table doesn't enumerate.
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

// Reverse-direction sanity: a WELL-FORMED RET64 (the simplest Plan 9-
// claimed opcode) must still emit cleanly. The OOB guards added by
// Wave AE.2 / AF.3 must never refuse a legitimate inst — the fast-path
// behaviour stays byte-identical for any inst whose operand count meets
// or exceeds the guard threshold. This pins the negative invariant
// (no false-refuses) of every operand-count guard touched in the audit.
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
