//===- X86Plan9InstPrinter.cpp - Plan 9 X86 syntax InstPrinter -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Wave Y Track C — minimal viable Plan 9 (Go assembler) syntax printer for
// X86 / X86_64 MCInsts. Used by `MCPlan9AsmStreamer` when the c2go
// `OutputAsmVariant == 2` pipeline is in effect on an X86 triple.
//
// Scope (this drop):
//   * Translate the instruction families a NOSPLIT leaf flipped to
//     `c2goabiinternalcc` actually emits — RET, direct CALL, conditional /
//     unconditional branches, SP-relative load/store/adjust, GPR-base
//     load/store/arith, MOV-immediate, symbolic LEA / MOV, SSE moves and
//     CVTTSD2SQ (the only float→int truncation abitest exercises).
//   * Mnemonic / register renaming via the `X86Plan9MnemonicMap.h` data
//     table delivered by Track B.
//   * For any opcode this drop does not recognise return `false` from
//     `tryPrintInst`; the streamer falls back to its raw-byte WORD path
//     (or PLAN9-ERROR if the inst carries fixups).
//
// Deferred to Wave Z:
//   * Full x86_64 ISA coverage (AVX-512 mask / EVEX-broadcast, x87, MPX,
//     etc.) — the table-driven mnemonic mapping already lists most of the
//     entries; only the operand-shape dispatchers need extending.
//   * morestack / copystack / stackmap / statepoint integration. A leaf
//     NOSPLIT function has no calls into morestack and no GC safepoints,
//     so FUNCDATA $0 / FUNCDATA $1 / FUNCDATA $2 are not emitted here.
//
// Wave AF.3 operand-access audit (2026-06-09):
//   Systematic sweep of every `MI->getOperand(N)` site in this TU. Each
//   tryPrint* dispatcher now refuses with `getNumOperands() < N` *before*
//   touching slot N, mirroring the Wave Y C6 (tryPrintSPAdjust) and Wave
//   AE.2 (tryPrintArithReg) fixes. Closed in this round:
//     * tryPrintIndirectCall mem path — guard `< 5`
//     * tryPrintUnconditionalBranch JMP64r — guard `< 1`
//     * tryPrintUnconditionalBranch JMP64m — guard `< 5`
//   All other dispatchers (CALL pcrel32, JCC, MOV{rr,ri,mr,rm,imm},
//   LEA, SSE mov, PUSH/POP, ArithReg, TypeinfoLoad, printOperand) were
//   already gated and remain byte-identical for the well-formed
//   `>= N` case. Fast-path behaviour is unchanged. Helper
//   `printPlan9MemRef` is caller-guarded — every call site holds either
//   `< 6` (5-tuple mem with explicit reg) or `< 5` (mem-only forms).
//
// Track AN.1 symbolic mem-direct MOV-imm / integer-ALU coverage
// (2026-06-10):
//   X86 lowers global RMW / compare / store-imm to memory-direct forms
//   (`MOV64mi32 $0, gSink(%rip)`, `XOR64mr %rax, gSink+16(%rip)`,
//   `CMP64mi8 $0, gSink+8(%rip)`, `INC64m`, ...) that AArch64 has no
//   equivalent of (its RMW expands to already-covered LDR/op/STR).
//   Pre-AN.1 these fixup-bearing MCInsts had no dispatcher and fell to
//   `MCPlan9AsmStreamer::emitRawBytesOrFail`, which swallowed them
//   behind a `// PLAN9-ERROR` comment the Go assembler ignores —
//   global stores no-op'd and Jcc/CMOV consumed stale EFLAGS (amd64
//   `STRESS ASCAST FAIL validator=0`, deterministic at every depth and
//   at -O0/-O2 alike). `tryPrintMovImmToMemSymbolic` +
//   `tryPrintArithMemSymbolic` below close the gap; the streamer side
//   is now fail-closed (report_fatal_error) for any remaining
//   symbol-bearing miss.
//
// Track AO.3 symbolic SSE / scalar-FP / widening-load / x87 coverage
// (2026-06-11):
//   The SQLite amd64 WF1 -O2 inventory (Wave AM fail-open snapshot,
//   2358 PLAN9-ERROR lines) leaves 884 lines / 29 opcodes after the
//   AN.1 integer family — four families, all memory-operand forms whose
//   displacement is a RIP-relative constant-pool entry (`_LCPI*`) or
//   global: (1) SSE packed/integer SIMD (MOVDQArm 440, MOVDI2PDIrm,
//   MOVQI2PQIrm [HEAD's first fatal], PAND/POR/PADD*/PMULLD/PUNPCKLDQ/
//   XORP*/SUBPDrm, MOVDQU{rm,mr}), (2) SSE scalar FP (MOVSDrm_alt 116,
//   UCOMISDrm, ADD/SUB/MUL/DIVSDrm), (3) widening integer loads
//   (MOVZX32rm8/16, MOVSX64rm32), (4) x87 long-double constant loads
//   (LD_F80m/LD_F64m/LD_F32m, MUL_F32m — SQLite's LONGDOUBLE_TYPE
//   arithmetic). `tryPrintSSEX87MemSymbolic` below closes all four
//   table-driven; every Plan-9 spelling was verified against `go tool
//   asm` + `go tool objdump` (encodings match the original Intel forms,
//   incl. MOVO=movdqa, MOVL/MOVQ-to-X=movd/movq, PADDL=paddd,
//   PUNPCKLLQ=punpckldq, FMOVX=fld tbyte, FMULF=fmul m32). Same AN.1
//   policy: only symbolic (fixup-bearing) shapes are claimed — the
//   non-symbolic raw-byte fallback stays byte-identical.
//
//===----------------------------------------------------------------------===//

#include "X86Plan9InstPrinter.h"
#include "MCTargetDesc/X86BaseInfo.h"
#include "MCTargetDesc/X86MCAsmInfo.h"
#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86Plan9MnemonicMap.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <cstring>
#include <string>

using namespace llvm;

X86Plan9InstPrinter::X86Plan9InstPrinter(const MCAsmInfo &MAI,
                                          const MCInstrInfo &MII,
                                          const MCRegisterInfo &MRI)
    : X86InstPrinterCommon(MAI, MII, MRI) {}

X86Plan9InstPrinter::~X86Plan9InstPrinter() = default;

namespace {

// Walk a `MCBinaryExpr` add/sub chain summing all integer constants. Used
// to extract `sym+N` / `sym-N` byte offsets from a symbol reference.
// Mirrors `AArch64Plan9InstPrinter`'s helper of the same name.
static int64_t getReferencedSymbolOffset(const MCExpr *E) {
  int64_t Off = 0;
  while (E) {
    switch (E->getKind()) {
    case MCExpr::Constant:
      return Off + cast<MCConstantExpr>(E)->getValue();
    case MCExpr::SymbolRef:
      return Off;
    case MCExpr::Unary:
      E = cast<MCUnaryExpr>(E)->getSubExpr();
      break;
    case MCExpr::Binary: {
      const MCBinaryExpr *B = cast<MCBinaryExpr>(E);
      if (B->getOpcode() == MCBinaryExpr::Add ||
          B->getOpcode() == MCBinaryExpr::Sub) {
        int64_t Sign = (B->getOpcode() == MCBinaryExpr::Add) ? 1 : -1;
        if (const auto *CR = dyn_cast<MCConstantExpr>(B->getRHS()))
          Off += Sign * CR->getValue();
        E = B->getLHS();
        break;
      }
      return Off;
    }
    case MCExpr::Target:
    case MCExpr::Specifier:
      E = cast<MCSpecifierExpr>(E)->getSubExpr();
      break;
    }
  }
  return Off;
}

// Pull the underlying `MCSymbol` name out of an `MCExpr` that may be
// wrapped in MCBinary / MCUnary / MCSpecifier layers.
static StringRef getReferencedSymbolName(const MCExpr *E) {
  while (E) {
    switch (E->getKind()) {
    case MCExpr::Constant:
      return StringRef();
    case MCExpr::SymbolRef:
      return cast<MCSymbolRefExpr>(E)->getSymbol().getName();
    case MCExpr::Unary:
      E = cast<MCUnaryExpr>(E)->getSubExpr();
      break;
    case MCExpr::Binary: {
      const MCBinaryExpr *B = cast<MCBinaryExpr>(E);
      if (B->getOpcode() == MCBinaryExpr::Add ||
          B->getOpcode() == MCBinaryExpr::Sub) {
        E = B->getLHS();
        break;
      }
      return StringRef();
    }
    case MCExpr::Target:
    case MCExpr::Specifier:
      E = cast<MCSpecifierExpr>(E)->getSubExpr();
      break;
    }
  }
  return StringRef();
}

// Convert a Go-style symbol name to its Plan 9 textual form. Mirrors the
// dual-path policy used by `AArch64Plan9InstPrinter::goSymToPlan9` and
// `MCPlan9AsmStreamer::symbolToPlan9` (keep the three in sync).
static std::string goSymToPlan9(StringRef Name) {
  static const char DotUTF8[]   = "\xc2\xb7";       // U+00B7 ·
  static const char SlashUTF8[] = "\xe2\x88\x95";   // U+2215 ∕
  auto sanitiseToIdent = [](std::string &S) {
    for (char &C : S) {
      if ((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
          (C >= '0' && C <= '9') || C == '_')
        continue;
      C = '_';
    }
  };

  // Mach-O / ELF local labels: sanitise, no middle-dot. #654b: shared
  // predicate (MCPlan9AsmStreamer.h) — a static C function named `l_alloc`
  // or `LTnum` (Lua) stays an ordinary `·` symbol matching its TEXT
  // definition; only compiler-generated privates render file-local.
  if (isPlan9CompilerLocalSym(Name)) {
    std::string Out = Name.str();
    sanitiseToIdent(Out);
    // #586: a local symbol reaching goSymToPlan9 is a private DATA symbol
    // (string literal / constant pool); the streamer emits it file-local
    // (`name<>(SB)`) to avoid cross-package link collisions, so the reference
    // must carry the same `<>` scope.
    Out += "<>";
    return Out;
  }

  bool HasSlash = Name.contains('/');
  bool HasIllegal = false;
  for (char C : Name)
    if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
          (C >= '0' && C <= '9') || C == '_' || C == '/' || C == '.')) {
      HasIllegal = true;
      break;
    }

  // Path (a): import-path-style and fully transformable.
  if (HasSlash && !HasIllegal) {
    std::string Out;
    Out.reserve(Name.size() + 8);
    for (char C : Name) {
      if (C == '.')      Out.append(DotUTF8);
      else if (C == '/') Out.append(SlashUTF8);
      else               Out.push_back(C);
    }
    return Out;
  }

