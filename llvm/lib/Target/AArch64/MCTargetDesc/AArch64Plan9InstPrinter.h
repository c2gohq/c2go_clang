//===- AArch64Plan9InstPrinter.h - Plan 9 ARM64 syntax InstPrinter -*-C++-*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Plan 9 (Go assembler) syntax InstPrinter for AArch64. Emits text that
// `go tool asm` can ingest, with all the rules from
// `cmd/internal/obj/arm64/doc.go` applied:
//
//   - Width via mnemonic suffix (ADD/ADDW for 64-bit/32-bit).
//   - Load/store as MOV family (MOVD/MOVW/MOVWU/MOVBU/MOVHU/MOVB/MOVH).
//   - .P / .W suffix for post/pre-increment.
//   - Condition codes in opcode suffix (BLT not B.LT).
//   - SIMD with V prefix (VADD V5.H8, ...).
//   - br→JMP, blr→CALL/`BL (Rn)`.
//   - Operands in left-to-right assignment order (target last in most insns).
//
// Reference: docs/c2go_design.md §4, docs/c2go_asm_emission.md §9.
//
// v0 stub: full table-driven mnemonic mapping is a large effort. This
// skeleton documents the interface; per-instruction mapping comes in
// Phase E2.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AARCH64_MCTARGETDESC_AARCH64PLAN9INSTPRINTER_H
#define LLVM_LIB_TARGET_AARCH64_MCTARGETDESC_AARCH64PLAN9INSTPRINTER_H

#include "AArch64InstPrinter.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/MC/MCPlan9AsmStreamer.h"

#include <string>

