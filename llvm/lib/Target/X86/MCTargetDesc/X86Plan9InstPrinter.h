//===- X86Plan9InstPrinter.h - Plan 9 X86 syntax InstPrinter ------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Plan 9 (Go assembler) syntax InstPrinter for X86 / X86_64. Emits text that
// `go tool asm` (cmd/internal/obj/x86) can ingest. The class declaration +
// an `MCPlan9SymbolicPrinter` overrider let the generic `MCPlan9AsmStreamer`
// (llvm/lib/MC/MCPlan9AsmStreamer.cpp) pick this up via
// `MCInstPrinter::getPlan9SymbolicPrinter()`. The mnemonic / operand
// translation is implemented in X86Plan9InstPrinter.cpp.
//
//===----------------------------------------------------------------------===//
//
// === Design summary ===
//
// 1.  ARCHITECTURE MIRRORS AArch64
//
//     The cross-target Plan 9 emit chain is already in place:
//
//         clang -fc2go-emit-plan9-asm
//           ⇒ BackendUtil.cpp sets MCOptions.OutputAsmVariant = 2
//           ⇒ CodeGenTargetMachineImpl creates MCPlan9AsmStreamer when
//             AsmVariant == 2  (target-agnostic)
//           ⇒ MCPlan9AsmStreamer calls
//             `InstPrinter->getPlan9SymbolicPrinter()->tryPrintInst(...)`
//
//     AArch64 plugs in by:
//       a. registering `AArch64Plan9InstPrinter` for variant 2 in
//          `createAArch64MCInstPrinter` (AArch64MCTargetDesc.cpp:383);
//       b. implementing `MCPlan9SymbolicPrinter` (multiple inheritance with
//          `AArch64InstPrinter`);
//       c. overriding `getPlan9SymbolicPrinter()` to expose the base.
//
//     X86 mirrors the same shape — see `X86Plan9InstPrinter` below.
//
// 2.  INTEGRATION POINTS IN X86 BACKEND (implemented; mirrors AArch64)
//
//     a. Registration in `createX86MCInstPrinter`
//        (X86MCTargetDesc.cpp): variant 2 returns `X86Plan9InstPrinter`.
//     b. `X86AsmPrinter::emitFunctionEntryLabel()` republishes per-function
//        metadata from `X86MachineFunctionInfo` into the Plan 9 streamer
//        (Stage-1 dispatch keyed by mangled name).
//     c. `X86AsmPrinter::LowerSTACKMAP` / `LowerSTATEPOINT` forward
//        Direct(SP,N) / Indirect(SP/FP,N) entries to the Plan 9 streamer.
//     d. `X86AsmPrinter::emitEndOfAsmFile` early-returns for the Plan 9
//        streamer so object-format finalization is skipped.
//
//     The publishing side (clang manifest →
//     `MCPlan9AsmStreamer::enqueueC2GoBoundary`) is target-agnostic.
//
// 3.  PLAN 9 X86_64 SYNTAX RULES (Go obj/x86 — reference for Track C
//     mnemonic mapping; tabulated from the abitest_amd64 baseline, see
//     [project_298_abitest_amd64_baseline_2026_06_07])
//
//     a. Register names are bare, no `%` prefix:
//             AX, BX, CX, DX, BP, SI, DI, SP, R8..R15
//             X0..X15 (SSE), Y0..Y15 (AVX), Z0..Z31 (AVX-512)
//        Width via mnemonic suffix (MOVQ / MOVL / MOVW / MOVB).
//
//     b. Operand order is left-to-right (source first, destination last) —
//        same as AT&T but written as `MOVQ src, dst` with no `%` and no `$`
//        on register names. Immediates keep the `$` (`MOVQ $0, AX`).
//
//     c. Memory operand is `disp(base)` or `disp(base)(idx*scale)`:
//             MOVQ 8(SP), AX                  // load
//             MOVQ AX, 0(SP)                  // store outgoing arg
//             LEAQ ·sym(SB), AX               // load symbol address
//             MOVQ ·sym+8(SB), AX             // load from sym + 8
//        Plan-9 textual symbols use `·` (middle-dot, U+00B7 / `\xc2\xb7`)
//        as the package separator and end with `(SB)` for the static base.
//
//     d. Calls use `CALL ·target(SB)` for direct, `CALL AX` for indirect.
//        Returns use `RET`. No PUSH/POP retPC — `CALL` and `RET` handle
//        the return address implicitly (unlike AArch64's explicit LR slot).
//
//     e. Branches: `JMP label`, `JEQ label`, `JNE label`, ... — single
//        mnemonic per condition (no `J{cc}` with separate condition arg).
//
//     f. 128-bit moves are `MOVUPS X0, mem` (unaligned 16) or `MOVQ X0, mem`
//        (low 64 only). Float→int64 trunc is `CVTTSD2SQ` not `FCVTZSD`.
//
// 4.  GATE
//
//     The X86 Plan-9 streamer is reached only when:
//       i)  `OutputAsmVariant == 2` (set by clang `-fc2go-emit-plan9-asm`),
//      ii)  the per-Plan9 codegen TM is in use,
//     iii)  `c2go.goabi` module flag is ON (foundation gate; required for
//           any c2go behavior — also gates the leaf CC flip, mirroring
//           AArch64; the former second gate `c2go.x86-leaf-abi` was
//           removed, see `X86C2GoLeafABI.cpp`).
//
//     With `c2go.goabi` OFF NO X86 function is flipped to
//     `c2goabiinternalcc`, so even if a streamer is constructed the only
//     functions emitted are ABI0/SysV — Plan 9 syntax is still correct,
//     just no register optimization.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_MCTARGETDESC_X86PLAN9INSTPRINTER_H
#define LLVM_LIB_TARGET_X86_MCTARGETDESC_X86PLAN9INSTPRINTER_H