  // Path (b): sanitise to a Plan 9-legal current-pkg local name.
  if (HasIllegal) {
    std::string Sanit = Name.str();
    sanitiseToIdent(Sanit);
    return std::string(DotUTF8) + Sanit;
  }

  size_t Last = StringRef::npos;
  for (size_t I = 0, E = Name.size(); I != E; ++I)
    if (Name[I] == '.')
      Last = I;
  if (Last == StringRef::npos) {
    std::string Out = Name.str();
    sanitiseToIdent(Out);
    return std::string(DotUTF8) + Out;
  }
  std::string Path(Name.begin(), Name.begin() + Last);
  sanitiseToIdent(Path);
  std::string Tail(Name.begin() + Last + 1, Name.end());
  sanitiseToIdent(Tail);
  return Path + DotUTF8 + Tail;
}

// c2go #298 Track AN.2: keep this predicate EXACTLY in sync with the
// local-label test in `goSymToPlan9` above and the definition-side
// `MCPlan9AsmStreamer::symbolToPlan9` (lib/MC/MCPlan9AsmStreamer.cpp:671)
// — only the Mach-O/ELF private forms `L*`, `.L*`, `l_*` and `l<UPPER>*`
// are local. The pre-AN.2 broad test (`Name[0] == 'L' || 'l'`) classified
// ordinary static C globals whose names start with a lowercase `l`
// (SQLite's `likeInfoNorm` / `likeInfoAlt` / `likeFunc` / `leadName` /
// `lagName`) as local labels, so `printPlan9MemRef` rendered the code
// reference as `LEAQ likeInfoNorm(SB), DX` (no `·`) while the streamer
// emitted the definition as `GLOBL ·likeInfoNorm(SB)` — the Go linker
// saw two distinct symbols and failed with `relocation target
// likeInfoNorm not defined`. Same #276 bug class the streamer comment
// ("Keep both in sync") already reified on the definition side.
// (AArch64's broad `isLocalLabelName` is branch-target-only — its data
// refs always route through `goSymToPlan9` — so it is unaffected.)
static bool isLocalLabelName(StringRef Name) {
  // #654b: shared predicate (MCPlan9AsmStreamer.h) — keeps static C symbols
  // like `l_alloc` / `LTnum` (Lua) out of the local class on data refs too.
  return isPlan9CompilerLocalSym(Name);
}

static std::string sanitizeLocalLabel(StringRef Name) {
  std::string Out = Name.str();
  for (char &C : Out)
    if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
          (C >= '0' && C <= '9') || C == '_'))
      C = '_';
  return Out;
}

// #586: a local symbol used as an (SB) DATA reference (string literal / constant
// pool) must carry the file-local `<>` scope the streamer emits on its
// definition (`name<>(SB)`), or two separately-compiled c2go packages collide
// on a plain global `_L_str` at Go link time. Branch targets (JMP/Jcc, no
// `(SB)`) keep using sanitizeLocalLabel directly.
static std::string sanitizeLocalDataRef(StringRef Name) {
  return sanitizeLocalLabel(Name) + "<>";
}

// Lookup a Plan 9 register name from the AT&T-style enum-derived name.
// AT&T `getRegisterName` returns a leading `%` (e.g. `%rax`); strip and
// linear-scan `kPlan9RegMap` (small table — 100-ish entries). Returns
// nullptr on miss.
static const char *plan9RegName(StringRef AttName) {
  if (AttName.starts_with("%"))
    AttName = AttName.drop_front();
  for (const auto &E : X86Plan9::kPlan9RegMap) {
    if (AttName.equals_insensitive(E.Att))
      return E.Plan9;
  }
  return nullptr;
}

// `plan9Mnemonic` (AT&T -> Plan-9 string-table lookup) is intentionally
// not used by this drop — each opcode family handler maps directly via
// per-family switches for clarity. Wave Z opcodes covering the bulk of
// the ISA will introduce a table-driven dispatch and re-link Track B's
// `kPlan9MnemonicMap` then.

} // namespace

void X86Plan9InstPrinter::printRegName(raw_ostream &OS, MCRegister Reg) {
  // X86 tablegen-emitted register names are the bare AT&T spelling
  // *without* the leading `%` (the `%` is added by the InstPrinter at
  // operand-print time, not by getName). Look up directly in the
  // Plan-9 register table; on miss fall back to the bare AT&T spelling
  // — that produces a syntactically wrong line (won't load in
  // `go tool asm`) but is preferable to silently dropping the operand.
  StringRef Name = MRI.getName(Reg);
  if (Name.starts_with("%"))
    Name = Name.drop_front();
  if (const char *P9 = plan9RegName(Name)) {
    OS << P9;
    return;
  }
  OS << Name;
}

// Plan 9 textual rendering of a single MCOperand. Handles register,
// immediate and symbolic-expression operands. Memory operands are
// composite (5 sub-operands per X86 addressing mode) — those are
// printed by `printMemRef` below, not here.
//
// `ForceSymPrefix` selects between `LEAQ $·sym(SB), AX` (address-of,
// true) and `MOVQ ·sym(SB), AX` (value load, false).
static void printPlan9Reg(raw_ostream &OS, MCRegister Reg,
                          const MCRegisterInfo &MRI) {
  StringRef AttName = MRI.getName(Reg);
  if (const char *P9 = plan9RegName(AttName)) {
    OS << P9;
    return;
  }
  OS << AttName;
}

static void printPlan9Imm(raw_ostream &OS, int64_t Imm) {
  OS << "$" << Imm;
}

// X86 memory addressing on a 5-operand sub-tuple
// {BaseReg, ScaleAmt, IndexReg, Disp, Segment}:
//   * `disp(BASE)`                — base only
//   * `disp(BASE)(IDX*scale)`     — base + indexed
//   * `disp(,IDX*scale)`          — index only (rare)
//   * `·sym+N(SB)`                — symbolic LEA / MOV operand
// In Plan 9 syntax base/index live in *two* parenthesised groups
// (note: NOT comma-separated like AT&T) — see Track B header §3.
static void printPlan9MemRef(raw_ostream &OS, const MCInst *MI,
                             unsigned OpStart, const MCRegisterInfo &MRI) {
  const MCOperand &Base    = MI->getOperand(OpStart + 0);
  const MCOperand &Scale   = MI->getOperand(OpStart + 1);
  const MCOperand &Index   = MI->getOperand(OpStart + 2);
  const MCOperand &Disp    = MI->getOperand(OpStart + 3);
  // Segment is OpStart+4 — we don't model segment overrides yet.

  bool HasBase  = Base.isReg()  && Base.getReg().isValid();
  bool HasIndex = Index.isReg() && Index.getReg().isValid();

  // Symbolic displacement (e.g. `·sym(SB)` or `·sym+N(SB)`).
  if (Disp.isExpr()) {
    StringRef Sym = getReferencedSymbolName(Disp.getExpr());
    int64_t Off = getReferencedSymbolOffset(Disp.getExpr());
    if (!Sym.empty()) {
      // c2go #298 Wave AG.2: Plan-9 mem-ref symbolic disps are *always*
      // SB-relative — local labels (Mach-O `l_.str.<n>`, `LBB*`, `LCPI*`,
      // `.L*`) and global externs share the same `sym[+N](SB)` grammar
      // in `go tool asm`. The streamer side renders the label definition
      // via `MCPlan9AsmStreamer::symbolToPlan9` (lib/MC/MCPlan9AsmStreamer.cpp
      // :585) which maps `l_.str.42` → `l__str_42:` (all non-ident chars
      // → `_`); `sanitizeLocalLabel` mirrors that mapping for refs. The
      // pre-AG.2 code printed the bare sanitized label and `return`ed
      // before any `(SB)` or `+N` was emitted, so the resulting line
      //   `LEAQ l__str_0, AX`
      // hit `go tool asm: illegal addressing mode` — string-literal LEAs
      // produced by `-fc2go-emit-plan9-asm` against any TU with a string
      // constant (e.g. SQLite's `sqlite3_libversion()`) failed the .s
      // load. Mirrors the AArch64 fallback path (no isLocalLabel branch:
      // AArch64Plan9InstPrinter.cpp:580 always emits `goSymToPlan9 + Off
      // + (SB)`), reified per CLAUDE.md §3 cross-arch symmetry.
      if (isLocalLabelName(Sym))
        OS << sanitizeLocalDataRef(Sym); // #586: file-local (SB) data ref
      else
        OS << goSymToPlan9(Sym);
      if (Off > 0) OS << "+" << Off;
      else if (Off < 0) OS << Off;
      OS << "(SB)";
      // Symbolic accesses (RIP-relative on x86_64) usually do NOT
      // also carry a (BASE)(IDX*scale) tail; skip the rest.
      return;
    }
    // Fall through with Off = displacement-only.
    if (Off != 0) OS << Off;
  } else if (Disp.isImm()) {
    int64_t D = Disp.getImm();
    // Go runtime asm uses canonical `0(SP)` even when disp is zero — emit
    // the literal whenever a base/index follows.  Pure absolute addressing
    // (no base, no index) still suppresses a zero disp.
    if (D != 0 || HasBase || HasIndex) OS << D;
  }

  if (HasBase) {
    OS << "(";
    printPlan9Reg(OS, Base.getReg(), MRI);
    OS << ")";
  }
  if (HasIndex) {
    OS << "(";
    printPlan9Reg(OS, Index.getReg(), MRI);
    int64_t S = Scale.isImm() ? Scale.getImm() : 1;
    OS << "*" << S << ")";
  }
}

// ===== tryPrint* dispatchers =====

bool X86Plan9InstPrinter::tryPrintRet(const MCInst *MI, raw_ostream &O) {
  StringRef Mn = MII.getName(MI->getOpcode());
  if (Mn != "RETQ" && Mn != "RET64" && Mn != "RET" && Mn != "RETL" &&
      Mn != "RETW" && Mn != "RET32")
    return false;
  O << "\tRET\n";
  return true;
}

bool X86Plan9InstPrinter::tryPrintBranchCall(const MCInst *MI,
                                              raw_ostream &O) {
  StringRef Mn = MII.getName(MI->getOpcode());
  // Direct PC-relative CALL forms.
  if (Mn != "CALL64pcrel32" && Mn != "CALLpcrel32" && Mn != "CALL64pcrel32_RVMARKER")
    return false;
  if (MI->getNumOperands() < 1 || !MI->getOperand(0).isExpr())
    return false;
  StringRef Sym = getReferencedSymbolName(MI->getOperand(0).getExpr());
  if (Sym.empty()) return false;
  O << "\tCALL " << goSymToPlan9(Sym) << "(SB)\n";
  return true;
}

