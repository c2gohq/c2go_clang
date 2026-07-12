//===- MCPlan9AsmStreamer.h - Plan 9 (Go assembler) MCStreamer ----*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// MCStreamer subclass that writes Plan 9 syntax (consumable by Go's
// `go tool asm`). Phase E v0+1 of the c2go tooling, step 2/5.
//
// Differences from MCAsmStreamer:
//   * File header is `#include "textflag.h"` (no .file/.section).
//   * Symbol labels for c2go_extern functions emit
//       `TEXT ·name(SB), NOSPLIT|NOFRAME, $framesize-argsize`
//     rather than `_name:`.  framesize/argsize come from a side-channel
//     populated by clang from the manifest before AsmPrinter runs.
//   * .section / .globl / .quad / .byte / .data directives are
//     suppressed — Plan 9 has no equivalent and we only emit text.
//   * emitInstruction delegates to the per-target Plan 9 InstPrinter
//     (AArch64Plan9InstPrinter, ...).
//
// See docs/c2go_phase_e_redo_plan.md.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCPLAN9ASMSTREAMER_H
#define LLVM_MC_MCPLAN9ASMSTREAMER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/MC/MCC2GoFunctionMetadata.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace llvm {

class MCContext;
class MCInstPrinter;
class MCCodeEmitter;
class formatted_raw_ostream;

/// #654b: single source of truth for "is this a COMPILER-GENERATED private /
/// local symbol" in the Plan 9 pipeline. Locals render file-scoped
/// (`name<>(SB)`, sanitised, no middle-dot); everything else is an ordinary
/// package symbol (`·name`). This predicate used to be duplicated (with
/// "keep in lockstep" comments) across MCPlan9AsmStreamer
/// {isPlan9LocalDataSym, emitLabel stage-3, symbolToPlan9} and the
/// {AArch64,X86} InstPrinters {goSymToPlan9, isLocalLabelName}; the copies
/// drifted twice (#276 `likeInfoNorm`, #654b Lua's `l_alloc`/`LTnum`), each
/// time splitting a static C function's TEXT definition (`·name`) from its
/// references (`name<>`). Key invariant: a C identifier can never contain a
/// `.`, so a dot marks a generated name; the only dot-free generated shapes
/// are the well-known label/constant-pool prefixes below.
/// (AArch64Plan9InstPrinter::isLocalLabelName stays broader by design — it
/// classifies BRANCH TARGETS only, which are never C function symbols.)
inline bool isPlan9CompilerLocalSym(StringRef Name) {
  if (Name.starts_with(".L"))
    return true; // ELF locals: .LBB / .Ltmp / .L.str / .LCPI / .LJTI ...
  if (Name.empty())
    return false;
  if (Name[0] == 'L')
    return Name.contains('.') || Name.starts_with("LBB") ||
           Name.starts_with("Ltmp") || Name.starts_with("LCPI") ||
           Name.starts_with("LJTI") || Name.starts_with("Lloh") ||
           Name.starts_with("Lfunc_"); // Mach-O-style, e.g. NOT Lua's LTnum
  if (Name.size() >= 2 && Name[0] == 'l' &&
      (Name[1] == '_' || (Name[1] >= 'A' && Name[1] <= 'Z')))
    return Name.contains('.') || Name.starts_with("lCPI") ||
           Name.starts_with("lJTI"); // l_.str.N / l_c2go.*, NOT Lua's l_alloc
  return false;
}