#include "X86InstPrinterCommon.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/MC/MCPlan9AsmStreamer.h"

#include <string>

namespace llvm {

class MCExpr;

/// Plan 9 (Go assembler) syntax InstPrinter for X86 / X86_64.
///
/// Inherits AT&T as the syntactic ancestor (operand order is L-to-R for
/// both) and `MCPlan9SymbolicPrinter` so the generic MCPlan9AsmStreamer
/// can call `tryPrintInst` through the same interface used by AArch64.
///
/// The `tryPrint*` mnemonic table is implemented in the .cpp. Instructions
/// not covered fall through to the streamer's raw-byte WORD fallback (which
/// hits PLAN9-ERROR for any symbol-bearing inst). X86 Plan 9 emission is
/// gated by `c2go.goabi`, so this is unreachable in clang/c2go-lto's
/// default path.
class X86Plan9InstPrinter : public X86InstPrinterCommon,
                            public MCPlan9SymbolicPrinter {
public:
  X86Plan9InstPrinter(const MCAsmInfo &MAI, const MCInstrInfo &MII,
                       const MCRegisterInfo &MRI);
  ~X86Plan9InstPrinter() override;

  // ---- MCPlan9SymbolicPrinter overrides (Track C body) ----
  //
  // Entry point called by MCPlan9AsmStreamer::emitInstruction. Returns
  // true if the instruction was rendered into Plan 9 syntax; false sends
  // the caller to the raw-byte WORD fallback (and to PLAN9-ERROR if the
  // encoded bytes carry fixups).
  bool tryPrintInst(const MCInst *MI, const MCSubtargetInfo &STI,
                    raw_ostream &O) override;

  // Flush any cross-instruction state held by Track C's printer (e.g. a
  // half-emitted multi-instruction sequence). Called by the streamer at
  // section / label boundaries.
  void finishPending(raw_ostream &O) override;

  // Notify that the streamer fell back to raw-byte WORD for `MI`. Track C
  // uses this to invalidate any register-state tracker the printer keeps.
  void notifyRawBytesEmitted(const MCInst *MI) override;

  // ---- MCInstPrinter hook ----
  //
  // Expose our MCPlan9SymbolicPrinter base so the generic streamer's
  // bootstrap (CodeGenTargetMachineImpl::createMCObjectStreamer when
  // AsmVariant == 2) can pair this InstPrinter with an MCPlan9AsmStreamer.
  MCPlan9SymbolicPrinter *getPlan9SymbolicPrinter() override { return this; }

  // Standalone llvm-mc path — `llvm-mc --x86-asm-syntax=plan9` (if Track C
  // wires the variant into MC's command-line dispatch). Forwards to
  // tryPrintInst, falling back to the AT&T printer for misses so the
  // standalone disassembler still produces *something*.
  void printInst(const MCInst *MI, uint64_t Address, StringRef Annot,
                 const MCSubtargetInfo &STI, raw_ostream &O) override;

  // Plan 9 X86 register names — bare, no `%` prefix, width via mnemonic.
  // Track C provides the static name table (AX/BX/.../R15, X0..X15, ...).
  void printRegName(raw_ostream &OS, MCRegister Reg) override;

  // Pure-virtual hook on X86InstPrinterCommon. Not used by the MCPlan9
  // AsmStreamer path (which calls tryPrintInst directly) — provide a
  // best-effort stub so the class is non-abstract and the standalone
  // llvm-mc path produces *something* rather than failing to link.
  void printOperand(const MCInst *MI, unsigned OpNo, raw_ostream &O) override;

  // Pure-virtual hook on MCInstPrinter. The tablegen-generated AT&T /
  // Intel printers return (mnemonic-string, BitsLeft); MCPlan9AsmStreamer
  // never calls this — the streamer-side dispatch is `tryPrintInst`. We
  // hand back the raw MII name (Plan-9-style mnemonic lookup is done
  // inside the tryPrint* helpers, not here).
  std::pair<const char *, uint64_t>
  getMnemonic(const MCInst &MI) const override;

private:
  // ---- Track C: per-opcode-family translation helpers ----
  //
  // Same dispatch shape as AArch64Plan9InstPrinter — one helper per
  // address-aware or high-frequency opcode family. Each returns true on
  // success (output written to O); false sends the caller to the raw-byte
  // fallback. Wave Y Track A leaves every helper as a forward declaration;
  // Track C lands the bodies.

  // Function-entry / function-exit
  bool tryPrintRet(const MCInst *MI, raw_ostream &O);            // RET / RET64

  // Calls
  //   * Direct call:   `CALL ·target(SB)`        — CALL64pcrel32, CALLpcrel32
  //   * Indirect call: `CALL AX`                 — CALL64r, CALL32r
  bool tryPrintBranchCall(const MCInst *MI, raw_ostream &O);
  bool tryPrintIndirectCall(const MCInst *MI, raw_ostream &O);

  // Branches
  //   * Unconditional: `JMP label`               — JMP_1 / JMP_4
  //   * Conditional:   `JEQ`/`JNE`/`JLT`/...     — JCC_1 / JCC_4 with cc
  bool tryPrintUnconditionalBranch(const MCInst *MI, raw_ostream &O);
  bool tryPrintConditionalBranch(const MCInst *MI, raw_ostream &O);

  // SP-relative load / store / arith — `MOVQ off(SP), AX`, `LEAQ off(SP), AX`,
  // `ADDQ $imm, SP`, `SUBQ $imm, SP`. These are what populate the Go
  // assembler's pcsp table; without them copystack/morestack/GC stack-unwind
  // cannot reason about the frame.
  bool tryPrintSPAdjust(const MCInst *MI, raw_ostream &O);
  bool tryPrintSPMemImm(const MCInst *MI, raw_ostream &O);

  // GPR-base load / store (non-SP). `MOVQ off(Rn), Rt` / `MOVL ...`.
  bool tryPrintGPRMemImm(const MCInst *MI, raw_ostream &O);

  // Symbol-bearing LEA / MOV — `LEAQ ·sym(SB), AX`, `MOVQ ·sym+8(SB), AX`.
  // The MCOperand is an MCSymbolRefExpr after MCInstLower; needs the
  // `·`-prefixed Plan 9 textual form plus `(SB)` suffix.
  bool tryPrintLEASymbolic(const MCInst *MI, raw_ostream &O);
  bool tryPrintMOVSymbolic(const MCInst *MI, raw_ostream &O);

  // c2go (#298 Track AF.1) — c2go.typeinfo.<X> / type:<pkg>.<X> reference
  // rewrite. Mirrors `AArch64Plan9InstPrinter`'s typeinfo MOV path
  // (AArch64Plan9InstPrinter.cpp:518/543): rewrites a symbolic LEA64r /
  // MOV64rm whose displacement references `c2go.typeinfo.<X>` or
  // `type:<pkg>.<X>` into `MOVQ ·_typeinfo_<X>(SB), Rd` — load the value
  // (the *_type pointer) of c2gobind's per-type indirection var. Without
  // this rewrite the X86 .s references `c2go_typeinfo·<X>(SB)` (the
  // mangled raw-name form produced by `goSymToPlan9`), which c2gobind
  // never emits — link fails with "relocation target c2go_typeinfo.X not
  // defined". Dispatched ahead of `tryPrintLEASymbolic` / `tryPrintSPMemImm`
  // so it can intercept both the address-of and the load form before the
  // generic symbolic-memref path renders the raw name.
  bool tryPrintTypeinfoLoad(const MCInst *MI, raw_ostream &O);

  // c2go (#298 Track AL.2) — generic `@GOTPCREL` MOV64rm → Plan-9 LEAQ
  // rewrite for non-typeinfo external globals (runtime.writeBarrier,
  // runtime.<flag>, type.*, etc.). X86 ISel lowers a `load i64/i32/i8,
  // ptr @global` in ELF small-PIC code-model to a two-instruction
  // sequence:
  //
  //     %tmp:gr64 = MOV64rm $rip, 1, $noreg,
  //                 target-flags(x86-gotpcrel) @<sym>, $noreg   ; 8B GOT
  //     %val      = MOV<W>rm killed %tmp, 1, $noreg, 0, $noreg  ; real
  //
  // The first MOV64rm is *not* a real LOAD in Plan-9 semantics — the
  // Go assembler / linker has no notion of `@GOTPCREL` (no GOT slot:
  // SB-relative references resolve directly at link time). Printing it
  // as `MOVQ ·sym(SB), Rd` would make the Go assembler treat the value
  // at `sym`'s address as a pointer (8 bytes), then the follow-up
  // load dereferences that misinterpreted value → nil-deref or wild
  // pointer (Wave AK BLOCKER-3, -O0 release_blocker root).
  //
  // The correct Plan-9 form is an immediate LEA (mirror of AArch64's
  // `MOVD $·sym(SB), Rd` + `MOVWU 0(Rd), Rt` pair, see
  // AArch64Plan9InstPrinter.cpp line range printing ADRP+ADD->MOVD):
  //
  //     LEAQ ·sym(SB), Rd
  //
  // The follow-up real-width MOV<W>rm is unchanged: its base reg holds
  // the real address of `sym` (rather than a GOT slot value), so its
  // existing tryPrintSPMemImm / tryPrintGPRMemImm dispatch prints it
  // verbatim with the correct semantics.
  //
  // Dispatched AFTER `tryPrintTypeinfoLoad` (which intercepts the
  // `c2go.typeinfo.*` / `type:*` prefixes with their own MOVQ form,
  // because the typeinfo VAR _itself_ stores the *_type pointer — a
  // single MOVQ load IS correct for typeinfo) and BEFORE
  // `tryPrintLEASymbolic` (which doesn't know about GOTPCREL).
  bool tryPrintGOTPCRELLoadAsLEA(const MCInst *MI, raw_ostream &O);

  // Move-immediate variants — `MOVQ $imm, Rd`, `MOVL $imm, Rd`.
  bool tryPrintMovImm(const MCInst *MI, raw_ostream &O);

  // Reg-reg arithmetic — `ADDQ src, dst`, `SUBQ`, `ANDQ`, `ORQ`, `XORQ`,
  // `CMPQ`, etc. Width via opcode suffix; operands left-to-right.
  bool tryPrintArithReg(const MCInst *MI, raw_ostream &O);

  // SSE / SIMD moves — `MOVUPS X0, mem` (unaligned 128), `MOVQ X0, mem`
  // (low 64), `MOVAPS X0, mem` (aligned 128). Float→int trunc: `CVTTSD2SQ`.
  bool tryPrintSSEMov(const MCInst *MI, raw_ostream &O);

  // MOV{8,16,32,64}rr reg-reg matching path. Routed via a single helper
  // so the rr-only mnemonic table can be reused across widths without
  // duplicating dispatch.
  bool tryPrintMatchRR(const MCInst *MI, raw_ostream &O);

  // ---- Symbol formatting helper ----
  //
  // Format an MCExpr branch / call / LEA target into Plan 9 textual form:
  //   * local label `Ltmp0`            → bare name
  //   * external `_foo`                → `·foo(SB)`
  //   * dotted `runtime.morestack`     → `runtime·morestack(SB)`
  //   * additive `sym+8`               → `·sym+8(SB)`
  //
  // Mirror of `AArch64Plan9InstPrinter::formatBranchTarget` /
  // `MCPlan9AsmStreamer::symbolToPlan9` so the two sides stay in sync.
  std::string formatBranchTarget(const MCExpr *E) const;
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_X86_MCTARGETDESC_X86PLAN9INSTPRINTER_H
