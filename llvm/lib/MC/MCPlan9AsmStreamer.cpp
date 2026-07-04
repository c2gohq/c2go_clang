//===- MCPlan9AsmStreamer.cpp - Plan 9 (Go assembler) MCStreamer ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MCStreamer subclass that writes Plan 9 syntax (consumable by Go's
// `go tool asm`). Holds the MCCodeEmitter and arbitrates between:
//
//   1. The per-target Plan 9 InstPrinter's `tryPrintInst` — succeeds
//      for address-aware mnemonics (branches, ADRP pairs, RET).
//   2. Raw-byte WORD/BYTE fallback via the held MCCodeEmitter — used
//      only when the encoded MCInst has NO fixups (i.e. it's a
//      self-contained instruction whose bytes don't need linker
//      relocation).
//   3. Hard PLAN9-ERROR fail-loud — when the InstPrinter missed AND
//      the encoded bytes contain fixups (symbol-bearing instruction
//      not yet translated; emitting zero-offset WORD would be a
//      silent miscompile).
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCPlan9AsmStreamer.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormattedStream.h"

using namespace llvm;

MCPlan9AsmStreamer::MCPlan9AsmStreamer(MCContext &Ctx,
                                       std::unique_ptr<formatted_raw_ostream> OS,
                                       std::unique_ptr<MCInstPrinter> InstPrinter,
                                       MCPlan9SymbolicPrinter *SymPrinter,
                                       std::unique_ptr<MCCodeEmitter> CE)
    : MCStreamer(Ctx), OS(std::move(OS)),
      InstPrinter(std::move(InstPrinter)), SymPrinter(SymPrinter),
      CE(std::move(CE)) {
  // Mirror MCAsmStreamer: block labels (MCContext::createBlockSymbol)
  // are anonymous when UseNamesOnTempLabels is false. Without this,
  // local branch targets render as `""`.
  Ctx.setUseNamesOnTempLabels(true);

  // c2go #376: drain any boundary-symbol metadata clang's manifest pass
  // queued on the thread_local pending list before this streamer was
  // constructed. The clang→streamer hand-off can't go via MCContext (the
  // MCContext is created INSIDE emitBackendOutput, which runs after the
  // manifest pass), so the narrow per-thread queue stays.
  for (auto &M : drainPendingC2GoBoundaries())
    publishC2GoFunction(std::move(M));

  // c2go #387: same single-construction-time drain pattern for Go-owned
  // file-scope globals. The per-instance set is consulted by
  // emitCommonSymbol / emitZerofill to suppress GLOBL emission so the
  // Go-side `var <name> unsafe.Pointer` is the unique storage def.
  for (std::string &N : drainPendingC2GoGoOwnedGlobals())
    C2GoGoOwnedGlobals.insert(std::move(N));
}

MCPlan9AsmStreamer::~MCPlan9AsmStreamer() = default;

// c2go #402 sub-part (b): forward-declared so emitC2GoDataLabel (defined
// above the helper's body in this TU) can call into the same predicate
// that emitCommonSymbol / emitZerofill use. Definition is at the bottom
// of the file alongside its callers.
static bool isC2GoGoOwnedGlobal(const StringSet<> &Set, StringRef Plan9);

// #586: clang emits anonymous string literals (and constant pools / jump
// tables) as private/local globals — `.L.str`, `l_.str`, `.LCPI...`. In Plan 9
// these MUST be file-local (`name<>(SB)`) or they collide across separately
// compiled c2go packages at Go link time: two packages each define a plain
// global `_L_str`, and the Go linker rejects the duplicate. The name pattern is
// the same one symbolToPlan9 / goSymToPlan9 use to detect local labels; when it
// matches, append `<>` at the DATA definition AND every (SB) reference site
// (the streamer's data-symbol open + data-to-data ref, and the InstPrinters'
// goSymToPlan9). Branch/CFI labels never reach those sites (they go through
// formatBranchTarget / text-label emission), so only real private data symbols
// are scoped. Keep this predicate in lockstep with the local-label detection in
// symbolToPlan9 / goSymToPlan9.
static bool isPlan9LocalDataSym(StringRef Name) {
  return Name.starts_with("L") || Name.starts_with(".L") ||
         (Name.size() >= 2 && Name[0] == 'l' &&
          (Name[1] == '_' || (Name[1] >= 'A' && Name[1] <= 'Z')));
}

namespace {
// c2go #376: 6 thread_local side-channels for function metadata
// (g_C2GoMetadata, g_C2GoArgPtrMask, g_C2GoLocalsAggMask,
// g_C2GoLocalsAmbigMask, g_C2GoNoSplit, g_C2GoStackObjects) were merged
// into a single per-streamer-instance StringMap<C2GoFunctionMetadata>
// (MCPlan9AsmStreamer::C2GoFnMeta) — see MCPlan9AsmStreamer.h. The
// publish API is now an instance method.
//
// One narrow thread_local channel remains: the boundary-symbol queue
// (g_PendingC2GoBoundaries). Clang's CodeGenAction populates this from
// the JSON manifest BEFORE the streamer is constructed (the MC layer is
// set up inside emitBackendOutput, which runs after the manifest pass);
// the streamer constructor drains the queue into its per-instance
// C2GoFnMeta. Single channel, drained once at construction, outside the
// hot per-function emit path.
thread_local std::vector<llvm::C2GoFunctionMetadata> g_PendingC2GoBoundaries;

// c2go #387 §B4 phase 6 sub-step 1: single-pointer-word file-scope
// globals whose storage is owned by the generated Go package. Same
// hand-off pattern as g_PendingC2GoBoundaries: CodeGenAction populates
// this before the streamer is constructed; the constructor drains it
// into a per-instance StringSet. Drained as bare var names (no `·`
// prefix, no `(SB)` suffix) so emitCommonSymbol / emitZerofill can
// match against `symbolToPlan9` output.
thread_local std::vector<std::string> g_PendingC2GoGoOwnedGlobals;

// #240: structural extraction of (symbol, additive-offset) from an
// MCExpr, mirroring AArch64Plan9InstPrinter's getReferencedSymbol*
// helpers. Used by emitDataSymDirective so a `sym+N` initializer is
// emitted as `$sym+N(SB)` rather than rendered to text and then
// re-sanitised (which would mangle the `+N` offset into `_N`,
// referencing a non-existent symbol). Returns the underlying
// MCSymbol* (or nullptr if the expr isn't a plain symbol[+const]).
const llvm::MCSymbol *referencedSymbol(const llvm::MCExpr *E) {
  while (E) {
    switch (E->getKind()) {
    case llvm::MCExpr::SymbolRef:
      return &llvm::cast<llvm::MCSymbolRefExpr>(E)->getSymbol();
    case llvm::MCExpr::Unary:
      E = llvm::cast<llvm::MCUnaryExpr>(E)->getSubExpr();
      break;
    case llvm::MCExpr::Binary: {
      const auto *B = llvm::cast<llvm::MCBinaryExpr>(E);
      if (B->getOpcode() == llvm::MCBinaryExpr::Add ||
          B->getOpcode() == llvm::MCBinaryExpr::Sub) {
        E = B->getLHS();
        break;
      }
      return nullptr;
    }
    case llvm::MCExpr::Target:
    case llvm::MCExpr::Specifier:
      E = llvm::cast<llvm::MCSpecifierExpr>(E)->getSubExpr();
      break;
    default:
      return nullptr;
    }
  }
  return nullptr;
}

int64_t referencedOffset(const llvm::MCExpr *E) {
  int64_t Off = 0;
  while (E) {
    switch (E->getKind()) {
    case llvm::MCExpr::Constant:
      return Off + llvm::cast<llvm::MCConstantExpr>(E)->getValue();
    case llvm::MCExpr::SymbolRef:
      return Off;
    case llvm::MCExpr::Unary:
      E = llvm::cast<llvm::MCUnaryExpr>(E)->getSubExpr();
      break;
    case llvm::MCExpr::Binary: {
      const auto *B = llvm::cast<llvm::MCBinaryExpr>(E);
      if (B->getOpcode() == llvm::MCBinaryExpr::Add ||
          B->getOpcode() == llvm::MCBinaryExpr::Sub) {
        int64_t Sign = (B->getOpcode() == llvm::MCBinaryExpr::Add) ? 1 : -1;
        if (const auto *CR = llvm::dyn_cast<llvm::MCConstantExpr>(B->getRHS()))
          Off += Sign * CR->getValue();
        E = B->getLHS();
        break;
      }
      return Off;
    }
    case llvm::MCExpr::Target:
    case llvm::MCExpr::Specifier:
      E = llvm::cast<llvm::MCSpecifierExpr>(E)->getSubExpr();
      break;
    default:
      return Off;
    }
  }
  return Off;
}
} // namespace