bool X86Plan9InstPrinter::tryPrintIndirectCall(const MCInst *MI,
                                                raw_ostream &O) {
  StringRef Mn = MII.getName(MI->getOpcode());
  // Indirect register / memory CALL.
  if (Mn != "CALL64r" && Mn != "CALL32r" && Mn != "CALL16r" &&
      Mn != "CALL64m" && Mn != "CALL32m" && Mn != "CALL16m")
    return false;
  if (MI->getNumOperands() < 1) return false;
  if (Mn == "CALL64r" || Mn == "CALL32r" || Mn == "CALL16r") {
    if (!MI->getOperand(0).isReg()) return false;
    O << "\tCALL ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << "\n";
    return true;
  }
  // Memory-indirect call (CALL64m / CALL32m / CALL16m): mem operand is the
  // 5-tuple {Base, Scale, Idx, Disp, Seg} starting at op 0. Mirror Wave Y C6
  // / AE.2 — refuse early if the MCInst doesn't carry the 5 operands so
  // printPlan9MemRef doesn't OOB on a malformed inst (or one whose operand
  // list was lowered/stripped before reaching us).
  if (MI->getNumOperands() < 5) return false;
  O << "\tCALL ";
  printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
  O << "\n";
  return true;
}

bool X86Plan9InstPrinter::tryPrintUnconditionalBranch(const MCInst *MI,
                                                      raw_ostream &O) {
  StringRef Mn = MII.getName(MI->getOpcode());
  if (Mn != "JMP_1" && Mn != "JMP_2" && Mn != "JMP_4" &&
      Mn != "JMP64r" && Mn != "JMP64m")
    return false;
  if (Mn == "JMP_1" || Mn == "JMP_2" || Mn == "JMP_4") {
    if (MI->getNumOperands() < 1 || !MI->getOperand(0).isExpr())
      return false;
    StringRef Sym = getReferencedSymbolName(MI->getOperand(0).getExpr());
    if (Sym.empty()) return false;
    if (isLocalLabelName(Sym))
      O << "\tJMP " << sanitizeLocalLabel(Sym) << "\n";
    else
      O << "\tJMP " << goSymToPlan9(Sym) << "(SB)\n";
    return true;
  }
  if (Mn == "JMP64r") {
    // Wave AF.3 audit: guard before getOperand(0) — the JMP_1/2/4 branch
    // above hard-checks `< 1`, but JMP64r reached the dispatch without one
    // (only `Mn ==` narrowing). Mirror Wave Y C6 / AE.2.
    if (MI->getNumOperands() < 1 || !MI->getOperand(0).isReg()) return false;
    O << "\tJMP ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << "\n";
    return true;
  }
  // JMP64m — mem operand 5-tuple {Base, Scale, Idx, Disp, Seg} at op 0.
  // Wave AF.3 audit: refuse if the MCInst doesn't carry the 5 operands
  // (printPlan9MemRef would otherwise OOB on getOperand(0..3)).
  if (MI->getNumOperands() < 5) return false;
  O << "\tJMP ";
  printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
  O << "\n";
  return true;
}

// Map X86 condition codes (X86::CondCode enum) → Plan 9 J<cc> mnemonics.
// Mirrors `kPlan9CondSuffix` documented in Track B.
static const char *plan9JCC(unsigned CC) {
  switch (CC) {
  case X86::COND_O:  return "JOS";
  case X86::COND_NO: return "JOC";
  case X86::COND_B:  return "JCS";  // unsigned <
  case X86::COND_AE: return "JCC";  // unsigned >=
  case X86::COND_E:  return "JEQ";
  case X86::COND_NE: return "JNE";
  case X86::COND_BE: return "JLS";
  case X86::COND_A:  return "JHI";
  case X86::COND_S:  return "JMI";
  case X86::COND_NS: return "JPL";
  case X86::COND_P:  return "JPS";
  case X86::COND_NP: return "JPC";
  case X86::COND_L:  return "JLT";
  case X86::COND_GE: return "JGE";
  case X86::COND_LE: return "JLE";
  case X86::COND_G:  return "JGT";
  default: return nullptr;
  }
}

bool X86Plan9InstPrinter::tryPrintConditionalBranch(const MCInst *MI,
                                                     raw_ostream &O) {
  StringRef Mn = MII.getName(MI->getOpcode());
  if (Mn != "JCC_1" && Mn != "JCC_2" && Mn != "JCC_4")
    return false;
  if (MI->getNumOperands() < 2 || !MI->getOperand(0).isExpr() ||
      !MI->getOperand(1).isImm())
    return false;
  const char *J = plan9JCC((unsigned)MI->getOperand(1).getImm());
  if (!J) return false;
  StringRef Sym = getReferencedSymbolName(MI->getOperand(0).getExpr());
  if (Sym.empty()) return false;
  if (isLocalLabelName(Sym))
    O << "\t" << J << " " << sanitizeLocalLabel(Sym) << "\n";
  else
    O << "\t" << J << " " << goSymToPlan9(Sym) << "(SB)\n";
  return true;
}

bool X86Plan9InstPrinter::tryPrintSPAdjust(const MCInst *MI, raw_ostream &O) {
  // Guard before any getOperand() access: 0-operand opcodes (e.g. ENDBR64
  // emitted by -fcf-protection=branch) would otherwise OOB.
  if (MI->getNumOperands() < 3) return false;
  unsigned Op = MI->getOpcode();
  // Only handle the rr / ri forms whose destination and source[0] are RSP.
  // ADD64ri{8,32} / SUB64ri{8,32} / ADD64ri / SUB64ri / ADD32ri / SUB32ri
  StringRef Nm = MII.getName(Op);
  bool IsAdd = Nm.starts_with("ADD64ri") || Nm.starts_with("ADD32ri") ||
               Nm.starts_with("ADD16ri") || Nm.starts_with("ADD64rr") ||
               Nm.starts_with("ADD32rr");
  bool IsSub = Nm.starts_with("SUB64ri") || Nm.starts_with("SUB32ri") ||
               Nm.starts_with("SUB16ri") || Nm.starts_with("SUB64rr") ||
               Nm.starts_with("SUB32rr");
  if (!IsAdd && !IsSub) return false;
  if (!MI->getOperand(0).isReg() || !MI->getOperand(1).isReg()) return false;
  MCRegister Rd = MI->getOperand(0).getReg();
  MCRegister Rn = MI->getOperand(1).getReg();
  if (!(Rd == X86::RSP || Rd == X86::ESP)) return false;
  if (!(Rn == X86::RSP || Rn == X86::ESP)) return false;
  const char *MnP9 = nullptr;
  if (Nm.starts_with("ADD64")) MnP9 = "ADDQ";
  else if (Nm.starts_with("SUB64")) MnP9 = "SUBQ";
  else if (Nm.starts_with("ADD32")) MnP9 = "ADDL";
  else if (Nm.starts_with("SUB32")) MnP9 = "SUBL";
  else if (Nm.starts_with("ADD16")) MnP9 = "ADDW";
  else if (Nm.starts_with("SUB16")) MnP9 = "SUBW";
  if (!MnP9) return false;
  // ri-form: src is imm; rr-form: src is reg.
  O << "\t" << MnP9 << " ";
  const MCOperand &Src = MI->getOperand(2);
  if (Src.isImm()) {
    printPlan9Imm(O, Src.getImm());
  } else if (Src.isReg()) {
    printPlan9Reg(O, Src.getReg(), MRI);
  } else {
    return false;
  }
  O << ", ";
  printPlan9Reg(O, Rd, MRI);
  O << "\n";
  return true;
}

// Find the size suffix character for a width in bits.
static char suffixForBits(unsigned Bits) {
  switch (Bits) {
  case 8:  return 'B';
  case 16: return 'W';
  case 32: return 'L';
  case 64: return 'Q';
  default: return 0;
  }
}

// MOV<width> imm → reg. Covers MOV{8,16,32,64}ri / MOV64ri32 forms.
bool X86Plan9InstPrinter::tryPrintMovImm(const MCInst *MI, raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  StringRef Nm = MII.getName(Op);
  // Match shape: MOV<bits>ri / MOV<bits>ri8 / MOV64ri32.
  if (!(Nm == "MOV8ri" || Nm == "MOV16ri" || Nm == "MOV32ri" ||
        Nm == "MOV64ri" || Nm == "MOV64ri32"))
    return false;
  if (MI->getNumOperands() < 2 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isImm())
    return false;
  const char *MnP9 = nullptr;
  if (Nm == "MOV8ri")  MnP9 = "MOVB";
  else if (Nm == "MOV16ri") MnP9 = "MOVW";
  else if (Nm == "MOV32ri") MnP9 = "MOVL";
  else                       MnP9 = "MOVQ"; // 64-bit forms
  O << "\t" << MnP9 << " ";
  printPlan9Imm(O, MI->getOperand(1).getImm());
  O << ", ";
  printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
  O << "\n";
  return true;
}

// MOV<width> reg, reg.
static bool tryPrintMovReg(const MCInst *MI, raw_ostream &O,
                            const MCRegisterInfo &MRI,
                            const MCInstrInfo &MII) {
  unsigned Op = MI->getOpcode();
  StringRef Nm = MII.getName(Op);
  const char *MnP9 = nullptr;
  if (Nm == "MOV8rr")       MnP9 = "MOVB";
  else if (Nm == "MOV16rr") MnP9 = "MOVW";
  else if (Nm == "MOV32rr") MnP9 = "MOVL";
  else if (Nm == "MOV64rr") MnP9 = "MOVQ";
  if (!MnP9) return false;
  if (MI->getNumOperands() < 2 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg())
    return false;
  O << "\t" << MnP9 << " ";
  printPlan9Reg(O, MI->getOperand(1).getReg(), MRI);
  O << ", ";
  printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
  O << "\n";
  return true;
}

