//===- AArch64Plan9InstPrinter.cpp - Plan 9 ARM64 syntax InstPrinter -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Plan 9 (Go assembler) syntax printer for AArch64 MCInsts. Used by
// MCPlan9AsmStreamer when c2go-mode emits `.s`.
//
// Role: translate **address-aware** instructions (PC-rel branches,
// ADRP+ADD/LDR pairs, RET) to Plan 9 mnemonics. Non-address-aware
// instructions and unrecognised opcodes return `false` from
// tryPrintInst; the streamer falls back to raw-byte WORD emission
// via its own MCCodeEmitter, and fails loudly if any fixups remain
// (i.e. a symbol-bearing instruction we didn't translate would
// otherwise turn into a silent zero-offset miscompile).
//
// Coverage (Go 1.25.9 arm64 mnemonics):
//   * Branches:
//       - B / BL / Bcc (14 cond codes)        → JMP / CALL / BEQ ... BLE
//       - CBZW/X, CBNZW/X                     → CBZW/CBZ, CBNZW/CBNZ
//       - TBZW/X, TBNZW/X                     → TBZ, TBNZ
//   * Address-of: ADRP+ADD pair               → MOVD $·sym(SB), Rd
//                 ADRP+LDR(X|W)ui pair         → MOVD/MOVW ·sym(SB), Rd
//   * Special: ADRP referencing
//       `c2go.typeinfo.<X>`                   → MOVD $type·<Pkg>·<X>(SB), Rd
//       `type:<linkname>`                     → MOVD $type·<linkname pkg>·<X>(SB), Rd
//   * RET
//
// ADRP buffering: ADRP only produces output when followed by a
// matching ADD/LDR that re-uses the ADRP destination as its source
// base. finishPending() flushes a stale buffered ADRP at section /
// label / stream boundaries.
//
//===----------------------------------------------------------------------===//

#include "AArch64Plan9InstPrinter.h"
#include "MCTargetDesc/AArch64AddressingModes.h"
#include "MCTargetDesc/AArch64MCAsmInfo.h"
#include "MCTargetDesc/AArch64MCTargetDesc.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

using namespace llvm;

AArch64Plan9InstPrinter::AArch64Plan9InstPrinter(const MCAsmInfo &MAI,
                                                 const MCInstrInfo &MII,
                                                 const MCRegisterInfo &MRI)
    : AArch64InstPrinter(MAI, MII, MRI) {}

AArch64Plan9InstPrinter::~AArch64Plan9InstPrinter() = default;

// c2go #239: previously this file declared two thread_local globals —
// `Plan9LinknameBridges` (a (sanitised_local_name, raw_path) registry)
// and `Plan9PackageName` (the Go package short name) — defined in
// MCPlan9AsmStreamer.cpp. Both were written here / by the streamer but
// had no live reader; they were removed as part of the per-compile
// thread_local reduction. If a future symbolic-print pass needs either,
// it should live on the streamer instance (reachable from this printer
// via the MCStreamer pointer threaded through the symbolic-print path).

namespace {

// Walk a MCBinaryExpr's RHS chain summing all integer constants. Used
// to extract the `+N` / `-N` byte offset that adheres to a symbol
// reference (e.g. `sqlite3Config@PAGE+64`).
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
    default:
      return Off;
    }
  }
  return Off;
}

// Pull the underlying MCSymbol name out of an MCExpr that may be
// wrapped in MCBinary/MCUnary/MCSpecifierExpr (Specifier/Target) layers.
static StringRef getReferencedSymbolName(const MCExpr *E) {
  while (E) {
    switch (E->getKind()) {
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
    default:
      return StringRef();
    }
  }
  return StringRef();
}

// Convert a Go-style symbol name to its Plan 9 form per the c2go
// dual-path scheme.
//
// Path (a) — raw import-path-style name without `-`:
//   Use Plan 9 assembler's Unicode substitutes:
//     `.` → U+00B7 (·)   restored to `.` by the assembler
//     `/` → U+2215 (∕)   restored to `/` by the assembler
//   Other characters pass through. Linker symbol name is the raw
//   path byte-for-byte, so it matches the corresponding Go-side
//   export directly with zero further bridging.
//
// Path (b) — name contains `-` (Plan 9 lexer rejects it):
//   Replace every non-ident character (`/`, `-`, `.`, ...) with
//   `_` to produce a Plan 9-legal local symbol, then emit as
//   `·<sanitised>` (current package). Record the (sanitised, raw)
//   pair so the manifest can expose it to c2gobind for a Go-side
//   `//go:linkname` bridge.
//
// Other shapes:
//   * Mach-O local labels (`l_.str.<n>`, `lCPI<n>_<m>`, `L*`, `.L*`)
//     — current-TU-local, sanitise to underscores, no middle-dot.
//   * Plain `pkg.name` (no slash, no hyphen) — convert last `.` to
//     middle-dot (Go runtime convention), sanitise the rest.
//   * Bare ident (no `.` no `/`) — treat as current-pkg `·name`.
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
    // #586: goSymToPlan9 only ever renders (SB) operands (data / functions);
    // branch/CFI labels go through formatBranchTarget. A local symbol reaching
    // here is a private DATA symbol (string literal / constant pool), which the
    // streamer emits file-local (`name<>(SB)`) to avoid cross-package link
    // collisions — the reference must carry the same `<>` scope.
    Out += "<>";
    return Out;
  }

  bool HasSlash = Name.contains('/');
  // #274: any char outside [A-Za-z0-9_/.] cannot be carried raw in a Plan 9
  // symbol (the Unicode-substitute path only handles `.`→· and `/`→∕) — `-`
  // (hyphenated import paths) and `(`/`*`/`)` (Go method symbols `pkg.(*T).M`)
  // must take the //go:linkname bridge (path b). Mirrors
  // MCPlan9AsmStreamer::symbolToPlan9 (keep both in sync).
  bool HasIllegal = false;
  for (char C : Name)
    if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
          (C >= '0' && C <= '9') || C == '_' || C == '/' || C == '.')) {
      HasIllegal = true;
      break;
    }

  // Path (a): import-path-style and fully transformable — Unicode-substitute
  // emit. assembler decodes byte-for-byte back to raw path.
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

  // Path (b): contains a Plan-9-illegal char (`-`, or method `(`/`*`/`)`).
  // Sanitise to a Plan 9-legal current-pkg local name. The (local, raw)
  // pair was previously written to a thread_local Plan9LinknameBridges
  // registry for a manifest consumer; that registry had no reader and was
  // removed in c2go #239. If a manifest bridge is needed again it should
  // hang off the streamer instance.
  if (HasIllegal) {
    std::string Sanit = Name.str();
    sanitiseToIdent(Sanit);
    return std::string(DotUTF8) + Sanit;
  }

  // No slash, no hyphen: typical `pkg.name`. Convert last `.` to
  // middle-dot; sanitise the rest.
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

// Canonicalize W<n> → X<n> for register-state tracking purposes.
// AArch64 writes to W<n> zero-extend to the full X<n>, so for the
// purpose of "this register currently holds value V", W<n> writes
// invalidate X<n> state and X<n> set-up applies to W<n> reads.
static MCRegister canonicalXReg(MCRegister R) {
  if (R >= AArch64::W0 && R <= AArch64::W30)
    return MCRegister(AArch64::X0 + (R - AArch64::W0));
  if (R == AArch64::WSP) return AArch64::SP;
  if (R == AArch64::WZR) return AArch64::XZR;
  return R;
}

// Plan 9 FP/SIMD register printing. AArch64 has separate enum
// classes B/H/S/D/Q each containing 32 registers — Plan 9 collapses
// them to F0..F31 (width is in the mnemonic suffix: FMOVS/FMOVD/
// FMOVQ). Returns true if Reg was an FP/SIMD reg and was printed.
static bool printPlan9FPR(raw_ostream &O, MCRegister Reg) {
  if (Reg >= AArch64::B0 && Reg <= AArch64::B31) {
    O << "F" << (unsigned)(Reg - AArch64::B0); return true;
  }
  if (Reg >= AArch64::H0 && Reg <= AArch64::H31) {
    O << "F" << (unsigned)(Reg - AArch64::H0); return true;
  }
  if (Reg >= AArch64::S0 && Reg <= AArch64::S31) {
    O << "F" << (unsigned)(Reg - AArch64::S0); return true;
  }
  if (Reg >= AArch64::D0 && Reg <= AArch64::D31) {
    O << "F" << (unsigned)(Reg - AArch64::D0); return true;
  }
  if (Reg >= AArch64::Q0 && Reg <= AArch64::Q31) {
    O << "F" << (unsigned)(Reg - AArch64::Q0); return true;
  }
  return false;
}

// Plan 9 GPR printing. X0..X28 → R0..R28; FP → R29; LR → R30; SP →
// RSP; X/WZR → ZR. W variants share the R<n> name (width via mnemonic).
static void printPlan9GPR(raw_ostream &O, MCRegister Reg) {
  if (Reg >= AArch64::X0 && Reg <= AArch64::X28) {
    O << "R" << (unsigned)(Reg - AArch64::X0);
    return;
  }
  if (Reg == AArch64::FP)  { O << "R29"; return; }
  if (Reg == AArch64::LR)  { O << "R30"; return; }
  if (Reg == AArch64::SP)  { O << "RSP"; return; }
  if (Reg == AArch64::XZR) { O << "ZR";  return; }
  if (Reg >= AArch64::W0 && Reg <= AArch64::W30) {
    O << "R" << (unsigned)(Reg - AArch64::W0);
    return;
  }
  if (Reg == AArch64::WSP) { O << "RSP"; return; }
  if (Reg == AArch64::WZR) { O << "ZR";  return; }
  O << "R?" << (unsigned)Reg;
}

// Plan 9 conditional branch mnemonic for ARM cond imm 0..15.
static const char *plan9CondMnemonic(unsigned Cond) {
  switch (Cond & 0xf) {
  case 0:  return "BEQ";
  case 1:  return "BNE";
  case 2:  return "BHS";
  case 3:  return "BLO";
  case 4:  return "BMI";
  case 5:  return "BPL";
  case 6:  return "BVS";
  case 7:  return "BVC";
  case 8:  return "BHI";
  case 9:  return "BLS";
  case 10: return "BGE";
  case 11: return "BLT";
  case 12: return "BGT";
  case 13: return "BLE";
  case 14: return "JMP";
  default: return nullptr;
  }
}

// Mach-O / LLVM private label prefixes — we emit these as bare
// `Name:` labels and reference them by bare name. Mach-O private
// labels can use either uppercase `L` (LLVM convention) or lowercase
// `l_` (clang's MachOAsmInfo for string literals, e.g. `l_.str.42`).
static bool isLocalLabelName(StringRef Name) {
  if (Name.empty()) return false;
  if (Name[0] == 'L' || Name[0] == 'l') return true;
  if (Name.starts_with(".L")) return true;
  return false;
}

// A local-label REFERENCE (branch target) must match the identifier the
// streamer emits for the label DEFINITION. MCPlan9AsmStreamer::symbolToPlan9
// maps every non-identifier char to '_' (so `.LBB0_2` → `_LBB0_2`); the raw
// `.L`-form is also rejected by `go tool asm` ("expected '(', found ..."),
// so a bare ref both mis-matches the def and is invalid syntax. Apply the
// same sanitisation here.
static std::string sanitizeLocalLabel(StringRef Name) {
  std::string Out = Name.str();
  for (char &C : Out)
    if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
          (C >= '0' && C <= '9') || C == '_'))
      C = '_';
  return Out;
}

// Does this relocation expression use a GOT specifier (:got:/:got_lo12: on
// ELF, @GOTPAGE/@GOTPAGEOFF on Mach-O)? clang accesses external globals
// (e.g. c2go_extern `gDefault`) GOT-indirectly: ADRP@GOTPAGE + LDR@GOTPAGEOFF
// loads the ADDRESS of the global (the GOT slot holds &sym), then a separate
// LDR dereferences it. In Plan 9 / Go asm there is no GOT — the symbol is
// directly addressable — so the GOT pair must lower to an address-of
// (`MOVD $·sym(SB)`), not a value load. (#258)
static bool referencesGOT(const MCExpr *E) {
  while (E) {
    switch (E->getKind()) {
    case MCExpr::Target:
    case MCExpr::Specifier: {
      const auto *S = cast<MCSpecifierExpr>(E);
      switch (S->getSpecifier()) {
      case AArch64::S_GOT:
      case AArch64::S_GOT_PAGE:
      case AArch64::S_GOT_LO12:
      case AArch64::S_MACHO_GOTPAGE:
      case AArch64::S_MACHO_GOTPAGEOFF:
        return true;
      default:
        break;
      }
      E = S->getSubExpr();
      break;
    }
    case MCExpr::Unary:
      E = cast<MCUnaryExpr>(E)->getSubExpr();
      break;
    case MCExpr::Binary:
      E = cast<MCBinaryExpr>(E)->getLHS();
      break;
    default:
      return false;
    }
  }
  return false;
}

// Does this relocation expression carry the ELF `:lo12:` ADRP-companion
// specifier (AArch64::S_LO12)? Used by the Q-reg LDR/STR coverage to
// distinguish an ADRP-paired access (the base register holds the FULL
// symbol address — every ADRP this printer sees lowers to
// `MOVD $·sym(SB), Rn` via tryPrintADRPPairCompletion/flushPendingADRP,
// page and lo12 both folded) from a c2go field-access rewrite (a bare
// MCSymbolRefExpr planted by AArch64AsmPrinter::
// rewriteC2GoFieldImmToSymbol, where the base register holds a runtime
// object pointer and the symbol is a link-time field offset). (#298 AO.2)
static bool referencesLo12(const MCExpr *E) {
  while (E) {
    switch (E->getKind()) {
    case MCExpr::Target:
    case MCExpr::Specifier: {
      const auto *S = cast<MCSpecifierExpr>(E);
      if (S->getSpecifier() == AArch64::S_LO12)
        return true;
      E = S->getSubExpr();
      break;
    }
    case MCExpr::Unary:
      E = cast<MCUnaryExpr>(E)->getSubExpr();
      break;
    case MCExpr::Binary:
      E = cast<MCBinaryExpr>(E)->getLHS();
      break;
    default:
      return false;
    }
  }
  return false;
}

} // namespace