// c2go #239: `Plan9LinknameBridges` and `Plan9PackageName` were
// thread_local registries that this streamer / its InstPrinter wrote
// but nothing read (the original manifest-consumer paths were
// rewritten to use the JSON manifest pipeline directly). They were
// removed as part of the per-compile thread_local reduction. Any
// future linkname-bridge surfacing should live on `MCPlan9AsmStreamer`
// as an instance member (the InstPrinter can reach it via the
// streamer pointer the symbolic-print path already uses).
//
// LLVM-22 cleanup: `ForceBlockAddressJumpTable` (#120 jump-table
// encoding override) and `DisableRegisterCoalescing` (#309/#310
// coalescer disable) used to live here as thread_local globals too;
// they now live on `AArch64TargetMachine` as instance fields, set on
// the Plan 9-codegen-only TM by clang's BackendUtil. Readers reach
// them via getTM<AArch64TargetMachine>() / TargetLowering's
// getTargetMachine() back-pointer. No global state remains for these
// two; the .o pipeline TM has them unset by default.

void MCPlan9AsmStreamer::setFunctionMetadata(StringRef MangledName,
                                             int FrameSize, int ArgSize) {
  FunctionMetadata[MangledName] = std::make_pair(FrameSize, ArgSize);
}

// Strip clang's `\01` no-mangle marker so the side-channel key matches the
// post-Mach-O-asm label the streamer sees at emit time.
static StringRef stripNoMangle(StringRef N) {
  if (!N.empty() && N[0] == '\01') return N.drop_front();
  return N;
}

void MCPlan9AsmStreamer::publishC2GoFunction(C2GoFunctionMetadata M) {
  // c2go #330: strip the `\01` no-mangle marker so the streamer's emitLabel
  // lookup hits (MF.getName() may carry the marker; emitLabel sees the
  // post-Mach-O-asm name without it).
  std::string Name = std::string(stripNoMangle(M.Name));

  // c2go #376: read-modify-write a single entry in the per-instance map,
  // preserving partial-update semantics for every field. The lookup is
  // done once and the slot reference is reused for all fields below.
  auto &E = C2GoFnMeta[Name];
  // Ensure the entry's name is recorded (cheap, idempotent).
  if (E.Name.empty())
    E.Name = Name;

  // ---- framesize + argsize ----
  // FrameSize < 0 / ArgSize nullopt means "preserve the prior value".
  if (M.FrameSize >= 0)
    E.FrameSize = M.FrameSize;
  if (M.ArgSize.has_value())
    E.ArgSize = *M.ArgSize;

  // ---- NoSplit (set-only, never cleared) ----
  if (M.NoSplit)
    E.NoSplit = true;

  // ---- ArgPtrMask: non-empty publishes ----
  if (!M.ArgPtrMaskBytes.empty())
    E.ArgPtrMaskBytes = std::move(M.ArgPtrMaskBytes);

  // ---- LocalsAggMask ----
  if (!M.LocalsAggMaskBytes.empty())
    E.LocalsAggMaskBytes = std::move(M.LocalsAggMaskBytes);

  // ---- LocalsAmbigMask ----
  if (!M.LocalsAmbigMaskBytes.empty())
    E.LocalsAmbigMaskBytes = std::move(M.LocalsAmbigMaskBytes);

  // ---- Frame contract (c2go #298 Wave AA Track A / F3) ----
  // Wave AA GPT NEEDS_FIX Fix 1 (A3 publishC2GoFunction API foot-gun):
  // partial-update semantics — nullopt preserves the prior published
  // value; a has_value() field overwrites. Previously the streamer
  // unconditionally overwrote with M.{SavedLinkSize,FrameAlignment},
  // which silently reset an X86 producer's 0 back to 8/16 when a later
  // secondary publish (e.g. AArch64-style stkobj harvest, which builds
  // a fresh aggregate carrying only the StackObjects payload) reached
  // the streamer with the legacy non-optional struct defaults. With
  // both sides std::optional the secondary publish leaves these
  // fields nullopt by default, and the primary producer's value
  // survives unchanged.
  if (M.SavedLinkSize.has_value())
    E.SavedLinkSize = M.SavedLinkSize;
  if (M.FrameAlignment.has_value())
    E.FrameAlignment = M.FrameAlignment;
}

void MCPlan9AsmStreamer::resetC2GoNoSplit() {
  for (auto &KV : C2GoFnMeta)
    KV.second.NoSplit = false;
}

void MCPlan9AsmStreamer::resetC2GoAll() { C2GoFnMeta.clear(); }

void MCPlan9AsmStreamer::enqueueC2GoBoundary(C2GoFunctionMetadata M) {
  g_PendingC2GoBoundaries.push_back(std::move(M));
}

std::vector<C2GoFunctionMetadata>
MCPlan9AsmStreamer::drainPendingC2GoBoundaries() {
  std::vector<C2GoFunctionMetadata> Out;
  Out.swap(g_PendingC2GoBoundaries);
  return Out;
}

void MCPlan9AsmStreamer::enqueueC2GoGoOwnedGlobal(StringRef Name) {
  g_PendingC2GoGoOwnedGlobals.emplace_back(Name);
}

std::vector<std::string>
MCPlan9AsmStreamer::drainPendingC2GoGoOwnedGlobals() {
  std::vector<std::string> Out;
  Out.swap(g_PendingC2GoGoOwnedGlobals);
  return Out;
}


void MCPlan9AsmStreamer::emitFileHeaderIfNeeded() {
  if (HeaderEmitted)
    return;
  HeaderEmitted = true;
  *OS << "// Code generated by clang (-fc2go-emit-plan9-asm); DO NOT EDIT.\n";
  *OS << "#include \"textflag.h\"\n";
  // c2go §D2: include the Go-compiler-generated `go_asm.h` so the
  // assembler can resolve symbolic struct-layout offsets of the form
  // `<RecName>_<FieldName>` / `<RecName>__size`. The header is emitted
  // by `go build`'s `-asmhdr` step for the host package and lives at
  // the same path as `textflag.h` in Go's asm include search list.
  //
  // Phase 1 (this drop) only emits the include — the InstPrinter still
  // produces byte-offset LDR/STR for managed-struct field access; Phase 2
  // (a follow-up task) will use IR-level metadata to rewrite suitable
  // load/store offsets into symbolic form. Including the header
  // unconditionally is harmless when no symbolic refs are used.
  *OS << "#include \"go_asm.h\"\n\n";
}

void MCPlan9AsmStreamer::emitRawTextImpl(StringRef String) {
  emitFileHeaderIfNeeded();
  *OS << String;
  if (!String.ends_with("\n"))
    *OS << "\n";
}

void MCPlan9AsmStreamer::emitLabel(MCSymbol *Symbol, SMLoc /*Loc*/) {
  emitFileHeaderIfNeeded();

  std::string Rendered;
  {
    llvm::raw_string_ostream RS(Rendered);
    Symbol->print(RS, getContext().getAsmInfo());
  }
  StringRef Name = Rendered;

  // Mach-O Linker Optimization Hint labels (`Lloh<n>:`) sit between
  // an ADRP and its paired ADD/LDR; they're purely link-time metadata
  // and have no control-flow effect. Treating them as control-flow
  // labels would flush the pending ADRP and force the ADD into the
  // raw-byte fallback. Skip the label entirely AND skip
  // finishPending — the pair must remain "open" across this label.
  if (Name.starts_with("Lloh"))
    return;

  // Before crossing a real label boundary, flush any pending ADRP
  // state held in the InstPrinter; otherwise the deferred output
  // would appear AFTER the new label.
  if (SymPrinter)
    SymPrinter->finishPending(*OS);
  // c2go #321: do NOT strip a leading `_`. The Plan 9 codegen pipeline
  // uses a neutral ELF triple/DataLayout (RunC2GoPlan9Pipeline) whose
  // mangler does NOT add the Mach-O `_` global prefix, so any leading
  // `_` here is a real character of the source-level C symbol (e.g. a
  // C function literally named `_helper`). Stripping it would diverge
  // from buildC2GoManifest, which writes `asm_symbol = "·_helper"` and
  // c2go-bind's `//go:linkname` target.
  StringRef Mangled = Name;

  // Stage 1 dispatch: known c2go function (manifest- or backend-registered).
  std::pair<int, int> Sz{0, 0};
  bool Found = false;
  auto It = FunctionMetadata.find(Name);
  if (It != FunctionMetadata.end()) {
    Sz = It->second;
    Found = true;
  } else {
    // c2go #376: instance-side per-function metadata (replaces the old
    // thread_local g_C2GoMetadata).
    auto MetaIt = C2GoFnMeta.find(Mangled);
    if (MetaIt == C2GoFnMeta.end())
      MetaIt = C2GoFnMeta.find(Name);
    if (MetaIt != C2GoFnMeta.end() &&
        (MetaIt->second.FrameSize >= 0 || MetaIt->second.ArgSize.has_value())) {
      Sz = std::make_pair(
          MetaIt->second.FrameSize >= 0 ? MetaIt->second.FrameSize : 0,
          MetaIt->second.ArgSize.value_or(0));
      Found = true;
    }
  }
  if (Found) {
    emitC2GoFunctionLabel(Name, Mangled, Sz);
    return;
  }

  // Stage 2 dispatch: data-section label.
  if (InDataSection) {
    emitC2GoDataLabel(Name, Mangled);
    return;
  }

  // Stage 3 dispatch: AArch64 backend local label.
  // Mach-O private labels can use either uppercase `L` or lowercase `l_`
  // (clang's MachOAsmInfo names string literals `l_.str.NN`). #276: keep
  // this test as narrow as symbolToPlan9's so an ordinary static C
  // function whose name starts with a lowercase `l` (e.g. `lengthFunc`)
  // is NOT mistaken for a private label here.
  if (!Name.empty() &&
      (Name[0] == 'L' || Name.starts_with(".L") ||
       (Name.size() >= 2 && Name[0] == 'l' &&
        (Name[1] == '_' || (Name[1] >= 'A' && Name[1] <= 'Z'))))) {
    emitC2GoLocalLabel(Name);
    return;
  }

  // Stage 4 dispatch: TU-local C function fallback.
  emitC2GoTULocalFunctionLabel(Name);
}