/// Per-target Plan 9 symbolic printing interface. AArch64Plan9InstPrinter
/// implements this (multiple inheritance alongside AArch64InstPrinter).
/// MCPlan9AsmStreamer holds a non-owning pointer to this interface so
/// the target's tryPrintInst can be called without lib/MC depending on
/// a specific target library.
class MCPlan9SymbolicPrinter {
public:
  virtual ~MCPlan9SymbolicPrinter() = default;
  /// Try to print MI as a Plan 9 symbolic directive. Returns true on
  /// success (output written to O); false on miss — caller falls back
  /// to raw-byte encoding via MCCodeEmitter.
  virtual bool tryPrintInst(const MCInst *MI, const MCSubtargetInfo &STI,
                             raw_ostream &O) = 0;
  /// Flush any state buffered across instructions (e.g. a pending
  /// ADRP whose paired ADD/LDR hasn't arrived). Called at section /
  /// label / stream boundaries.
  virtual void finishPending(raw_ostream &O) {}

  /// Notify the printer that an instruction was just emitted via
  /// the streamer's raw-byte fallback (tryPrintInst returned false).
  /// The printer uses this to keep cross-instruction state
  /// consistent — e.g. invalidating the register-holds-page tracker
  /// when the raw instruction defines a register.
  virtual void notifyRawBytesEmitted(const MCInst *MI) {}
};

class MCPlan9AsmStreamer : public MCStreamer {
public:
  /// Construct. Takes ownership of OS, InstPrinter, and CodeEmitter.
  /// `SymPrinter` is a non-owning pointer to the same object as
  /// `InstPrinter` (via the MCPlan9SymbolicPrinter base) — the
  /// streamer calls through it for Plan 9 symbolic translation.
  MCPlan9AsmStreamer(MCContext &Ctx,
                     std::unique_ptr<formatted_raw_ostream> OS,
                     std::unique_ptr<MCInstPrinter> InstPrinter,
                     MCPlan9SymbolicPrinter *SymPrinter,
                     std::unique_ptr<MCCodeEmitter> CE);
  ~MCPlan9AsmStreamer() override;

  // Side-channel API: clang populates this before AsmPrinter runs so
  // emitLabel can emit a complete TEXT directive when it encounters
  // the matching symbol.  Keyed by Mach-O-mangled name (`_foo` on
  // darwin, `foo` on ELF).
  void setFunctionMetadata(StringRef MangledName, int FrameSize, int ArgSize);

  // c2go #376: single-entry publication of per-function metadata into this
  // streamer instance's C2GoFnMeta map. Read by emitLabel / recordC2Go
  // StackmapSite / flushC2GoStackmaps. (Was static + 6 thread_local maps;
  // collapsed to a single per-instance StringMap<C2GoFunctionMetadata>.)
  // The metadata struct itself is declared in MCC2GoFunctionMetadata.h.
  void publishC2GoFunction(C2GoFunctionMetadata M);
  // c2go #376: clear ONLY the NOSPLIT bit on every entry. Called when a
  // fresh codegen pass wants to re-decide NOSPLIT eligibility (e.g. the
  // Plan 9 .s pass uses a different frame layout than the .o pass) but
  // still wants to preserve all other metadata (framesize / argsize from
  // the manifest, etc.).
  void resetC2GoNoSplit();
  // c2go #376: clear ALL per-function metadata. Used per-TU between
  // emit-runs sharing a streamer instance (currently unused — every TU
  // gets a fresh streamer — but retained for symmetry).
  void resetC2GoAll();

  // c2go #376: boundary-symbol queue. Clang's CodeGenAction populates this
  // from the JSON manifest BEFORE the streamer is constructed (the MC layer
  // is set up inside emitBackendOutput, which runs AFTER the manifest pass).
  // The streamer constructor drains the queue into C2GoFnMeta. This is a
  // single per-thread channel (not 6) — narrow, drained-at-construction,
  // and outside the per-function emit path where the bulk of the old
  // thread_local state lived.
  static void enqueueC2GoBoundary(C2GoFunctionMetadata M);
  /// Drain the queue (moves contents out). Returns ownership to caller.
  static std::vector<C2GoFunctionMetadata> drainPendingC2GoBoundaries();