std::string AArch64Plan9InstPrinter::formatBranchTarget(const MCExpr *E) const {
  // #240: read the referenced MCSymbol structurally instead of
  // rendering the MCExpr to text and re-parsing the string. Branch
  // operands are a single MCSymbolRefExpr (a local block label or an
  // external symbol). getReferencedSymbolName walks the SymbolRef/
  // Binary/Specifier layers and returns the underlying name directly.
  StringRef R = getReferencedSymbolName(E);
  if (!R.empty()) {
    if (isLocalLabelName(R))
      return sanitizeLocalLabel(R); // match the streamer's label def form
    // c2go #321: Plan 9 codegen runs with a neutral ELF triple whose
    // mangler does NOT add a `_` global prefix, so any leading `_` is a
    // real character of the C source-level symbol (e.g. `_helper`) and
    // must be preserved to match buildC2GoManifest's `·_helper` asm_symbol.
    return goSymToPlan9(R);
  }
  // Defensive fallback for any exotic MCExpr shape that doesn't carry a
  // plain symbol reference: render via its own print method, which
  // handles MCBinaryExpr offsets and AArch64MCExpr modifiers.
  std::string S;
  raw_string_ostream Stream(S);
  MAI.printExpr(Stream, *E);
  Stream.flush();
  StringRef RS(S);
  if (isLocalLabelName(RS))
    return sanitizeLocalLabel(RS); // match the streamer's label def form
  // c2go #321: leading `_` is part of the C symbol name; do not strip.
  return goSymToPlan9(RS);
}

// Helper: emit "sym" or "sym+N" / "sym-N" suffix into O. Caller has
// already emitted any leading `$` and the goSymToPlan9-transformed
// symbol; this appends the `+N`/`-N` part if Off != 0.
static void emitSymbolOffsetSuffix(raw_ostream &O, int64_t Off) {
  if (Off > 0) O << "+" << Off;
  else if (Off < 0) O << Off; // negative number prints its own '-'
}

bool AArch64Plan9InstPrinter::tryPrintADRP(const MCInst *MI, raw_ostream &O) {
  if (MI->getOpcode() != AArch64::ADRP)
    return false;
  if (MI->getNumOperands() < 2 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isExpr())
    return false;
  // Buffer until we see the paired ADD/LDR. flushPendingADRP() or
  // tryPrintADRPPairCompletion() resolves the deferred output.
  if (HavePendingADRP)
    flushPendingADRP(O); // shouldn't normally happen, but be safe
  HavePendingADRP = true;
  PendingADRPDest = MI->getOperand(0).getReg();
  PendingADRPSymbol = getReferencedSymbolName(MI->getOperand(1).getExpr()).str();
  PendingADRPOffset = getReferencedSymbolOffset(MI->getOperand(1).getExpr());
  PendingADRPIsGOT = referencesGOT(MI->getOperand(1).getExpr());
  return true;
}

void AArch64Plan9InstPrinter::flushPendingADRP(raw_ostream &O) {
  if (!HavePendingADRP) return;
  // Bare ADRP — emit address-of as a best-effort AND remember the
  // register→symbol mapping so subsequent LDR/STR with `:lo12:sym`
  // and matching base register can complete the translation.
  // c2go #321: do NOT strip leading `_` (neutral-ELF mangler, see
  // formatBranchTarget comment).
  StringRef Sym = PendingADRPSymbol;
  O << "\tMOVD $" << goSymToPlan9(Sym);
  emitSymbolOffsetSuffix(O, PendingADRPOffset);
  O << "(SB), ";
  printPlan9GPR(O, PendingADRPDest);
  O << "\n";
  rememberRegHoldsPage(PendingADRPDest, PendingADRPSymbol);
  HavePendingADRP = false;
  PendingADRPSymbol.clear();
  PendingADRPOffset = 0;
  PendingADRPIsGOT = false;
}

void AArch64Plan9InstPrinter::rememberRegHoldsPage(MCRegister Reg,
                                                    StringRef Sym) {
  RegHoldsPage[canonicalXReg(Reg).id()] = Sym.str();
}

void AArch64Plan9InstPrinter::invalidateRegDefs(const MCInst *MI) {
  if (RegHoldsPage.empty()) return;
  const MCInstrDesc &Desc = MII.get(MI->getOpcode());
  unsigned NumDefs = Desc.getNumDefs();
  for (unsigned I = 0; I < NumDefs && I < MI->getNumOperands(); ++I) {
    const MCOperand &Op = MI->getOperand(I);
    if (!Op.isReg() || !Op.getReg())
      continue;
    RegHoldsPage.erase(canonicalXReg(Op.getReg()).id());
  }
  // Implicit defs (e.g., LR for BL, etc.) — being conservative,
  // invalidate any register the instruction implicitly defines.
  for (MCPhysReg ImpDef : Desc.implicit_defs())
    RegHoldsPage.erase(canonicalXReg(MCRegister(ImpDef)).id());
}

void AArch64Plan9InstPrinter::clearRegState() {
  RegHoldsPage.clear();
}

bool AArch64Plan9InstPrinter::tryPrintADRPPairCompletion(const MCInst *MI,
                                                          raw_ostream &O) {
  if (!HavePendingADRP) return false;
  unsigned Op = MI->getOpcode();

  // The pair must (a) target the same symbol AND (b) use the pending
  // ADRP destination as its source base register (operand 1). Without
  // (b) we could fold an unrelated nearby ADD/LDR by accident.
  auto matchesPair = [&](unsigned BaseRegOpIdx, unsigned ExprOpIdx) -> bool {
    if (MI->getNumOperands() <= ExprOpIdx) return false;
    if (!MI->getOperand(BaseRegOpIdx).isReg()) return false;
    if (MI->getOperand(BaseRegOpIdx).getReg() != PendingADRPDest)
      return false;
    if (!MI->getOperand(ExprOpIdx).isExpr()) return false;
    StringRef Sym = getReferencedSymbolName(
        MI->getOperand(ExprOpIdx).getExpr());
    return Sym == PendingADRPSymbol;
  };

  if (Op == AArch64::ADDXri && matchesPair(/*BaseRegOpIdx=*/1,
                                            /*ExprOpIdx=*/2)) {
    // ADRP+ADD → MOVD $·sym+N(SB), Rd  (address-of). The byte offset
    // is the sum of the ADRP's `sym@PAGE+N1` and the ADD's
    // `sym@PAGEOFF+N2` — N1==N2 by LLVM convention, but be defensive
    // and take the ADD-side which represents the final lo12 contrib.
    int64_t Off = getReferencedSymbolOffset(MI->getOperand(2).getExpr());
    // c2go #321: do NOT strip leading `_`; preserved as part of the
    // source-level C symbol name.
    StringRef Sym = PendingADRPSymbol;
    MCRegister Rd = MI->getOperand(0).getReg();
    if (Sym.consume_front("type:")) {
      // c2go §A2 Go-owner case: the C side declared
      // `struct __attribute__((c2go_struct, c2go_linkname("pkg.X"))) X`,
      // so the typeinfo lives Go-side and clang emitted an external decl
      // `@"type:pkg.X"`. #218: reference c2gobind's per-type indirection
      // var `·_typeinfo_<X>(SB)` (current pkg) which holds the *rtype
      // pointer obtained via the unsafe-iface trick on `any(X{})`. Go
      // assembler / linker can't resolve `type:pkg.X` directly from .s
      // (the bare form is a parse error; `type·pkg·X` resolves to
      // `type.pkg.X` which is a different symbol from the compiler's
      // emitted `type:pkg.X`). The indirection var is a normal Go
      // package symbol and resolves cleanly.
      std::string Pkg = Sym.str();
      std::string Name = Pkg;
      auto Dot = Pkg.rfind('.');
      if (Dot != std::string::npos)
        Name = Pkg.substr(Dot + 1);
      // Load the value (the *rtype pointer), not the var's address.
      O << "\tMOVD \xc2\xb7_typeinfo_" << Name << "(SB), ";
      printPlan9GPR(O, Rd);
      if (Off) {
        O << "\n\tADD $" << Off << ", ";
        printPlan9GPR(O, Rd);
      }
      O << "\n";
    } else if (Sym.consume_front("c2go.typeinfo.")) {
      // c2go (#134, §A2, #218): emit `MOVD ·_typeinfo_<X>(SB), Rd` —
      // loads the *runtime._type pointer for the C-owner
      // `type X struct {...}` that c2gobind exports via its per-type
      // indirection var (`var _typeinfo_<X> unsafe.Pointer` initialised
      // from `(*iface)(unsafe.Pointer(&any(X{}))).data`). mallocgc et
      // al expect a *_type pointer; the indirection var holds exactly
      // that. Leading `·` = current package.
      //
      // #218 fix: previous `MOVD $type·<pkg>·<X>(SB)` form resolved to
      // linker symbol `type.<pkg>.<X>` (literal '.'), but the Go
      // compiler emits `type:<pkg>.<X>` (literal ':') — different
      // symbols, so the .s reference never resolved. Plan 9 .s syntax
      // also cannot lex bare `type:<pkg>.<X>(SB)` (the ':' triggers
      // a parser error). The indirection var sidesteps both issues.
      //
      // §A2 dropped the legacy `struct.` infix; the residue here is the
      // bare AST record name.
      //
      // GPT round 1 P1 fix: c2go-synthesized anonymous record names
      // (e.g. `c2go.anon.6d1daeec...`) get sanitized by c2gobind to
      // `c2go_anon_6d1daeec...` (Go identifiers can't contain '.').
      // Flatten dots before emit.
      std::string SymStr(Sym);
      if (SymStr.rfind("c2go.", 0) == 0) {
        for (char &C : SymStr) if (C == '.') C = '_';
      }
      O << "\tMOVD \xc2\xb7_typeinfo_" << SymStr << "(SB), ";
      printPlan9GPR(O, Rd);
      if (Off) {
        O << "\n\tADD $" << Off << ", ";
        printPlan9GPR(O, Rd);
      }
      O << "\n";
      // The MOVD result is a typeinfo *pointer*, not the page-address
      // of any symbol — do not record it in RegHoldsPage.
    } else {
      O << "\tMOVD $" << goSymToPlan9(Sym);
      emitSymbolOffsetSuffix(O, Off);
      O << "(SB), ";
      printPlan9GPR(O, Rd);
      O << "\n";
      // Rd now holds the address of `sym+Off`. The tracked-ADRP path
      // for downstream LDR/STR uses the symbol name as key only —
      // we don't currently propagate the offset further, but the
      // common case (struct field load) has the offset already
      // baked into this pair.
      rememberRegHoldsPage(Rd, PendingADRPSymbol);
    }
    HavePendingADRP = false;
    PendingADRPSymbol.clear();
    PendingADRPOffset = 0;
    return true;
  }
  if ((Op == AArch64::LDRXui || Op == AArch64::LDRWui) &&
      matchesPair(/*BaseRegOpIdx=*/1, /*ExprOpIdx=*/2)) {
    int64_t Off = getReferencedSymbolOffset(MI->getOperand(2).getExpr());
    // c2go #321: leading `_` is part of the C symbol; do not strip.
    StringRef Sym = PendingADRPSymbol;
    MCRegister Rd = MI->getOperand(0).getReg();
    if (PendingADRPIsGOT) {
      // c2go (#218 parity): an external typeinfo descriptor (a Go-owner
      // managed struct) is GOT-indirect here; route it to the SAME
      // Â·_typeinfo_<X>(SB) reflect-pin var as the direct C-owner ADD path
      // above, so c2gobind resolves it. X86 already composes the GOT path
      // with this rewrite; AArch64 previously fell through to the raw
      // c2go_typeinfoÂ·<X> mangling (link-time undefined).
      StringRef Ti = Sym;
      bool TiRewrote = true;
      if (Ti.consume_front("type:")) {
        std::string Name = Ti.str();
        auto Dot = Name.rfind('.');
        if (Dot != std::string::npos)
          Name = Name.substr(Dot + 1);
        O << "\tMOVD \xc2\xb7_typeinfo_" << Name << "(SB), ";
        printPlan9GPR(O, Rd);
      } else if (Ti.consume_front("c2go.typeinfo.")) {
        std::string SymStr(Ti);
        if (SymStr.rfind("c2go.", 0) == 0)
          for (char &C : SymStr)
            if (C == '.')
              C = '_';
        O << "\tMOVD \xc2\xb7_typeinfo_" << SymStr << "(SB), ";
        printPlan9GPR(O, Rd);
      } else {
        TiRewrote = false;
      }
      if (TiRewrote) {
        if (Off) {
          O << "\n\tADD $" << Off << ", ";
          printPlan9GPR(O, Rd);
        }
        O << "\n";
        // typeinfo *pointer*, not a page address — do not RegHoldsPage.
      } else {
        // GOT-indirect (#258): ADRP@GOTPAGE + LDR@GOTPAGEOFF loads the
        // ADDRESS of `sym` (the GOT slot holds &sym), so this lowers to an
        // address-of, NOT a value load. The actual value load is the
        // SUBSEQUENT `LDR [Rd]`.
        O << "\tMOVD $" << goSymToPlan9(Sym);
        emitSymbolOffsetSuffix(O, Off);
        O << "(SB), ";
        printPlan9GPR(O, Rd);
        O << "\n";
        rememberRegHoldsPage(Rd, PendingADRPSymbol);
      }
    } else {
      // ADRP+LDR → MOVD/MOVW ·sym+N(SB), Rd  (direct value load).
      O << "\t" << (Op == AArch64::LDRXui ? "MOVD" : "MOVW")
        << " " << goSymToPlan9(Sym);
      emitSymbolOffsetSuffix(O, Off);
      O << "(SB), ";
      printPlan9GPR(O, Rd);
      O << "\n";
    }
    HavePendingADRP = false;
    PendingADRPSymbol.clear();
    PendingADRPOffset = 0;
    PendingADRPIsGOT = false;
    return true;
  }
  // ADRP + STR-with-:lo12: pair. Without folding here, we'd emit
  // ADRP+ADD for the standalone ADRP (8 bytes) followed by STR's own
  // ADRP+STR (another 8 bytes) — total 16 bytes vs raw's 8.  The
  // resulting layout shift makes raw-WORD-encoded PC-relative
  // branches (CBZ/CBNZ/etc emitted as bytes) point at wrong targets.
  // STRQui (Q-reg / 128-bit) handled separately below: Go's assembler
  // rejects BOTH direct Q spellings (`FMOVQ sym(SB), F0` and
  // `FMOVQ F0, sym(SB)` — "illegal combination", go tool asm verified
  // 2026-06-11), so the Q store needs the register-indirect lowering
  // (#298 Track AO.2 follow-up; pre-AO this branch shipped the direct
  // store form, a loud Go-assemble-time failure had the shape arisen).
  if ((Op == AArch64::STRXui || Op == AArch64::STRWui ||
       Op == AArch64::STRBBui || Op == AArch64::STRHHui ||
       Op == AArch64::STRSui || Op == AArch64::STRDui) &&
      matchesPair(/*BaseRegOpIdx=*/1, /*ExprOpIdx=*/2)) {
    int64_t Off = getReferencedSymbolOffset(MI->getOperand(2).getExpr());
    // c2go #321: leading `_` is part of the C symbol; do not strip.
    StringRef Sym = PendingADRPSymbol;
    MCRegister Rt = MI->getOperand(0).getReg();
    const char *Mn = nullptr;
    switch (Op) {
    case AArch64::STRXui:  Mn = "MOVD";  break;
    case AArch64::STRWui:  Mn = "MOVW";  break;
    case AArch64::STRBBui: Mn = "MOVB";  break;
    case AArch64::STRHHui: Mn = "MOVH";  break;
    case AArch64::STRSui:  Mn = "FMOVS"; break;
    case AArch64::STRDui:  Mn = "FMOVD"; break;
    }
    O << "\t" << Mn << " ";
    if (!printPlan9FPR(O, Rt)) printPlan9GPR(O, Rt);
    O << ", " << goSymToPlan9(Sym);
    emitSymbolOffsetSuffix(O, Off);
    O << "(SB)\n";
    HavePendingADRP = false;
    PendingADRPSymbol.clear();
    PendingADRPOffset = 0;
    return true;
  }
  // ADRP + LDR-with-:lo12: extended variants (B/H/S/D widths and
  // signed-LDR forms). LDRXui/LDRWui already handled above.
  // LDRQui (Q-reg / 128-bit) handled separately below because Plan 9
  // ARM64 assembler doesn't accept `FMOVQ sym(SB), F0` form.
  if ((Op == AArch64::LDRBBui || Op == AArch64::LDRHHui ||
       Op == AArch64::LDRSBWui || Op == AArch64::LDRSHWui ||
       Op == AArch64::LDRSWui ||
       Op == AArch64::LDRSui || Op == AArch64::LDRDui) &&
      matchesPair(/*BaseRegOpIdx=*/1, /*ExprOpIdx=*/2)) {
    int64_t Off = getReferencedSymbolOffset(MI->getOperand(2).getExpr());
    // c2go #321: leading `_` is part of the C symbol; do not strip.
    StringRef Sym = PendingADRPSymbol;
    MCRegister Rt = MI->getOperand(0).getReg();
    const char *Mn = nullptr;
    switch (Op) {
    case AArch64::LDRBBui:  Mn = "MOVBU"; break;
    case AArch64::LDRHHui:  Mn = "MOVHU"; break;
    case AArch64::LDRSBWui: Mn = "MOVB";  break;
    case AArch64::LDRSHWui: Mn = "MOVH";  break;
    case AArch64::LDRSWui:  Mn = "MOVW";  break;
    case AArch64::LDRSui:   Mn = "FMOVS"; break;
    case AArch64::LDRDui:   Mn = "FMOVD"; break;
    }
    O << "\t" << Mn << " " << goSymToPlan9(Sym);
    emitSymbolOffsetSuffix(O, Off);
    O << "(SB), ";
    if (!printPlan9FPR(O, Rt)) printPlan9GPR(O, Rt);
    O << "\n";
    HavePendingADRP = false;
    PendingADRPSymbol.clear();
    PendingADRPOffset = 0;
    return true;
  }
  // LDRQui (Q-reg load) via ADRP+LDR pair: Plan 9 doesn't accept
  // `FMOVQ sym(SB), F0`. Emit ADRP+ADD into the BASE register (the
  // one the ADRP+LDR pair already names), then a register-indirect
  // FMOVQ. Total still 12 bytes (4 ADRP + 4 ADD + 4 FMOVQ), which
  // matches what raw ADRP+LDR Q-load would assemble to anyway.
  if (Op == AArch64::LDRQui &&
      matchesPair(/*BaseRegOpIdx=*/1, /*ExprOpIdx=*/2)) {
    int64_t Off = getReferencedSymbolOffset(MI->getOperand(2).getExpr());
    // c2go #321: leading `_` is part of the C symbol; do not strip.
    StringRef Sym = PendingADRPSymbol;
    MCRegister Rn = MI->getOperand(1).getReg(); // base reg from LDRQui
    MCRegister Rt = MI->getOperand(0).getReg(); // dest FP reg
    O << "\tMOVD $" << goSymToPlan9(Sym);
    emitSymbolOffsetSuffix(O, Off);
    O << "(SB), ";
    printPlan9GPR(O, Rn);
    O << "\n\tFMOVQ (";
    printPlan9GPR(O, Rn);
    O << "), ";
    printPlan9FPR(O, Rt);
    O << "\n";
    HavePendingADRP = false;
    PendingADRPSymbol.clear();
    PendingADRPOffset = 0;
    return true;
  }
  // STRQui (Q-reg store) via ADRP+STR pair: same Q-reg representability
  // constraint as the LDRQui branch above — Go's assembler rejects the
  // direct `FMOVQ F0, sym(SB)` spelling just like the load direction
  // ("illegal combination", go tool asm verified 2026-06-11). Emit the
  // full address into the BASE register the pair already names (the
  // ADRP wrote it anyway), then a register-indirect FMOVQ store.
  if (Op == AArch64::STRQui &&
      matchesPair(/*BaseRegOpIdx=*/1, /*ExprOpIdx=*/2)) {
    int64_t Off = getReferencedSymbolOffset(MI->getOperand(2).getExpr());
    // c2go #321: leading `_` is part of the C symbol; do not strip.
    StringRef Sym = PendingADRPSymbol;
    MCRegister Rn = MI->getOperand(1).getReg(); // base reg from STRQui
    MCRegister Rt = MI->getOperand(0).getReg(); // stored FP reg
    O << "\tMOVD $" << goSymToPlan9(Sym);
    emitSymbolOffsetSuffix(O, Off);
    O << "(SB), ";
    printPlan9GPR(O, Rn);
    O << "\n\tFMOVQ ";
    printPlan9FPR(O, Rt);
    O << ", (";
    printPlan9GPR(O, Rn);
    O << ")\n";
    HavePendingADRP = false;
    PendingADRPSymbol.clear();
    PendingADRPOffset = 0;
    return true;
  }
  return false;
}