// MOV<width> mem(SP|GPR), reg  and reg, mem(SP|GPR). Covers the rm/mr
// scaled-offset memory load/store forms.
bool X86Plan9InstPrinter::tryPrintSPMemImm(const MCInst *MI, raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  StringRef Nm = MII.getName(Op);
  // mr (store): operands {mem[5], src reg}.
  // rm (load):  operands {dst reg, mem[5]}.
  const char *MnP9 = nullptr;
  bool IsLoad = false;
  if (Nm == "MOV8mr" || Nm == "MOV16mr" || Nm == "MOV32mr" ||
      Nm == "MOV64mr") {
    IsLoad = false;
    if (Nm == "MOV8mr") MnP9 = "MOVB";
    else if (Nm == "MOV16mr") MnP9 = "MOVW";
    else if (Nm == "MOV32mr") MnP9 = "MOVL";
    else MnP9 = "MOVQ";
  } else if (Nm == "MOV8rm" || Nm == "MOV16rm" || Nm == "MOV32rm" ||
             Nm == "MOV64rm") {
    IsLoad = true;
    if (Nm == "MOV8rm") MnP9 = "MOVB";
    else if (Nm == "MOV16rm") MnP9 = "MOVW";
    else if (Nm == "MOV32rm") MnP9 = "MOVL";
    else MnP9 = "MOVQ";
  } else {
    return false;
  }
  if (IsLoad) {
    // {Rd, Base, Scale, Idx, Disp, Seg}
    if (MI->getNumOperands() < 6 || !MI->getOperand(0).isReg()) return false;
    O << "\t" << MnP9 << " ";
    printPlan9MemRef(O, MI, /*OpStart=*/1, MRI);
    O << ", ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << "\n";
  } else {
    // {Base, Scale, Idx, Disp, Seg, Rs}
    if (MI->getNumOperands() < 6 || !MI->getOperand(5).isReg()) return false;
    O << "\t" << MnP9 << " ";
    printPlan9Reg(O, MI->getOperand(5).getReg(), MRI);
    O << ", ";
    printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
    O << "\n";
  }
  return true;
}

// c2go #298 Track AF.1: rewrite a symbolic LEA64r / MOV64rm whose
// displacement references `c2go.typeinfo.<X>` or `type:<pkg>.<X>` into
//
//     MOVQ ·_typeinfo_<X>(SB), Rd
//
// — load the value (the *runtime._type pointer) of c2gobind's per-type
// indirection var `_typeinfo_<X> unsafe.Pointer`. Mirrors AArch64's
// post-ADRP+ADD rewrite (AArch64Plan9InstPrinter.cpp:518/543); see also
// CGExpr.cpp:3267 for the clang-side contract that emits the LValue as
// a global-var address (later loaded by EmitLoadOfPointer) — the .s
// side must perform the load too, even when the original IR was an LEA
// form, because the c2gobind indirection holds the *_type pointer.
//
// Two source shapes:
//   * `c2go.typeinfo.<X>`  — C-owner struct (CGC2GoTypeInfo emits the
//     descriptor locally; c2gobind generates the indirection var).
//   * `type:<pkg>.<X>`     — Go-owner case (c2go_linkname). Same target
//     symbol on the Go side: `·_typeinfo_<X>(SB)`.
//
// Off (symbol+N) is supported defensively — codegen rarely emits a
// non-zero offset against a typeinfo descriptor, but if it does the
// trailing `ADDQ $N, Rd` keeps semantics aligned with the AArch64
// path. The rewrite is single-instruction (no register-state to
// retain) so it can sit ahead of `tryPrintLEASymbolic` /
// `tryPrintSPMemImm` without disturbing other symbolic dispatches.
bool X86Plan9InstPrinter::tryPrintTypeinfoLoad(const MCInst *MI,
                                                raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  StringRef Nm = MII.getName(Op);
  // Accept the 64-bit symbolic LEA and the 64-bit memory load — both
  // forms can carry an `@c2go.typeinfo.<X>` symref on x86_64 (RIP-rel).
  // Narrower widths are not used for typeinfo pointers (always 8B).
  if (Nm != "LEA64r" && Nm != "MOV64rm")
    return false;
  // Operand shape for both: {Rd, Base, Scale, Idx, Disp, Seg}.
  if (MI->getNumOperands() < 6) return false;
  if (!MI->getOperand(0).isReg()) return false;
  const MCOperand &Disp = MI->getOperand(4);
  if (!Disp.isExpr()) return false;
  StringRef Sym = getReferencedSymbolName(Disp.getExpr());
  if (Sym.empty()) return false;
  std::string Name;
  if (Sym.consume_front("c2go.typeinfo.")) {
    // c2gobind sanitises anon-record names by flattening `.` → `_`
    // (Go identifiers cannot contain `.`). Mirror AArch64
    // (AArch64Plan9InstPrinter.cpp:566-569) — the flatten is *conditional*
    // on the remainder still starting with `c2go.` (the anon-record
    // shape `c2go.anon.<hash>` produced by CGC2GoTypeInfo). Plain
    // C-owner names that happen to carry a `.` (e.g. mangled tail
    // forms from non-anon producers) must NOT be flattened, otherwise
    // the rewrite would diverge from AArch64 and the Go linker would
    // fail to resolve `·_typeinfo_<X>(SB)`. Cross-arch byte-for-byte
    // symmetry is a hard invariant of the Plan-9 emit path.
    Name = Sym.str();
    if (StringRef(Name).starts_with("c2go.")) {
      for (char &C : Name) if (C == '.') C = '_';
    }
  } else if (Sym.consume_front("type:")) {
    // §A2 Go-owner case: tail after the last `.` is the bare type name
    // (the prefix is `<pkg>.<X>`). c2gobind emits the indirection var
    // under the local name `_typeinfo_<X>` in the current package.
    std::string Pkg = Sym.str();
    Name = Pkg;
    auto Dot = Pkg.rfind('.');
    if (Dot != std::string::npos)
      Name = Pkg.substr(Dot + 1);
  } else {
    return false;
  }
  int64_t Off = getReferencedSymbolOffset(Disp.getExpr());
  MCRegister Rd = MI->getOperand(0).getReg();
  O << "\tMOVQ \xc2\xb7_typeinfo_" << Name << "(SB), ";
  printPlan9Reg(O, Rd, MRI);
  O << "\n";
  if (Off) {
    O << "\tADDQ $" << Off << ", ";
    printPlan9Reg(O, Rd, MRI);
    O << "\n";
  }
  return true;
}

// Walk an MCExpr possibly wrapped in MCBinary/MCUnary/MCSpecifier layers
// and report whether the underlying MCSymbolRefExpr carries the
// `@GOTPCREL` (or `@GOTPCREL_NORELAX`) specifier — the marker X86
// MCInstLower attaches when ELF small-PIC lowering of a `load <T>, ptr
// @global` chose the two-step GOT-indirect sequence. Returns the
// `MCSymbolRefExpr*` on hit (so the caller can pull its bare name
// through `getReferencedSymbolName`) and nullptr on miss. See
// X86MCInstLower.cpp:298 (`X86II::MO_GOTPCREL → X86::S_GOTPCREL`) for
// the publishing side.
static const MCSymbolRefExpr *
findGOTPCRELSymRef(const MCExpr *E) {
  while (E) {
    switch (E->getKind()) {
    case MCExpr::Constant:
      return nullptr;
    case MCExpr::SymbolRef: {
      const auto *S = cast<MCSymbolRefExpr>(E);
      uint16_t Spec = S->getSpecifier();
      if (Spec == X86::S_GOTPCREL || Spec == X86::S_GOTPCREL_NORELAX)
        return S;
      return nullptr;
    }
    case MCExpr::Unary:
      E = cast<MCUnaryExpr>(E)->getSubExpr();
      break;
    case MCExpr::Binary: {
      // Defensive: the GOTPCREL form rarely carries a constant offset
      // (and the ELF assembler rejects @GOTPCREL+N), but walk the LHS
      // anyway so a stray `+0` binary wrapper does not hide the symref.
      const auto *B = cast<MCBinaryExpr>(E);
      if (B->getOpcode() == MCBinaryExpr::Add ||
          B->getOpcode() == MCBinaryExpr::Sub) {
        E = B->getLHS();
        break;
      }
      return nullptr;
    }
    case MCExpr::Target:
    case MCExpr::Specifier:
      E = cast<MCSpecifierExpr>(E)->getSubExpr();
      break;
    }
  }
  return nullptr;
}

// c2go #298 Track AL.2 — rewrite `MOV64rm $rip, 1, $noreg, @sym@GOTPCREL,
// $noreg` (ELF small-PIC two-step external-global LOAD, first step)
// into the Plan-9 equivalent `LEAQ ·sym(SB), Rd`. The mirror of
// AArch64's `ADRP+ADD → MOVD $·sym(SB), Rd` immediate-LEA form.
//
// IMPORTANT — what this does NOT rewrite:
//   * `c2go.typeinfo.<X>` / `type:<pkg>.<X>` displacements are already
//     intercepted by `tryPrintTypeinfoLoad` (which fires BEFORE this
//     helper). Those keep their `MOVQ ·_typeinfo_<X>(SB), Rd` value
//     load — the typeinfo VAR itself stores the *runtime._type pointer
//     so a single MOVQ load IS correct for that case.
//   * Symbolic LEA (`LEA64r` with a symref disp) — already correct
//     because LEA is an address-of by construction; handled by
//     `tryPrintLEASymbolic`.
//
// Without this rewrite the X86 .s prints `MOVQ runtime·writeBarrier(SB),
// Rd` (a value LOAD in Plan-9 semantics) — Go's assembler/linker has no
// `@GOTPCREL` and would load 8 bytes from `runtime.writeBarrier`'s
// address, which is a 4-byte i32 flag followed by padding. The follow-up
// real-width MOV<W>rm then dereferences that misinterpreted value →
// nil-deref or wild pointer (Wave AK BLOCKER-3 — `runtime.writeBarrier`
// is the canonical case; identical pattern for any external global
// access at -O0).
bool X86Plan9InstPrinter::tryPrintGOTPCRELLoadAsLEA(const MCInst *MI,
                                                     raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  StringRef Nm = MII.getName(Op);
  // The GOTPCREL two-step lowering is always MOV64rm — the 8-byte GOT
  // slot load, regardless of the real-width follow-up MOV<W>rm.
  if (Nm != "MOV64rm")
    return false;
  // Operand shape: {Rd, Base, Scale, Idx, Disp, Seg}. RIP-relative
  // (Base==RIP, Idx==noreg, Scale==1) is the canonical ELF small-PIC
  // GOTPCREL form X86MCInstLower emits.
  if (MI->getNumOperands() < 6) return false;
  if (!MI->getOperand(0).isReg()) return false;
  const MCOperand &Base = MI->getOperand(1);
  const MCOperand &Idx  = MI->getOperand(3);
  const MCOperand &Disp = MI->getOperand(4);
  if (!Base.isReg() || Base.getReg() != X86::RIP) return false;
  if (!Idx.isReg()  || Idx.getReg().isValid())    return false;
  if (!Disp.isExpr()) return false;
  if (!findGOTPCRELSymRef(Disp.getExpr())) return false;
  StringRef Sym = getReferencedSymbolName(Disp.getExpr());
  if (Sym.empty()) return false;
  // Emit `LEAQ ·sym(SB), Rd` — immediate LEA of the symbol's address.
  // The follow-up MOV<W>rm reads from `0(Rd)` with the now-correct base.
  std::string Plan9 = isLocalLabelName(Sym) ? sanitizeLocalDataRef(Sym)
                                            : goSymToPlan9(Sym);
  O << "\tLEAQ " << Plan9 << "(SB), ";
  printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
  O << "\n";
  return true;
}