namespace llvm {

class MCExpr;

class AArch64Plan9InstPrinter : public AArch64InstPrinter,
                                 public MCPlan9SymbolicPrinter {
public:
  AArch64Plan9InstPrinter(const MCAsmInfo &MAI, const MCInstrInfo &MII,
                          const MCRegisterInfo &MRI);
  ~AArch64Plan9InstPrinter() override;

  // MCPlan9SymbolicPrinter overrides — these are the entry points the
  // MCPlan9AsmStreamer calls. printInst (the MCInstPrinter override)
  // forwards to tryPrintInst for the standalone llvm-mc path.
  bool tryPrintInst(const MCInst *MI, const MCSubtargetInfo &STI,
                     raw_ostream &O) override;
  void finishPending(raw_ostream &O) override;
  void notifyRawBytesEmitted(const MCInst *MI) override;

  // MCInstPrinter hook: expose our MCPlan9SymbolicPrinter base.
  MCPlan9SymbolicPrinter *getPlan9SymbolicPrinter() override {
    return this;
  }

  void printInst(const MCInst *MI, uint64_t Address, StringRef Annot,
                 const MCSubtargetInfo &STI, raw_ostream &O) override;

  // Plan 9 register names: R0..R30, ZR, SP (no `X` / `W` prefix; width
  // via mnemonic suffix instead).
  void printRegName(raw_ostream &OS, MCRegister Reg) override;

  // Plan 9 operand formatting (`disp(Rn)`, register pairs, etc.) is a
  // step-5 concern. The base class printOperand is non-virtual, so we
  // do not override it here — operand formatting will live inside the
  // per-opcode print routines once the mnemonic table grows.

private:
  // ADRP buffering. ADRP loads a page address into a register, and
  // is almost always immediately followed by an ADD/LDR/STR with the
  // matching `:lo12:` MCExpr — only the *pair* can be translated to
  // a single Plan 9 MOVD directive. Buffer ADRP and emit on the
  // following instruction; flush as standalone if no matching pair.
  bool HavePendingADRP = false;
  MCRegister PendingADRPDest;
  std::string PendingADRPSymbol; // raw symbol name (with leading _ if Mach-O)
  int64_t PendingADRPOffset = 0; // additive byte offset (`sym+N`)
  bool PendingADRPIsGOT = false; // ADRP used a GOT specifier (:got: / @GOTPAGE)

  // Register-state tracker — clang -O hoists ADRP out of loops so a
  // later LDR/STR with `:lo12:sym` reads sym's address from a
  // register populated several instructions earlier (the ADRP is
  // not immediately adjacent). After a successful ADRP[+ADD|+LDR]
  // emit, we remember `register Xn currently holds page address of
  // sym`. Subsequent LDR/STR using Xn as base + MCExpr-referencing
  // the same sym translate to Plan 9 symbolic form. Any instruction
  // that overwrites Xn invalidates the entry. Cleared at section /
  // label boundaries (via finishPending).
  llvm::DenseMap<unsigned /*canonical X reg enum*/, std::string> RegHoldsPage;
  void rememberRegHoldsPage(MCRegister Reg, StringRef Sym);
  void invalidateRegDefs(const MCInst *MI);
  void clearRegState();

  // Handle the various address-aware instruction families in one
  // place. Returns true if the instruction was emitted (caller
  // should NOT fall through to raw bytes).
  bool tryPrintADRP(const MCInst *MI, raw_ostream &O);
  bool tryPrintADRPPairCompletion(const MCInst *MI, raw_ostream &O);
  void flushPendingADRP(raw_ostream &O); // emit MOVD $·sym(SB), Rd
  bool tryPrintADR(const MCInst *MI, raw_ostream &O);             // ADR
  // ADDXri Rd, Rn, :lo12:sym after a non-adjacent ADRP. The ADRP was
  // already lowered to the full address, so preserve this instruction as a
  // register move instead of adding the low 12 bits a second time.
  bool tryPrintADDXriViaMaterializedAddress(const MCInst *MI, raw_ostream &O);
  bool tryPrintLDRLiteral(const MCInst *MI, raw_ostream &O);      // LDRXl etc.
  // LDR/STR with `:lo12:sym` whose base register was set up by an
  // earlier ADRP (tracked via RegHoldsPage). Handles W/X/B/H/S/D/Q
  // variants of LDR + the symmetric STR forms.
  bool tryPrintLDRSTRViaTrackedADRP(const MCInst *MI, raw_ostream &O);

  // c2go §D2 phase 2: LDR/STR whose imm12 was rewritten by the
  // AArch64 AsmPrinter into an MCSymbolRefExpr (`<Rec>_<Field>`).
  // Emits Plan 9 `MOVW <Sym>(Rn), Rt` form where Rn carries an object
  // pointer (no SB suffix, unlike the tracked-ADRP family).
  bool tryPrintLDRSTRSymbolicField(const MCInst *MI, raw_ostream &O);

  // #271: SP-relative load/store/adjust. base register = SP (reg 31)
  // forms that otherwise degrade to raw WORD. Translating these to Go
  // mnemonics is what populates the assembler's pcsp table, so a
  // per-callsite `SUB/ADD $imm, RSP, RSP` (call-frame adjustment) and
  // every `MOVx off(RSP)` frame slot access is visible to copystack /
  // morestack / GC stack-unwind. Covers:
  //   * SUBXri/ADDXri with Rd=Rn=SP  → SUB/ADD $imm, RSP, RSP
  //   * ADDXri with Rn=SP, Rd!=SP    → MOVD RSP, Rd (#0) / ADD $imm, RSP, Rd
  //   * LDR/STR {X,W,SW,BB,SBW,HH,SHW,S,D,Q}ui with base=SP
  //                                  → MOVD/MOVW/MOVWU/MOVBU/... off(RSP)
  //   * STP/LDP Xi with base=SP      → STP (Ra,Rb),off(RSP) / LDP ...
  bool tryPrintSPAdjust(const MCInst *MI, raw_ostream &O);   // ADD/SUB SP
  bool tryPrintSPMemImm(const MCInst *MI, raw_ostream &O);   // LDR/STR off(SP)
  bool tryPrintSPPair(const MCInst *MI, raw_ostream &O);     // STP/LDP off(SP)

  // #271: high-frequency non-address-aware opcodes that otherwise degrade
  // to raw WORD. GPR-base LDR/STR, ADD/SUB imm, CMP/CMN, shifted-reg
  // ADD/SUB/AND/ORR/EOR (incl. ORR-as-MOV), and MOVZ.
  bool tryPrintGPRMemImm(const MCInst *MI, raw_ostream &O);  // MOVx off(Rn)
  bool tryPrintAddSubImm(const MCInst *MI, raw_ostream &O);  // ADD/SUB $imm
  bool tryPrintArithSReg(const MCInst *MI, raw_ostream &O);  // ADD/SUB/log reg
  bool tryPrintCmp(const MCInst *MI, raw_ostream &O);        // CMP/CMN
  bool tryPrintMovImm(const MCInst *MI, raw_ostream &O);     // MOVZ

  bool tryPrintBranchCall(const MCInst *MI, raw_ostream &O); // BL
  bool tryPrintIndirectCall(const MCInst *MI, raw_ostream &O); // BLR/BR
  bool tryPrintUnconditionalBranch(const MCInst *MI, raw_ostream &O); // B
  bool tryPrintConditionalBranch(const MCInst *MI, raw_ostream &O);   // Bcc
  bool tryPrintCBZBranch(const MCInst *MI, raw_ostream &O);           // CB(N)Z*
  bool tryPrintTBZBranch(const MCInst *MI, raw_ostream &O);           // TB(N)Z*
  bool tryPrintRet(const MCInst *MI, raw_ostream &O);                  // RET

  // #271 round-2: high-frequency WORD-fallback opcodes (extension).
  //   * tryPrintGPRPair        — STP/LDP X/W via GPR base (non-SP, signed-off)
  //   * tryPrintGPRMemUnscaled — LDUR/STUR X/W/H/B/S/D/Q via signed unscaled imm
  //   * tryPrintMovWide        — MOVK/MOVN (MOVZ already handled by tryPrintMovImm)
  //   * tryPrintTST            — ANDS-zero-reg alias (imm and shifted-reg forms)
  //   * tryPrintSXT            — SBFM-aliased SXTB/SXTH/SXTW sign-extend moves
  bool tryPrintGPRPair(const MCInst *MI, raw_ostream &O);
  bool tryPrintGPRMemUnscaled(const MCInst *MI, raw_ostream &O);
  bool tryPrintMovWide(const MCInst *MI, raw_ostream &O);
  bool tryPrintTST(const MCInst *MI, raw_ostream &O);
  bool tryPrintSXT(const MCInst *MI, raw_ostream &O);

  // #271 round-3: more high-frequency WORD-fallback opcodes.
  //   * tryPrintLogicalImm  — AND/ORR/EOR with bitmask immediate
  //   * tryPrintCSEL        — conditional select (CSEL/CSELW)
  bool tryPrintLogicalImm(const MCInst *MI, raw_ostream &O);
  bool tryPrintCSEL(const MCInst *MI, raw_ostream &O);

  // #271 round-4: multiply-accumulate and FMOV X<->FPR family.
  //   * tryPrintMulAcc — MADD/SMADDL/UMADDL/MSUB/SMSUBL/UMSUBL
  //   * tryPrintFMovGPR — FMOV between GPR and S/D regs
  bool tryPrintMulAcc(const MCInst *MI, raw_ostream &O);
  bool tryPrintFMovGPR(const MCInst *MI, raw_ostream &O);

  // #271 round-5: CSINC alias family — CSET (Rn=Rm=ZR) / CINC (Rn==Rm).
  bool tryPrintCSINC(const MCInst *MI, raw_ostream &O);

  // Format a symbol target operand (MCExpr) for use after a
  // mnemonic. Local labels (LBB*) become bare names; external
  // symbols get `·` prefix + last-dot middle-dot translation.
  std::string formatBranchTarget(const MCExpr *E) const;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_AARCH64_MCTARGETDESC_AARCH64PLAN9INSTPRINTER_H