bool AArch64Plan9InstPrinter::tryPrintADR(const MCInst *MI, raw_ostream &O) {
  if (MI->getOpcode() != AArch64::ADR)
    return false;
  if (MI->getNumOperands() < 2 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isExpr())
    return false;
  StringRef Sym = getReferencedSymbolName(MI->getOperand(1).getExpr());
  if (Sym.empty()) return false;
  int64_t Off = getReferencedSymbolOffset(MI->getOperand(1).getExpr());
  // c2go #321: leading `_` is part of the C symbol; do not strip.
  O << "\tMOVD $" << goSymToPlan9(Sym);
  emitSymbolOffsetSuffix(O, Off);
  O << "(SB), ";
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << "\n";
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintADDXriViaMaterializedAddress(
    const MCInst *MI, raw_ostream &O) {
  if (MI->getOpcode() != AArch64::ADDXri || MI->getNumOperands() < 3 ||
      !MI->getOperand(0).isReg() || !MI->getOperand(1).isReg() ||
      !MI->getOperand(2).isExpr() ||
      !referencesLo12(MI->getOperand(2).getExpr()))
    return false;

  StringRef Sym = getReferencedSymbolName(MI->getOperand(2).getExpr());
  if (Sym.empty())
    return false;

  MCRegister Rn = MI->getOperand(1).getReg();
  auto It = RegHoldsPage.find(canonicalXReg(Rn).id());
  // A known different symbol is evidence that this is not the ADRP companion
  // we are looking for. With no entry, the tracker may simply have been
  // cleared at a label: AArch64 address lowering still constructs PAGE and
  // PAGEOFF from the same symbol, so the base contains this symbol's full
  // address under our ADRP lowering (the same invariant used for Q loads).
  if (It != RegHoldsPage.end() && It->second != Sym)
    return false;

  MCRegister Rd = MI->getOperand(0).getReg();
  O << "\tMOVD ";
  printPlan9GPR(O, Rn);
  O << ", ";
  printPlan9GPR(O, Rd);
  O << "\n";

  invalidateRegDefs(MI);
  rememberRegHoldsPage(Rd, Sym);
  return true;
}

// Map an AArch64 LDR/STR opcode to the Plan 9 mnemonic that loads/
// stores from a labeled symbol. Returns nullptr if the opcode isn't
// a recognised LDR/STR-with-immediate-base form.
static const char *plan9LDRMnemonic(unsigned Op, bool &IsLoad) {
  switch (Op) {
  case AArch64::LDRXui:   IsLoad = true;  return "MOVD";
  case AArch64::LDRWui:   IsLoad = true;  return "MOVW";
  case AArch64::LDRBBui:  IsLoad = true;  return "MOVBU";  // byte zero-ext
  case AArch64::LDRHHui:  IsLoad = true;  return "MOVHU";  // half zero-ext
  case AArch64::LDRSBWui: IsLoad = true;  return "MOVB";   // byte signed
  case AArch64::LDRSHWui: IsLoad = true;  return "MOVH";   // half signed
  case AArch64::LDRSWui:  IsLoad = true;  return "MOVW";   // word sign-ext (Plan 9 MOVW already sign-ext to 64)
  // FP / SIMD loads: rodata FP constants reach LDR via ADRP+ADD.
  case AArch64::LDRSui:   IsLoad = true;  return "FMOVS";
  case AArch64::LDRDui:   IsLoad = true;  return "FMOVD";
  case AArch64::LDRQui:   IsLoad = true;  return "FMOVQ";
  case AArch64::STRXui:   IsLoad = false; return "MOVD";
  case AArch64::STRWui:   IsLoad = false; return "MOVW";
  case AArch64::STRBBui:  IsLoad = false; return "MOVB";
  case AArch64::STRHHui:  IsLoad = false; return "MOVH";
  case AArch64::STRSui:   IsLoad = false; return "FMOVS";
  case AArch64::STRDui:   IsLoad = false; return "FMOVD";
  case AArch64::STRQui:   IsLoad = false; return "FMOVQ";
  default: return nullptr;
  }
}

bool AArch64Plan9InstPrinter::tryPrintLDRSTRViaTrackedADRP(const MCInst *MI,
                                                            raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  bool IsLoad = false;
  const char *Mn = plan9LDRMnemonic(Op, IsLoad);
  if (!Mn) return false;
  // Operand layout for these LDR/STR-immediate forms is:
  //   op[0] = Rt  (data reg — defs for LDR, src for STR)
  //   op[1] = Rn  (base addr reg)
  //   op[2] = imm12 (byte offset / scale-encoded) OR MCExpr (`:lo12:sym`)
  if (MI->getNumOperands() < 3) return false;
  if (!MI->getOperand(0).isReg() || !MI->getOperand(1).isReg())
    return false;
  if (!MI->getOperand(2).isExpr())
    return false;
  StringRef Sym = getReferencedSymbolName(MI->getOperand(2).getExpr());
  if (Sym.empty()) return false;
  int64_t Off = getReferencedSymbolOffset(MI->getOperand(2).getExpr());
  // c2go §D2 phase 2: an LDR/STR with an MCExpr-imm operand can be one
  // of two patterns:
  //   (a) Tracked-ADRP — the base register Rn was loaded with an ADRP
  //       page address earlier in the same function. Plan 9 form is
  //       `MOVD ·sym(SB), Rt` (SB-relative, link-time resolved).
  //   (b) c2go field access — the AsmPrinter rewrote a raw imm12 byte
  //       offset into a `<Rec>_<Field>` MCSymbolRefExpr; Rn holds a
  //       runtime object pointer. Plan 9 form is `MOVW Sym(Rn), Rt`.
  //
  // Distinguish via the register-state tracker: tracked-ADRP requires
  // Rn to be a registered page-holder for `Sym`. If RegHoldsPage has
  // no entry for Rn matching this symbol, fall through so the field-
  // access printer (tryPrintLDRSTRSymbolicField) can claim the MCInst
  // instead.
  {
    auto It = RegHoldsPage.find(canonicalXReg(MI->getOperand(1).getReg()).id());
    if (It == RegHoldsPage.end() || It->second != Sym)
      return false;
  }
  // c2go #321: leading `_` is part of the C symbol; do not strip.
  StringRef Pretty = Sym;
  auto PrintRt = [&](MCRegister Rt) {
    if (!printPlan9FPR(O, Rt))
      printPlan9GPR(O, Rt);
  };
  // Plan 9 ARM64's `FMOVQ sym(SB), F0` form is rejected by the
  // assembler. For Q-reg loads/stores via tracked-ADRP, the base
  // register holds the FULL symbol address (planted by an earlier
  // ADRP[+ADD] which we lowered to `MOVD $·sym(SB), Rn`), so emit
  // register-indirect form instead. Offset isn't representable in
  // the register-indirect form — if Off is non-zero, that means an
  // upstream pass should have folded the offset into the ADRP+ADD
  // pair; warn loudly if it didn't.
  bool IsQReg = (Op == AArch64::LDRQui || Op == AArch64::STRQui);
  if (IsQReg) {
    if (Off != 0) {
      O << "\t// PLAN9-WARN: Q-reg LDR/STR via tracked-ADRP has non-zero "
        << "offset " << Off << " (sym=" << Pretty << ")\n";
    }
    if (IsLoad) {
      O << "\t" << Mn << " (";
      printPlan9GPR(O, MI->getOperand(1).getReg());
      O << "), ";
      PrintRt(MI->getOperand(0).getReg());
    } else {
      O << "\t" << Mn << " ";
      PrintRt(MI->getOperand(0).getReg());
      O << ", (";
      printPlan9GPR(O, MI->getOperand(1).getReg());
      O << ")";
    }
    O << "\n";
    return true;
  }
  if (IsLoad) {
    O << "\t" << Mn << " " << goSymToPlan9(Pretty);
    emitSymbolOffsetSuffix(O, Off);
    O << "(SB), ";
    PrintRt(MI->getOperand(0).getReg());
  } else {
    O << "\t" << Mn << " ";
    PrintRt(MI->getOperand(0).getReg());
    O << ", " << goSymToPlan9(Pretty);
    emitSymbolOffsetSuffix(O, Off);
    O << "(SB)";
  }
  O << "\n";
  return true;
}

// c2go §D2 phase 2: LDR/STR with op[2] = MCExpr where the base register
// Rn is NOT a tracked-ADRP page-holder. This is the c2go-managed
// struct-field access pattern: AArch64AsmPrinter rewrote the imm12 of
// the lowered MCInst from a raw byte offset into an MCSymbolRefExpr
// pointing at `<Rec>_<Field>` (a constant Go's compiler exports into
// `go_asm.h`). The Plan 9 syntax for this is `MOVW <Sym>(<Rn>), Rt` for
// loads and `MOVW Rt, <Sym>(<Rn>)` for stores — Rn carries the runtime
// object pointer, the symbol contributes the field offset at link time.
//
// Must run AFTER tryPrintLDRSTRViaTrackedADRP — if Rn happens to hold
// a page address, the (SB) form below would be wrong. The tracked-ADRP
// path consumes such cases first; this function only fires when the
// caller didn't.
bool AArch64Plan9InstPrinter::tryPrintLDRSTRSymbolicField(
    const MCInst *MI, raw_ostream &O) {
  // #214 restored — OOM was caused by `_c2go_union_write_barrier`
  // CALLs (now removed in A1), not by SB-relative LDR/STR emit
  // volume. Original §D2-companion semantics: symbol-bearing
  // LDR/STR that escaped tryPrintLDRSTRViaTrackedADRP emits the
  // `<sym>(SB)` form which assembles as a normal PC-relative
  // global access.
  unsigned Op = MI->getOpcode();
  bool IsLoad = false;
  const char *Mn = plan9LDRMnemonic(Op, IsLoad);
  if (!Mn) return false;
  if (MI->getNumOperands() < 3)
    return false;
  if (!MI->getOperand(0).isReg() || !MI->getOperand(1).isReg())
    return false;
  if (!MI->getOperand(2).isExpr())
    return false;
  // c2go #298 Track AO.2 — LDRQui/STRQui (128-bit Q-reg) coverage. Go's
  // assembler rejects the direct `FMOVQ sym(SB), F0` form, so the
  // `(SB)` lowering used below for the narrower widths is unavailable.
  // The one Q shape this path can prove correct is the ADRP-companion
  // `:lo12:` form: every ADRP this printer sees plants the FULL symbol
  // address in its destination register (`MOVD $·sym(SB), Rn` — both
  // tryPrintADRPPairCompletion and flushPendingADRP fold page+lo12), so
  // even when the tracker entry was dropped at a label boundary
  // (finishPending → clearRegState; the SQLite sqlite3FindInIndex
  // vectorised-loop shape: ADRP flushed before a BHS, LDRQui after the
  // following label) the register-indirect `FMOVQ (Rn), Fd` is exact.
  // A bare-SymbolRef Q access (c2go field-access rewrite — Rn holds an
  // object pointer, the symbol is a link-time field offset; see the
  // function comment above) is NOT representable register-indirect;
  // return false so the streamer's fail-closed guard aborts the compile
  // instead of shipping a silent miscompile.
  //
  // Soundness of trusting the (tracker-dropped) ADRP pairing: a
  // cross-symbol "page-sharing" pair — an ADRP of sym1's page consumed
  // by a `:lo12:sym2` access — does not exist in the current pipeline.
  // AArch64ISelLowering::getAddr builds the MO_PAGE and MO_PAGEOFF
  // operands from the SAME node (one symbol, one addend),
  // AArch64ExpandPseudoInsts expands MOVaddr* from that same machine
  // operand, and AArch64CollectLOH only attaches linker hints (it never
  // rewrites MIR operands). MachineCSE can only merge ADRPs whose
  // target operands are identical (same symbol). So a `:lo12:sym` Q
  // access implies its base register was materialised from sym itself,
  // i.e. holds sym's FULL address under this printer's ADRP→MOVD
  // lowering.
  if (Op == AArch64::LDRQui || Op == AArch64::STRQui) {
    if (!referencesLo12(MI->getOperand(2).getExpr()))
      return false;
    int64_t QOff = getReferencedSymbolOffset(MI->getOperand(2).getExpr());
    if (QOff != 0) {
      // Mirror tryPrintLDRSTRViaTrackedADRP's Q-reg policy: the MOVD
      // that planted Rn already folded `sym+N` (the ADRP's `@PAGE+N`
      // and the access's `:lo12:sym+N` carry the same N by LLVM
      // convention), so the indirect form stays exact; keep the loud
      // breadcrumb anyway.
      O << "\t// PLAN9-WARN: Q-reg LDR/STR :lo12: with non-zero offset "
        << QOff << " (sym="
        << getReferencedSymbolName(MI->getOperand(2).getExpr()) << ")\n";
    }
    if (IsLoad) {
      O << "\t" << Mn << " (";
      printPlan9GPR(O, MI->getOperand(1).getReg());
      O << "), ";
      if (!printPlan9FPR(O, MI->getOperand(0).getReg()))
        printPlan9GPR(O, MI->getOperand(0).getReg());
    } else {
      O << "\t" << Mn << " ";
      if (!printPlan9FPR(O, MI->getOperand(0).getReg()))
        printPlan9GPR(O, MI->getOperand(0).getReg());
      O << ", (";
      printPlan9GPR(O, MI->getOperand(1).getReg());
      O << ")";
    }
    O << "\n";
    return true;
  }

  StringRef Sym = getReferencedSymbolName(MI->getOperand(2).getExpr());
  if (Sym.empty()) return false;
  // We expect Go-style `<Rec>_<Field>` — bare identifier, created via
  // Ctx.getOrCreateSymbol in AArch64AsmPrinter::rewriteC2GoFieldImmToSymbol.
  // c2go #321: do NOT strip a leading `_` — Plan 9 codegen uses neutral
  // ELF mangling (no `_` global prefix), so a leading `_` is part of the
  // symbol name.
  int64_t Off = getReferencedSymbolOffset(MI->getOperand(2).getExpr());

  MCRegister Rt = MI->getOperand(0).getReg();
  MCRegister Rn = MI->getOperand(1).getReg();
  auto PrintRt = [&](MCRegister Reg) {
    if (!printPlan9FPR(O, Reg))
      printPlan9GPR(O, Reg);
  };
  // #211 — emit `<sym>(SB)` not `<sym>(<Rn>)`. Plan 9 only accepts
  // pseudo-register base for symbol-bearing memory operands. Rn is
  // dropped because the only correct interpretation of a symbol-
  // bearing LDR/STR in c2go output is a PC-relative global access,
  // and the SB form produces the equivalent linker relocation.
  //
  // goSymToPlan9 converts LLVM-style names (`name.0`/`name.1` from
  // SROA-split globals like `sqlite3Autoext.0`) to Plan 9 form
  // (`name·0`/`name·1` with U+00B7 middle-dot) — the bare `.` is
  // not a valid Plan 9 identifier character.
  std::string PrettyP9 = goSymToPlan9(Sym);
  if (IsLoad) {
    O << "\t" << Mn << " " << PrettyP9;
    if (Off != 0)
      O << "+" << Off;
    O << "(SB), ";
    PrintRt(Rt);
  } else {
    O << "\t" << Mn << " ";
    PrintRt(Rt);
    O << ", " << PrettyP9;
    if (Off != 0)
      O << "+" << Off;
    O << "(SB)";
  }
  O << "\n";
  (void)Rn;
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintLDRLiteral(const MCInst *MI,
                                                  raw_ostream &O) {
  // LDR-literal forms: Rd, label. Operand 0 = Rd, operand 1 = MCExpr.
  // Plan 9: width-suffixed MOV mnemonic loading from sym(SB).
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  switch (Op) {
  case AArch64::LDRXl:   Mn = "MOVD";  break;
  case AArch64::LDRWl:   Mn = "MOVW";  break;
  case AArch64::LDRSWl:  Mn = "MOVW";  break; // sign-extending — same Go mnemonic
  case AArch64::LDRSl:   Mn = "FMOVS"; break;
  case AArch64::LDRDl:   Mn = "FMOVD"; break;
  case AArch64::LDRQl:   Mn = "VMOVQ"; break;
  default: return false;
  }
  if (MI->getNumOperands() < 2 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isExpr())
    return false;
  StringRef Sym = getReferencedSymbolName(MI->getOperand(1).getExpr());
  if (Sym.empty()) return false;
  int64_t Off = getReferencedSymbolOffset(MI->getOperand(1).getExpr());
  // c2go #321: leading `_` is part of the C symbol; do not strip.
  O << "\t" << Mn << " " << goSymToPlan9(Sym);
  emitSymbolOffsetSuffix(O, Off);
  O << "(SB), ";
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << "\n";
  return true;
}

// #271: SP-relative LDR/STR-immediate descriptor. Maps an AArch64
// `*ui` opcode to the Go mnemonic, the access scale (byte offset =
// imm_field × Scale — LLVM stores the *unscaled* index in the imm
// operand), whether it's a load, and whether the data reg is FP/SIMD.
//
// CRITICAL: load extension width is encoded in the Go mnemonic, NOT
// the offset. `MOVW off(RSP), Rt` assembles to LDRSW (sign-extend),
// while `MOVWU` assembles to LDR-w (zero-extend). So a plain 32-bit
// `LDRWui` (zero-ext) MUST become MOVWU, and only the sign-extending
// `LDRSWui` becomes MOVW. Same MOVBU/MOVB and MOVHU/MOVH split. (The
// store side has no extension, MOVW/MOVB/MOVH stores the low bits.)
struct Plan9SPMemForm {
  const char *Mn;
  unsigned Scale;
  bool IsLoad;
  bool IsFP;
};
static bool plan9SPMemForm(unsigned Op, Plan9SPMemForm &F) {
  switch (Op) {
  case AArch64::LDRXui:   F = {"MOVD",  8,  true,  false}; return true;
  case AArch64::STRXui:   F = {"MOVD",  8,  false, false}; return true;
  case AArch64::LDRWui:   F = {"MOVWU", 4,  true,  false}; return true; // zero-ext
  case AArch64::LDRSWui:  F = {"MOVW",  4,  true,  false}; return true; // sign-ext
  case AArch64::STRWui:   F = {"MOVW",  4,  false, false}; return true;
  case AArch64::LDRBBui:  F = {"MOVBU", 1,  true,  false}; return true; // zero-ext
  case AArch64::LDRSBWui: F = {"MOVB",  1,  true,  false}; return true; // sign-ext→W
  case AArch64::LDRSBXui: F = {"MOVB",  1,  true,  false}; return true; // sign-ext→X
  case AArch64::STRBBui:  F = {"MOVB",  1,  false, false}; return true;
  case AArch64::LDRHHui:  F = {"MOVHU", 2,  true,  false}; return true; // zero-ext
  case AArch64::LDRSHWui: F = {"MOVH",  2,  true,  false}; return true; // sign-ext→W
  case AArch64::LDRSHXui: F = {"MOVH",  2,  true,  false}; return true; // sign-ext→X
  case AArch64::STRHHui:  F = {"MOVH",  2,  false, false}; return true;
  case AArch64::LDRSui:   F = {"FMOVS", 4,  true,  true};  return true;
  case AArch64::STRSui:   F = {"FMOVS", 4,  false, true};  return true;
  case AArch64::LDRDui:   F = {"FMOVD", 8,  true,  true};  return true;
  case AArch64::STRDui:   F = {"FMOVD", 8,  false, true};  return true;
  case AArch64::LDRQui:   F = {"FMOVQ", 16, true,  true};  return true;
  case AArch64::STRQui:   F = {"FMOVQ", 16, false, true};  return true;
  default: return false;
  }
}

bool AArch64Plan9InstPrinter::tryPrintSPAdjust(const MCInst *MI,
                                                raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  if (Op != AArch64::SUBXri && Op != AArch64::ADDXri)
    return false;
  // Layout: op[0]=Rd, op[1]=Rn, op[2]=imm12, op[3]=shifter.
  if (MI->getNumOperands() < 4 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isImm() ||
      !MI->getOperand(3).isImm())
    return false;
  MCRegister Rd = MI->getOperand(0).getReg();
  MCRegister Rn = MI->getOperand(1).getReg();
  // Only SP-as-source forms — Rd=Rn=SP (frame adjust) or Rd!=SP, Rn=SP
  // (SP materialisation). Rn must be SP; otherwise this is a normal
  // arithmetic op we don't translate here.
  if (Rn != AArch64::SP)
    return false;
  int64_t Imm = MI->getOperand(2).getImm();
  // shifter operand encodes the LSL amount (0 or 12). Decode via the
  // AArch64_AM helper that the standard printer uses.
  unsigned ShiftVal = AArch64_AM::getShiftValue(MI->getOperand(3).getImm());
  int64_t Bytes = Imm << ShiftVal;
  bool IsSub = (Op == AArch64::SUBXri);

  if (Rd == AArch64::SP) {
    // Per-callsite call-frame adjust: SUB/ADD $imm, RSP, RSP. This is
    // the pcsp-relevant case (#277). Go syntax: `SUB $imm, Rn, Rd`.
    O << "\t" << (IsSub ? "SUB" : "ADD") << " $" << Bytes << ", RSP, RSP\n";
    return true;
  }
  // SP materialisation into a GPR. ADD #0 is `MOV Xd, SP` → MOVD RSP, Rd.
  // (SUB-from-SP into another reg is rare and still expressible.)
  if (!IsSub && Bytes == 0) {
    O << "\tMOVD RSP, ";
    printPlan9GPR(O, Rd);
    O << "\n";
    return true;
  }
  O << "\t" << (IsSub ? "SUB" : "ADD") << " $" << Bytes << ", RSP, ";
  printPlan9GPR(O, Rd);
  O << "\n";
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintSPMemImm(const MCInst *MI,
                                                raw_ostream &O) {
  Plan9SPMemForm F;
  if (!plan9SPMemForm(MI->getOpcode(), F))
    return false;
  // Layout: op[0]=Rt, op[1]=Rn(base), op[2]=imm12 (unscaled index).
  if (MI->getNumOperands() < 3 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg())
    return false;
  if (MI->getOperand(1).getReg() != AArch64::SP)
    return false;
  // Only the plain immediate form — a symbol-bearing imm operand is a
  // tracked-ADRP / field-access case handled by the earlier printers
  // (which run before this one) and never has base=SP anyway.
  if (!MI->getOperand(2).isImm())
    return false;
  int64_t Off = MI->getOperand(2).getImm() * (int64_t)F.Scale;
  MCRegister Rt = MI->getOperand(0).getReg();
  auto PrintRt = [&](MCRegister Reg) {
    if (!printPlan9FPR(O, Reg))
      printPlan9GPR(O, Reg);
  };
  if (F.IsLoad) {
    O << "\t" << F.Mn << " " << Off << "(RSP), ";
    PrintRt(Rt);
  } else {
    O << "\t" << F.Mn << " ";
    PrintRt(Rt);
    O << ", " << Off << "(RSP)";
  }
  O << "\n";
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintSPPair(const MCInst *MI,
                                              raw_ostream &O) {
  // STP/LDP signed-offset, 64-bit, base=SP.
  //   Go: STP (Ra, Rb), off(RSP)  /  LDP off(RSP), (Ra, Rb)
  // Layout for the *i (signed offset) forms: op[0]=Rt, op[1]=Rt2,
  // op[2]=Rn(base), op[3]=imm7 (unscaled index, ×8 for X-pairs).
  unsigned Op = MI->getOpcode();
  bool IsLoad;
  if (Op == AArch64::STPXi)      IsLoad = false;
  else if (Op == AArch64::LDPXi) IsLoad = true;
  else return false;
  if (MI->getNumOperands() < 4 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isReg() ||
      !MI->getOperand(3).isImm())
    return false;
  if (MI->getOperand(2).getReg() != AArch64::SP)
    return false;
  int64_t Off = MI->getOperand(3).getImm() * 8;
  MCRegister Ra = MI->getOperand(0).getReg();
  MCRegister Rb = MI->getOperand(1).getReg();
  if (IsLoad) {
    O << "\tLDP " << Off << "(RSP), (";
    printPlan9GPR(O, Ra);
    O << ", ";
    printPlan9GPR(O, Rb);
    O << ")\n";
  } else {
    O << "\tSTP (";
    printPlan9GPR(O, Ra);
    O << ", ";
    printPlan9GPR(O, Rb);
    O << "), " << Off << "(RSP)\n";
  }
  return true;
}

// #271: general GPR-base LDR/STR with plain immediate offset (base is
// any GPR other than SP — SP is handled by tryPrintSPMemImm, which runs
// first). Plan 9 syntax: `MOVx off(Rn), Rt` (load) / `MOVx Rt, off(Rn)`
// (store). Reuses the same opcode→mnemonic+scale table as the SP form,
// so the load-extension-width rule (MOVWU zero-ext vs MOVW sign-ext,
// etc.) is identical. The Go assembler picks the scaled (LDR uimm) or
// unscaled (LDUR) encoding automatically from the byte offset, so a
// `*ui` opcode and its `*ur` cousin both map here.
bool AArch64Plan9InstPrinter::tryPrintGPRMemImm(const MCInst *MI,
                                                 raw_ostream &O) {
  Plan9SPMemForm F;
  if (!plan9SPMemForm(MI->getOpcode(), F))
    return false;
  // Layout: op[0]=Rt, op[1]=Rn(base), op[2]=imm12 (unscaled index).
  if (MI->getNumOperands() < 3 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isImm())
    return false;
  MCRegister Rn = MI->getOperand(1).getReg();
  // SP base is the tryPrintSPMemImm case (runs earlier). XZR/WZR is not a
  // valid base register here. A symbol-bearing operand is op[2].isExpr()
  // (filtered above) and handled by the tracked-ADRP / field printers.
  if (Rn == AArch64::SP || Rn == AArch64::WSP || Rn == AArch64::XZR ||
      Rn == AArch64::WZR)
    return false;
  int64_t Off = MI->getOperand(2).getImm() * (int64_t)F.Scale;
  MCRegister Rt = MI->getOperand(0).getReg();
  auto PrintRt = [&](MCRegister Reg) {
    if (!printPlan9FPR(O, Reg))
      printPlan9GPR(O, Reg);
  };
  if (F.IsLoad) {
    O << "\t" << F.Mn << " " << Off << "(";
    printPlan9GPR(O, Rn);
    O << "), ";
    PrintRt(Rt);
  } else {
    O << "\t" << F.Mn << " ";
    PrintRt(Rt);
    O << ", " << Off << "(";
    printPlan9GPR(O, Rn);
    O << ")";
  }
  O << "\n";
  return true;
}

// #271: ADD/SUB immediate on general GPRs (Rn != SP — the SP forms are
// claimed by tryPrintSPAdjust, which runs first). Go syntax reorders the
// LLVM `Rd, Rn, imm` into `$imm, Rn, Rd`; the 32-bit variants use the W
// suffix. Operand layout: op[0]=Rd, op[1]=Rn, op[2]=imm12, op[3]=shifter
// (LSL #0 or #12).
bool AArch64Plan9InstPrinter::tryPrintAddSubImm(const MCInst *MI,
                                                 raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  switch (Op) {
  case AArch64::ADDWri: Mn = "ADDW"; break;
  case AArch64::ADDXri: Mn = "ADD";  break;
  case AArch64::SUBWri: Mn = "SUBW"; break;
  case AArch64::SUBXri: Mn = "SUB";  break;
  default: return false;
  }
  if (MI->getNumOperands() < 4 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isImm() ||
      !MI->getOperand(3).isImm())
    return false;
  MCRegister Rd = MI->getOperand(0).getReg();
  MCRegister Rn = MI->getOperand(1).getReg();
  // SP-as-source / SP-as-dest forms belong to tryPrintSPAdjust.
  if (Rn == AArch64::SP || Rn == AArch64::WSP || Rd == AArch64::SP ||
      Rd == AArch64::WSP)
    return false;
  unsigned ShiftVal = AArch64_AM::getShiftValue(MI->getOperand(3).getImm());
  int64_t Imm = MI->getOperand(2).getImm() << ShiftVal;
  O << "\t" << Mn << " $" << Imm << ", ";
  printPlan9GPR(O, Rn);
  O << ", ";
  printPlan9GPR(O, Rd);
  O << "\n";
  return true;
}

// #271: ADD/SUB and bitwise logical (AND/ORR/EOR) shifted-register forms.
// Operand layout: op[0]=Rd, op[1]=Rn, op[2]=Rm, op[3]=shift (8-bit
// type+amount encoding). Go syntax: `OP Rm{<<n}, Rn, Rd` (shifted source
// first). The special case ORR Rd, ZR, Rm with no shift is the canonical
// `MOV Rm, Rd` register move — emit that shorter form.
bool AArch64Plan9InstPrinter::tryPrintArithSReg(const MCInst *MI,
                                                 raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  bool Is64 = false;
  switch (Op) {
  case AArch64::ADDWrs: Mn = "ADDW"; Is64 = false; break;
  case AArch64::ADDXrs: Mn = "ADD";  Is64 = true;  break;
  case AArch64::SUBWrs: Mn = "SUBW"; Is64 = false; break;
  case AArch64::SUBXrs: Mn = "SUB";  Is64 = true;  break;
  case AArch64::ANDWrs: Mn = "ANDW"; Is64 = false; break;
  case AArch64::ANDXrs: Mn = "AND";  Is64 = true;  break;
  case AArch64::ORRWrs: Mn = "ORRW"; Is64 = false; break;
  case AArch64::ORRXrs: Mn = "ORR";  Is64 = true;  break;
  case AArch64::EORWrs: Mn = "EORW"; Is64 = false; break;
  case AArch64::EORXrs: Mn = "EOR";  Is64 = true;  break;
  default: return false;
  }
  if (MI->getNumOperands() < 4 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isReg() ||
      !MI->getOperand(3).isImm())
    return false;
  MCRegister Rd = MI->getOperand(0).getReg();
  MCRegister Rn = MI->getOperand(1).getReg();
  MCRegister Rm = MI->getOperand(2).getReg();
  unsigned ShEnc = MI->getOperand(3).getImm();
  AArch64_AM::ShiftExtendType ShType = AArch64_AM::getShiftType(ShEnc);
  unsigned ShAmt = AArch64_AM::getShiftValue(ShEnc);
  bool NoShift = (ShAmt == 0 && ShType == AArch64_AM::LSL);
  bool RnIsZR = (Rn == AArch64::XZR || Rn == AArch64::WZR);
  // ORR Rd, ZR, Rm (no shift) == MOV Rd, Rm.
  if ((Op == AArch64::ORRWrs || Op == AArch64::ORRXrs) && RnIsZR && NoShift) {
    // Writing a W register zeroes the upper half of the corresponding X
    // register. Go's Plan 9 MOVW register form instead sign-extends (SXTW),
    // so the 32-bit alias must use MOVWU to preserve AArch64 semantics.
    O << "\t" << (Is64 ? "MOVD " : "MOVWU ");
    printPlan9GPR(O, Rm);
    O << ", ";
    printPlan9GPR(O, Rd);
    O << "\n";
    return true;
  }
  // Only the no-shift register form is emitted directly; shifted forms
  // need the `Rm<<n` / `Rm>>n` / `Rm->n` (ASR) operand syntax which we
  // leave to a later extension — fall back to WORD for those.
  if (!NoShift)
    return false;
  O << "\t" << Mn << " ";
  printPlan9GPR(O, Rm);
  O << ", ";
  printPlan9GPR(O, Rn);
  O << ", ";
  printPlan9GPR(O, Rd);
  O << "\n";
  return true;
}

// #271: flag-setting SUBS/ADDS (immediate or no-shift register). When the
// destination is the zero register this is a bare CMP/CMN — Go syntax
// `CMP src, Rn`. When it keeps a result it stays a SUBS/ADDS that writes
// Rd — Go syntax `SUBS src, Rn, Rd` (source first, dest last). 32-bit
// forms take the W suffix.
bool AArch64Plan9InstPrinter::tryPrintCmp(const MCInst *MI, raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  // CmpMn = mnemonic when Rd is ZR; ResMn = mnemonic when Rd is kept.
  const char *CmpMn = nullptr, *ResMn = nullptr;
  bool IsImm = false;
  switch (Op) {
  case AArch64::SUBSWri: CmpMn = "CMPW"; ResMn = "SUBSW"; IsImm = true;  break;
  case AArch64::SUBSXri: CmpMn = "CMP";  ResMn = "SUBS";  IsImm = true;  break;
  case AArch64::ADDSWri: CmpMn = "CMNW"; ResMn = "ADDSW"; IsImm = true;  break;
  case AArch64::ADDSXri: CmpMn = "CMN";  ResMn = "ADDS";  IsImm = true;  break;
  case AArch64::SUBSWrs: CmpMn = "CMPW"; ResMn = "SUBSW"; IsImm = false; break;
  case AArch64::SUBSXrs: CmpMn = "CMP";  ResMn = "SUBS";  IsImm = false; break;
  case AArch64::ADDSWrs: CmpMn = "CMNW"; ResMn = "ADDSW"; IsImm = false; break;
  case AArch64::ADDSXrs: CmpMn = "CMN";  ResMn = "ADDS";  IsImm = false; break;
  default: return false;
  }
  if (MI->getNumOperands() < 1 || !MI->getOperand(0).isReg())
    return false;
  MCRegister Rd = MI->getOperand(0).getReg();
  bool RdIsZR = (Rd == AArch64::WZR || Rd == AArch64::XZR);
  if (IsImm) {
    // op[1]=Rn, op[2]=imm12, op[3]=shifter.
    if (MI->getNumOperands() < 4 || !MI->getOperand(1).isReg() ||
        !MI->getOperand(2).isImm() || !MI->getOperand(3).isImm())
      return false;
    unsigned ShiftVal = AArch64_AM::getShiftValue(MI->getOperand(3).getImm());
    int64_t Imm = MI->getOperand(2).getImm() << ShiftVal;
    O << "\t" << (RdIsZR ? CmpMn : ResMn) << " $" << Imm << ", ";
    printPlan9GPR(O, MI->getOperand(1).getReg());
    if (!RdIsZR) { O << ", "; printPlan9GPR(O, Rd); }
    O << "\n";
    return true;
  }
  // Register form: op[1]=Rn, op[2]=Rm, op[3]=shift. Only no-shift emitted.
  if (MI->getNumOperands() < 4 || !MI->getOperand(1).isReg() ||
      !MI->getOperand(2).isReg() || !MI->getOperand(3).isImm())
    return false;
  unsigned ShEnc = MI->getOperand(3).getImm();
  if (AArch64_AM::getShiftValue(ShEnc) != 0 ||
      AArch64_AM::getShiftType(ShEnc) != AArch64_AM::LSL)
    return false;
  O << "\t" << (RdIsZR ? CmpMn : ResMn) << " ";
  printPlan9GPR(O, MI->getOperand(2).getReg());
  O << ", ";
  printPlan9GPR(O, MI->getOperand(1).getReg());
  if (!RdIsZR) { O << ", "; printPlan9GPR(O, Rd); }
  O << "\n";
  return true;
}

// #271: MOVZ (move wide with zero) — load a 16-bit immediate, optionally
// shifted, with all other bits cleared. Go expresses this as
// `MOVD $value, Rd` (`MOVW` for the 32-bit form); the assembler selects
// the MOVZ/MOVN/MOVK sequence. Because MOVZ zeroes the rest of the
// register, `value = imm << shift` is the exact final register content.
// Operand layout: op[0]=Rd, op[1]=imm16, op[2]=shift (0/16/32/48).
bool AArch64Plan9InstPrinter::tryPrintMovImm(const MCInst *MI,
                                              raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  bool Is64 = false;
  if (Op == AArch64::MOVZWi)      Is64 = false;
  else if (Op == AArch64::MOVZXi) Is64 = true;
  else return false;
  if (MI->getNumOperands() < 3 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isImm() || !MI->getOperand(2).isImm())
    return false;
  uint64_t Imm = (uint64_t)MI->getOperand(1).getImm();
  unsigned Shift = (unsigned)MI->getOperand(2).getImm();
  uint64_t Value = Imm << Shift;
  O << "\t" << (Is64 ? "MOVD" : "MOVW") << " $" << Value << ", ";
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << "\n";
  return true;
}

// #271 round-2: STP/LDP X/W/S/D/Q with GPR base (non-SP, signed-offset form).
// Plan 9 Go-asm syntax: `STP (Ra, Rb), $off(Rn)` / `LDP $off(Rn), (Ra, Rb)`.
// X-width keeps the bare mnemonic; W-width uses the `STPW`/`LDPW` mnemonic.
// FP-pair (S/D/Q) — Plan 9 ARM64 lacks a pair mnemonic for vector regs (Go
// runtime never spills FP pairs) so we don't claim them yet; FPR pair forms
// remain WORD fallback.
//
// Operand layout for the *i signed-offset forms:
//   op[0] = Rt   (first data reg)
//   op[1] = Rt2  (second data reg)
//   op[2] = Rn   (base addr reg)
//   op[3] = imm7 (UNSCALED index — already scaled by LLVM tablegen).
//
// CRITICAL: AArch64 stores the SCALED imm in op[3] (LLVM uses simm7sN
// operand types where N=4 for W/S, N=8 for X/D, N=16 for Q), so the byte
// offset is op[3] × scale.
bool AArch64Plan9InstPrinter::tryPrintGPRPair(const MCInst *MI,
                                               raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  unsigned Scale = 0;
  bool IsLoad = false;
  bool IsFP = false;
  switch (Op) {
  case AArch64::STPXi:  Mn = "STP";  Scale = 8;  IsLoad = false; IsFP = false; break;
  case AArch64::LDPXi:  Mn = "LDP";  Scale = 8;  IsLoad = true;  IsFP = false; break;
  case AArch64::STPWi:  Mn = "STPW"; Scale = 4;  IsLoad = false; IsFP = false; break;
  case AArch64::LDPWi:  Mn = "LDPW"; Scale = 4;  IsLoad = true;  IsFP = false; break;
  // FP-pair: Go assembler has no direct STP-FPR / LDP-FPR mnemonic, so
  // these stay WORD-fallback. (Listed here only as documentation.)
  default: return false;
  }
  if (MI->getNumOperands() < 4 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isReg() ||
      !MI->getOperand(3).isImm())
    return false;
  MCRegister Rn = MI->getOperand(2).getReg();
  // SP-base is already handled by tryPrintSPPair (runs earlier). FP-base
  // form for SP is captured there; this function handles GPR base.
  if (Rn == AArch64::SP)
    return false;
  int64_t Off = MI->getOperand(3).getImm() * (int64_t)Scale;
  MCRegister Ra = MI->getOperand(0).getReg();
  MCRegister Rb = MI->getOperand(1).getReg();
  auto PrintRt = [&](MCRegister Reg) {
    if (IsFP) { printPlan9FPR(O, Reg); }
    else      { printPlan9GPR(O, Reg); }
  };
  if (IsLoad) {
    O << "\t" << Mn << " " << Off << "(";
    printPlan9GPR(O, Rn);
    O << "), (";
    PrintRt(Ra);
    O << ", ";
    PrintRt(Rb);
    O << ")\n";
  } else {
    O << "\t" << Mn << " (";
    PrintRt(Ra);
    O << ", ";
    PrintRt(Rb);
    O << "), " << Off << "(";
    printPlan9GPR(O, Rn);
    O << ")\n";
  }
  return true;
}

// #271 round-2: LDUR/STUR — unscaled signed 9-bit offset loads/stores.
// Same data-reg widths as the *ui (scaled imm12) forms but with negative
// or non-scale-aligned offsets. Plan 9 Go asm uses the SAME mnemonic
// (`MOVD off(Rn), Rt` etc.) — the assembler picks LDR-uimm12 vs LDUR-imm9
// automatically based on the byte offset.
//
// Operand layout for the *i unscaled forms:
//   op[0] = Rt  (data reg)
//   op[1] = Rn  (base addr reg)
//   op[2] = imm9 (BYTE offset, signed — no scaling).
struct Plan9UnscaledMemForm {
  const char *Mn;
  bool IsLoad;
  bool IsFP;
};
static bool plan9UnscaledMemForm(unsigned Op, Plan9UnscaledMemForm &F) {
  switch (Op) {
  case AArch64::LDURXi:   F = {"MOVD",  true,  false}; return true;
  case AArch64::STURXi:   F = {"MOVD",  false, false}; return true;
  case AArch64::LDURWi:   F = {"MOVWU", true,  false}; return true; // zero-ext
  case AArch64::LDURSWi:  F = {"MOVW",  true,  false}; return true; // sign-ext
  case AArch64::STURWi:   F = {"MOVW",  false, false}; return true;
  case AArch64::LDURBBi:  F = {"MOVBU", true,  false}; return true;
  case AArch64::LDURSBWi: F = {"MOVB",  true,  false}; return true;
  case AArch64::LDURSBXi: F = {"MOVB",  true,  false}; return true;
  case AArch64::STURBBi:  F = {"MOVB",  false, false}; return true;
  case AArch64::LDURHHi:  F = {"MOVHU", true,  false}; return true;
  case AArch64::LDURSHWi: F = {"MOVH",  true,  false}; return true;
  case AArch64::LDURSHXi: F = {"MOVH",  true,  false}; return true;
  case AArch64::STURHHi:  F = {"MOVH",  false, false}; return true;
  case AArch64::LDURSi:   F = {"FMOVS", true,  true};  return true;
  case AArch64::STURSi:   F = {"FMOVS", false, true};  return true;
  case AArch64::LDURDi:   F = {"FMOVD", true,  true};  return true;
  case AArch64::STURDi:   F = {"FMOVD", false, true};  return true;
  case AArch64::LDURQi:   F = {"FMOVQ", true,  true};  return true;
  case AArch64::STURQi:   F = {"FMOVQ", false, true};  return true;
  default: return false;
  }
}

bool AArch64Plan9InstPrinter::tryPrintGPRMemUnscaled(const MCInst *MI,
                                                      raw_ostream &O) {
  Plan9UnscaledMemForm F;
  if (!plan9UnscaledMemForm(MI->getOpcode(), F))
    return false;
  // op[0]=Rt, op[1]=Rn (base, may be SP), op[2]=imm9 (byte offset).
  if (MI->getNumOperands() < 3 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isImm())
    return false;
  MCRegister Rt = MI->getOperand(0).getReg();
  MCRegister Rn = MI->getOperand(1).getReg();
  int64_t Off = MI->getOperand(2).getImm(); // signed byte offset, no scale
  auto PrintBase = [&]() {
    if (Rn == AArch64::SP || Rn == AArch64::WSP) O << "RSP";
    else printPlan9GPR(O, Rn);
  };
  auto PrintRt = [&](MCRegister Reg) {
    if (!printPlan9FPR(O, Reg))
      printPlan9GPR(O, Reg);
  };
  if (F.IsLoad) {
    O << "\t" << F.Mn << " " << Off << "(";
    PrintBase();
    O << "), ";
    PrintRt(Rt);
  } else {
    O << "\t" << F.Mn << " ";
    PrintRt(Rt);
    O << ", " << Off << "(";
    PrintBase();
    O << ")";
  }
  O << "\n";
  return true;
}

// #271 round-2: MOVK/MOVN — 16-bit immediate keep / negated.
//   MOVK: writes imm<<shift into bits [shift+15:shift], keeping the rest.
//         Plan 9: `MOVK $(imm<<shift), Rd`  (W variant: `MOVKW`).
//   MOVN: writes ~(imm<<shift) into the full register.
//         Plan 9: `MOVD $value, Rd` — let the assembler pick MOVN/MOVZ.
//         (The MOVN immediate is well-defined for any final 64-bit value
//         expressible as a single bitwise complement of `imm<<shift`.)
//
// MOVZ is already handled by tryPrintMovImm (runs earlier).
//
// Operand layout:
//   MOVN: op[0]=Rd, op[1]=imm16, op[2]=shift  (3 operands, no source-tie).
//   MOVK: op[0]=Rd, op[1]=src(tied=Rd), op[2]=imm16, op[3]=shift  (4 ops —
//         destination is also a SOURCE because MOVK keeps the other bits).
bool AArch64Plan9InstPrinter::tryPrintMovWide(const MCInst *MI,
                                               raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  bool Is64 = false;
  bool IsMOVK = false;
  switch (Op) {
  case AArch64::MOVKWi: Is64 = false; IsMOVK = true;  break;
  case AArch64::MOVKXi: Is64 = true;  IsMOVK = true;  break;
  case AArch64::MOVNWi: Is64 = false; IsMOVK = false; break;
  case AArch64::MOVNXi: Is64 = true;  IsMOVK = false; break;
  default: return false;
  }
  // MOVK has the tied-source operand at op[1], so imm/shift live at op[2]/[3];
  // MOVN has imm/shift at op[1]/[2].
  unsigned ImmIdx  = IsMOVK ? 2 : 1;
  unsigned ShiftIdx = IsMOVK ? 3 : 2;
  if (MI->getNumOperands() <= ShiftIdx || !MI->getOperand(0).isReg() ||
      !MI->getOperand(ImmIdx).isImm() || !MI->getOperand(ShiftIdx).isImm())
    return false;
  uint64_t Imm = (uint64_t)MI->getOperand(ImmIdx).getImm();
  unsigned Shift = (unsigned)MI->getOperand(ShiftIdx).getImm();
  if (IsMOVK) {
    // MOVK keeps existing bits; emit the literal shifted-imm operand.
    uint64_t Shifted = Imm << Shift;
    // Go assembler rejects `MOVK $0, Rd` ("zero shifts cannot be handled
    // correctly"). The encoding is legal but a semantic no-op (Rd
    // unchanged); fall back to WORD so the raw bytes still emit.
    if (Shifted == 0)
      return false;
    O << "\t" << (Is64 ? "MOVK" : "MOVKW") << " $" << Shifted << ", ";
    printPlan9GPR(O, MI->getOperand(0).getReg());
    O << "\n";
    return true;
  }
  // MOVN — bitwise-complemented full register value.
  uint64_t Shifted = Imm << Shift;
  if (Is64) {
    uint64_t Value = ~Shifted;
    O << "\tMOVD $" << (int64_t)Value << ", ";
  } else {
    // W-form: zero-extends the 32-bit complement into the 64-bit reg.
    uint32_t Value = ~(uint32_t)Shifted;
    O << "\tMOVW $" << (int64_t)(int32_t)Value << ", ";
  }
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << "\n";
  return true;
}

// #271 round-2: TST — ANDS Rd=ZR, Rn, Rm/imm alias.
//   Plan 9 syntax:
//     TST  imm/Rm, Rn          (X-form)
//     TSTW imm/Rm, Rn          (W-form)
//
// Mirrors how tryPrintCmp emits CMP/CMN (the SUBS/ADDS dest-is-ZR alias):
// when ANDSWri/ANDSXri / ANDSWrs/ANDSXrs has Rd == ZR, this is the TST
// alias. Bitmask-immediate decoding uses AArch64_AM::decodeLogicalImmediate;
// for the shifted-register form we only emit the no-shift case (the
// shifted-source operand syntax isn't worth carrying yet).
bool AArch64Plan9InstPrinter::tryPrintTST(const MCInst *MI, raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  bool Is64 = false;
  bool IsImm = false;
  switch (Op) {
  case AArch64::ANDSWri: Is64 = false; IsImm = true;  break;
  case AArch64::ANDSXri: Is64 = true;  IsImm = true;  break;
  case AArch64::ANDSWrs: Is64 = false; IsImm = false; break;
  case AArch64::ANDSXrs: Is64 = true;  IsImm = false; break;
  default: return false;
  }
  if (MI->getNumOperands() < 1 || !MI->getOperand(0).isReg())
    return false;
  MCRegister Rd = MI->getOperand(0).getReg();
  // Only the Rd=ZR (TST alias) case — non-ZR keeps the ANDS result and
  // would need an ANDS/ANDSW emit pattern which we don't add here.
  if (Rd != AArch64::XZR && Rd != AArch64::WZR)
    return false;
  const char *Mn = Is64 ? "TST" : "TSTW";
  if (IsImm) {
    // op[1]=Rn, op[2]=logical-imm encoding.
    if (MI->getNumOperands() < 3 || !MI->getOperand(1).isReg() ||
        !MI->getOperand(2).isImm())
      return false;
    uint64_t LogImm = (uint64_t)MI->getOperand(2).getImm();
    uint64_t Decoded =
        AArch64_AM::decodeLogicalImmediate(LogImm, Is64 ? 64 : 32);
    O << "\t" << Mn << " $" << Decoded << ", ";
    printPlan9GPR(O, MI->getOperand(1).getReg());
    O << "\n";
    return true;
  }
  // Shifted-register form: op[1]=Rn, op[2]=Rm, op[3]=shift encoding.
  if (MI->getNumOperands() < 4 || !MI->getOperand(1).isReg() ||
      !MI->getOperand(2).isReg() || !MI->getOperand(3).isImm())
    return false;
  unsigned ShEnc = MI->getOperand(3).getImm();
  if (AArch64_AM::getShiftValue(ShEnc) != 0 ||
      AArch64_AM::getShiftType(ShEnc) != AArch64_AM::LSL)
    return false;
  O << "\t" << Mn << " ";
  printPlan9GPR(O, MI->getOperand(2).getReg());
  O << ", ";
  printPlan9GPR(O, MI->getOperand(1).getReg());
  O << "\n";
  return true;
}

// #271 round-2: SXTB / SXTH / SXTW — sign-extend aliases of SBFM.
// Plan 9: `SXTB Rn, Rd` / `SXTH Rn, Rd` / `SXTW Rn, Rd`. Source operand
// is always the W form even when the SBFM record is X-typed (the
// instruction reads the low bits of the X reg, which Plan 9 names the
// same as the X reg — no W prefix in Go asm).
//
// Decoded from SBFMWri/SBFMXri whose (immr, imms) = (0, 7) | (0, 15) |
// (0, 31). All other SBFM patterns (UBFM/UBFX/EXTR-as-LSL etc.) stay
// WORD fallback for now.
bool AArch64Plan9InstPrinter::tryPrintSXT(const MCInst *MI, raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  if (Op != AArch64::SBFMWri && Op != AArch64::SBFMXri)
    return false;
  if (MI->getNumOperands() < 4 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isImm() ||
      !MI->getOperand(3).isImm())
    return false;
  unsigned Immr = (unsigned)MI->getOperand(2).getImm();
  unsigned Imms = (unsigned)MI->getOperand(3).getImm();
  if (Immr != 0) return false;
  const char *Mn = nullptr;
  if (Imms == 7)       Mn = "SXTB";
  else if (Imms == 15) Mn = "SXTH";
  else if (Imms == 31 && Op == AArch64::SBFMXri) Mn = "SXTW";
  else return false;
  O << "\t" << Mn << " ";
  printPlan9GPR(O, MI->getOperand(1).getReg());
  O << ", ";
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << "\n";
  return true;
}

// #271 round-3: AND/ORR/EOR with bitmask immediate.
// LLVM ANDWri/ANDXri/ORRWri/ORRXri/EORWri/EORXri: operand layout =
//   op[0]=Rd, op[1]=Rn, op[2]=logical-imm encoding.
// Plan 9 Go syntax: `AND $imm, Rn, Rd` (with `ANDW`/`ORRW`/`EORW` W-form).
// The bitmask encoding decodes via AArch64_AM::decodeLogicalImmediate.
//
// Note: ORR Rd, ZR, $imm is the canonical `MOV Rd, #imm` (logical-imm).
// Plan 9 also accepts `MOVD $imm, Rd` for these patterns and the Go
// assembler picks the shorter encoding; we still emit the explicit
// `ORR $imm, ZR, Rd` form, which is valid syntax.
bool AArch64Plan9InstPrinter::tryPrintLogicalImm(const MCInst *MI,
                                                  raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  bool Is64 = false;
  switch (Op) {
  case AArch64::ANDWri: Mn = "ANDW"; Is64 = false; break;
  case AArch64::ANDXri: Mn = "AND";  Is64 = true;  break;
  case AArch64::ORRWri: Mn = "ORRW"; Is64 = false; break;
  case AArch64::ORRXri: Mn = "ORR";  Is64 = true;  break;
  case AArch64::EORWri: Mn = "EORW"; Is64 = false; break;
  case AArch64::EORXri: Mn = "EOR";  Is64 = true;  break;
  default: return false;
  }
  if (MI->getNumOperands() < 3 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isImm())
    return false;
  uint64_t LogImm = (uint64_t)MI->getOperand(2).getImm();
  uint64_t Decoded =
      AArch64_AM::decodeLogicalImmediate(LogImm, Is64 ? 64 : 32);
  // Print as signed for natural readability on widely-set masks (e.g. ~7
  // shows up as -8) — matches Go runtime asm conventions (`AND $~7, R13`).
  // Use uint64 raw value; the assembler accepts both forms equivalently.
  O << "\t" << Mn << " $" << Decoded << ", ";
  printPlan9GPR(O, MI->getOperand(1).getReg());
  O << ", ";
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << "\n";
  return true;
}

// #271 round-3: CSEL/CSELW — conditional select.
// LLVM CSELWr/CSELXr operand layout: op[0]=Rd, op[1]=Rn, op[2]=Rm, op[3]=cond.
// Plan 9 Go syntax: `CSEL <cond>, Rn, Rm, Rd` (cond is a bare symbolic name
// EQ/NE/HS/LO/MI/PL/VS/VC/HI/LS/GE/LT/GT/LE/AL/NV — same order as the
// AArch64 cond imm 0..15).
bool AArch64Plan9InstPrinter::tryPrintCSEL(const MCInst *MI, raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  if (Op == AArch64::CSELWr)      Mn = "CSELW";
  else if (Op == AArch64::CSELXr) Mn = "CSEL";
  else return false;
  if (MI->getNumOperands() < 4 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isReg() ||
      !MI->getOperand(3).isImm())
    return false;
  unsigned Cond = (unsigned)MI->getOperand(3).getImm() & 0xf;
  static const char *CondNames[16] = {
      "EQ","NE","HS","LO","MI","PL","VS","VC",
      "HI","LS","GE","LT","GT","LE","AL","NV"};
  O << "\t" << Mn << " " << CondNames[Cond] << ", ";
  printPlan9GPR(O, MI->getOperand(1).getReg());
  O << ", ";
  printPlan9GPR(O, MI->getOperand(2).getReg());
  O << ", ";
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << "\n";
  return true;
}

// #271 round-4: multiply-accumulate family — MADD / SMADDL / UMADDL /
// MSUB / SMSUBL / UMSUBL.
// LLVM operand layout (all six share `(Rd, Rn, Rm, Ra)`):
//   op[0]=Rd, op[1]=Rn, op[2]=Rm, op[3]=Ra.
// Plan 9 Go syntax: `MADD Rm, Ra, Rn, Rd` — operand order op[2], op[3],
// op[1], op[0]. Validated against go tool asm encoding (assembled-then-
// objdump round-trip). The Go ARM64 asm doc comment `// MADD Rn,Rm,Ra,Rd`
// is misleading; the actual emitted-order names are Rm, Ra, Rn, Rd.
bool AArch64Plan9InstPrinter::tryPrintMulAcc(const MCInst *MI,
                                              raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  switch (Op) {
  case AArch64::MADDWrrr:   Mn = "MADDW";  break;
  case AArch64::MADDXrrr:   Mn = "MADD";   break;
  case AArch64::MSUBWrrr:   Mn = "MSUBW";  break;
  case AArch64::MSUBXrrr:   Mn = "MSUB";   break;
  case AArch64::SMADDLrrr:  Mn = "SMADDL"; break;
  case AArch64::SMSUBLrrr:  Mn = "SMSUBL"; break;
  case AArch64::UMADDLrrr:  Mn = "UMADDL"; break;
  case AArch64::UMSUBLrrr:  Mn = "UMSUBL"; break;
  default: return false;
  }
  if (MI->getNumOperands() < 4 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isReg() ||
      !MI->getOperand(3).isReg())
    return false;
  // Go emit order: Rm (op2), Ra (op3), Rn (op1), Rd (op0).
  O << "\t" << Mn << " ";
  printPlan9GPR(O, MI->getOperand(2).getReg());
  O << ", ";
  printPlan9GPR(O, MI->getOperand(3).getReg());
  O << ", ";
  printPlan9GPR(O, MI->getOperand(1).getReg());
  O << ", ";
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << "\n";
  return true;
}

// #271 round-4: FMOV between GPR and FPR (D/S widths).
// LLVM opcodes & operand layout:
//   FMOVDXr  (GPR→FPR-D): op[0]=FPR(Rd), op[1]=GPR(Rn)
//   FMOVXDr  (FPR-D→GPR): op[0]=GPR(Rd), op[1]=FPR(Rn)
//   FMOVSWr  (GPR→FPR-S): op[0]=FPR(Rd), op[1]=GPR(Rn)
//   FMOVWSr  (FPR-S→GPR): op[0]=GPR(Rd), op[1]=FPR(Rn)
//   FMOVDr   (FPR-D→FPR-D, register move)
//   FMOVSr   (FPR-S→FPR-S, register move)
// Plan 9 Go syntax for all: `FMOVD src, dst` / `FMOVS src, dst`.
// (FMOVDXHighr / FMOVXDHighr — high-half of v.d[1] — are NOT handled here;
//  they need a `V<n>.D[1]` operand syntax we don't synthesize. Rare enough
//  to leave as WORD.)
bool AArch64Plan9InstPrinter::tryPrintFMovGPR(const MCInst *MI,
                                               raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  switch (Op) {
  case AArch64::FMOVDXr:
  case AArch64::FMOVXDr:
  case AArch64::FMOVDr:    Mn = "FMOVD"; break;
  case AArch64::FMOVSWr:
  case AArch64::FMOVWSr:
  case AArch64::FMOVSr:    Mn = "FMOVS"; break;
  default: return false;
  }
  if (MI->getNumOperands() < 2 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg())
    return false;
  MCRegister Rd = MI->getOperand(0).getReg();
  MCRegister Rn = MI->getOperand(1).getReg();
  auto PrintReg = [&](MCRegister Reg) {
    if (!printPlan9FPR(O, Reg))
      printPlan9GPR(O, Reg);
  };
  O << "\t" << Mn << " ";
  PrintReg(Rn);   // src
  O << ", ";
  PrintReg(Rd);   // dst
  O << "\n";
  return true;
}

// #271 round-5: CSINC alias family — `CSET <cond>, Rd` (Rn=Rm=ZR, the
// emitted cond is the INVERSE of the CSINC field cond) and
// `CINC <cond>, Rn, Rd` (Rn==Rm).
// LLVM CSINCWr/CSINCXr operand layout: (Rd, Rn, Rm, cond_imm).
// Plan 9 syntax:
//   `CSET   <cond>, Rd`       — X form, condition is inv(LLVM cond_imm)
//   `CSETW  <cond>, Rd`       — W form, same inversion rule
//   `CINC   <cond>, Rn, Rd`   — X form
//   `CINCW  <cond>, Rn, Rd`   — W form
// (CSINC's plain Rn!=Rm,!=ZR form stays WORD — small share of remaining.)
bool AArch64Plan9InstPrinter::tryPrintCSINC(const MCInst *MI, raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  bool Is64 = false;
  if (Op == AArch64::CSINCWr)      Is64 = false;
  else if (Op == AArch64::CSINCXr) Is64 = true;
  else return false;
  if (MI->getNumOperands() < 4 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isReg() || !MI->getOperand(2).isReg() ||
      !MI->getOperand(3).isImm())
    return false;
  MCRegister Rd = MI->getOperand(0).getReg();
  MCRegister Rn = MI->getOperand(1).getReg();
  MCRegister Rm = MI->getOperand(2).getReg();
  unsigned Cond = (unsigned)MI->getOperand(3).getImm() & 0xf;
  static const char *CondNames[16] = {
      "EQ","NE","HS","LO","MI","PL","VS","VC",
      "HI","LS","GE","LT","GT","LE","AL","NV"};
  bool RnIsZR = (Rn == AArch64::XZR || Rn == AArch64::WZR);
  bool RmIsZR = (Rm == AArch64::XZR || Rm == AArch64::WZR);
  // CSET: CSINC with Rn=Rm=ZR. The user-visible condition is the inverse
  // of the encoded cond (cond ^ 1).
  if (RnIsZR && RmIsZR) {
    unsigned InvCond = Cond ^ 1;
    O << "\t" << (Is64 ? "CSET" : "CSETW") << " " << CondNames[InvCond] << ", ";
    printPlan9GPR(O, Rd);
    O << "\n";
    return true;
  }
  // CINC: CSINC with Rn==Rm and neither is ZR; user-visible cond is the
  // inverse of the encoded cond.
  // Both registers are GPR-32 or GPR-64 depending on Is64; compare by id().
  if (Rn == Rm && !RnIsZR) {
    unsigned InvCond = Cond ^ 1;
    O << "\t" << (Is64 ? "CINC" : "CINCW") << " " << CondNames[InvCond] << ", ";
    printPlan9GPR(O, Rn);
    O << ", ";
    printPlan9GPR(O, Rd);
    O << "\n";
    return true;
  }
  // Plain CSINC: not currently emitted in Go-syntax form.
  return false;
}

bool AArch64Plan9InstPrinter::tryPrintBranchCall(const MCInst *MI,
                                                  raw_ostream &O) {
  if (MI->getOpcode() != AArch64::BL)
    return false;
  if (MI->getNumOperands() < 1 || !MI->getOperand(0).isExpr())
    return false;
  StringRef Sym = getReferencedSymbolName(MI->getOperand(0).getExpr());
  if (Sym.empty()) return false;
  // c2go #321: leading `_` is part of the C symbol; do not strip.
  O << "\tCALL " << goSymToPlan9(Sym) << "(SB)\n";
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintIndirectCall(const MCInst *MI,
                                                    raw_ostream &O) {
  // BLR Xn → Go `CALL (Rn)`; BR Xn → Go `JMP (Rn)`. These MUST use the
  // Go register-indirect pseudo-ops, NOT a raw-byte WORD: the Go
  // assembler has to recognize a register-indirect BLR as a *call* so it
  // does not treat a function whose only calls are indirect as a leaf —
  // otherwise the epilogue skips reloading LR and the trailing `RET`
  // returns into the BLR-clobbered LR (infinite self-jump). See #275.
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  if (Op == AArch64::BLR)
    Mn = "CALL";
  else if (Op == AArch64::BR)
    Mn = "JMP";
  else
    return false;
  if (MI->getNumOperands() < 1 || !MI->getOperand(0).isReg())
    return false;
  O << "\t" << Mn << " (";
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << ")\n";
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintUnconditionalBranch(const MCInst *MI,
                                                           raw_ostream &O) {
  if (MI->getOpcode() != AArch64::B)
    return false;
  if (MI->getNumOperands() < 1 || !MI->getOperand(0).isExpr())
    return false;
  // Local label vs external symbol — tail-call B to external target
  // needs the `(SB)` suffix; local block branch does not.
  StringRef Sym = getReferencedSymbolName(MI->getOperand(0).getExpr());
  if (!Sym.empty() && !isLocalLabelName(Sym)) {
    // c2go #321: leading `_` is part of the C symbol; do not strip.
    O << "\tJMP " << goSymToPlan9(Sym) << "(SB)\n";
    return true;
  }
  O << "\tJMP " << formatBranchTarget(MI->getOperand(0).getExpr()) << "\n";
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintConditionalBranch(const MCInst *MI,
                                                         raw_ostream &O) {
  if (MI->getOpcode() != AArch64::Bcc)
    return false;
  if (MI->getNumOperands() < 2 || !MI->getOperand(0).isImm() ||
      !MI->getOperand(1).isExpr())
    return false;
  unsigned Cond = (unsigned)MI->getOperand(0).getImm();
  const char *Mn = plan9CondMnemonic(Cond);
  if (!Mn) return false;
  O << "\t" << Mn << " "
    << formatBranchTarget(MI->getOperand(1).getExpr()) << "\n";
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintCBZBranch(const MCInst *MI,
                                                 raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  switch (Op) {
  case AArch64::CBZW:  Mn = "CBZW";  break;
  case AArch64::CBZX:  Mn = "CBZ";   break;
  case AArch64::CBNZW: Mn = "CBNZW"; break;
  case AArch64::CBNZX: Mn = "CBNZ";  break;
  default: return false;
  }
  if (MI->getNumOperands() < 2 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isExpr())
    return false;
  O << "\t" << Mn << " ";
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << ", " << formatBranchTarget(MI->getOperand(1).getExpr()) << "\n";
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintTBZBranch(const MCInst *MI,
                                                 raw_ostream &O) {
  unsigned Op = MI->getOpcode();
  const char *Mn = nullptr;
  switch (Op) {
  case AArch64::TBZW:
  case AArch64::TBZX:  Mn = "TBZ";  break;
  case AArch64::TBNZW:
  case AArch64::TBNZX: Mn = "TBNZ"; break;
  default: return false;
  }
  if (MI->getNumOperands() < 3 || !MI->getOperand(0).isReg() ||
      !MI->getOperand(1).isImm() || !MI->getOperand(2).isExpr())
    return false;
  // Plan 9 arm64 syntax for TBZ/TBNZ: `TBZ $bit, Rn, target`. Note
  // bit is FIRST, register SECOND — opposite of ARM toolchain order.
  O << "\t" << Mn << " $" << MI->getOperand(1).getImm() << ", ";
  printPlan9GPR(O, MI->getOperand(0).getReg());
  O << ", " << formatBranchTarget(MI->getOperand(2).getExpr()) << "\n";
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintRet(const MCInst *MI, raw_ostream &O) {
  if (MI->getOpcode() != AArch64::RET)
    return false;
  O << "\tRET\n";
  return true;
}

bool AArch64Plan9InstPrinter::tryPrintInst(const MCInst *MI,
                                            const MCSubtargetInfo & /*STI*/,
                                            raw_ostream &O) {
  // ADRP pair completion runs FIRST — if buffered ADRP pairs with
  // this instruction, the whole pair becomes one MOVD and the
  // current MI is consumed.
  if (tryPrintADRPPairCompletion(MI, O))
    return true;
  // If a pending ADRP didn't pair with this instruction, flush it as
  // a standalone address-of before processing.
  if (HavePendingADRP)
    flushPendingADRP(O);

  // Address-aware instruction families. The hoisted-ADRP form must
  // run BEFORE we invalidate (an LDR Rt uses base register Rn, but
  // also defs Rt — if Rt happens to be the same as some tracked
  // page-holder, the def would erase the entry before we check).
  if (tryPrintLDRSTRViaTrackedADRP(MI, O)) {
    invalidateRegDefs(MI);
    return true;
  }
  // c2go §D2 phase 2: LDR/STR whose imm12 was rewritten to a
  // `<Rec>_<Field>` MCSymbolRefExpr by AArch64AsmPrinter. Must run
  // after tracked-ADRP so symbol-bearing forms whose Rn holds a
  // PC-relative page-address aren't accidentally claimed as field
  // accesses.
  if (tryPrintLDRSTRSymbolicField(MI, O)) {
    invalidateRegDefs(MI);
    return true;
  }
  // A non-adjacent ADRP has already been emitted as a complete SB-relative
  // address by flushPendingADRP. Its :lo12: ADD is therefore a register copy,
  // not another address calculation. Keep one emitted instruction so raw
  // PC-relative WORD fallbacks retain their expected layout.
  if (tryPrintADDXriViaMaterializedAddress(MI, O))
    return true;
  // #271: SP-relative adjust / load / store / pair. These run after the
  // symbol-bearing LDR/STR printers (those require an MCExpr imm operand
  // and never have base=SP) and before ADRP buffering. Translating them
  // to Go mnemonics is what feeds the assembler's pcsp table.
  if (tryPrintSPAdjust(MI, O))              { invalidateRegDefs(MI); return true; }
  if (tryPrintSPMemImm(MI, O))              { invalidateRegDefs(MI); return true; }
  if (tryPrintSPPair(MI, O))                { invalidateRegDefs(MI); return true; }
  // #271: high-frequency non-address-aware opcodes. These run AFTER the
  // SP / symbol-bearing printers (which claim their forms first) and
  // BEFORE ADRP buffering. Each guards its operand shape and bails to the
  // next on a mismatch.
  if (tryPrintGPRMemImm(MI, O))             { invalidateRegDefs(MI); return true; }
  // #271 round-2: unscaled-offset (LDUR/STUR) and GPR-base STP/LDP. The
  // unscaled-memory printer accepts SP base too — placed after the
  // tryPrintSPMemImm/tryPrintGPRMemImm pair so the scaled forms claim
  // first; the unscaled handler then catches LDUR/STUR opcodes the
  // scaled table doesn't cover.
  if (tryPrintGPRMemUnscaled(MI, O))        { invalidateRegDefs(MI); return true; }
  if (tryPrintGPRPair(MI, O))               { invalidateRegDefs(MI); return true; }
  if (tryPrintAddSubImm(MI, O))             { invalidateRegDefs(MI); return true; }
  if (tryPrintCmp(MI, O))                   { invalidateRegDefs(MI); return true; }
  if (tryPrintArithSReg(MI, O))             { invalidateRegDefs(MI); return true; }
  if (tryPrintMovImm(MI, O))                { invalidateRegDefs(MI); return true; }
  // #271 round-2: MOVK/MOVN (MOVZ already in tryPrintMovImm), TST (ANDS-
  // ZR alias), SXTB/SXTH/SXTW (SBFM alias). All Rd-defining, no side effects
  // beyond the destination, so invalidate as usual.
  if (tryPrintMovWide(MI, O))               { invalidateRegDefs(MI); return true; }
  if (tryPrintTST(MI, O))                   { invalidateRegDefs(MI); return true; }
  if (tryPrintSXT(MI, O))                   { invalidateRegDefs(MI); return true; }
  // #271 round-3: AND/ORR/EOR bitmask-immediate logical + CSEL.
  if (tryPrintLogicalImm(MI, O))            { invalidateRegDefs(MI); return true; }
  if (tryPrintCSEL(MI, O))                  { invalidateRegDefs(MI); return true; }
  // #271 round-4: MADD/SMADDL/UMADDL/MSUB/SMSUBL/UMSUBL + FMOV X<->FPR.
  if (tryPrintMulAcc(MI, O))                { invalidateRegDefs(MI); return true; }
  if (tryPrintFMovGPR(MI, O))               { invalidateRegDefs(MI); return true; }
  // #271 round-5: CSINC alias family (CSET / CINC).
  if (tryPrintCSINC(MI, O))                 { invalidateRegDefs(MI); return true; }
  if (tryPrintADRP(MI, O))                  return true;
  if (tryPrintADR(MI, O)) {
    rememberRegHoldsPage(MI->getOperand(0).getReg(),
                          getReferencedSymbolName(MI->getOperand(1).getExpr()));
    return true;
  }
  if (tryPrintLDRLiteral(MI, O))            { invalidateRegDefs(MI); return true; }
  if (tryPrintBranchCall(MI, O))            { invalidateRegDefs(MI); return true; }
  if (tryPrintIndirectCall(MI, O))          { invalidateRegDefs(MI); return true; }
  if (tryPrintUnconditionalBranch(MI, O))   return true;
  if (tryPrintConditionalBranch(MI, O))     return true;
  if (tryPrintCBZBranch(MI, O))             return true;
  if (tryPrintTBZBranch(MI, O))             return true;
  if (tryPrintRet(MI, O))                   return true;

  return false;
}

void AArch64Plan9InstPrinter::finishPending(raw_ostream &O) {
  if (HavePendingADRP)
    flushPendingADRP(O);
  // Section / label boundaries invalidate the register-state
  // tracker: register contents do not persist across them in our
  // Plan 9 emission model.
  clearRegState();
}

void AArch64Plan9InstPrinter::notifyRawBytesEmitted(const MCInst *MI) {
  // A raw-byte fallback still executes the instruction (it just
  // wasn't symbolically translatable). Keep the register tracker in
  // sync by invalidating any tracked def.
  invalidateRegDefs(MI);
}

void AArch64Plan9InstPrinter::printInst(const MCInst *MI, uint64_t /*Address*/,
                                        StringRef /*Annot*/,
                                        const MCSubtargetInfo &STI,
                                        raw_ostream &O) {
  // Standalone path (`llvm-mc -output-asm-variant=2`): no streamer
  // owns a fallback, so emit a PLAN9-TODO comment for misses. The
  // MCPlan9AsmStreamer never goes through printInst — it calls
  // tryPrintInst directly so it can detect misses and do its own
  // raw-byte / fail-loud fallback.
  if (tryPrintInst(MI, STI, O))
    return;
  StringRef OpName = MII.getName(MI->getOpcode());
  O << "\t// PLAN9-TODO opcode=" << OpName
    << " (standalone InstPrinter has no MCCodeEmitter; "
       "use MCPlan9AsmStreamer for raw-byte fallback)\n";
}

void AArch64Plan9InstPrinter::printRegName(raw_ostream &OS, MCRegister Reg) {
  AArch64InstPrinter::printRegName(OS, Reg);
}