// LEA<width> mem -> reg. Symbolic-LEA path emits `LEAQ ·sym(SB), AX`.
bool X86Plan9InstPrinter::tryPrintLEASymbolic(const MCInst *MI,
                                               raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  StringRef Nm = MII.getName(Op);
  const char *MnP9 = nullptr;
  if (Nm == "LEA64r")   MnP9 = "LEAQ";
  else if (Nm == "LEA32r")   MnP9 = "LEAL";
  else if (Nm == "LEA64_32r") MnP9 = "LEAQ"; // 32→64 zero-ext variant
  else if (Nm == "LEA16r")    MnP9 = "LEAW";
  else return false;
  // {Rd, Base, Scale, Idx, Disp, Seg}
  if (MI->getNumOperands() < 6 || !MI->getOperand(0).isReg()) return false;
  O << "\t" << MnP9 << " ";
  printPlan9MemRef(O, MI, /*OpStart=*/1, MRI);
  O << ", ";
  printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
  O << "\n";
  return true;
}

// Pass-through: tryPrintMOVSymbolic is folded into tryPrintSPMemImm — a
// memory MOV whose disp operand is an MCExpr will render with the
// symbolic form via `printPlan9MemRef`. Kept as a stub so the header
// API stays accurate.
bool X86Plan9InstPrinter::tryPrintMOVSymbolic(const MCInst *MI,
                                               raw_ostream &O) {
  (void)MI; (void)O;
  return false;
}

// Map an arithmetic / logical AT&T opcode-name root to its Plan-9 form.
// Returns nullptr on miss.
static const char *plan9ArithMnemonic(StringRef Nm, char SizeChar) {
  // Strip `<size>rr` / `<size>ri` / `<size>ri8` etc. suffix to get root.
  // Cheap: examine first 3-4 letters.
  struct R { const char *Root; const char *MnPrefix; };
  static const R kRoots[] = {
    {"ADD", "ADD"}, {"SUB", "SUB"}, {"AND", "AND"}, {"OR",  "OR"},
    {"XOR", "XOR"}, {"CMP", "CMP"}, {"TEST","TEST"},{"IMUL","IMUL"},
    {"SHL", "SHL"}, {"SHR", "SHR"}, {"SAR", "SAR"},
  };
  for (auto &E : kRoots) {
    size_t RootLen = std::strlen(E.Root);
    if (Nm.size() > RootLen && Nm.starts_with(E.Root) &&
        std::isdigit((unsigned char)Nm[RootLen])) {
      static thread_local char Buf[8];
      std::snprintf(Buf, sizeof(Buf), "%s%c", E.MnPrefix, SizeChar);
      return Buf;
    }
  }
  return nullptr;
}

// rr / ri arithmetic — `ADDQ src, dst`, `SUBQ`, ...
bool X86Plan9InstPrinter::tryPrintArithReg(const MCInst *MI, raw_ostream &O) {
  // Hard precondition for the rr/ri/ri8 bodies below: every accepted shape
  // touches MI->getOperand(2). The per-branch `< (NumDefs + 2)` checks
  // (lines ~687/702) only narrow when NumDefs >= 1; for opcodes whose
  // InstDesc reports NumDefs == 0 (or any MCInst whose operand list has
  // been simplified) the < 2 expression still admits MCInsts with only
  // 0-2 operands, then operand(2) OOBs SmallVector. Mirror the Wave Y C6
  // fix in tryPrintSPAdjust (line 455): refuse early when fewer than 3
  // operands are present. Byte-identical for the common >= 3 case.
  if (MI->getNumOperands() < 3) return false;
  unsigned Op = MI->getOpcode();
  StringRef Nm = MII.getName(Op);
  // Extract width — char immediately after the alphabetic root.
  size_t RootEnd = 0;
  while (RootEnd < Nm.size() && std::isalpha((unsigned char)Nm[RootEnd]))
    ++RootEnd;
  if (RootEnd == 0 || RootEnd >= Nm.size()) return false;
  unsigned Bits = 0;
  // Parse the digit suffix (8/16/32/64).
  size_t I = RootEnd;
  while (I < Nm.size() && std::isdigit((unsigned char)Nm[I])) {
    Bits = Bits * 10 + (Nm[I] - '0');
    ++I;
  }
  char SizeC = suffixForBits(Bits);
  if (!SizeC) return false;
  StringRef Form = Nm.substr(I); // e.g. "rr", "ri", "ri8", "rr_REV"
  // We handle the most common shapes here:
  //   rr           — {Rd, Rd, Rs}                  (X86 3-addr form)
  //   ri / ri8     — {Rd, Rd, Imm}
  //   rm           — defer (memory operand → falls to tryPrintSPMemImm)
  bool ThreeAddr = false;
  // Most X86 ALU rr's have form {DestIn, DestOut, Src}; the printer
  // really only sees {Dest, Src} in 2-addr form for some opcodes. We
  // detect by checking the InstDesc operand count.
  const MCInstrDesc &Desc = MII.get(Op);
  unsigned NumDefs = Desc.getNumDefs();
  if (Form == "rr" || Form == "rr_REV") {
    // {Dest, Src1, Src2} 3-addr — we want Src2 (the non-tied operand)
    // → Dest, but most ALU forms in X86 are 2-addr. Detect via TIED
    // constraint heuristic: src1 == dest typically.
    if (MI->getNumOperands() < (NumDefs + 2)) return false;
    if (!MI->getOperand(0).isReg() ||
        !MI->getOperand(1).isReg() ||
        !MI->getOperand(2).isReg())
      return false;
    const char *MnP9 = plan9ArithMnemonic(Nm, SizeC);
    if (!MnP9) return false;
    O << "\t" << MnP9 << " ";
    printPlan9Reg(O, MI->getOperand(2).getReg(), MRI);
    O << ", ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << "\n";
    return true;
  }
  if (Form == "ri" || Form == "ri8" || Form == "ri32" || Form == "ri8_REV") {
    if (MI->getNumOperands() < (NumDefs + 2)) return false;
    if (!MI->getOperand(0).isReg() || !MI->getOperand(2).isImm())
      return false;
    const char *MnP9 = plan9ArithMnemonic(Nm, SizeC);
    if (!MnP9) return false;
    O << "\t" << MnP9 << " ";
    printPlan9Imm(O, MI->getOperand(2).getImm());
    O << ", ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << "\n";
    return true;
  }
  (void)ThreeAddr;
  return false;
}

// Memory-form LD/ST for GPR base (non-SP). Already handled by
// tryPrintSPMemImm — both SP-base and GPR-base go through it because
// `printPlan9MemRef` renders any base register. Stub for header parity.
bool X86Plan9InstPrinter::tryPrintGPRMemImm(const MCInst *MI,
                                             raw_ostream &O) {
  (void)MI; (void)O;
  return false;
}

// SSE / SIMD moves. Covers MOVSS/MOVSD/MOVAPS/MOVUPS/MOVAPD/MOVUPD
// in both rr/rm/mr shapes, plus CVTTSD2SI.
//
// Go's amd64 TEXT entry provides only 8-byte stack alignment.  LLVM may still
// select an aligned 128-bit move for an 8-byte-aligned stack object (for
// example, when lowering a 16-byte memcpy).  A native X86 prologue would
// realign SP, but the c2go Plan 9 path replaces that prologue with a Go frame.
// Therefore aligned moves whose memory base is RSP/RBP are deliberately
// printed with the equivalent unaligned encoding.  Moves through other bases
// keep LLVM's selected spelling, as do register-to-register forms.
bool X86Plan9InstPrinter::tryPrintSSEMov(const MCInst *MI, raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  StringRef Nm = MII.getName(Op);
  struct Map { const char *Root; const char *P9; };
  static const Map kRoots[] = {
      {"MOVSSrr", "MOVSS"},           {"MOVSDrr", "MOVSD"},
      {"MOVSSrm", "MOVSS"},           {"MOVSDrm", "MOVSD"},
      {"MOVSSmr", "MOVSS"},           {"MOVSDmr", "MOVSD"},
      {"MOVAPSrr", "MOVAPS"},         {"MOVAPDrr", "MOVAPD"},
      {"MOVAPSrm", "MOVAPS"},         {"MOVAPDrm", "MOVAPD"},
      {"MOVAPSmr", "MOVAPS"},         {"MOVAPDmr", "MOVAPD"},
      {"MOVUPSrr", "MOVUPS"},         {"MOVUPDrr", "MOVUPD"},
      {"MOVUPSrm", "MOVUPS"},         {"MOVUPDrm", "MOVUPD"},
      {"MOVUPSmr", "MOVUPS"},         {"MOVUPDmr", "MOVUPD"},
      {"MOVDQArm", "MOVO"},           {"MOVDQAmr", "MOVO"},
      {"CVTTSD2SI64rr", "CVTTSD2SQ"}, {"CVTTSD2SIrr", "CVTTSD2SL"},
      {"CVTTSD2SI64rm", "CVTTSD2SQ"}, {"CVTTSD2SIrm", "CVTTSD2SL"},
      {"CVTTSS2SI64rr", "CVTTSS2SQ"}, {"CVTTSS2SIrr", "CVTTSS2SL"},
      {"CVTSI2SD64rr", "CVTSQ2SD"},   {"CVTSI2SDrr", "CVTSL2SD"},
      {"CVTSI2SS64rr", "CVTSQ2SS"},   {"CVTSI2SSrr", "CVTSL2SS"},
      {"ADDSDrr", "ADDSD"},           {"ADDSSrr", "ADDSS"},
      {"SUBSDrr", "SUBSD"},           {"SUBSSrr", "SUBSS"},
      {"MULSDrr", "MULSD"},           {"MULSSrr", "MULSS"},
      {"DIVSDrr", "DIVSD"},           {"DIVSSrr", "DIVSS"},
  };
  const char *MnP9 = nullptr;
  for (auto &E : kRoots) {
    if (Nm == E.Root) { MnP9 = E.P9; break; }
  }
  if (!MnP9) return false;

  auto stackSafeMnemonic = [&](unsigned MemStart) -> const char * {
    if (MI->getNumOperands() <= MemStart || !MI->getOperand(MemStart).isReg())
      return MnP9;
    MCRegister Base = MI->getOperand(MemStart).getReg();
    if (Base != X86::RSP && Base != X86::RBP)
      return MnP9;
    if (Nm.starts_with("MOVAPS"))
      return "MOVUPS";
    if (Nm.starts_with("MOVAPD"))
      return "MOVUPD";
    if (Nm.starts_with("MOVDQA"))
      return "MOVOU";
    return MnP9;
  };

  bool IsRR = Nm.ends_with("rr");
  bool IsRM = Nm.ends_with("rm");
  bool IsMR = Nm.ends_with("mr");
  if (IsRR) {
    // Destination is operand 0; the true source is the LAST operand. A plain
    // move (MOVSDrr) has 2 operands [dst, src], but a 2-address SSE arithmetic
    // op (ADDSD/SUBSD/MULSD/DIVSD, and the tied CVTSI2SD/CVTSI2SS) has 3
    // [dst, src1(tied=dst), src2] — taking operand 1 there prints the tied dst
    // as the source (e.g. `ADDSD X0, X0` for `addsd %xmm1, %xmm0`, computing
    // x+x instead of x+y).
    unsigned N = MI->getNumOperands();
    unsigned SrcIdx = N - 1;
    if (N < 2 || !MI->getOperand(0).isReg() || !MI->getOperand(SrcIdx).isReg())
      return false;
    O << "\t" << MnP9 << " ";
    printPlan9Reg(O, MI->getOperand(SrcIdx).getReg(), MRI);
    O << ", ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << "\n";
    return true;
  }
  if (IsRM) {
    // {Rd, Base, Scale, Idx, Disp, Seg}
    if (MI->getNumOperands() < 6 || !MI->getOperand(0).isReg()) return false;
    O << "\t" << stackSafeMnemonic(/*MemStart=*/1) << " ";
    printPlan9MemRef(O, MI, /*OpStart=*/1, MRI);
    O << ", ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << "\n";
    return true;
  }
  if (IsMR) {
    // {Base, Scale, Idx, Disp, Seg, Rs}
    if (MI->getNumOperands() < 6 || !MI->getOperand(5).isReg()) return false;
    O << "\t" << stackSafeMnemonic(/*MemStart=*/0) << " ";
    printPlan9Reg(O, MI->getOperand(5).getReg(), MRI);
    O << ", ";
    printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
    O << "\n";
    return true;
  }
  return false;
}