  // c2go #387 §B4 phase 6 sub-step 1: register a file-scope global as
  // "Go-owned storage". When `emitCommonSymbol` / `emitZerofill` would
  // otherwise emit a `GLOBL ·<name>(SB), NOPTR, $<size>` trailer, the
  // streamer suppresses the GLOBL so the Go-side bodyless
  // `var <name> unsafe.Pointer` declaration is the unique storage
  // definition (and the Go linker registers it in moduledata.gcdata
  // for native root-scan). Companion to the boundary queue:
  // populated by CodeGenAction before the MC layer is constructed,
  // drained into a per-instance set in the streamer ctor.
  static void enqueueC2GoGoOwnedGlobal(StringRef Name);
  static std::vector<std::string> drainPendingC2GoGoOwnedGlobals();

  // ------- MCStreamer overrides (minimal set for v0+1 skeleton) -----
  //
  // The skeleton only overrides the methods strictly required to
  // produce a syntactically-valid Plan 9 stream for the c2go pool
  // test. Other directives fall through to MCStreamer's default
  // (mostly no-op for asm output).  Step 5 fills in:
  //   * .section -> no-op
  //   * .globl   -> no-op (TEXT is its own global)
  //   * .data*   -> no-op or TODO error
  //   * line/loc -> no-op (debug info not yet supported)

  bool isPlan9AsmStreamer() const override { return true; }

  // File-level
  void emitRawTextImpl(StringRef String) override;

  // Labels and symbols
  void emitLabel(MCSymbol *Symbol, SMLoc Loc = SMLoc()) override;