// Stage 1 body: known c2go function (manifest- or backend-registered).
void MCPlan9AsmStreamer::emitC2GoFunctionLabel(StringRef Name,
                                               StringRef Mangled,
                                               std::pair<int, int> Sz) {
  closeCurrentDataSymbol(); // function labels close any open data
  // c2go #330: when the name contains a `.` (a c2go_linkname-renamed
  // boundary symbol like `probemod.ProbeInterior`), let symbolToPlan9 do
  // the Unicode middle-dot substitution; otherwise emit the bare name
  // with a `·` prefix as before.
  std::string TextSym;
  if (Mangled.contains('.') || Mangled.contains('/'))
    TextSym = symbolToPlan9(Mangled);
  else
    TextSym = std::string("\xc2\xb7") + std::string(Mangled);
  // c2go (§B1): surface the real framesize from the c2go prologue
  // (populated by AArch64FrameLowering::emitC2GoPrologue via
  // updateFrameSize) so Go runtime traceback can compute saved-LR /
  // saved-FP slot offsets and GC stackmaps can reason about frame
  // slots. ArgSize is manifest-provided (used by Go's argument-area
  // layout for register-to-stack ABI translation).
  //
  // #226 / #229 / GC-S2b — emit splittable (no NOSPLIT bit) so the
  // declared frame participates in Go's morestack stack-growth check,
  // and declare the REAL framesize so the runtime's pcsp/traceback and
  // the locals pointer-map agree on the frame extent.
  //
  // `go tool asm` OWNS the prologue/epilogue for a framed TEXT: it
  // injects `MOVD.W R30,-autosize(RSP); MOVD R29,-8(RSP); SUB $8,RSP,
  // R29` at entry and expands RET into the matching teardown
  // (obj7.go preprocess). To avoid double-framing the SP, the c2go
  // hand-rolled prologue/epilogue (AArch64FrameLowering::emitC2Go*) is
  // suppressed in the .s path (AArch64AsmPrinter::emitInstruction skips
  // FrameSetup/FrameDestroy MIs in Plan 9 mode). GoABI0 uses
  // CSR_AArch64_NoRegs, so those are the ONLY frame-flagged
  // instructions — nothing else is lost.
  //
  // Declared `$framesize` vs the physical SP decrement D: go asm adds
  // back the saved-LR word (+8) and 16-byte-alignment padding, so the
  // final autosize equals D when we declare D-16 (D is 16-aligned, so
  // (D-16)+8 = D-8 is 8 mod 16 → +8 extrasize → D). D==0 leaf frames
  // stay NOFRAME, $0 (go asm injects nothing).
  flushC2GoStackmaps();
  // c2go #297 (L0): strict-leaf functions (no call, frame within the Go
  // NOSPLIT budget) are marked NOSPLIT by AArch64FrameLowering so the Go
  // assembler omits the morestack stack-growth check at entry. A strict
  // leaf has no callee, so the linker's nosplit-chain graph terminates at
  // it immediately — no all-pairs explosion (that only afflicts marking
  // EVERY function NOSPLIT). textflag.h: NOSPLIT=4, NOFRAME=512.
  // c2go #376: NOSPLIT bit lives on the per-function metadata entry.
  // c2go #298 Wave AA Track A (F3 contract lock): the per-arch frame
  // layout fixup (`FrameAlignment` — declared autosize = FrameSize -
  // FrameAlignment) and the saved-LR slot size (`SavedLinkSize` — drives
  // locals-bitmap Nbit) are also per-function metadata fields. Defaults
  // are 16 / 8 = AArch64 production values; an X86 producer that sets
  // them to 0 / 0 makes the same code path emit a physically-correct
  // `$framesize` for x86_64 (no saved-LR slot, no Go-assembler 16-byte
  // padding fixup). Lookup order matches NoSplit: try the Mach-O-mangled
  // name first (the public form publishC2GoFunction normalises to), then
  // the LLVM-IR name.
  // Wave AA GPT NEEDS_FIX Fix 1 (A3): the metadata fields are
  // `std::optional<unsigned>` with nullopt = "no producer has published
  // this field for this function". Fall back to the AArch64 production
  // defaults (16 / 8) so the existing AArch64 byte-identical path is
  // preserved when no producer ever touches the fields. An X86 producer
  // that publishes 0 / 0 lights up the X86 contract; a later secondary
  // publish carrying nullopt does NOT overwrite — see publishC2GoFunction
  // above.
  bool NoSplit = false;
  unsigned FrameAlignment = 16; // AArch64 default
  unsigned SavedLinkSize = 8;   // AArch64 default
  {
    auto NSIt = C2GoFnMeta.find(Mangled);
    if (NSIt == C2GoFnMeta.end())
      NSIt = C2GoFnMeta.find(Name);
    if (NSIt != C2GoFnMeta.end()) {
      NoSplit = NSIt->second.NoSplit;
      if (NSIt->second.FrameAlignment.has_value())
        FrameAlignment = *NSIt->second.FrameAlignment;
      if (NSIt->second.SavedLinkSize.has_value())
        SavedLinkSize = *NSIt->second.SavedLinkSize;
    }
  }
  if (Sz.first >= static_cast<int>(FrameAlignment) && FrameAlignment > 0)
    *OS << "TEXT " << TextSym << "(SB), "
        << (NoSplit ? "NOSPLIT, $" : "$")
        << (Sz.first - static_cast<int>(FrameAlignment)) << "-" << Sz.second
        << "\n";
  else if (Sz.first > 0 && FrameAlignment == 0)
    // X86-style: the producer's FrameSize is already the declared autosize;
    // no per-arch fixup to subtract.
    *OS << "TEXT " << TextSym << "(SB), "
        << (NoSplit ? "NOSPLIT, $" : "$") << Sz.first << "-" << Sz.second
        << "\n";
  else
    *OS << "TEXT " << TextSym << "(SB), "
        << (NoSplit ? "NOSPLIT|NOFRAME" : "NOFRAME") << ", $0-" << Sz.second
        << "\n";
  // c2go Phase 1: open a new stackmap window for this function and
  // emit FUNCDATA $1 + initial PCDATA $1, $-1. Skipped when frame
  // size is zero (NOFRAME funcs hold no managed allocas of their
  // own; any callee-borne managed ptrs are tracked at the caller).
  emitC2GoFuncDataPreamble(Mangled, Sz.first, Sz.second, SavedLinkSize);
}