// PUSH / POP — single GPR operand. Emitted by the codegen for caller-
// saved spills around calls inside an otherwise leaf function (e.g. a
// flipped `c2goabiinternalcc` caller that needs to preserve a value
// across a `CALL ·callee(SB)` site).
static bool tryPrintPushPop(const MCInst *MI, raw_ostream &O,
                            const MCRegisterInfo &MRI,
                            const MCInstrInfo &MII) {
  unsigned Op = MI->getOpcode();
  StringRef Nm = MII.getName(Op);
  const char *MnP9 = nullptr;
  if (Nm == "PUSH64r")      MnP9 = "PUSHQ";
  else if (Nm == "PUSH32r") MnP9 = "PUSHL";
  else if (Nm == "POP64r")  MnP9 = "POPQ";
  else if (Nm == "POP32r")  MnP9 = "POPL";
  if (!MnP9) return false;
  if (MI->getNumOperands() < 1 || !MI->getOperand(0).isReg())
    return false;
  O << "\t" << MnP9 << " ";
  printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
  O << "\n";
  return true;
}

// Track AN.1 shared gate for the two symbolic mem-direct dispatchers
// below: claim ONLY `sym(SB)`-renderable memory operands — symbolic
// disp with Base ∈ {noreg, RIP} and no index register.
// `printPlan9MemRef` renders a symbolic disp as `·sym[+N](SB)` and
// returns without the (BASE)(IDX*scale) tail; accepting e.g.
// `cmpl %eax, gArr(,%rdi,4)` here would silently drop the index — a
// worse miscompile than the swallowed-instruction bug this track
// fixes. Refusing routes those shapes to the streamer's fail-closed
// guard (report_fatal_error) so the first real producer instance is
// loud. No c2go workload (stress / SQLite amd64 inventory 2026-06-10)
// produces them today.
static bool isPlan9SymbolicMemTuple(const MCInst *MI, unsigned OpStart) {
  const MCOperand &Base = MI->getOperand(OpStart + 0);
  const MCOperand &Idx  = MI->getOperand(OpStart + 2);
  const MCOperand &Disp = MI->getOperand(OpStart + 3);
  const MCOperand &Seg  = MI->getOperand(OpStart + 4);
  if (!Disp.isExpr())
    return false; // non-symbolic: keep raw-byte fallback byte-identical
  if (Base.isReg() && Base.getReg().isValid() && Base.getReg() != X86::RIP)
    return false;
  if (Idx.isReg() && Idx.getReg().isValid())
    return false;
  // Segment override (FS/GS, TLS-style lowering): `printPlan9MemRef`
  // does not model segments (line ~335) and would render plain
  // `·sym(SB)`, silently dropping the segment base — refuse so the
  // streamer's fail-closed guard reports it instead (GPT round-1
  // audit, Wave AN). No c2go workload produces segment-relative
  // symbolic mem today; this keeps the AN.1 fail-closed invariant
  // airtight rather than fixing a live bug.
  if (Seg.isReg() && Seg.getReg().isValid())
    return false;
  return true;
}

// c2go #298 Track AN.1 — symbolic-displacement memory-direct MOV-imm.
// Covers MOV{8,16,32}mi / MOV64mi32: store-immediate straight into a
// RIP-relative global (`gSink[i] = 0`, `gMaxDepth = 0`). Operand shape:
// {Base, Scale, Idx, Disp, Seg, Imm}.
//
// Scope gate: only the *symbolic* (fixup-bearing) form is claimed.
// Non-symbolic mem-imm stores keep the raw-byte fallback byte-identical
// — those encodings are already correct and re-printing them textually
// would churn every existing .s baseline for zero semantic gain.
static bool tryPrintMovImmToMemSymbolic(const MCInst *MI, raw_ostream &O,
                                        const MCRegisterInfo &MRI,
                                        const MCInstrInfo &MII) {
  StringRef Nm = MII.getName(MI->getOpcode());
  const char *MnP9 = nullptr;
  if (Nm == "MOV8mi")         MnP9 = "MOVB";
  else if (Nm == "MOV16mi")   MnP9 = "MOVW";
  else if (Nm == "MOV32mi")   MnP9 = "MOVL";
  else if (Nm == "MOV64mi32") MnP9 = "MOVQ"; // imm32 sign-extends, same as
                                             // Go asm's REX.W C7 /0 encoding
  else return false;
  if (MI->getNumOperands() < 6 || !MI->getOperand(5).isImm())
    return false;
  if (!isPlan9SymbolicMemTuple(MI, /*OpStart=*/0))
    return false;
  O << "\t" << MnP9 << " ";
  printPlan9Imm(O, MI->getOperand(5).getImm());
  O << ", ";
  printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
  O << "\n";
  return true;
}

// c2go #298 Track AN.1 — symbolic-displacement memory-direct integer
// ALU forms: {ADD,SUB,AND,OR,XOR,CMP,IMUL,SHL,SHR,SAR}<bits>{rm,mr,mi,
// mi8,mi32} plus {INC,DEC}<bits>m. These are the X86 lowerings of
// global RMW (`gSink[2] ^= v` → XOR64mr, `gSink[0]++` → INC64m) and
// global compares (`gSink[1] != 0` → CMP64mi8, `n > gMaxDepth` →
// CMP32rm) — AArch64 has no mem-direct ALU so only the X86 port had
// this gap.
//
// Operand layouts (disp position gates the symbolic claim):
//   rm  RMW (NumDefs>=1):  {Rd, Rd(tied), Base, Scale, Idx, Disp, Seg}
//   rm  compare (0 defs):  {Rs, Base, Scale, Idx, Disp, Seg}
//   mr / mi / mi8 / mi32:  {Base, Scale, Idx, Disp, Seg, Rs|Imm}
//   m   (INC/DEC):         {Base, Scale, Idx, Disp, Seg}
//
// Plan-9 operand order (mirrors Go runtime asm conventions):
//   * compares (mayStore()==0): subject first — `CMPQ ·sym(SB), $0` /
//     `CMPL AX, ·sym(SB)` — flags = left − right, matching the Intel
//     semantics of the original encoding so the following Jcc/CMOV
//     read the same predicate.
//   * RMW: data flows left → right — `XORQ AX, ·sym(SB)`,
//     `ADDQ ·sym(SB), DX`, `INCQ ·sym(SB)`.
// TEST<w>{mr,mi} is deliberately NOT claimed (Go asm spells it
// imm-first, e.g. `TESTB $1, sym(SB)`, the reverse of CMP; no c2go
// workload has produced a symbolic TEST yet — the streamer fail-closed
// guard will surface the first one loudly instead of guessing).
static bool tryPrintArithMemSymbolic(const MCInst *MI, raw_ostream &O,
                                     const MCRegisterInfo &MRI,
                                     const MCInstrInfo &MII) {
  unsigned Op = MI->getOpcode();
  StringRef Nm = MII.getName(Op);
  // Parse `<ROOT><bits><form>` — alphabetic root, decimal width, operand
  // form tail. Mirrors tryPrintArithReg's parse.
  size_t RootEnd = 0;
  while (RootEnd < Nm.size() && std::isalpha((unsigned char)Nm[RootEnd]))
    ++RootEnd;
  if (RootEnd == 0 || RootEnd >= Nm.size()) return false;
  StringRef Root = Nm.take_front(RootEnd);
  unsigned Bits = 0;
  size_t I = RootEnd;
  while (I < Nm.size() && std::isdigit((unsigned char)Nm[I])) {
    Bits = Bits * 10 + (Nm[I] - '0');
    ++I;
  }
  char SizeC = suffixForBits(Bits);
  if (!SizeC) return false;
  StringRef Form = Nm.substr(I); // "rm" | "mr" | "mi" | "mi8" | "mi32" | "m"

  // INC<bits>m / DEC<bits>m — single mem-operand RMW.
  if ((Root == "INC" || Root == "DEC") && Form == "m") {
    if (MI->getNumOperands() < 5) return false;
    if (!isPlan9SymbolicMemTuple(MI, /*OpStart=*/0)) return false;
    O << "\t" << Root << SizeC << " ";
    printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
    O << "\n";
    return true;
  }

  if (Root == "TEST") return false; // see header comment
  const char *MnP9 = plan9ArithMnemonic(Nm, SizeC);
  if (!MnP9) return false;
  const MCInstrDesc &Desc = MII.get(Op);

  if (Form == "rm") {
    // Compare (no defs, e.g. CMP<w>rm): {Rs, mem[5]} — subject reg
    // first. RMW reg-dest (ADD/XOR/IMUL/...<w>rm): {Rd, tied, mem[5]}.
    bool IsCompare = Desc.getNumDefs() == 0;
    unsigned MemStart = IsCompare ? 1 : 2;
    if (MI->getNumOperands() < MemStart + 5) return false;
    if (!MI->getOperand(0).isReg()) return false;
    if (!isPlan9SymbolicMemTuple(MI, MemStart)) return false;
    O << "\t" << MnP9 << " ";
    if (IsCompare) {
      printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
      O << ", ";
      printPlan9MemRef(O, MI, MemStart, MRI);
    } else {
      printPlan9MemRef(O, MI, MemStart, MRI);
      O << ", ";
      printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    }
    O << "\n";
    return true;
  }

  if (Form == "mr" || Form == "mi" || Form == "mi8" || Form == "mi32") {
    // {Base, Scale, Idx, Disp, Seg, Rs|Imm}. mayStore() splits RMW
    // (mem is the destination) from compare (flags only).
    if (MI->getNumOperands() < 6) return false;
    if (!isPlan9SymbolicMemTuple(MI, /*OpStart=*/0)) return false;
    const MCOperand &Src = MI->getOperand(5);
    bool SrcIsReg = (Form == "mr");
    if (SrcIsReg ? !Src.isReg() : !Src.isImm()) return false;
    bool IsCompare = !Desc.mayStore();
    O << "\t" << MnP9 << " ";
    if (IsCompare) {
      printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
      O << ", ";
      if (SrcIsReg)
        printPlan9Reg(O, Src.getReg(), MRI);
      else
        printPlan9Imm(O, Src.getImm());
    } else {
      if (SrcIsReg)
        printPlan9Reg(O, Src.getReg(), MRI);
      else
        printPlan9Imm(O, Src.getImm());
      O << ", ";
      printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
    }
    O << "\n";
    return true;
  }

  return false;
}