  // Sections — Plan 9 has no equivalent; swallow silently.
  void switchSection(MCSection *Section, uint32_t Subsec = 0) override;
  bool emitSymbolAttribute(MCSymbol *Symbol,
                            MCSymbolAttr Attribute) override {
    return true; // accepted, ignored
  }
  void emitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                        Align ByteAlignment) override;
  void emitZerofill(MCSection *Section, MCSymbol *Symbol = nullptr,
                    uint64_t Size = 0, Align ByteAlignment = Align(1),
                    SMLoc Loc = SMLoc()) override;

  // Instructions — delegate to per-target Plan 9 InstPrinter.
  void emitInstruction(const MCInst &Inst,
                       const MCSubtargetInfo &STI) override;

  // No-op stubs for data/alignment directives (Plan 9 doesn't have
  // them in our text-only output mode). emitValueImpl is the
  // virtual hook; the public emitValue helpers funnel through it.
  void emitValueImpl(const MCExpr *Value, unsigned Size,
                     SMLoc Loc = SMLoc()) override;
  void emitFill(const MCExpr &NumBytes, uint64_t FillValue,
                SMLoc Loc = SMLoc()) override;
  // Raw byte stream (used by AsmPrinter for constant pools / string
  // literals / typeinfo blobs). MCStreamer's default is a no-op,
  // which would silently drop SQLite's FP constant pool — leaving
  // GLOBL $0 trailers with no DATA. Translate into DATA chunks.
  void emitBytes(StringRef Data) override;
  // Finish — close any open data symbol at stream end so its GLOBL
  // trailer is emitted. AsmPrinter's last section is often a string
  // pool that ends without an explicit section-switch; the closing
  // GLOBL gets lost without this override.
  void finishImpl() override;

  // ===== c2go Phase 1: native Go funcdata/pcdata emit =====
  //
  // AArch64AsmPrinter::LowerSTACKMAP calls this when it sees a
  // TargetOpcode::STACKMAP MachineInstr AND the streamer is Plan 9.
  // We compute a locals pointer bitmap from the SP-relative offsets,
  // intern it into the current function's unique-bitmap table, and
  // emit `PCDATA $1, $<idx>` textually right before the safepoint
  // call's BL emission so Go runtime can resolve the bitmap at this
  // PC.
  //
  // `SpOffsets` is the list of frame offsets (in bytes, relative to
  // post-prologue SP) whose 8-byte words hold a c2go-managed pointer
  // at this safepoint. Computed by the caller from
  // StackMapOpers(MI).getNumOperands() iteration; only Direct(SP, N)
  // entries are recorded.
  void recordC2GoStackmapSite(ArrayRef<int64_t> SpOffsets);

  // c2go Phase 1: emitted after a TEXT directive in emitLabel() to
  // wire FUNCDATA $1 and the initial PCDATA $1, $-1 (no live ptrs
  // yet). Idempotent for a given function name; subsequent calls
  // within the same function are silent. Returns false if the func
  // is not a c2go-managed (NoFrame) function — caller should skip.
  //
  // c2go #298 Wave AA Track A (F3 contract lock): SavedLinkSize is the
  // per-arch saved-LR slot size inside the physical frame reported as
  // FrameSize. Drives the locals-bitmap width
  // (`Nbit = (FrameSize - SavedLinkSize) / PtrSize`). AArch64 = 8,
  // X86 amd64 = 0. Default kept at 8 to preserve the existing AArch64
  // call-site behaviour.
  void emitC2GoFuncDataPreamble(StringRef FnName, int FrameSize, int ArgSize,
                                unsigned SavedLinkSize = 8);

  // Emit `FUNCDATA $<idx>, gclocals·<hash>(SB)` for the just-closed function
  // plus the symbol's DATA/GLOBL (deduped by content hash via
  // EmittedGclocalsSyms). \p Bitmaps is the list of N pointer maps, each
  // \p Nbit bits wide, in Go's stackmap struct format
  // ({int32 n; int32 nbit; byte bytedata[...]}). Used for both the args map
  // (idx 0, FUNCDATA_ArgsPointerMaps) and the locals map (idx 1).
  void emitC2GoFuncDataSymbol(unsigned FuncDataIdx, uint32_t Nbit,
                              ArrayRef<std::vector<uint8_t>> Bitmaps);

  // c2go Phase 1: flush the gclocals symbol for the most-recently
  // entered c2go function. Called when a new TEXT label arrives in
  // emitLabel(), and from finishImpl() at end of stream. Emits the
  // GLOBL + DATA chain in Go runtime's stackmap struct format:
  //
  //   DATA  ·fn·c2go_localsmap+0(SB)/4, $n      // bitmap count
  //   DATA  ·fn·c2go_localsmap+4(SB)/4, $nbit   // bits per bitmap
  //   DATA  ·fn·c2go_localsmap+8(SB)/1, $0xNN   // first bitmap byte
  //   ... (n × ceil(nbit/8) byte slots) ...
  //   GLOBL ·fn·c2go_localsmap(SB), RODATA, $size
  //
  // Bitmap byte encoding mirrors Go's bitvec: 1 bit per pointer-sized
  // word, LSB-first within each byte. Symbol suffix `·c2go_localsmap`
  // distinguishes from Go-compiler-emitted `·foo.gclocals` so the two
  // pipelines can coexist during transition.
  void flushC2GoStackmaps();