// Stage 2 body: data-section label. In a data section, every label
// STARTS a new data symbol — close the previous one's GLOBL trailer
// first, then either suppress (typeinfo/gcbitmap) or open a fresh
// CurrentDataSym.
void MCPlan9AsmStreamer::emitC2GoDataLabel(StringRef Name, StringRef Mangled) {
  closeCurrentDataSymbol();
  // Wave AP.3: a fresh data label ends any prior suppressed-blob window;
  // the two suppress returns below re-arm it.
  DataSymSuppressed = false;
  // c2go #402 sub-part (b): Go-owned 二重防护 (BACKSTOP for #394's IR
  // PREDICATE). The aux audit cleanup #6 noted that #387 Go-owned
  // suppression today rides BSS-only because CodeGenModule routes
  // tagged go-owned globals through @llvm.compiler.used + no
  // initializer. If a future sub-step adds single-pointer init, or
  // a mid-end const-fold pass promotes a tagged global onto the DATA
  // path, we'd silently emit a duplicate DATA/GLOBL definition that
  // collides with the Go-side `var X unsafe.Pointer` storage. Guard
  // by suppressing the DATA-label open the same way emitCommonSymbol
  // / emitZerofill (line ~1222 / ~1242) do — leave CurrentDataSym
  // empty so all downstream emitData*/emitBytes calls short-circuit
  // and closeCurrentDataSymbol emits no GLOBL trailer.
  std::string Plan9 = symbolToPlan9(Name);
  if (isC2GoGoOwnedGlobal(C2GoGoOwnedGlobals, Plan9)) {
    DataSymSuppressed = true; // Wave AP.3: intended drop, not a gap
    CurrentDataOffset = 0;
    CurrentDataSize = 0;
    return;
  }
  // c2go (#134): skip emit of clang-generated typeinfo DATA blocks
  // (`c2go.typeinfo.struct.<X>` and the associated `c2go.gcbitmap.
  // struct.<X>` PrivateLinkage helper, which appears as
  // `l_c2go.gcbitmap.struct.<X>` in Mach-O). The Plan 9 .s references
  // the Go-compiler-emitted `type:<pkg>.<X>` symbol directly (see
  // AArch64Plan9InstPrinter.cpp's typeinfo MOVD rewrite), so any
  // local DATA blob would be a stale duplicate that may collide with
  // the canonical Go runtime type identity (breaking interface
  // assertions / reflect.TypeOf consistency).
  StringRef RawName = Mangled;  // already stripped leading underscore
  // GPT round 2 P1 E: §A2 dropped the `struct.` infix on typeinfo
  // globals (now `c2go.typeinfo.<X>` produced by CGC2GoTypeInfo);
  // gcbitmap globals also follow the new naming. Match both old and
  // new prefixes — old prefixes stay for legacy fallback paths
  // (C2GoMallocReplacement still emits literal/anon struct typeinfo
  // via the old name).
  if (RawName.starts_with("c2go.typeinfo.") ||
      RawName.starts_with("c2go.gcbitmap.") ||
      RawName.starts_with("l_c2go.typeinfo.") ||
      RawName.starts_with("l_c2go.gcbitmap.") ||
      RawName.starts_with("l.c2go.typeinfo.") ||
      RawName.starts_with("l.c2go.gcbitmap.") ||
      // Legacy/fallback (kept for safety):
      RawName.starts_with("c2go.typeinfo.struct.") ||
      RawName.starts_with("c2go.gcbitmap.struct.") ||
      RawName.starts_with("l_c2go.gcbitmap.struct.") ||
      RawName.starts_with("l.c2go.gcbitmap.struct.")) {
    // Leave CurrentDataSym empty — all subsequent emitDataIntDirective
    // / emitDataSymDirective / emitBytes / closeCurrentDataSymbol
    // calls short-circuit when CurrentDataSym is empty.
    // Wave AP.3: also mark the window suppressed so emitValueImpl drops
    // the blob's SYMBOLIC words (e.g. the typeinfo's `.quad
    // c2go.gcbitmap.<X>` field) silently instead of emitting a
    // misleading `// PLAN9-ERROR emitValue` comment — the blob's
    // integer words already flow through emitIntValue→emitBytes and
    // are dropped without a trace, so the lone symbolic word was the
    // only one leaving a fail-open marker (stress TU: 3 occurrences,
    // one per suppressed typeinfo of Node/NodeArr/AsFrame).
    DataSymSuppressed = true;
    CurrentDataOffset = 0;
    CurrentDataSize = 0;
    return;
  }
  CurrentDataSym = symbolToPlan9(Name);
  if (isPlan9LocalDataSym(Name))
    CurrentDataSym += "<>"; // #586: file-local scope for private data symbols
  CurrentDataOffset = 0;
  CurrentDataSize = 0;
}

// Stage 3 body: AArch64 backend basic-block label / CFI / debug temp /
// Mach-O private symbol. Sanitised via symbolToPlan9 (dots →
// underscores) to match the tryPrintInst-side rewrite.
void MCPlan9AsmStreamer::emitC2GoLocalLabel(StringRef Name) {
  std::string Local = symbolToPlan9(Name);
  *OS << Local << ":\n";
}

// Stage 4 body: non-local, non-data, non-c2go-metadata label — a
// regular C function defined inside the translation unit (e.g. SQLite's
// static helpers). These call each other via standard C ABI (raw bytes
// preserve register-passing); they're never called directly from Go.
// Emit a Plan 9 TEXT directive marked as splittable + NOFRAME:
//   * NOFRAME (512) — Go assembler doesn't insert a frame pointer save
//     (our raw bytes already manage SP).
//   * Splittable (no NOSPLIT bit) — Go linker's stackCheck graph
//     traversal short-circuits at splittable nodes (replacing the callee
//     subtree with a morestack edge), avoiding the exponential-path
//     explosion that NOSPLIT all-pairs caller graphs (1384 SQLite
//     helpers) would otherwise trigger.
// Tradeoff: the Go assembler injects a stackguard check + possible
// morestack call at function entry. The check is a few read-only
// instructions and doesn't disturb SP, so the raw-byte prologue remains
// correct. morestack itself never fires unless we run out of goroutine
// stack, which doesn't happen for SQLite's call depths on the default
// 8KB initial stack.
void MCPlan9AsmStreamer::emitC2GoTULocalFunctionLabel(StringRef Name) {
  std::string Plan9 = symbolToPlan9(Name);
  *OS << "TEXT " << Plan9 << "(SB), NOFRAME, $0\n";
}

void MCPlan9AsmStreamer::switchSection(MCSection *Section,
                                       uint32_t /*Subsec*/) {
  // Plan 9 has no `.section` directive. Use the section kind to
  // decide whether subsequent label/data calls produce text (TEXT
  // directive + instructions) or data (DATA / GLOBL).
  if (SymPrinter)
    SymPrinter->finishPending(*OS);
  closeCurrentDataSymbol();
  // Wave AP.3: leaving the section ends any suppressed-blob window.
  DataSymSuppressed = false;

  if (!Section) {
    InDataSection = false;
    return;
  }
  if (Section->isText()) {
    InDataSection = false;
    return;
  }
  // Heuristic by section name. Mach-O uses `__TEXT,__cstring`,
  // `__DATA,__const`, `__DATA,__data`, `__DATA,__bss`. ELF uses
  // `.rodata`, `.data`, `.bss`. RODATA = "read-only" name component;
  // anything else writeable.
  StringRef SN = Section->getName();
  InDataSection = true;
  DataIsROnly = SN.contains("rodata") || SN.contains("const") ||
                SN.contains("cstring");
}