// c2go #298 Track AP.1 — symbolic-displacement SETcc-to-memory. The X86
// lowering of `globalFlag = (a OP b)` stores the EFLAGS predicate straight
// into a RIP-relative i8 global (`SETCCm {Base, Scale, Idx, Disp, Seg,
// CondCode}`). Surfaced by the AP.1 va_arg fix: sqlite3_config's previously
// UB-deleted body came back alive and produced the first symbolic SETCCm
// (`sqlite3Config.bCoreMutex = ...`-style byte stores). Go's assembler
// accepts `SET<cc> ·sym(SB)` (asm6.go yscond/Ymb); the Plan-9 SET<cc>
// suffixes mirror the J<cc> table exactly, so reuse plan9JCC's suffix.
// Same AN.1 scope gate: only the symbolic (fixup-bearing) form is claimed;
// non-symbolic SETCCm/SETCCr keep the raw-byte fallback byte-identical.
static bool tryPrintSetccMemSymbolic(const MCInst *MI, raw_ostream &O,
                                     const MCRegisterInfo &MRI,
                                     const MCInstrInfo &MII) {
  if (MII.getName(MI->getOpcode()) != "SETCCm")
    return false;
  if (MI->getNumOperands() < 6 || !MI->getOperand(5).isImm())
    return false;
  if (!isPlan9SymbolicMemTuple(MI, /*OpStart=*/0))
    return false;
  const char *J = plan9JCC((unsigned)MI->getOperand(5).getImm());
  if (!J)
    return false;
  // J<suffix> → SET<suffix> (e.g. JNE → SETNE) — Go derives both mnemonic
  // families from the same Plan-9 condition suffix set.
  O << "\tSET" << (J + 1) << " ";
  printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
  O << "\n";
  return true;
}

// c2go #298 Track AO.3 — symbolic-displacement SSE packed/scalar, widening
// integer load and x87 memory forms (see the file-header AO.3 note for the
// SQLite amd64 inventory that scoped this table). X86 ISel folds RIP-rel
// constant-pool / global loads straight into the consuming SSE/x87
// instruction; AArch64 splits the same access into an already-covered
// ADRP+LDR pair, so only the X86 port had the gap.
//
// Operand layouts (mirror tryPrintArithMemSymbolic; symbolic-mem gate via
// isPlan9SymbolicMemTuple keeps the non-symbolic raw-byte path
// byte-identical):
//   Load6:  {Rd, Base, Scale, Idx, Disp, Seg}        → `MN ·sym(SB), Rd`
//           (also UCOMIS*rm compares {Rs, mem5} — the reg operand is the
//            Intel reg-field subject in both, so the print form is shared;
//            Go encodes `UCOMISD mem, X1` as ucomisd %xmm1, mem — flags
//            match the original predicate)
//   RMW7:   {Rd, Rd(tied), Base, Scale, Idx, Disp, Seg} → `MN ·sym(SB), Rd`
//   Store6: {Base, Scale, Idx, Disp, Seg, Rs}        → `MN Rs, ·sym(SB)`
//   X87M5:  {Base, Scale, Idx, Disp, Seg}            → `MN ·sym(SB), F0`
//           (fld/fmul-from-mem push onto / operate on the x87 stack top;
//            Go asm6.go yfmvx ytab is exactly {Ym, Ynone, Yf0})
//
// Plan-9 spellings that differ from the Intel mnemonic (all verified via
// `go tool asm` + `go tool objdump` round-trip, 2026-06-11):
//   movdqa→MOVO, movdqu→MOVOU, movd(m32→xmm)→MOVL, movq(m64→xmm)→MOVQ,
//   paddd→PADDL, punpckldq→PUNPCKLLQ, fld m80/m64/m32→FMOVX/FMOVD/FMOVF,
//   fmul m32→FMULF.
//
// The SS-width scalar duals (MOVSSrm_alt/UCOMISSrm/ADD..DIVSSrm) are
// included alongside the inventory-hit SD forms — same dispatcher, same
// shape, identical Go-asm grammar — so a float-typed TU doesn't
// immediately re-open the family. Everything else (e.g. symbolic x87
// stores ST_FP*m, packed shuffles beyond PUNPCKLDQ) stays fail-closed
// behind the streamer guard until a real workload produces it.
static bool tryPrintSSEX87MemSymbolic(const MCInst *MI, raw_ostream &O,
                                      const MCRegisterInfo &MRI,
                                      const MCInstrInfo &MII) {
  enum Form { Load6, RMW7, Store6, X87M5, BlendRMW7, CmpRMW8 };
  struct Ent { const char *Op; const char *P9; Form F; };
  static const Ent kTable[] = {
      // (0) SSE4.1 implicit-XMM0 blends (#600: sqlite3AtoF selects
      // `blendvpd LCPI, xmm` once c2go long double == double routes its
      // rounding through SSE instead of x87). Same RMW7 operand layout;
      // Go asm spells the implicit mask register explicitly
      // (`BLENDVPD X0, m/x, x` — cmd/asm testdata amd64enc.s).
      {"BLENDVPDrm0", "BLENDVPD",  BlendRMW7},
      {"BLENDVPSrm0", "BLENDVPS",  BlendRMW7},
      {"PBLENDVBrm0", "PBLENDVB",  BlendRMW7},
      // (1) SSE packed / integer SIMD
      {"MOVDQArm",    "MOVO",      Load6},
      {"MOVDQUrm",    "MOVOU",     Load6},
      {"MOVDQUmr",    "MOVOU",     Store6},
      {"MOVDI2PDIrm", "MOVL",      Load6},   // movd m32 → xmm (zero-ext)
      {"MOVQI2PQIrm", "MOVQ",      Load6},   // movq m64 → xmm (zero-ext)
      {"PANDrm",      "PAND",      RMW7},
      {"PORrm",       "POR",       RMW7},
      {"PXORrm",      "PXOR",      RMW7},
      {"PADDBrm",     "PADDB",     RMW7},
      {"PADDWrm",     "PADDW",     RMW7},
      {"PADDDrm",     "PADDL",     RMW7},
      {"PADDQrm",     "PADDQ",     RMW7},
      // packed integer equality vs a pool constant (#654 Lua amd64 first
      // run: byte-compare loops vectorize into `pcmpeqb LCPI, xmm`); W/L
      // duals per the family convention — Go spells the dword form PCMPEQL.
      {"PCMPEQBrm",   "PCMPEQB",   RMW7},
      {"PCMPEQWrm",   "PCMPEQW",   RMW7},
      {"PCMPEQDrm",   "PCMPEQL",   RMW7},
      {"PMULLDrm",    "PMULLD",    RMW7},
      {"PUNPCKLDQrm", "PUNPCKLLQ", RMW7},
      {"ANDPSrm",     "ANDPS",     RMW7},
      {"ANDPDrm",     "ANDPD",     RMW7},
      {"ORPSrm",      "ORPS",      RMW7},
      {"ORPDrm",      "ORPD",      RMW7},
      {"XORPSrm",     "XORPS",     RMW7},
      {"XORPDrm",     "XORPD",     RMW7},
      {"SUBPDrm",     "SUBPD",     RMW7},
      // (2) SSE scalar FP (+ SS duals)
      {"MOVSDrm_alt", "MOVSD",     Load6},
      {"MOVSSrm_alt", "MOVSS",     Load6},
      {"UCOMISDrm",   "UCOMISD",   Load6},   // compare: shares Load6 print
      {"UCOMISSrm",   "UCOMISS",   Load6},
      // predicate compares producing an all-ones/zero mask (#665: the fp
      // addrspace lowering let -O2 fold `fcmp oeq; zext` through the SSE
      // mask form against a pool constant — {Rd, Rd(tied), mem5, pred-imm})
      {"CMPSDrmi",    "CMPSD",     CmpRMW8},
      {"CMPSSrmi",    "CMPSS",     CmpRMW8},
      {"ADDSDrm",     "ADDSD",     RMW7},
      {"SUBSDrm",     "SUBSD",     RMW7},
      {"MULSDrm",     "MULSD",     RMW7},
      {"DIVSDrm",     "DIVSD",     RMW7},
      {"ADDSSrm",     "ADDSS",     RMW7},
      {"SUBSSrm",     "SUBSS",     RMW7},
      {"MULSSrm",     "MULSS",     RMW7},
      {"DIVSSrm",     "DIVSS",     RMW7},
      // (3) widening integer loads
      {"MOVZX32rm8",  "MOVBLZX",   Load6},
      {"MOVZX32rm16", "MOVWLZX",   Load6},
      {"MOVSX64rm32", "MOVLQSX",   Load6},
      // (4) x87 (SQLite LONGDOUBLE_TYPE constant loads / fmul)
      {"LD_F32m",     "FMOVF",     X87M5},
      {"LD_F64m",     "FMOVD",     X87M5},
      {"LD_F80m",     "FMOVX",     X87M5},
      {"MUL_F32m",    "FMULF",     X87M5},
  };
  StringRef Nm = MII.getName(MI->getOpcode());
  const Ent *E = nullptr;
  for (const auto &T : kTable)
    if (Nm == T.Op) { E = &T; break; }
  if (!E) return false;

  switch (E->F) {
  case Load6:
    if (MI->getNumOperands() < 6 || !MI->getOperand(0).isReg()) return false;
    if (!isPlan9SymbolicMemTuple(MI, /*OpStart=*/1)) return false;
    O << "\t" << E->P9 << " ";
    printPlan9MemRef(O, MI, /*OpStart=*/1, MRI);
    O << ", ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << "\n";
    return true;
  case RMW7:
    if (MI->getNumOperands() < 7 || !MI->getOperand(0).isReg()) return false;
    if (!isPlan9SymbolicMemTuple(MI, /*OpStart=*/2)) return false;
    O << "\t" << E->P9 << " ";
    printPlan9MemRef(O, MI, /*OpStart=*/2, MRI);
    O << ", ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << "\n";
    return true;
  case Store6:
    if (MI->getNumOperands() < 6 || !MI->getOperand(5).isReg()) return false;
    if (!isPlan9SymbolicMemTuple(MI, /*OpStart=*/0)) return false;
    O << "\t" << E->P9 << " ";
    printPlan9Reg(O, MI->getOperand(5).getReg(), MRI);
    O << ", ";
    printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
    O << "\n";
    return true;
  case X87M5:
    if (MI->getNumOperands() < 5) return false;
    if (!isPlan9SymbolicMemTuple(MI, /*OpStart=*/0)) return false;
    O << "\t" << E->P9 << " ";
    printPlan9MemRef(O, MI, /*OpStart=*/0, MRI);
    O << ", F0\n";
    return true;
  case BlendRMW7:
    // dst @0 (tied to src1 @1), mem tuple @2-6; XMM0 mask is an implicit
    // use in the MCInst but an explicit first operand in Go asm.
    if (MI->getNumOperands() < 7 || !MI->getOperand(0).isReg()) return false;
    if (!isPlan9SymbolicMemTuple(MI, /*OpStart=*/2)) return false;
    O << "\t" << E->P9 << " X0, ";
    printPlan9MemRef(O, MI, /*OpStart=*/2, MRI);
    O << ", ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << "\n";
    return true;
  case CmpRMW8:
    // dst @0 (tied to src1 @1), mem tuple @2-6, predicate imm @7. Go asm
    // wants the predicate LAST: `CMPSD mem, xmm, $pred`
    // (cmd/asm testdata amd64enc.s: `CMPSD (BX), X2, $7`).
    if (MI->getNumOperands() < 8 || !MI->getOperand(0).isReg() ||
        !MI->getOperand(7).isImm())
      return false;
    if (!isPlan9SymbolicMemTuple(MI, /*OpStart=*/2)) return false;
    O << "\t" << E->P9 << " ";
    printPlan9MemRef(O, MI, /*OpStart=*/2, MRI);
    O << ", ";
    printPlan9Reg(O, MI->getOperand(0).getReg(), MRI);
    O << ", $" << MI->getOperand(7).getImm() << "\n";
    return true;
  }
  return false;
}