private:
  std::unique_ptr<formatted_raw_ostream> OS;
  std::unique_ptr<MCInstPrinter> InstPrinter;
  MCPlan9SymbolicPrinter *SymPrinter; // not owned — same object as InstPrinter
  std::unique_ptr<MCCodeEmitter> CE;
  bool HeaderEmitted = false;

  // FunctionMetadata: framesize and argsize per c2go_extern function.
  // First = framesize, Second = argsize.
  // (Legacy setFunctionMetadata API; coexists with the broader C2GoFnMeta
  //  below — see emitLabel's two-stage lookup.)
  StringMap<std::pair<int, int>> FunctionMetadata;

  // c2go #376: per-function metadata map. Replaces the 6 thread_local
  // StringMaps that used to live as file-scope globals in
  // MCPlan9AsmStreamer.cpp (g_C2GoMetadata, g_C2GoArgPtrMask,
  // g_C2GoLocalsAggMask, g_C2GoLocalsAmbigMask, g_C2GoNoSplit,
  // g_C2GoStackObjects). Keyed by Mach-O-mangled (or LLVM IR) function
  // symbol; each entry aggregates framesize/argsize, NOSPLIT bit, args
  // pointer mask, locals agg/ambig masks, and the stkobj table.
  StringMap<C2GoFunctionMetadata> C2GoFnMeta;

  // c2go #387 §B4 phase 6 sub-step 1: bare-name set of file-scope
  // globals whose storage is owned by the generated Go package. The
  // streamer suppresses GLOBL emission for these so the Go-side
  // `var <name> unsafe.Pointer` is the unique definition (and Go's
  // moduledata machinery scans it as a root). Populated at
  // construction by draining the static pending queue.
  StringSet<> C2GoGoOwnedGlobals;

  // Data section tracker. Plan 9 emits each global as a sequence of
  // `DATA ·sym+off(SB)/N, $val` directives terminated by a single
  // `GLOBL ·sym(SB), RODATA, $size` trailer. We're in a data section
  // when the current MCSection is a read-only-data / data / BSS
  // SectionKind.
  bool InDataSection = false;
  bool DataIsROnly = true; // RODATA flag vs writable DATA
  std::string CurrentDataSym;        // rendered Plan 9 symbol (with ·)
  uint64_t CurrentDataOffset = 0;     // bytes emitted into current sym
  uint64_t CurrentDataSize = 0;       // total size emitted; for GLOBL
  // c2go #298 Wave AP.3 (Wave AO F4 follow-up): true while the current
  // data label was DELIBERATELY suppressed by emitC2GoDataLabel (c2go
  // typeinfo / gcbitmap duplicate blobs, #387 Go-owned globals). While
  // set, emitValueImpl drops the suppressed blob's values silently —
  // the same intended short-circuit emitBytes / emitData* already
  // perform on the empty CurrentDataSym. With it CLEAR, an emitValue
  // outside a data section / before any label is a genuine coverage
  // gap and fails closed (report_fatal_error) instead of shipping a
  // `// PLAN9-ERROR` comment the Go assembler silently accepts.
  bool DataSymSuppressed = false;

  void emitFileHeaderIfNeeded();
  // Close the current data symbol (emit its GLOBL trailer). Called
  // when switching sections, when a new label arrives in a data
  // section, and from the streamer's finish() at stream end.
  void closeCurrentDataSymbol();
  // Helper: convert a rendered symbol name to its Plan 9 form
  // (`_foo` → `·foo`, `runtime.bar` → `runtime·bar`, local → bare).
  std::string symbolToPlan9(StringRef RenderedName);
  // c2go #401(c): shared body for emitCommonSymbol / emitZerofill. Both
  // map to the same Plan 9 shape — a `GLOBL ·sym(SB), <Tag>, $<Size>`
  // trailer with no DATA backing — and both honour the Go-owned-global
  // suppression set. Factored so future cross-cutting changes (e.g.
  // section flags, EH info) only need to touch one site.
  void emitGLOBLOrSuppress(MCSymbol *Symbol, uint64_t Size, StringRef Tag);

  // emitValueImpl helper — emit one DATA directive with the given
  // integer value at the current offset, advancing it.
  void emitDataIntDirective(uint64_t Value, unsigned Size);
  // emitValueImpl helper — emit a DATA directive whose right-hand
  // side is a symbol reference (e.g. `$·target(SB)`).
  void emitDataSymDirective(const MCExpr *Expr, unsigned Size);

  // Emit raw bytes via CE, fail-loud if any fixups remain (i.e. the
  // MCInst was symbol-bearing and tryPrintInst didn't handle it —
  // emitting zero-offset WORD would be a silent miscompile).
  void emitRawBytesOrFail(const MCInst &Inst,
                          const MCSubtargetInfo &STI);

  // ===== c2go Phase 1 state =====
  //
  // Per-function stackmap accumulator. Reset on each new TEXT label;
  // flushed (emit GLOBL + DATA) when the next TEXT arrives OR
  // finishImpl runs. Bitmap interning happens in-line on each
  // recordC2GoStackmapSite call so the PCDATA index can be emitted
  // immediately into the .s stream.
  struct C2GoFuncStackmap {
    std::string FnName;       // bare Go-form name (no leading "·" prefix)
    int FrameSize = 0;        // bytes — drives nbit = FrameSize/PtrSize
    uint32_t Nbit = 0;        // bits per LOCALS bitmap (== (FrameSize-8)/8)
    uint32_t ArgNbit = 0;     // bits per ARGS bitmap (== argsize/8) for FUNCDATA $0
    // Each unique bitmap is stored as a packed byte vector (LSB-first
    // within each byte). Index into this vector is the PCDATA $1 value.
    std::vector<std::vector<uint8_t>> Bitmaps;
    // Hash → index for interning. Keyed by hex-rendered bitmap bytes
    // (cheap, avoids designing a custom DenseMapInfo for vector<u8>).
    StringMap<unsigned> BitmapIndex;
    // True once emitC2GoFuncDataPreamble has been called for FnName —
    // suppress duplicate FUNCDATA emit on re-entry.
    bool PreambleEmitted = false;
  };
  std::optional<C2GoFuncStackmap> CurFn;

  // #220 — Cross-function dedup of gclocals RODATA symbols. Symbol
  // name = `c2go_localsmap_<hex>` where <hex> is a hash of the full
  // (n, nbit, bytedata) blob. Subsequent functions with identical
  // bitmap content reference the same symbol via FUNCDATA $1, and
  // the GLOBL+DATA is emitted only once (DUPOK guards against any
  // accidental re-emit). Without this, every c2go function in
  // SQLite produced its own unique gclocals symbol (1068 of them),
  // which ballooned the Go linker's pclntab/funcdata processing
  // to multi-GB RSS during link. Content-hashed dedup matches Go's
  // own `gclocals·<HASH>` convention.
  StringSet<> EmittedGclocalsSyms;

  // Intern Bits into CurFn.Bitmaps; return the assigned index.
  // The caller has already packed bits LSB-first per byte using
  // CurFn.Nbit as the bitmap width.
  unsigned internC2GoBitmap(ArrayRef<uint8_t> Bits);

  // ===== emitLabel split helpers =====
  // emitLabel is a 4-stage dispatcher; each branch is one of these.
  // Pre: emitFileHeaderIfNeeded done; Lloh skip + finishPending applied.

  // Stage 1: metadata-known function (c2go_extern or .o-pass-registered).
  // Emits a full Plan 9 `TEXT … $framesize-argsize` directive with
  // NOSPLIT/NOFRAME flags + opens the FUNCDATA preamble.
  void emitC2GoFunctionLabel(StringRef Name, StringRef Mangled,
                             std::pair<int, int> Sz);

  // Stage 2: data-section label. Closes the previous data symbol, then
  // either skips clang-emitted typeinfo/gcbitmap blobs (#134) or starts
  // a new CurrentDataSym for subsequent DATA directives.
  void emitC2GoDataLabel(StringRef Name, StringRef Mangled);

  // Stage 3: AArch64 backend local label (`Lxxx`, `.Lxxx`, `l_xxx`).
  // Sanitised via symbolToPlan9 and emitted bare (`<local>:`).
  void emitC2GoLocalLabel(StringRef Name);

  // Stage 4: fallback — a translation-unit-local C function (e.g. SQLite
  // static helpers). Emitted as a splittable `TEXT … NOFRAME, $0`.
  void emitC2GoTULocalFunctionLabel(StringRef Name);
};

} // end namespace llvm

#endif // LLVM_MC_MCPLAN9ASMSTREAMER_H