// Render an MCSymbol-style name to its Plan 9 form. Plan 9 (`go tool
// asm`) accepts `·` (middle-dot, U+00B7) as the path/name separator
// and `/` for nested paths, but does NOT accept bare `.` in symbol
// names. We sanitise as follows:
//   `_foo`                  → `·foo`
//   `runtime.bar`           → `runtime·bar`     (last '.' = pkg sep)
//   `c2go.typeinfo.struct.Node`
//                           → `c2go_typeinfo_struct·Node`   (other '.' → '_')
//   `l_.str.42`             → `l__str_42`                   (local sym, no ·)
//   `LBB0_1`                → `LBB0_1`                      (bare local)
std::string MCPlan9AsmStreamer::symbolToPlan9(StringRef Name) {
  if (Name.empty()) return std::string();
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

  // Local labels (assembler-private). Whole name is identifier-form,
  // no middle-dot. #276: only Mach-O/ELF private forms are local —
  // `L*`, `.L*`, and `l_*` / `l<UPPER>*` (clang names string literals
  // `l_.str.NN` and CPI blocks `lCPI*`). A bare lowercase `l` followed
  // by a lowercase letter is an ordinary identifier (e.g. the static C
  // symbols `lengthFunc`, `likeInfoAlt`); treating it as a local label
  // dropped its `·` prefix, so its DATA/GLOBL definition and the
  // InstPrinter-emitted code reference disagreed (goSymToPlan9 uses the
  // narrower test below). Keep both in sync.
  if (Name[0] == 'L' || Name.starts_with(".L") ||
      (Name.size() >= 2 && Name[0] == 'l' &&
       (Name[1] == '_' || (Name[1] >= 'A' && Name[1] <= 'Z')))) {
    std::string Out = Name.str();
    sanitiseToIdent(Out);
    return Out;
  }

  // c2go #321: do NOT strip a leading `_` — see emitLabel comment above.
  // Neutral-ELF mangler in the Plan 9 codegen doesn't add the Mach-O
  // `_` global prefix, so a leading `_` is part of the source symbol.
  StringRef R = Name;

  bool HasSlash = R.contains('/');
  // #274: any char outside [A-Za-z0-9_/.] cannot be carried in a Plan 9
  // symbol even via the Unicode-substitute path — `-` (hyphenated import
  // paths) and `(`/`*`/`)` (Go method symbols `pkg.(*T).M`) must take the
  // //go:linkname bridge (path b) instead. (`.` and `/` ARE transformable.)
  bool HasIllegal = false;
  for (char C : R)
    if (!((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
          (C >= '0' && C <= '9') || C == '_' || C == '/' || C == '.')) {
      HasIllegal = true;
      break;
    }

  // Path (a): import-path-style and fully transformable — Unicode-substitute
  // emit. `.` → ·, `/` → ∕, everything else raw.
  if (HasSlash && !HasIllegal) {
    std::string Out;
    Out.reserve(R.size() + 8);
    for (char C : R) {
      if (C == '.')      Out.append(DotUTF8);
      else if (C == '/') Out.append(SlashUTF8);
      else               Out.push_back(C);
    }
    return Out;
  }

  // Path (b): contains a Plan-9-illegal char (`-`, or method `(`/`*`/`)`) —
  // sanitise to a current-pkg local name. The (local, raw) pair used to be
  // recorded in a thread_local `Plan9LinknameBridges` registry for a
  // manifest consumer, but that registry had no reader and was removed
  // as part of c2go #239; if surfacing is needed later it should hang off
  // the streamer instance.
  if (HasIllegal) {
    std::string Sanit = R.str();
    sanitiseToIdent(Sanit);
    return std::string(DotUTF8) + Sanit;
  }

  // No slash, no hyphen: plain `pkg.name`. Last `.` becomes middle-
  // dot; rest sanitised.
  size_t Last = StringRef::npos;
  for (size_t I = 0, E = R.size(); I != E; ++I)
    if (R[I] == '.')
      Last = I;
  if (Last == StringRef::npos) {
    std::string Out = R.str();
    sanitiseToIdent(Out);
    return std::string(DotUTF8) + Out;
  }
  std::string Path(R.begin(), R.begin() + Last);
  sanitiseToIdent(Path);
  std::string Tail(R.begin() + Last + 1, R.end());
  sanitiseToIdent(Tail);
  return Path + DotUTF8 + Tail;
}

void MCPlan9AsmStreamer::closeCurrentDataSymbol() {
  if (CurrentDataSym.empty()) return;
  // Plan 9 GLOBL trailer. RODATA flag = 8 (textflag.h). DUPOK = 2,
  // NOPTR = 16, etc. For c2go we conservatively use RODATA only
  // when DataIsROnly; otherwise plain global DATA (flag 0).
  const char *Flag = DataIsROnly ? "RODATA" : "NOPTR";
  *OS << "GLOBL " << CurrentDataSym << "(SB), " << Flag
      << ", $" << CurrentDataSize << "\n\n";
  CurrentDataSym.clear();
  CurrentDataOffset = 0;
  CurrentDataSize = 0;
}

void MCPlan9AsmStreamer::emitDataIntDirective(uint64_t Value, unsigned Size) {
  if (CurrentDataSym.empty()) return; // shouldn't happen
  *OS << "DATA " << CurrentDataSym << "+" << CurrentDataOffset
      << "(SB)/" << Size << ", $0x" << format_hex_no_prefix(Value, Size * 2)
      << "\n";
  CurrentDataOffset += Size;
  if (CurrentDataOffset > CurrentDataSize)
    CurrentDataSize = CurrentDataOffset;
}

void MCPlan9AsmStreamer::emitDataSymDirective(const MCExpr *Expr,
                                               unsigned Size) {
  if (CurrentDataSym.empty()) return;
  // #240: pull the referenced MCSymbol and its additive byte offset out
  // of the MCExpr structurally instead of rendering the expr to text and
  // re-sanitising the string. The text round-trip mangled a `sym+N`
  // initializer's `+N` into `_N` (symbolToPlan9 maps any non-ident char
  // to `_`), producing a dangling `$·sym_N(SB)` reference. Splitting the
  // symbol from the offset lets us emit the correct `$·sym+N(SB)` form.
  if (const MCSymbol *Sym = referencedSymbol(Expr)) {
    std::string Plan9 = symbolToPlan9(Sym->getName());
    if (isPlan9LocalDataSym(Sym->getName()))
      Plan9 += "<>"; // #586: match the file-local data-symbol definition
    int64_t Off = referencedOffset(Expr);
    *OS << "DATA " << CurrentDataSym << "+" << CurrentDataOffset << "(SB)/"
        << Size << ", $" << Plan9;
    if (Off > 0)
      *OS << "+" << Off;
    else if (Off < 0)
      *OS << Off; // negative value prints its own '-'
    *OS << "(SB)\n";
    CurrentDataOffset += Size;
    if (CurrentDataOffset > CurrentDataSize)
      CurrentDataSize = CurrentDataOffset;
    return;
  }
  // Defensive fallback for any exotic MCExpr shape (no plain symbol
  // reference): render via its own print method then translate.
  std::string Rendered;
  {
    raw_string_ostream RS(Rendered);
    getContext().getAsmInfo()->printExpr(RS, *Expr);
  }
  std::string Plan9 = symbolToPlan9(Rendered);
  *OS << "DATA " << CurrentDataSym << "+" << CurrentDataOffset
      << "(SB)/" << Size << ", $" << Plan9 << "(SB)\n";
  CurrentDataOffset += Size;
  if (CurrentDataOffset > CurrentDataSize)
    CurrentDataSize = CurrentDataOffset;
}

void MCPlan9AsmStreamer::emitRawBytesOrFail(const MCInst &Inst,
                                             const MCSubtargetInfo &STI) {
  if (!CE) {
    *OS << "\t// PLAN9-ERROR no MCCodeEmitter; opcode "
        << InstPrinter->getOpcodeName(Inst.getOpcode()) << "\n";
    return;
  }
  SmallVector<char, 8> Bytes;
  SmallVector<MCFixup, 0> Fixups;
  CE->encodeInstruction(Inst, Bytes, Fixups, STI);
  if (!Fixups.empty()) {
    // Symbol-bearing MCInst that tryPrintInst didn't translate —
    // emitting WORD with zero offset would be a silent miscompile
    // (branch loops, ADR-of-nothing, etc.).
    //
    // c2go #298 Track AN.1 — fail CLOSED on X86. The pre-AN.1 code
    // emitted a `// PLAN9-ERROR ...` *comment* and continued; Go's
    // assembler does not reject comments, so the instruction was
    // silently swallowed: RIP-relative global stores no-op'd and
    // compare instructions left stale EFLAGS for the following
    // Jcc/CMOV (amd64 `STRESS ASCAST FAIL validator=0` root,
    // deterministic at -O0/-O2 and any depth). A coverage gap must
    // abort the compile so it is located at build time, never shipped
    // as a miscompiled .s. X86 producer harnesses already classify
    // this signature as PRODUCER_CRASH and SKIP honestly
    // (run_amd64.sh PRODUCER_CRASH_RE / regen_verify_amd64.sh SKIPPED
    // sentinel) — fail-closed does not redden any green amd64 gate.
    //
    // c2go #298 Track AO.2 — AArch64 now fails closed too (unified with
    // the AN.1 X86 policy above). The one production gap that motivated
    // the temporary AArch64 fail-open carve-out — a swallowed symbolic
    // `LDRQui` (128-bit SIMD load) in SQLite's sqlite3FindInIndex
    // vectorised loop, where the flushed-ADRP base register tracking is
    // dropped at a label boundary (2026-06-10) — is covered by
    // AArch64Plan9InstPrinter::tryPrintLDRSTRSymbolicField's Q-reg
    // `:lo12:` path. Any remaining miss on either target is a coverage
    // bug that must abort the compile at build time, never ship as a
    // silently truncated .s.
    report_fatal_error(
        Twine("MCPlan9AsmStreamer: unhandled symbol-bearing instruction "
              "(opcode=") +
        InstPrinter->getOpcodeName(Inst.getOpcode()) + ", " +
        Twine(Fixups.size()) +
        " fixup(s)) — extend the target's Plan-9 InstPrinter "
        "(X86Plan9InstPrinter / AArch64Plan9InstPrinter)");
  }
  if (Bytes.empty())
    return;
  // c2go #298 Wave AJ.1 — `WORD` is per-arch in Go's assembler. AArch64
  // (cmd/internal/obj/arm64/asm7.go:465 `{AWORD, ..., 14, 4, 0, 0, 0}`)
  // emits AWORD as 4 bytes (the fixed-width AArch64 instruction encoding
  // size). X86 (cmd/internal/obj/x86/asm6.go:1519 `{AWORD, ybyte, Px,
  // opBytes{2}}`) emits AWORD as 2 bytes; the 4-byte data pseudo on x86
  // is `LONG` (asm6.go:1176). Use AWORD on AArch64 (preserves the
  // byte-identical AArch64 path) and LONG on X86 — emitting WORD as
  // 4-bytes on x86 truncated each raw-byte instruction to its lower
  // 2 bytes, producing the SIGILL we observed at stress_init+0x34
  // (`48 c7 40 10 …` was emitted as `WORD $0x1040c748` → x86 obj asm
  // wrote only `48 c7` → next 2 bytes became disjoint data → CPU
  // decoded `MOVQ $0, (%rax)` mid-instruction and trapped).
  const llvm::Triple &TT = STI.getTargetTriple();
  StringRef FourByteMnemonic = TT.isX86() ? "LONG" : "WORD";
  size_t I = 0;
  while (I + 4 <= Bytes.size()) {
    uint32_t W = support::endian::read32le(Bytes.data() + I);
    *OS << "\t" << FourByteMnemonic << " $0x"
        << format_hex_no_prefix(W, 8) << "\n";
    I += 4;
  }
  while (I < Bytes.size()) {
    uint8_t B = (uint8_t)Bytes[I];
    *OS << "\tBYTE $0x" << format_hex_no_prefix(B, 2) << "\n";
    ++I;
  }
}

void MCPlan9AsmStreamer::emitInstruction(const MCInst &Inst,
                                          const MCSubtargetInfo &STI) {
  emitFileHeaderIfNeeded();
  // 1. Try Plan 9 symbolic translation (address-aware mnemonics).
  if (SymPrinter && SymPrinter->tryPrintInst(&Inst, STI, *OS))
    return;
  // 2. Raw-byte fallback with fixup guard.
  emitRawBytesOrFail(Inst, STI);
  // Notify the symbolic printer so its register-state tracker keeps
  // pace with the just-emitted (untranslated) instruction's defs.
  if (SymPrinter)
    SymPrinter->notifyRawBytesEmitted(&Inst);
}

void MCPlan9AsmStreamer::emitValueImpl(const MCExpr *Value, unsigned Size,
                                        SMLoc /*Loc*/) {
  if (!InDataSection || CurrentDataSym.empty()) {
    // c2go #298 Wave AP.3 (Wave AO F4 follow-up) — close the last
    // fail-open value path. Two distinct situations used to share one
    // `// PLAN9-ERROR emitValue` comment that Go's assembler accepts
    // silently:
    //  1. The current data label was DELIBERATELY suppressed by
    //     emitC2GoDataLabel (duplicate c2go typeinfo/gcbitmap blob,
    //     #387 Go-owned global). Dropping the blob's values is the
    //     suppression contract — its integer words already vanish
    //     silently via emitIntValue→emitBytes; only a symbolic word
    //     (e.g. typeinfo's `.quad c2go.gcbitmap.<X>`) reached here.
    //     Drop it silently too.
    //  2. A value emitted outside any data section / before any label
    //     — a genuine streamer coverage gap whose bytes would be
    //     MISSING from the .s (silent data corruption). Fail closed,
    //     unified with the AN.1/AO.2 swallowed-instruction policy:
    //     abort at build time, never ship a truncated .s. Production
    //     artifacts carry zero instances of this form (sqlite amd64 +
    //     aarch64, stress 4-config matrix — 2026-06-11 audit).
    if (InDataSection && DataSymSuppressed)
      return; // intended drop (suppressed typeinfo/gcbitmap/Go-owned)
    report_fatal_error(
        Twine("MCPlan9AsmStreamer: emitValue(size=") + Twine(Size) +
        ") outside data section / before label — value bytes would be "
        "silently dropped from the Plan-9 .s (fail-closed; see Wave AP.3)");
  }
  if (!Value) {
    emitDataIntDirective(0, Size);
    return;
  }
  // If the expression is a pure integer constant, evaluate and emit
  // an integer DATA directive. Otherwise it carries a symbol
  // reference — emit the symbolic form.
  int64_t IntVal = 0;
  if (Value->evaluateAsAbsolute(IntVal)) {
    uint64_t U = (uint64_t)IntVal;
    if (Size < 8)
      U &= ((uint64_t)1 << (Size * 8)) - 1;
    emitDataIntDirective(U, Size);
    return;
  }
  emitDataSymDirective(Value, Size);
}

void MCPlan9AsmStreamer::finishImpl() {
  if (SymPrinter)
    SymPrinter->finishPending(*OS);
  // c2go Phase 1: flush the final function's gclocals symbol before
  // closing any open data symbol. flushC2GoStackmaps emits DATA/GLOBL
  // directives that themselves switch into a data-emission posture,
  // so it must complete before the closing GLOBL of an unrelated
  // data symbol fires.
  flushC2GoStackmaps();
  closeCurrentDataSymbol();
}

// c2go Phase 1: emit FUNCDATA + initial PCDATA right after a TEXT
// directive. Idempotent per FnName.
//
// c2go #298 Wave AA Track A (F3 contract lock): SavedLinkSize is the
// per-arch saved-LR slot size inside the physical frame the producer
// reports (AArch64 = 8, X86 amd64 = 0). It drives the locals-bitmap
// width — see the long comment below for the derivation.
void MCPlan9AsmStreamer::emitC2GoFuncDataPreamble(StringRef FnName,
                                                  int FrameSize, int ArgSize,
                                                  unsigned SavedLinkSize) {
  // Skip if no managed frame slots — nothing for GC to scan.
  if (FrameSize <= 0)
    return;
  // Initialise / reset per-function accumulator.
  if (!CurFn || CurFn->FnName != std::string(FnName)) {
    CurFn.emplace();
    CurFn->FnName = std::string(FnName);
    CurFn->FrameSize = FrameSize;
    // ARGS pointer-map width (FUNCDATA $0): one bit per pointer-sized word of
    // the incoming arg+result frame (argsize). Pointer-aligned, so round up.
    CurFn->ArgNbit = ArgSize > 0 ? static_cast<uint32_t>((ArgSize + 7) / 8) : 0;
    // 8-byte pointer (64-bit AArch64 / x86_64). nbit = number of
    // pointer-sized words in the Go-runtime "locals" region, which is
    // [sp, varp) where varp = fp - SavedLinkSize. AArch64: fp = sp +
    // framesize and varp -= 8 for the saved-LR / saved-FP link
    // (traceback.go), so the locals region width is (framesize - 8)
    // and the runtime scans [varp - nbit*8, varp) with nbit =
    // (framesize-8)/8 — that region starts exactly at sp, making a
    // managed pointer at sp+off land on bit off/8 — matching how
    // LowerSTATEPOINT records SP-relative offsets. X86 amd64: no LR
    // slot inside the declared frame (return address is at rsp+0 but
    // sits above the autosize-allocated region), so SavedLinkSize = 0
    // and nbit = framesize/8. We only set bits that actually correspond
    // to managed pointers; the rest stay 0.
    int NbitBytes = FrameSize - static_cast<int>(SavedLinkSize);
    if (NbitBytes < 0)
      NbitBytes = 0;
    CurFn->Nbit = static_cast<uint32_t>(NbitBytes / 8);
    // #287 (Option 3): reserve locals bitmap ENTRY 0 = EMPTY. The Go runtime
    // falls back to entry 0 when PCDATA $1 == -1 — which holds at the
    // function-entry morestack PC, where the frame's locals are still
    // uninitialized garbage. entry 0 must therefore scan nothing. The full
    // pointer-slot mask (from the #287 entry stackmap) is interned later as
    // entry >= 1 and selected by the body's PCDATA $1, so it is used at every
    // call's return PC (where copystack may relocate live stack pointers)
    // while the entry morestack stays safe. Functions with no stackmap site
    // keep a single empty bitmap (unchanged behavior).
    {
      std::vector<uint8_t> EmptyBits((CurFn->Nbit + 7) / 8, 0);
      internC2GoBitmap(EmptyBits);
    }
  }
  if (CurFn->PreambleEmitted)
    return;
  CurFn->PreambleEmitted = true;
  // Plan 9 .s syntax: FUNCDATA $idx, sym(SB). Use middle-dot per Plan 9
  // convention. Initial PCDATA $1, $-1 marks the function entry as an
  // unsafe point (no map valid until a STACKMAP fires) — matches Go
  // compiler's prologue convention.
  // #220 — Don't emit FUNCDATA $1 here at function entry because
  // the gclocals symbol name is content-hashed and the hash isn't
  // known until flushC2GoStackmaps runs (we've seen all stackmap
  // sites by then). Plan 9 / Go assembler allows FUNCDATA anywhere
  // inside the function body — it associates by function not PC —
  // so emitting at function end (just before the next TEXT or EOF)
  // still attaches correctly. Initial PCDATA $1, $-1 is cheap and
  // emit-at-entry is fine.
  *OS << "\tPCDATA $1, $-1\n";
}

void MCPlan9AsmStreamer::recordC2GoStackmapSite(
    ArrayRef<int64_t> SpOffsets) {
  if (!CurFn || CurFn->Nbit == 0)
    return;
  uint32_t Nbit = CurFn->Nbit;
  size_t NbyteCount = (Nbit + 7) / 8;
  std::vector<uint8_t> Bits(NbyteCount, 0);
  for (int64_t Off : SpOffsets) {
    if (Off < 0)
      continue;
    uint64_t Word = static_cast<uint64_t>(Off) / 8;
    if (Word >= Nbit)
      continue;
    Bits[Word / 8] |= uint8_t(1) << (Word % 8);
  }
  // c2go #489 family (Wave AP.2, aarch64 -O0 stress root): OR the
  // backend-computed aggregate-field pointer mask into this site's bits
  // BEFORE interning. The flush-time OR in emitC2GoFuncData only rewrites
  // bitmaps at index >= 1; a site whose live SCALAR set is empty used to
  // intern its all-zero bits straight onto the reserved EMPTY entry 0, so a
  // function whose every site has an empty scalar live set (e.g. only
  // aggregate locals with pointer fields — stress_ascast_recurse's
  // `local`/`canary`) flushed FUNCDATA $1 as a single all-zero bitmap and
  // copystack never relocated the aggregate's pointer fields (GC
  // pointer-to-unallocated-span / SIGBUS). This implements the contract
  // already documented at the C2GoSafepoint emission site: "an empty live
  // set interns to bitmap index 0 only when the agg mask is also empty,
  // otherwise to a distinct >=1 entry". Statepoint mode (-O2, default
  // config) publishes no agg mask — #327 suppresses the aggregate-field
  // scan and M5 ptrslot-liveness (default on) keeps the #330 spill-tag
  // fallback off — so this is a no-op there. Under the non-default
  // `-c2go-disable=ptrslot-liveness` config the #330 M3 fallback CAN
  // publish anonymous-ptr-spill bits in statepoint mode too; OR-ing them
  // here merely matches the flush-time index>=1 OR those sites already
  // receive (GPT Wave AP F2: workload evidence, not a code invariant).
  // Entry 0 itself is pre-interned at preamble time and stays EMPTY
  // (#287 morestack-at-entry safety unchanged).
  {
    auto MetaIt = C2GoFnMeta.find(CurFn->FnName);
    if (MetaIt != C2GoFnMeta.end()) {
      const std::vector<uint8_t> &Agg = MetaIt->second.LocalsAggMaskBytes;
      for (size_t I = 0; I < Bits.size() && I < Agg.size(); ++I)
        Bits[I] |= Agg[I];
    }
  }
  // c2go #312: scrub union-ambiguous words from this per-PC stackmap bitmap.
  // Stack-slot coloring can place a managed pointer local onto a union's slot,
  // so the stackmap path may mark a union word that, at this PC, holds the
  // union's integer member (e.g. yy_reduce's YYMINORTYPE holding 0x9). The
  // static FUNCDATA $1 cannot disambiguate per-PC, so we conservatively drop
  // these words (#313 = sound per-PC fix).
  // c2go #376: union-ambig mask now lives on the per-function metadata entry.
  {
    auto MetaIt = C2GoFnMeta.find(CurFn->FnName);
    if (MetaIt != C2GoFnMeta.end()) {
      const std::vector<uint8_t> &Amb = MetaIt->second.LocalsAmbigMaskBytes;
      for (size_t I = 0; I < Bits.size() && I < Amb.size(); ++I)
        Bits[I] &= ~Amb[I];
    }
  }
  unsigned Idx = internC2GoBitmap(Bits);
  *OS << "\tPCDATA $1, $" << Idx << "\n";
}

unsigned MCPlan9AsmStreamer::internC2GoBitmap(ArrayRef<uint8_t> Bits) {
  // Render bytes as a hex string for StringMap keying. Avoids a
  // bespoke DenseMapInfo<vector<uint8_t>>.
  std::string Key;
  Key.reserve(Bits.size() * 2);
  static const char Hex[] = "0123456789abcdef";
  for (uint8_t B : Bits) {
    Key.push_back(Hex[B >> 4]);
    Key.push_back(Hex[B & 0xF]);
  }
  auto It = CurFn->BitmapIndex.find(Key);
  if (It != CurFn->BitmapIndex.end())
    return It->second;
  unsigned Idx = static_cast<unsigned>(CurFn->Bitmaps.size());
  CurFn->Bitmaps.emplace_back(Bits.begin(), Bits.end());
  CurFn->BitmapIndex[Key] = Idx;
  return Idx;
}

void MCPlan9AsmStreamer::emitC2GoFuncDataSymbol(
    unsigned FuncDataIdx, uint32_t Nbit,
    ArrayRef<std::vector<uint8_t>> Bitmaps) {
  uint32_t N = static_cast<uint32_t>(Bitmaps.size());
  size_t BytesPerBitmap = (Nbit + 7) / 8;
  uint64_t TotalSize = 8 + uint64_t(N) * BytesPerBitmap;
  // #220 + #228 — content-hashed name `gclocals·<hex>` (UTF-8 middle
  // dot). The `gclocals·` prefix is special-cased by the Go linker
  // (cmd/link/internal/ld/symtab.go:558) which auto-assigns it the
  // `go:func.*` carrier required for funcdata references. The hash
  // covers (n, nbit, all bitmap bytes) so any two functions whose
  // GC bitmap is byte-identical share a symbol — drastically cuts
  // the Go linker's pclntab/funcdata processing work. DUPOK on the
  // GLOBL line lets the linker accept any accidental re-emit. Both the
  // args map (FUNCDATA $0) and the locals map (FUNCDATA $1) are stackmap
  // structures and share this scheme/prefix.
  uint64_t Hash = 0xcbf29ce484222325ULL; // FNV-1a 64-bit
  auto MixByte = [&](uint8_t B) {
    Hash ^= uint64_t(B);
    Hash *= 0x100000001b3ULL;
  };
  for (int B : {int(N & 0xff), int((N >> 8) & 0xff), int((N >> 16) & 0xff),
                int((N >> 24) & 0xff)})
    MixByte(uint8_t(B));
  for (int B : {int(Nbit & 0xff), int((Nbit >> 8) & 0xff),
                int((Nbit >> 16) & 0xff), int((Nbit >> 24) & 0xff)})
    MixByte(uint8_t(B));
  for (const auto &Bitmap : Bitmaps) {
    for (size_t i = 0; i < BytesPerBitmap; ++i)
      MixByte(i < Bitmap.size() ? Bitmap[i] : uint8_t(0));
  }
  llvm::SmallString<32> SymBuf;
  llvm::raw_svector_ostream SymOS(SymBuf);
  // UTF-8 "·" (U+00B7) middle dot — Go linker pattern-matches the
  // `gclocals·` prefix on the symbol's name string.
  SymOS << "gclocals\xc2\xb7" << format_hex_no_prefix(Hash, 16);
  std::string Sym(SymBuf.begin(), SymBuf.end());

  // FUNCDATA association is by enclosing TEXT block (not by PC), so
  // emitting at function END (just before next TEXT or EOF) still
  // attaches to the function we just closed.
  *OS << "\tFUNCDATA $" << FuncDataIdx << ", " << Sym << "(SB)\n";

  // Emit GLOBL+DATA for this symbol only the first time we see
  // its content. Subsequent references just reuse it via FUNCDATA above.
  if (EmittedGclocalsSyms.insert(Sym).second) {
    *OS << "DATA " << Sym << "+0(SB)/4, $" << N << "\n";
    *OS << "DATA " << Sym << "+4(SB)/4, $" << Nbit << "\n";
    uint64_t Off = 8;
    for (const auto &Bitmap : Bitmaps) {
      for (size_t i = 0; i < BytesPerBitmap; ++i) {
        uint8_t B = i < Bitmap.size() ? Bitmap[i] : uint8_t(0);
        *OS << "DATA " << Sym << "+" << Off << "(SB)/1, $0x"
            << format_hex_no_prefix(B, 2) << "\n";
        ++Off;
      }
    }
    // DUPOK (Plan 9 flag 2) + RODATA (8) = 10.
    *OS << "GLOBL " << Sym << "(SB), DUPOK|RODATA, $" << TotalSize << "\n\n";
  }
}

void MCPlan9AsmStreamer::flushC2GoStackmaps() {
  if (!CurFn)
    return;
  // c2go #376: single lookup into per-function metadata; reused for the
  // args mask, locals agg mask, and ambig scrub.
  const C2GoFunctionMetadata *Meta = nullptr;
  {
    auto MetaIt = C2GoFnMeta.find(CurFn->FnName);
    if (MetaIt != C2GoFnMeta.end())
      Meta = &MetaIt->second;
  }
  // FUNCDATA $0 — ARGS pointer map (FUNCDATA_ArgsPointerMaps). Required so the
  // runtime's copystack / GC stack scan can type the incoming arg+result frame
  // of a splittable function; without it the runtime throws "missing stackmap"
  // / "untyped args". The args mask content is the same at every PC; it is
  // replicated below to match the locals-map bitmap count (the runtime indexes
  // both maps with the SAME PCDATA_StackMapIndex — see below).
  size_t ArgNbyte = (CurFn->ArgNbit + 7) / 8;
  std::vector<uint8_t> ArgBits(ArgNbyte, 0);
  // c2go #287 (Option 3): copy in the pointer-word mask published by clang
  // (c2go-argptrmask, forwarded via updateArgPtrMask). This sets a bit for
  // each pointer-typed argument word so Go's copystack relocates pointer args
  // that point into the moving goroutine stack (cross-Exec dangling-`&local`
  // fix). Absent (boundary symbols / no pointer args) => all-zeros, the prior
  // behavior.
  if (Meta) {
    const std::vector<uint8_t> &Mask = Meta->ArgPtrMaskBytes;
    for (size_t I = 0; I < ArgNbyte && I < Mask.size(); ++I)
      ArgBits[I] = Mask[I];
  }
  // c2go #287: the Go runtime indexes the args map (FUNCDATA $0) AND the
  // locals map (FUNCDATA $1) with the SAME PCDATA_StackMapIndex value, and
  // bounds-checks each against its own bitmap count `n`
  // (runtime/stkframe.go: `throw("bad symbol table")` when pcdata >= n). So
  // the args map MUST hold the same number of bitmaps as the locals map.
  // The args pointer mask is constant across all PCs (incoming args are live
  // for the whole call), so replicate it once per locals-map entry. (Before
  // #287 reserved an empty locals entry-0, slot functions had locals n==1 and
  // this matched a single args bitmap; with multiple locals entries the args
  // map must grow in lockstep or every body safepoint with PCDATA $1 >= 1
  // would throw.)
  size_t NMaps = CurFn->Bitmaps.empty() ? 1 : CurFn->Bitmaps.size();
  std::vector<std::vector<uint8_t>> ArgMaps;
  ArgMaps.reserve(NMaps);
  for (size_t I = 0; I + 1 < NMaps; ++I)
    ArgMaps.push_back(ArgBits);
  ArgMaps.push_back(std::move(ArgBits));
  emitC2GoFuncDataSymbol(/*FUNCDATA_ArgsPointerMaps=*/0, CurFn->ArgNbit,
                         ArgMaps);

  // FUNCDATA $1 — LOCALS pointer map. Managed-local bits are set at safepoints
  // via recordC2GoStackmapSite (indexed by PCDATA $1). Default to a single
  // all-zeros bitmap when no safepoint recorded any managed local.
  if (CurFn->Bitmaps.empty()) {
    size_t NbyteCount = (CurFn->Nbit + 7) / 8;
    CurFn->Bitmaps.emplace_back(NbyteCount, 0);
  }
  // c2go #288: OR the backend-computed aggregate-field pointer mask (pointer
  // FIELDS of struct/array stack locals, forwarded via updateLocalsAggMask)
  // into every BODY locals bitmap (index >= 1). Index 0 is the reserved EMPTY
  // entry-0 map (used at the entry morestack, locals uninitialized) and stays
  // untouched. Scalar pointer slots are already in these bitmaps (from the
  // entry stackmap); aggregate fields cannot go through stackmap operands
  // (GEP-per-field exhausts the -O0 register allocator), so they are added
  // here from the backend mask.
  if (Meta) {
    const std::vector<uint8_t> &Agg = Meta->LocalsAggMaskBytes;
    if (!Agg.empty()) {
      for (size_t B = 1; B < CurFn->Bitmaps.size(); ++B) {
        std::vector<uint8_t> &Bits = CurFn->Bitmaps[B];
        for (size_t I = 0; I < Bits.size() && I < Agg.size(); ++I)
          Bits[I] |= Agg[I];
      }
    }
  }
  // c2go #312: final scrub of union-ambiguous words across ALL locals bitmaps
  // (defensive; recordC2GoStackmapSite already scrubs per-PC stackmap bits and
  // the agg mask excludes them, but this guarantees no ambiguous word survives
  // regardless of source ordering).
  if (Meta) {
    const std::vector<uint8_t> &Amb = Meta->LocalsAmbigMaskBytes;
    if (!Amb.empty()) {
      for (std::vector<uint8_t> &Bits : CurFn->Bitmaps)
        for (size_t I = 0; I < Bits.size() && I < Amb.size(); ++I)
          Bits[I] &= ~Amb[I];
    }
  }
  emitC2GoFuncDataSymbol(/*FUNCDATA_LocalsPointerMaps=*/1, CurFn->Nbit,
                         CurFn->Bitmaps);

  CurFn.reset();
}

// c2go #387 §B4 phase 6 sub-step 1: bare-name lookup for the Go-owned
// suppression set. `Plan9` arrives as e.g. "·gRing"; the middle-dot is
// the UTF-8 two-byte sequence "\xc2\xb7". Returns true if the global's
// storage is owned by the Go side and the streamer should NOT emit a
// GLOBL trailer for it (the Go-side bodyless `var X unsafe.Pointer`
// is the unique storage definition, registered into moduledata.gcdata
// automatically so the runtime scans it as a root).
static bool isC2GoGoOwnedGlobal(const StringSet<> &Set, StringRef Plan9) {
  StringRef Bare = Plan9;
  // Two-byte UTF-8 middle-dot prefix used by symbolToPlan9 for the
  // c2go-extern visibility convention.
  if (Bare.consume_front("\xc2\xb7"))
    return Set.contains(Bare);
  return false;
}

// c2go #401(c): shared body for emitCommonSymbol / emitZerofill — both
// emit the same Plan 9 GLOBL trailer (no DATA backing) and both honour
// the Go-owned-global suppression set. See the header doc-comment for
// the rationale.
void MCPlan9AsmStreamer::emitGLOBLOrSuppress(MCSymbol *Symbol, uint64_t Size,
                                             StringRef Tag) {
  closeCurrentDataSymbol();
  emitFileHeaderIfNeeded();
  std::string Rendered;
  {
    raw_string_ostream RS(Rendered);
    Symbol->print(RS, getContext().getAsmInfo());
  }
  std::string Plan9 = symbolToPlan9(Rendered);
  if (isC2GoGoOwnedGlobal(C2GoGoOwnedGlobals, Plan9))
    return; // Go side owns the storage; no GLOBL trailer.
  *OS << "GLOBL " << Plan9 << "(SB), " << Tag << ", $" << Size << "\n\n";
}

void MCPlan9AsmStreamer::emitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                                          Align /*ByteAlignment*/) {
  // Plan 9 doesn't have .comm; the linker zero-inits any GLOBL that
  // doesn't have explicit DATA backing.
  emitGLOBLOrSuppress(Symbol, Size, "NOPTR");
}