bool X86Plan9InstPrinter::tryPrintInst(const MCInst *MI,
                                       const MCSubtargetInfo & /*STI*/,
                                       raw_ostream &O) {
  // Dispatch in coverage-density order — the families NOSPLIT leaves
  // hit most frequently get first claim, the broader recognisers come
  // after, the bare AT&T-mnemonic fallback (translate via the table)
  // runs last before giving up.
  if (tryPrintRet(MI, O))                 return true;
  if (tryPrintBranchCall(MI, O))          return true;
  if (tryPrintIndirectCall(MI, O))        return true;
  if (tryPrintUnconditionalBranch(MI, O)) return true;
  if (tryPrintConditionalBranch(MI, O))   return true;
  if (tryPrintSPAdjust(MI, O))            return true;
  // #298 Track AF.1: intercept LEA64r / MOV64rm targeting
  // `c2go.typeinfo.<X>` or `type:<pkg>.<X>` *before* the generic
  // symbolic LEA / mem path renders the raw mangled name (the c2gobind
  // peer never emits that name, so link fails). Mirrors AArch64.
  if (tryPrintTypeinfoLoad(MI, O))        return true;
  // #298 Track AL.2: intercept the ELF small-PIC `@GOTPCREL` two-step
  // external-global LOAD's first step (MOV64rm) and rewrite it as
  // `LEAQ ·sym(SB), Rd` so the Plan-9 assembler/linker sees an
  // address-of, not a value LOAD. Without this the Go assembler treats
  // `MOVQ ·runtime·writeBarrier(SB), Rd` as a real 8-byte load of the
  // i32 flag (with 4 bytes of trailing junk), then the follow-up
  // narrow-width MOV<W>rm dereferences a garbage pointer (-O0 nil-deref
  // at writeBarrier, Wave AK BLOCKER-3 root). Mirrors AArch64's
  // `MOVD $·sym(SB), Rd` + `MOVWU 0(Rd), Rt` pair.
  if (tryPrintGOTPCRELLoadAsLEA(MI, O))   return true;
  if (tryPrintLEASymbolic(MI, O))         return true;
  if (tryPrintSPMemImm(MI, O))            return true;
  if (tryPrintMovImm(MI, O))              return true;
  if (tryPrintMOVSymbolic(MI, O))         return true;
  if (tryPrintMatchRR(MI, O))             return true;
  if (tryPrintArithReg(MI, O))            return true;
  // #298 Track AN.1: symbolic mem-direct MOV-imm / integer-ALU forms
  // (MOV64mi32 / XOR64mr / CMP64mi8 / INC64m / ...). Pre-AN.1 these
  // fixup-bearing MCInsts fell to emitRawBytesOrFail and were swallowed
  // behind a PLAN9-ERROR comment (amd64 ascast validator=0 root).
  if (tryPrintMovImmToMemSymbolic(MI, O, MRI, MII)) return true;
  if (tryPrintArithMemSymbolic(MI, O, MRI, MII))    return true;
  // #298 Track AP.1: symbolic SETcc-to-memory (SETCCm) — surfaced once the
  // AP.1 X86 c2go va_arg fix revived sqlite3_config's UB-deleted body.
  if (tryPrintSetccMemSymbolic(MI, O, MRI, MII))    return true;
  // #298 Track AO.3: symbolic SSE packed/scalar, widening-load and x87
  // memory forms (MOVDQArm / MOVSDrm_alt / UCOMISDrm / MOVZX32rm8 /
  // LD_F80m / ...) — the four post-AN.1 SQLite amd64 inventory families.
  if (tryPrintSSEX87MemSymbolic(MI, O, MRI, MII))   return true;
  if (tryPrintSSEMov(MI, O))              return true;
  if (tryPrintPushPop(MI, O, MRI, MII))   return true;

  return false;
}

// Fallback: any reg-reg form whose mnemonic resolves through the
// Plan-9 mnemonic table. Used to cover MOV{8,16,32,64}rr in a single
// path without duplicating the table.
bool X86Plan9InstPrinter::tryPrintMatchRR(const MCInst *MI, raw_ostream &O) {
  return tryPrintMovReg(MI, O, MRI, MII);
}

void X86Plan9InstPrinter::finishPending(raw_ostream & /*O*/) {
  // No cross-instruction state held by this minimal printer (no ADRP-
  // equivalent pair on X86 — RIP-relative addressing is single-inst).
}

void X86Plan9InstPrinter::notifyRawBytesEmitted(const MCInst * /*MI*/) {
  // No state to invalidate.
}

std::pair<const char *, uint64_t>
X86Plan9InstPrinter::getMnemonic(const MCInst &MI) const {
  // The MCPlan9AsmStreamer path doesn't reach this method (it dispatches
  // via tryPrintInst); kept as a non-abstract-class fallback for the
  // standalone `llvm-mc` path. Return the raw MII opcode name with
  // BitsLeft=0 — caller has no further bits to render.
  return {MII.getName(MI.getOpcode()).data(), 0};
}

void X86Plan9InstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                        raw_ostream &O) {
  // Best-effort: render register / immediate / symbol operand. Not
  // covered: composite mem operands (callers must dispatch via the
  // tryPrint* path which knows the 5-operand sub-tuple shape).
  if (OpNo >= MI->getNumOperands()) return;
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isReg()) {
    printRegName(O, Op.getReg());
  } else if (Op.isImm()) {
    O << "$" << Op.getImm();
  } else if (Op.isExpr()) {
    StringRef Sym = getReferencedSymbolName(Op.getExpr());
    int64_t Off = getReferencedSymbolOffset(Op.getExpr());
    if (!Sym.empty()) {
      if (isLocalLabelName(Sym)) O << sanitizeLocalDataRef(Sym); // #586
      else                       O << goSymToPlan9(Sym);
      if (Off > 0) O << "+" << Off;
      else if (Off < 0) O << Off;
      O << "(SB)";
    } else {
      O << Off;
    }
  }
}

void X86Plan9InstPrinter::printInst(const MCInst *MI, uint64_t /*Address*/,
                                     StringRef /*Annot*/,
                                     const MCSubtargetInfo &STI,
                                     raw_ostream &O) {
  // Standalone path (`llvm-mc -output-asm-variant=2`): no streamer
  // owns a fallback, so emit a PLAN9-TODO comment for misses. The
  // MCPlan9AsmStreamer never goes through printInst — it calls
  // tryPrintInst directly so it can do its own raw-byte / fail-loud
  // fallback.
  if (tryPrintInst(MI, STI, O))
    return;
  StringRef OpName = MII.getName(MI->getOpcode());
  O << "\t// PLAN9-TODO opcode=" << OpName
    << " (X86 Plan9 InstPrinter — Wave Y Track C scope is leaf-NOSPLIT only)\n";
}