void MCPlan9AsmStreamer::emitZerofill(MCSection * /*Section*/, MCSymbol *Symbol,
                                       uint64_t Size, Align /*ByteAlignment*/,
                                       SMLoc /*Loc*/) {
  // Mach-O .zerofill — BSS-style zero-initialized global. In Plan 9
  // every GLOBL without DATA is auto-zero-init by the linker, so we
  // just emit the GLOBL trailer with the right size.
  if (!Symbol) return;
  emitGLOBLOrSuppress(Symbol, Size, "NOPTR");
}

void MCPlan9AsmStreamer::emitBytes(StringRef Data) {
  if (!InDataSection || CurrentDataSym.empty()) {
    // Outside data sections, raw-byte emission usually comes from
    // alignment fill in text — silently swallow (we don't model
    // text-section alignment in Plan 9 output).
    return;
  }
  // Emit DATA directives. Use /8 chunks for FP-pool aligned data,
  // tail bytes go through /1 chunks.
  size_t I = 0;
  const uint8_t *P = reinterpret_cast<const uint8_t *>(Data.data());
  size_t N = Data.size();
  while (I + 8 <= N) {
    uint64_t W = 0;
    for (int B = 0; B < 8; ++B)
      W |= ((uint64_t)P[I + B]) << (8 * B); // little-endian
    emitDataIntDirective(W, 8);
    I += 8;
  }
  while (I < N) {
    emitDataIntDirective(P[I], 1);
    ++I;
  }
}

void MCPlan9AsmStreamer::emitFill(const MCExpr &NumBytes,
                                   uint64_t FillValue, SMLoc /*Loc*/) {
  if (!InDataSection || CurrentDataSym.empty()) {
    *OS << "\t// PLAN9-ERROR emitFill outside data section / before label\n";
    return;
  }
  int64_t N = 0;
  if (!NumBytes.evaluateAsAbsolute(N) || N < 0) {
    *OS << "\t// PLAN9-ERROR emitFill non-constant count\n";
    return;
  }
  // Emit `FillValue`-repeated bytes as a sequence of DATA
  // directives. Use /8 chunks for speed when zero-filled.
  while (N >= 8) {
    uint64_t Chunk = 0;
    if (FillValue) {
      uint64_t B = FillValue & 0xff;
      Chunk = B;
      for (int s = 8; s < 64; s += 8) Chunk |= (B << s);
    }
    emitDataIntDirective(Chunk, 8);
    N -= 8;
  }
  while (N >= 1) {
    emitDataIntDirective(FillValue & 0xff, 1);
    N -= 1;
  }
}
