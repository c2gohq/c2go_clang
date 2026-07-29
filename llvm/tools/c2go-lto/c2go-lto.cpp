//===- c2go-lto.cpp - Whole-program stack->heap escape audit for c2go -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go-lto v0 — client B only: sound detection of "stack address stored into a
// heap object or mutable global" escapes (design: docs/c2go_design.md §4.2.2 +
// §4.3.1(b), tracking #289).
//
// Why this matters: c2go runs C on movable Go goroutine stacks. copystack only
// scans the stack, so a stack address (alloca / &local) that has been stored
// into a heap object (malloc result) or a mutable global becomes dangling the
// moment the stack moves. The canonical case is `db->pParse = &sParse` in
// SQLite. This tool finds every such store, whole-program, soundly.
//
// Algorithm: a flavored, field-insensitive, constraint-based Andersen-lite
// points-to with a memory model, solved online to a fixpoint with a worklist.
//
//   abstract cells:
//     - every AllocaInst        -> a Stack cell
//     - every heap-alloc call   -> a Heap cell
//     - every mutable global    -> a Global cell
//     - one synthetic HeapUnknown cell, self-pointing for soundness
//
//   nodes (each carries a pts-set = set of cell ids, plus copy-edges):
//     - one node per SSA pointer value         (pts = cells it may point to)
//     - one node per cell's "contents"         (pts = cells stored into it;
//       field-insensitive: all fields of an object share one contents node)
//
//   constraints:
//     gep/bitcast/phi/select/ptr-copy : copy edge  src -> dst
//     alloca/heap-call/global         : pts(node) >= {own cell} (base)
//     store ptr q, ptr p              : complex; for c in pts(p):
//                                         copy edge  valNode(q) -> contents(c)
//     v = load ptr, ptr p             : complex; for c in pts(p):
//                                         copy edge  contents(c) -> valNode(v)
//     direct call   : copy actual_i -> formal_i; copy ret operands -> call
//     indirect call : same, over address-taken functions of compatible arity
//
//   escape test, per `store ptr q, ptr p`:
//     pts(q) contains a Stack cell  AND  pts(p) contains Heap / Global / Unknown
//
//   asymmetric soundness (key): the value side (q) stays precise (only a real
//   alloca counts as Stack, to avoid drowning in false positives); the
//   destination side (p) is over-approximated — a function that is address-taken
//   or externally linked may be an external entry, so its pointer-typed params
//   are seeded with HeapUnknown (treated as may-point-to-heap). The HeapUnknown
//   cell points to itself, so loading through an unknown-heap pointer keeps
//   yielding heap. This catches prep1 (external), prep3 (address-taken), and
//   prep2 (`db = outer->self`, a load from a heap field).
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/MCPlan9AsmStreamer.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/C2Go/C2GoGCMaskUtils.h"
#include "llvm/Transforms/C2Go/C2GoPipeline.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"
#include "llvm/Transforms/IPO/Inliner.h"
#include "llvm/IR/PassInstrumentation.h"
#include "C2GoArCli.h"
#include "C2GoEscapeAudit.h"
#include "C2GoManifestRebuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/C2GoEmergencyFlag.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/C2GoBackendKnobs.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <optional>
#include <vector>

// c2go LLVM-22 cleanup: the AArch64 backend c2go knobs
// (ForceBlockAddressJumpTable / DisableRegisterCoalescing) used to live
// here as extern thread_local globals defined in
// llvm/lib/MC/MCPlan9AsmStreamer.cpp. They are now instance fields on
// `AArch64TargetMachine` (#375 slice 1) — set via the thin shim in
// `llvm/Target/C2GoBackendKnobs.h`. See the `setForceBlockAddressJumpTable`
// / `setDisableRegisterCoalescing` calls in RunPlan9Codegen below.

#define DEBUG_TYPE "c2go-lto"

using namespace llvm;

static cl::list<std::string> InputFiles(cl::Positional, cl::OneOrMore,
                                        cl::desc("<input .bc/.ll files>"));

// `--c2go-print-stats` / `-v` (Andersen solver stats) live in
// C2GoEscapeAudit.cpp alongside the only reader (PointsTo::solve).

// c2go WF2 (#319, M4 minimal): emit a manifest JSON rebuilt from the combined
// bitcode's c2go.* module flags and per-function "c2go-..." string attrs.
// Minimal v0 reads: pkgpath, target_go_version, and for every function with
// the "c2go-boundary" attr: name (c-name), kind (boundary vs unmanaged_extern
// boundary, decided from c2go-unmanaged-world / linkage), go_sig.
static cl::opt<std::string>
    EmitManifest("c2go-emit-manifest",
                 cl::desc("Rebuild a c2go manifest JSON from combined bitcode "
                          "and write to <file>"),
                 cl::value_desc("file"), cl::init(""));

// After manifest emit, drop declare-only c2go-boundary functions that were
// only kept alive by llvm.compiler.used (the keep-alive side-channel for
// declare-only c2go_extern). Writes the cleaned combined bitcode to <file>
// so callers can verify the residual declare set, and so a future opt
// pipeline does not carry them downstream.
static cl::opt<std::string>
    OutputBC("output-bc",
             cl::desc("Write cleaned combined bitcode to <file>"),
             cl::value_desc("file"), cl::init(""));

// c2go WF2 (#319, real M4): emit a Plan 9 (.s) using the same neutral
// aarch64-ELF + MCPlan9AsmStreamer pipeline that clang's
// EmitAssemblyHelper::RunC2GoPlan9Pipeline runs. The combined bitcode
// is expected to have completed c2go's late IR pipeline. Driver-produced
// inputs carry `c2go.lto.prelink=1`; main() links and inlines those inputs,
// runs the shared late pipeline exactly once, and marks the flag consumed
// before this helper drives codegen. The AArch64 backend auto-injects
// M1/M3/M5 MachineFunction passes via addPreEmitPass2.
static cl::opt<std::string>
    EmitAsm("c2go-emit-asm",
            cl::desc("Emit Plan 9 .s (via MCPlan9AsmStreamer) to <file>"),
            cl::value_desc("file"), cl::init(""));

// c2go: stack->heap escapes are normally reported via exit code 1 so a build
// step can gate on them. When c2go-lto runs INSIDE the clang driver (the WF2
// path for an ordinary `-fc2go` compile), an escape is a diagnostic, not a build
// failure — the legacy cc1-direct emit ran no escape audit at all, so gating
// here would be a new, surprising hard error. This flag keeps the per-escape
// diagnostics but returns 0; the standalone tool (build-gating callers such as
// the WF2 archive build) leaves it off and keeps the exit-1 contract.
static cl::opt<bool> EscapeNonFatal(
    "c2go-escape-nonfatal", cl::init(false),
    cl::desc("report stack->heap escapes as diagnostics but exit 0"));

// c2go WF2 (#319, M5): emit a real Unix `ar` archive bundling the same
// Plan 9 .s + manifest .json that --c2go-emit-asm / --c2go-emit-manifest
// would produce, as the two named members `<base>.s` and
// `<base>.c2go-export.json` (base = filename stem of the archive). This
// is the drop-in container c2go-bind already accepts (archive.go in
// c2go/c2go-bind: members dispatched by `.s` / `.json` suffix). The three
// emit flags (asm / manifest / archive) are independent and may be set
// in any combination; setting only --c2go-emit-archive runs codegen +
// manifest build silently and ships them together as one .a.
static cl::opt<std::string>
    EmitArchive("c2go-emit-archive",
                cl::desc("Emit Plan 9 .s + manifest as a single ar archive "
                         "to <file> (members: <base>.s + "
                         "<base>.c2go-export.json)"),
                cl::value_desc("file"), cl::init(""));

// c2go #303: run NewPM ModuleInlinerWrapperPass on the combined module after
// the escape audit so cross-TU calls (a TU calling an internal helper from
// another TU) get inlined before Plan 9 codegen. Default on; --c2go-lto-inline=false
// reverts to "link-only" behaviour for bisection / regression isolation.
static cl::opt<bool>
    RunInliner("c2go-lto-inline",
               cl::desc("Run cross-TU NewPM inliner on the combined module "
                        "before Plan 9 codegen (#303)"),
               cl::init(true));

// c2go #437(b/c): test-only knob. When set to a non-empty banned-pass name
// (GlobalOptPass / DeadArgumentEliminationPass / ArgumentPromotionPass /
// InternalizePass), c2go-lto adds that pass to its private MPM so the
// instrumentation callback fires and report_fatal_error()s. Used by the
// LIT case `c2go-lto-banlist-fatal.ll` to verify the ban-list pipeline-gate
// is wired. Hidden because no production path uses it.
static cl::opt<std::string> TestBanPass(
    "c2go-lto-test-banpass",
    cl::desc("[#437 test-only] inject a banned IPO pass into the private "
             "MPM to verify the ban-list pipeline gate fires"),
    cl::Hidden, cl::init(""));

// `--c2go-escape-dedup` (#295) and `--c2go-andersen-field-sensitive` (#295
// Phase 2) live in C2GoEscapeAudit.cpp alongside the only readers
// (PointsTo::cellAt / PointsTo::reportEscapes).

namespace {

// PointsTo / isHeapAllocatorName / cell-kinds — the entire Andersen-lite
// escape audit lives in C2GoEscapeAudit.cpp (#478 split). main() invokes
// it via `c2go::runAndersenEscapeAudit(M, outs())`.

// c2go WF2 (#319 M4): drive a single Plan 9 codegen pipeline on the
// combined module, writing a Plan 9 (.s) to `OutPath`. Mirrors
// EmitAssemblyHelper::RunC2GoPlan9Pipeline in
// clang/lib/CodeGen/BackendUtil.cpp:1544-1657:
//   * neutral aarch64-unknown-unknown ELF triple so the .s is
//     SYSTEM-INDEPENDENT (host Mach-O / COFF directives never leak in);
//   * Options.MCOptions.OutputAsmVariant = 2 activates
//     CodeGenTargetMachineImpl.cpp:204's MCPlan9AsmStreamer branch;
//   * Approach B M1/M3/M5 are MachineFunction passes injected by
//     AArch64PassConfig::addPreEmitPass2, so addPassesToEmitFile
//     transparently picks them up — no extra IR pipeline needed here
//     (the combined IR already carries RS4GC + GCSetup + FoldAlloca);
//   * the two AArch64 backend c2go knobs (ForceBlockAddressJumpTable +
//     DisableRegisterCoalescing) are set on this Plan-9-codegen TargetMachine
//     via `llvm::c2go::setForceBlockAddressJumpTable` /
//     `setDisableRegisterCoalescing` (#375 slice 1), for the same reasons
//     BackendUtil sets them (jump-table label-diff DATA + #309/#310
//     RegisterCoalescer unsoundness on the c2go CSR_AArch64_NoRegs contract).
// Stream-targeted overload: writes Plan 9 .s into `Out`. Used by both
// the file-emit path (--c2go-emit-asm) and the archive-emit path
// (--c2go-emit-archive, which targets a SmallString buffer).
//
// IR-level cross-TU inlining runs in main() before this codegen pipeline;
// see the ModuleInlinerWrapperPass invocation guarded by --c2go-lto-inline
// (#303).
bool RunPlan9Codegen(Module &M, raw_pwrite_stream &Out) {
  // #320 / #376: NOSPLIT decisions used to be a process-global thread_local
  // set; staying symmetric with BackendUtil meant resetting it here too. As
  // of #376 the metadata lives on the MCPlan9AsmStreamer instance and a
  // fresh streamer is built per emit call — the C2GoFnMeta map starts
  // empty, no explicit reset needed.

  // c2go #439: refactor-safe pre-check — assert the gcmask collect step
  // (BuildManifest above) ran BEFORE us so the `@c2go.global.gcmask.<var>`
  // GVs are still in IR. Codegen lowering drops those GVs; running the
  // collect afterwards would silently ship an empty `module_gcmask.vars`
  // and the Go side would lose every shared-state pointer. The watermark
  // is set inside the manifest-emission block in main().
  if (!M.getNamedMetadata(c2go::kGcmaskCollectedFlag)) {
    report_fatal_error(
        "c2go-lto: RunPlan9Codegen invoked before the gcmask collect step "
        "stamped `c2go.gcmask.collected` — manifest builder must run first "
        "(#439)",
        /*GenCrashDiag=*/false);
  }

  Triple NeutralTT(M.getTargetTriple());
  NeutralTT.setVendor(Triple::UnknownVendor);
  NeutralTT.setOS(Triple::UnknownOS);
  NeutralTT.setEnvironment(Triple::UnknownEnvironment);
  NeutralTT.setObjectFormat(Triple::ELF);

  std::string Error;
  const Target *T = TargetRegistry::lookupTarget(NeutralTT, Error);
  if (!T) {
    errs() << "c2go-lto: target lookup failed: " << Error << "\n";
    return false;
  }

  TargetOptions Options;
  Options.MCOptions.OutputAsmVariant = 2; // drive MCPlan9AsmStreamer
  // c2go #666: mirror the Go compiler's "an UNDEF follows every no-return
  // call" invariant. A c2go function that ENDS in a genuine call (exit /
  // abort / longjmp — clang drops the unreachable RET) leaves its return
  // address pointing at the go-asm morestack tail appended right after the
  // body, whose pcsp value is 0 (pre-prologue state). Go's unwinder looks up
  // spdelta with the RAW return address (traceback.go next(): findfunc /
  // funcspdelta on frame.lr, no -1), reads a zero frame, and copystack
  // throws "traceback did not unwind completely" (linux+darwin amd64
  // TestAtexitLIFO; arm64 escapes by pcsp-layout luck, but gets the same
  // guard). TrapUnreachable materializes `unreachable` as ud2/brk, pinning
  // the return address inside the body's pcsp range. NoTrapAfterNoreturn
  // must stay FALSE — the trap after a no-return call is exactly the point.
  Options.TrapUnreachable = true;
  Options.NoTrapAfterNoreturn = false;

  // c2go #433: read CPU + features + optlevel from the bc's c2go.* module
  // flags (clang/CodeGenModule.cpp stamps them at `-fc2go -emit-llvm` time)
  // instead of the previous hard-coded ("generic", "+neon", Aggressive) tuple.
  // Falls back to the legacy hard-code only when a flag is absent (older bc
  // produced before #433 — keeps `c2go-lto` regression-tolerant against
  // partially-rebuilt bitcode); the stricter "all three present" check sits
  // in `main()` where it can also enforce OptLevel >= 2 for the SQLite gate.
  auto getStringFlagFromModule = [&M](StringRef Name) -> std::string {
    if (auto *MD = M.getModuleFlag(Name))
      if (auto *S = dyn_cast<MDString>(MD))
        return S->getString().str();
    return "";
  };
  auto getIntFlagFromModule = [&M](StringRef Name, int Default) -> int {
    if (auto *MD = M.getModuleFlag(Name))
      if (auto *CAM = dyn_cast<ConstantAsMetadata>(MD))
        if (auto *CI = dyn_cast<ConstantInt>(CAM->getValue()))
          return (int)CI->getZExtValue();
    return Default;
  };
  std::string CPU = getStringFlagFromModule(c2go::kTargetCpuFlag);
  if (CPU.empty()) CPU = "generic";
  std::string Features = getStringFlagFromModule(c2go::kTargetFeaturesFlag);
  // The legacy (pre-#433, flag-absent) fallback is AArch64-specific; the
  // X86 producer postdates #433 so x86 bc always carries the flag — an
  // empty feature string is the correct x86 fallback (#298 Track AN.3).
  if (Features.empty() && NeutralTT.getArch() == Triple::aarch64)
    Features = "+neon";
  int OptLevelInt = getIntFlagFromModule(c2go::kOptLevelFlag, 3);
  CodeGenOptLevel OL;
  switch (OptLevelInt) {
  case 0: OL = CodeGenOptLevel::None; break;
  case 1: OL = CodeGenOptLevel::Less; break;
  case 2: OL = CodeGenOptLevel::Default; break;
  default: OL = CodeGenOptLevel::Aggressive; break;
  }

  std::unique_ptr<TargetMachine> TM(T->createTargetMachine(
      NeutralTT, CPU, Features, Options,
      Reloc::PIC_, /*CodeModel=*/std::nullopt, OL));
  if (!TM) {
    errs() << "c2go-lto: createTargetMachine failed\n";
    return false;
  }

  // Retarget the module to the neutral triple + its DataLayout. Safe
  // on arm64: host (Mach-O) vs neutral (ELF) DataLayouts differ only
  // in symbol-mangling prefix (c2go mangles its own Plan 9 names) and
  // i8/i16 PREFERRED alignment — struct ABI alignment is identical.
  M.setTargetTriple(NeutralTT);
  M.setDataLayout(TM->createDataLayout());

  // Mirror BackendUtil.cpp — set the AArch64 backend c2go knobs on this
  // local Plan-9-codegen TM via the #435 POD shim (one atomic write,
  // one applier-table lookup). No save/restore needed: TM is a
  // unique_ptr-owned local that goes out of scope at function exit.
  // #397 GlobalMerge bit included so WF2 codegen does not merge internal
  // pointer-bearing globals into `_MergedGlobals` before the go-owned
  // filter classifies them.
  {
    llvm::c2go::BackendConfig C2GoCfg;
    C2GoCfg.ForceBlockAddressJumpTable = true;
    C2GoCfg.DisableRegisterCoalescing = true;
    C2GoCfg.DisableGlobalMerge = true;
    llvm::c2go::applyC2GoBackendConfig(TM.get(), C2GoCfg);
  }

  // c2go #389 (WF2 mirror of #387 §B4 phase 6 sub-step 1): feed the
  // MCPlan9AsmStreamer's go-owned set before addPassesToEmitFile
  // constructs the streamer (its ctor drains the thread_local pending
  // queue into the per-instance set — see MCPlan9AsmStreamer.cpp:73).
  //
  // The WF1 path runs the same enqueue from
  // BackendConsumer::HandleTranslationUnit via the manifest
  // (CodeGenAction.cpp:1261-1278); here in WF2 we read the IR
  // named-metadata directly — it is the same source-of-truth without
  // depending on the manifest already being rendered, and it survives
  // bc round-trip cleanly.
  //
  // Defensive drain first to clear any stale entries from a caller
  // (mirrors CodeGenAction.cpp:1242-1243).
  (void)MCPlan9AsmStreamer::drainPendingC2GoGoOwnedGlobals();
  (void)MCPlan9AsmStreamer::drainPendingC2GoBoundaries();
  SmallVector<GlobalValue *, 4> GoOwnedGVs;
  if (const NamedMDNode *NMD =
          M.getNamedMetadata(c2go::kGoOwnedGlobalsMDName)) {
    for (const MDNode *Op : NMD->operands()) {
      if (!Op || Op->getNumOperands() == 0)
        continue;
      if (const auto *MS = dyn_cast<MDString>(Op->getOperand(0))) {
        StringRef Name = MS->getString();
        MCPlan9AsmStreamer::enqueueC2GoGoOwnedGlobal(Name);
        // The bare C identifier is the linker-visible name of the
        // file-scope GV that CGC2GoTypeInfo tagged Go-owned. Capture
        // it for the GlobalMerge keep-out below.
        //
        // c2go #402 aux audit cleanup #5: harden the lookup against
        // stale / mis-shaped named-metadata entries. CGC2GoTypeInfo's
        // §B4 phase 6 sub-step 1 eligibility (CGC2GoTypeInfo.cpp:875-
        // 877) admits only static file-scope globals whose static
        // layout is exactly one pointer word — i.e. IR linkage is
        // `internal` and the value type's primitive size matches the
        // module's pointer width. A NamedMD entry that resolves to a
        // GV outside that envelope (e.g. round-tripped from a stale
        // .bc whose source no longer matches the metadata, or a
        // multi-field aggregate accidentally referenced by name) must
        // NOT enter the GlobalMerge keep-out: appending it to
        // `llvm.compiler.used` would pin a value that the #387 plan
        // never reserved storage ownership for, masking the real
        // eligibility bug.
        //
        // The companion `enqueueC2GoGoOwnedGlobal(Name)` above is
        // already by-name and bounded by the streamer's own filter,
        // so this `continue` only narrows the keep-out: the streamer
        // suppression behaviour is unchanged.
        if (GlobalVariable *GV = M.getNamedGlobal(Name)) {
          unsigned PtrSizeBits = M.getDataLayout().getPointerSizeInBits();
          if (!GV->hasInternalLinkage() ||
              GV->getValueType()->getPrimitiveSizeInBits() != PtrSizeBits)
            continue;
          GoOwnedGVs.push_back(GV);
        }
      }
    }
  }

  // c2go #396 — WF2 mirror of CodeGenAction.cpp:1244-1259 boundary
  // enqueue. The WF1 path reads `symbols[].argsize` from the manifest
  // and seeds MCPlan9AsmStreamer's pending boundary queue BEFORE the
  // streamer ctor drains it (so the streamer's per-instance metadata
  // map knows the boundary's argsize at emit time and renders
  // `TEXT ·foo(SB), $N-<argsize>` plus FUNCDATA $0 with Nbit derived
  // from argsize).
  //
  // WF2's source-of-truth is the per-function IR string attribute
  // `c2go-boundary-argsize`, stamped at CodeGenModule.cpp:2912 for
  // every `c2go_extern` function. Reading the attr directly here
  // (rather than re-parsing the manifest) keeps the data flow
  // independent of manifest emission order and survives bc round-trip.
  //
  // Without this, boundary functions emit `$N-0` (argsize=0) and
  // FUNCDATA $0 with Nbit=0 / no bitmap bytes, so the Go runtime stack
  // scanner sees zero args for the boundary frame and skips relocating
  // the incoming managed pointer arg during copystack — the stale
  // stack pointer then captures into heap state on the next store
  // ("found bad pointer in Go heap" on the next GC).
  //
  // FrameSize=0 here mirrors CodeGenAction.cpp:1256: it primes the
  // entry without overwriting the real frame size, which the
  // C2GoFrameEmitter publishes later (publishC2GoFunction preserves
  // any value already set when M.FrameSize<0; M.FrameSize=0 also
  // satisfies the `M.FrameSize >= 0` overwrite path, so a later
  // strictly-positive publish wins via the read-modify-write semantics
  // at MCPlan9AsmStreamer.cpp:221-224).
  //
  // Name keying uses `c2go-c-name` (the bare C identifier) — this
  // matches what `emitC2GoFunctionLabel` looks up in C2GoFnMeta after
  // stripping `\01` no-mangle markers (see stripNoMangle +
  // publishC2GoFunction at MCPlan9AsmStreamer.cpp:200,209). Falls back
  // to the IR function name with the `\01` byte dropped if the attr is
  // absent (older bc).
  for (Function &F : M) {
    if (!F.hasFnAttribute("c2go-boundary"))
      continue;
    StringRef CName = F.getFnAttribute("c2go-c-name").getValueAsString();
    if (CName.empty()) {
      CName = F.getName();
      if (!CName.empty() && CName[0] == '\01')
        CName = CName.drop_front();
    }
    int ArgSize = 0;
    StringRef ArgSizeStr =
        F.getFnAttribute("c2go-boundary-argsize").getValueAsString();
    if (!ArgSizeStr.empty()) {
      int Parsed = 0;
      if (!ArgSizeStr.getAsInteger(10, Parsed))
        ArgSize = Parsed;
    }
    C2GoFunctionMetadata Meta;
    Meta.Name = CName.str();
    Meta.FrameSize = 0;
    Meta.ArgSize = ArgSize;
    MCPlan9AsmStreamer::enqueueC2GoBoundary(std::move(Meta));
  }

  // c2go #389 — pin go-owned GVs against GlobalMerge.
  //
  // c2go-lto's codegen TM runs at CodeGenOptLevel::Aggressive (intentional
  // — needed for the rest of the Plan-9 emit pipeline), which on AArch64
  // enables createGlobalMergePass (AArch64TargetMachine.cpp:705-727). Go-
  // owned globals (single-pointer-word, name ceded to a Go-side bodyless
  // `var X unsafe.Pointer`) MUST keep their original `·X(SB)` symbol or
  // every C-side load/store retargets to `_L_MergedGlobals+offset` and the
  // Go-side storage is silently bypassed — the runtime then scans the Go
  // var (which is never written) instead of the merged buffer (NOPTR), so
  // pointers fall off the GC root set (manifests as
  // `runtime: checkmarks found unexpected unmarked object` under
  // GODEBUG=gccheckmark=1).
  //
  // GlobalMerge already honours `llvm.compiler.used` as a keep-out
  // (GlobalMerge.cpp:632 → setMustKeepGlobalVariables collects both
  // `llvm.used` and `llvm.compiler.used`). Post-#397 c2go-lto sets
  // `BackendConfig::DisableGlobalMerge=true` through
  // `applyC2GoBackendConfig` near the top of this function, so the
  // GlobalMerge pass does not run on this TM at all and the keep-
  // out path is never exercised here. The `appendToCompilerUsed` call
  // below is therefore defense-in-depth for future mid-end passes that
  // may use `llvm.compiler.used` as a keep-out signal — keeping the
  // go-owned GVs registered there costs one entry per GV and guards
  // against silently re-introducing the #389 symptom if any later pass
  // (e.g. ConstantMerge, an internalize pass, or a follow-up backend
  // merge) starts honouring the same convention.
  if (!GoOwnedGVs.empty())
    appendToCompilerUsed(M, GoOwnedGVs);

  legacy::PassManager PM;
  if (TM->addPassesToEmitFile(PM, Out, /*DwoOut=*/nullptr,
                              CodeGenFileType::AssemblyFile,
                              /*DisableVerify=*/false)) {
    errs() << "c2go-lto: target rejected AssemblyFile emission\n";
    return false;
  }
  PM.run(M);
  return true;
}

// c2go WF2 (#319 M4): rebuild the manifest's `module_gcmask.vars[]`
// array from `@c2go.global.gcmask.<var>` GVs that
// CodeGenModule::emitC2GoGlobalGCMask stamped into the bc.
//
// #401(b) aux audit cleanup: implementation lifted into the shared
// helper `llvm::c2go::collectGCMaskVarsFromModule`
// (llvm/Transforms/C2Go/C2GoGCMaskUtils.{h,cpp}). The clang/ WF1 site
// (CodeGenAction.cpp::buildC2GoManifest) now calls the same helper, so
// the two character-for-character mirrors collapse to one source of
// truth — see #389 / #387 §B4 phase 6 sub-step 1 for the go_owned bit.
json::Array collectC2GoModuleGCMaskVars(const Module &M) {
  return llvm::c2go::collectGCMaskVarsFromModule(M);
}

} // namespace

namespace {
// Process exit codes for c2go-lto. The tool returns ExitOK on a clean
// audit (no escapes), ExitEscapesFound when at least one stack-to-heap
// escape was detected (so a build can gate on it), ExitToolError for
// I/O / parse / link failures, and ExitAttrConflict when cross-TU
// c2go-boundary attrs disagree.
enum ExitCode {
  ExitOK = 0,
  ExitEscapesFound = 1,
  ExitToolError = 2,
  ExitAttrConflict = 3,
};
} // namespace

// c2go WF2 (#319 M5, #478 split): ar-CLI compatibility shim lives in
// C2GoArCli.cpp. main() calls `c2go::rewriteArCliInPlace` before
// cl::ParseCommandLineOptions so the rewritten argv reaches the parser.


int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  // c2go WF2 (#319 M5): ar-CLI compatibility shim — must run before
  // cl::ParseCommandLineOptions so the rewritten argv reaches the parser.
  std::vector<std::string> ArShimStorage;
  std::vector<const char *> ArShimPointers;
  const char **ArgvPtr = const_cast<const char **>(argv);
  c2go::rewriteArCliInPlace(argc, ArgvPtr, ArShimStorage, ArShimPointers);

  cl::ParseCommandLineOptions(argc, ArgvPtr,
                              "c2go-lto: whole-program stack->heap escape "
                              "audit for c2go (#289 client B)\n");

  // c2go WF2 (#319 M4): initialise the AArch64 backend so
  // RunPlan9Codegen can call TargetRegistry::lookupTarget +
  // createTargetMachine + addPassesToEmitFile. Even when
  // --c2go-emit-asm is unused these init calls are cheap.
  LLVMInitializeAArch64TargetInfo();
  LLVMInitializeAArch64Target();
  LLVMInitializeAArch64TargetMC();
  LLVMInitializeAArch64AsmPrinter();
  LLVMInitializeAArch64AsmParser();

  // c2go #298 / Track AN.3: initialise the X86 backend too, so amd64
  // bitcode (clang -fc2go --target=x86_64-*) resolves the neutralised
  // x86_64-unknown-unknown-elf triple in RunPlan9Codegen instead of
  // failing with "No available targets are compatible with triple".
  // LLVMInitializeX86Target() also registers the per-arch BackendConfig
  // hook (X86TargetMachine.cpp:90-93) that applyC2GoBackendConfig
  // dispatches through, so the three c2go knobs reach the X86 TM.
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86Target();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86AsmPrinter();
  LLVMInitializeX86AsmParser();

  LLVMContext Context;
  SMDiagnostic Err;

  std::unique_ptr<Module> Composite = parseIRFile(InputFiles[0], Err, Context);
  if (!Composite) {
    Err.print(argv[0], errs());
    return ExitToolError;
  }

  // Preserve the producer's optimization level. It gates the cross-TU inliner
  // and selects statepoint GC at -O2+ versus the lightweight safepoint path at
  // -O0/-O1; it does not make c2go-lto impose an extra generic opt pipeline.
  int BitcodeOptLevel = 3; // flag-absent (pre-#433 bc) => treat as -O>=2
  if (auto *F = Composite->getModuleFlag(c2go::kOptLevelFlag))
    if (auto *CAM = dyn_cast<ConstantAsMetadata>(F))
      if (auto *CI = dyn_cast<ConstantInt>(CAM->getValue()))
        BitcodeOptLevel = (int)CI->getZExtValue();

  // Q2 fix: capture per-input c2go-boundary attrs BEFORE IRMover merges
  // declarations into definitions. IRMover does not diagnose c2go string
  // attr conflicts; it just copyAttributesFrom the linked-in source and the
  // surviving Function takes whatever attrs the winner had. To detect
  // cross-TU disagreement we record every input's view of the attrs by
  // c-name, then verify they all match at manifest emit time.
  struct BoundaryAttrs {
    std::string Origin; // input file
    std::string GoSig;
    std::string ArgSize;
    std::string ExportCase;
  };
  StringMap<SmallVector<BoundaryAttrs, 2>> PreLinkBoundaries;
  auto recordBoundaries = [&](Module &M, StringRef Origin) {
    for (Function &F : M) {
      if (!F.hasFnAttribute("c2go-boundary"))
        continue;
      StringRef CName = F.getFnAttribute("c2go-c-name").getValueAsString();
      BoundaryAttrs A;
      A.Origin = Origin.str();
      A.GoSig = F.getFnAttribute("c2go-go-sig").getValueAsString().str();
      A.ArgSize =
          F.getFnAttribute("c2go-boundary-argsize").getValueAsString().str();
      A.ExportCase =
          F.getFnAttribute("c2go-export-case").getValueAsString().str();
      PreLinkBoundaries[CName].push_back(std::move(A));
    }
  };
  recordBoundaries(*Composite, InputFiles[0]);

  Linker L(*Composite);
  for (unsigned i = 1; i < InputFiles.size(); ++i) {
    std::unique_ptr<Module> Mod = parseIRFile(InputFiles[i], Err, Context);
    if (!Mod) {
      Err.print(argv[0], errs());
      return ExitToolError;
    }
    recordBoundaries(*Mod, InputFiles[i]);
    if (L.linkInModule(std::move(Mod))) {
      errs() << argv[0] << ": error: failed to link '" << InputFiles[i]
             << "'\n";
      return ExitToolError;
    }
  }

  // Cross-TU conflict diagnosis on the pre-link attr snapshots. Multiple TUs
  // may name the same c2go boundary (a header prototype + a .c definition,
  // or a redeclaration in another TU); they must agree on the shaping attrs.
  for (auto &E : PreLinkBoundaries) {
    StringRef CName = E.first();
    auto &V = E.second;
    if (V.size() <= 1)
      continue;
    const BoundaryAttrs &Ref = V.front();
    for (const BoundaryAttrs &Other :
         ArrayRef<BoundaryAttrs>(V).drop_front()) {
      auto diff = [&](const char *Field, StringRef A, StringRef B) -> bool {
        if (A == B) return false;
        errs() << argv[0] << ": attr conflict for boundary " << CName << ": "
               << Field << " differs between '" << Ref.Origin << "' ('" << A
               << "') and '" << Other.Origin << "' ('" << B << "')\n";
        return true;
      };
      bool Bad = false;
      Bad |= diff("c2go-go-sig", Ref.GoSig, Other.GoSig);
      Bad |= diff("c2go-boundary-argsize", Ref.ArgSize, Other.ArgSize);
      Bad |= diff("c2go-export-case", Ref.ExportCase, Other.ExportCase);
      if (Bad)
        return ExitAttrConflict;
    }
  }

  // c2go (#672): reject an UNMANAGED HOST IMPORT whose bare name is also
  // DEFINED inside the package. c2go.h documents the intent (the C2GO_DYN
  // note): a bare `extern` import claims the bare wrapper name precisely so
  // that an accidental same-named internal definition collides — but the
  // wrapper is linkonce, so the IR linker resolves that collision SILENTLY
  // in favour of the definition and the guard never fires. The result is one
  // program with two meanings: -O0 call sites reach the package function
  // directly (bridge bypassed), while -O2 call sites carry the already-
  // inlined cgocall bridge and resolve the name against the HOST at runtime.
  // Detect the mix on the merged module instead: each TU's import identity
  // survives in its `c2go.func.<name>` MDNode (kind operand 2 ==
  // "unmanaged_extern") regardless of inlining or link order, and the
  // package's side is a defined Function under the same bare name.
  // Intentional coexistence has its own spelling — C2GO_DYN(<name>) imports
  // under the reserved __c2go_dynimp_ prefix and never shares the bare name.
  for (NamedMDNode &NMD : Composite->named_metadata()) {
    StringRef MDName = NMD.getName();
    if (!MDName.starts_with(llvm::c2go::kFuncMDPrefix))
      continue;
    StringRef CName = MDName.drop_front(llvm::c2go::kFuncMDPrefix.size());
    bool ImportedSomewhere = false;
    for (const MDNode *Op : NMD.operands()) {
      if (Op->getNumOperands() < 3)
        continue;
      auto *Kind = dyn_cast<MDString>(Op->getOperand(2));
      if (Kind && Kind->getString() == "unmanaged_extern") {
        ImportedSomewhere = true;
        break;
      }
    }
    if (!ImportedSomewhere)
      continue;
    Function *Def = Composite->getFunction(CName);
    if (!Def || Def->isDeclaration())
      continue;
    // The import's OWN dispatch body is a linkonce_odr definition under the
    // bare name, stamped "c2go-wrapper-in-asm" (clang emits it per-TU; the
    // .s carries it as TEXT ·<name>). A pure import therefore always shows
    // a defined function here — only a REAL package definition (no wrapper
    // stamp, and strong enough to have displaced the linkonce wrapper at IR
    // link) is the mixed-spelling error.
    if (Def->hasFnAttribute("c2go-wrapper-in-asm"))
      continue;
    errs() << argv[0] << ": error: '" << CName
           << "' is an unmanaged host import in one TU but is defined inside "
              "the package in another; declare the package-internal reference "
              "through a c2go_linkname header instead of a bare extern, or "
              "spell the import C2GO_DYN("
           << CName
           << ") if importing the host symbol alongside the package's own "
              "definition is intentional\n";
    return ExitAttrConflict;
  }

  // #478 split: the Andersen-lite stack-address escape audit lives in
  // C2GoEscapeAudit.cpp. It writes the per-escape lines plus the final
  // `<N> stack-address escape point(s)` summary to outs(); the bool return
  // is N==0 (clean audit), which we cache for the ExitEscapesFound decision
  // at the end of main().
  //
  // In --c2go-escape-nonfatal mode (the clang-driver path, where escapes do not
  // gate the build) the diagnostics are sent to a null stream: the legacy
  // cc1-direct -fc2go emit ran no audit and printed nothing, so a plain compile
  // keeps stdout clean. Standalone build-gating callers leave the flag off and
  // get the full report on stdout.
  llvm::raw_null_ostream NullOS;
  raw_ostream &EscapeOS = EscapeNonFatal ? static_cast<raw_ostream &>(NullOS)
                                         : static_cast<raw_ostream &>(outs());
  bool EscapeClean = c2go::runAndersenEscapeAudit(*Composite, EscapeOS);

  // Driver-produced pre-link inputs carry kLTOPreLinkFlag=1. Legacy inputs may
  // predate that protocol; a function with an attached GC strategy identifies
  // already-RS4GC IR, while plain hand-written/test IR is safe to treat as
  // pre-link. Never inline already-lowered IR: inlining can create a new call
  // in a function whose gc-live set has already been frozen.
  std::optional<bool> PreLinkFlag;
  if (auto *F = Composite->getModuleFlag(c2go::kLTOPreLinkFlag))
    if (auto *CAM = dyn_cast<ConstantAsMetadata>(F))
      if (auto *CI = dyn_cast<ConstantInt>(CAM->getValue()))
        PreLinkFlag = CI->getZExtValue() != 0;

  bool HasPostRS4GCFunction = false;
  for (const Function &F : *Composite)
    HasPostRS4GCFunction |= !F.isDeclaration() && F.hasGC();
  const bool NeedsLatePipeline = PreLinkFlag.value_or(!HasPostRS4GCFunction);
  const bool CanInline =
      NeedsLatePipeline && RunInliner && BitcodeOptLevel >= 2;

  // c2go #303: cross-TU inlining via NewPM ModuleInlinerWrapperPass, followed
  // by the complete c2go late sequence on pre-link IR. PointsTo above remains
  // pre-inline so diagnostics retain their original source call sites.
  //
  // We do NOT call buildLTODefaultPipeline: GlobalOpt, DAE, ArgPromotion, and
  // Internalize can break c2go's ABI and metadata invariants. The inliner is
  // the intentionally narrow IPO surface.
  {
    LoopAnalysisManager LAM;
    FunctionAnalysisManager FAM;
    CGSCCAnalysisManager CGAM;
    ModuleAnalysisManager MAM;
    // c2go #437(b): install an `IPO ban-list` instrumentation guard on the
    // private c2go-lto pipeline. The comment-only declaration above (that
    // we do not call buildLTODefaultPipeline because GlobalOpt / DAE /
    // ArgPromotion / FunctionInternalize break c2go invariants) was the
    // only protection until now; if a future refactor accidentally adds
    // those passes to `MPM` they would run silently. Register a
    // BeforeNonSkippedPassCallback that `report_fatal_error`s when one of
    // the banned passes is about to run on this private MPM — same set
    // the comment lists.
    PassInstrumentationCallbacks PIC;
    PIC.registerBeforeNonSkippedPassCallback(
        [&argv](StringRef PassName, Any) {
          static const char *Banned[] = {
              "GlobalOptPass",
              "DeadArgumentEliminationPass",
              "ArgumentPromotionPass",
              "InternalizePass",
          };
          for (const char *B : Banned) {
            if (PassName == B) {
              report_fatal_error(Twine(argv[0]) +
                                     ": c2go-lto private pipeline saw banned "
                                     "IPO pass '" +
                                     PassName +
                                     "' — c2go invariants (CSR_AArch64_NoRegs / "
                                     "FUNCDATA/PCDATA pairing / RS4GC results) "
                                     "would break (#437)",
                                 /*GenCrashDiag=*/false);
            }
          }
        });
    PassBuilder PB(/*TM=*/nullptr, PipelineTuningOptions(), std::nullopt,
                   &PIC);
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    // Snapshot c2go-boundary names only when the inliner will run, then assert
    // that its external-linkage contract preserved them.
    SmallVector<std::string, 32> BoundaryNames;
    if (CanInline)
      for (Function &F : *Composite)
        if (F.hasFnAttribute("c2go-boundary"))
          BoundaryNames.emplace_back(F.getName().str());

    ModulePassManager MPM;
    if (CanInline)
      MPM.addPass(ModuleInlinerWrapperPass(getInlineParams()));

    if (NeedsLatePipeline) {
      const bool UseStatepoint =
          BitcodeOptLevel >= 2 && !llvm::c2go::isC2GoDisabled("statepoint-gc");
      addC2GoLatePasses(MPM, UseStatepoint);
    } else if (BitcodeOptLevel >= 2) {
      // Compatibility for legacy already-RS4GC bitcode: do not inline or rerun
      // GC lowering, but retain the old idempotent late-leaf cleanup.
      addC2GoLateLeafPasses(MPM);
    }

    // c2go #437(b/c) test hook — inject a banned IPO pass when the hidden
    // CLI flag is set so the BeforeNonSkippedPassCallback above fires.
    // Production paths never set this flag; PB.parsePassPipeline is the
    // tidy way to map a string back to a pass instance.
    if (!TestBanPass.empty()) {
      std::string Pipeline = TestBanPass;
      // Map class-name → registered pass-name (only the 4 banned passes).
      if (TestBanPass == "GlobalOptPass")
        Pipeline = "globalopt";
      else if (TestBanPass == "DeadArgumentEliminationPass")
        Pipeline = "deadargelim";
      else if (TestBanPass == "InternalizePass")
        Pipeline = "internalize";
      else if (TestBanPass == "ArgumentPromotionPass")
        Pipeline = "cgscc(argpromotion)";
      if (auto E = PB.parsePassPipeline(MPM, Pipeline)) {
        errs() << argv[0] << ": c2go-lto-test-banpass parse error: "
               << toString(std::move(E)) << "\n";
        return ExitToolError;
      }
    }
    MPM.run(*Composite, MAM);

    if (NeedsLatePipeline && PreLinkFlag)
      Composite->setModuleFlag(Module::Error, c2go::kLTOPreLinkFlag,
                               uint32_t(0));

    for (StringRef Name : BoundaryNames) {
      if (!Composite->getNamedValue(Name)) {
        errs() << argv[0] << ": internal error: inliner removed c2go-boundary "
                  "function '" << Name << "'\n";
        return ExitToolError;
      }
    }
  }

  // c2go #446: ship-gate hoist — refuse to emit any artifact when any
  // C2GoCommon::enforceCallSiteCC sweep observed a call-site CC mismatch
  // anywhere in the combined module (in this run OR carried in via the
  // input bitcodes' per-TU pipeline).
  //
  // Placement: AFTER the RS4GC + late-c2go passes inside `MPM.run` above
  // (where the sweeps bump `c2go.cc.violations`) AND after the
  // BoundaryNames survival check; BEFORE the manifest / .s / archive
  // emitters below. The previous placement at the very end of `main`
  // ran AFTER EmitAsm/EmitArchive had already landed a release artifact
  // on disk, defeating the gate's purpose (a half-written .s/.a would
  // be left for the build system to pick up before the gate fired).
  if (auto *MD = Composite->getModuleFlag(c2go::kCCViolationsFlag)) {
    if (auto *CAM = dyn_cast<ConstantAsMetadata>(MD)) {
      if (auto *CI = dyn_cast<ConstantInt>(CAM->getValue())) {
        uint64_t V = CI->getZExtValue();
        if (V > 0) {
          // #460: the `c2go.cc.violations` module flag uses
          // Module::Max-merge semantics, so `V` is the running maximum
          // observed across the contributing TUs, not an additive count.
          // Wording is explicit about that: any positive Max is sufficient
          // to fail the gate (the V>0 condition is still sound), but the
          // number itself must not be read as "exactly V violations".
          errs() << argv[0]
                 << ": error: at least one call-site CC violation observed "
                    "during the C2Go pipeline (Max="
                 << V
                 << "; see prior `c2go: WARNING — call site CC mismatch` "
                    "lines); refusing to emit a release artifact (#437)\n";
          return ExitToolError;
        }
      }
    }
  }

  // c2go #600: amd64 stack-alignment ship-gate. Go's amd64 stack is only
  // 8-byte aligned, but IR-level known-bits folds trust an alloca's align
  // attribute: a claimed 16 licenses rewrites like `buf+9` -> `buf|9` that
  // are silently wrong when the frame lands at 8 mod 16 (fmt_fp's printf %e
  // dropped its exponent exactly this way). clang -fc2go no longer emits
  // >8-aligned allocas on x86-64 (the large-array raise is clamped to 8 and
  // long double == double), so a survivor here — an explicit alignas / __int128 /
  // vector local, or a pass raising an alloca's alignment
  // (tryEnforceAlignment does not consult the stack's natural alignment) —
  // cannot be lowered soundly on the Go stack: refuse to emit.
  if (Triple(Composite->getTargetTriple()).getArch() == Triple::x86_64) {
    bool OverAlignedAlloca = false;
    for (Function &F : *Composite)
      for (BasicBlock &BB : F)
        for (Instruction &I : BB)
          if (auto *AI = dyn_cast<AllocaInst>(&I))
            if (AI->getAlign() > Align(8)) {
              errs() << argv[0] << ": error: function '" << F.getName()
                     << "' has an alloca '" << AI->getName() << "' requiring "
                     << AI->getAlign().value()
                     << "-byte stack alignment, but the Go amd64 stack "
                        "guarantees only 8 — refusing to emit (#600)\n";
              OverAlignedAlloca = true;
            }
    if (OverAlignedAlloca)
      return ExitToolError;
  }

  // c2go WF2 (#319, M4 minimal): optional manifest rebuild from combined
  // bitcode. Implementation lives in C2GoManifestRebuilder.cpp (#465 split);
  // the helper renders the JSON into `ManifestText` so the EmitArchive path
  // below can fan out byte-identical bytes without re-rendering, then runs
  // the Q1 declare-only c2go-boundary cleanup on `Composite`.
  std::string ManifestText;
  bool BuildManifest = !EmitManifest.empty() || !EmitArchive.empty();
  if (c2go::rebuildManifestFromIR(*Composite, BuildManifest, EmitManifest,
                                  argv[0],
                                  ManifestText) !=
      c2go::ManifestRebuildStatus::OK) {
    return ExitToolError;
  }

  // Optionally write the (possibly Q1-cleaned) combined bitcode out so the
  // caller can inspect the residual declare set.
  if (!OutputBC.empty()) {
    std::error_code EC;
    raw_fd_ostream BCOut(OutputBC, EC, sys::fs::OF_None);
    if (EC) {
      errs() << argv[0] << ": cannot open '" << OutputBC << "': "
             << EC.message() << "\n";
      return ExitToolError;
    }
    WriteBitcodeToFile(*Composite, BCOut);
  }

  // c2go WF2 (#319 M4): drive the Plan 9 (.s) codegen pipeline on the
  // combined bitcode. Order matters — module_gcmask must be collected
  // BEFORE codegen mutates / drops the @c2go.global.gcmask.* GVs
  // (manifest emission above already ran); RunPlan9Codegen retargets
  // the module to a neutral aarch64-ELF triple so subsequent passes
  // on Composite would see a different DataLayout — keep it last.
  //
  // M5: same in-memory-buffer trick as manifest above — render once into
  // an asm SmallString, then fan out to the --c2go-emit-asm file and/or
  // the --c2go-emit-archive member so they are byte-identical.
  SmallString<0> AsmText;
  bool BuildAsm = !EmitAsm.empty() || !EmitArchive.empty();
  if (BuildAsm) {
    // c2go #439: stamp the gcmask-collect watermark immediately before
    // codegen. RunPlan9Codegen asserts the watermark is present; a future
    // refactor that reorders codegen earlier than the collect step (or
    // drops the manifest builder entirely) will trip that assert instead
    // of silently shipping an empty `module_gcmask.vars`. Always re-run
    // the collect helper here (besides the conditional manifest path
    // above) so the EmitAsm-only path stays sound — the helper is
    // const-read-only and ~ms-cost on SQLite-amalgamation scale.
    (void)collectC2GoModuleGCMaskVars(*Composite);
    NamedMDNode *GCMaskNMD =
        Composite->getOrInsertNamedMetadata(c2go::kGcmaskCollectedFlag);
    if (GCMaskNMD->getNumOperands() == 0) {
      Metadata *One = ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(Composite->getContext()), 1));
      GCMaskNMD->addOperand(MDNode::get(Composite->getContext(), {One}));
    }

    raw_svector_ostream OS(AsmText);
    if (!RunPlan9Codegen(*Composite, OS))
      return ExitToolError;
    if (!EmitAsm.empty()) {
      std::error_code EC;
      raw_fd_ostream Out(EmitAsm, EC, sys::fs::OF_TextWithCRLF);
      if (EC) {
        errs() << argv[0] << ": cannot open '" << EmitAsm << "': "
               << EC.message() << "\n";
        return ExitToolError;
      }
      Out << AsmText;
    }
  }

  // c2go WF2 (#319 M5): bundle the just-rendered manifest + asm into a
  // real Unix `ar` archive. Members are deterministic (mtime/uid/gid/mode
  // zeroed) so the archive is reproducible across runs. The two members
  // are named `<base>.s` and `<base>.c2go-export.json`, where <base> is
  // the archive filename stem — this matches c2go-bind's archive.go,
  // which dispatches members by `.s` / `.json` suffix.
  if (!EmitArchive.empty()) {
    StringRef Base = sys::path::stem(EmitArchive);
    if (Base.empty())
      Base = "c2go";
    std::string AsmName = (Base + ".s").str();
    std::string JsonName = (Base + ".c2go-export.json").str();

    SmallVector<NewArchiveMember, 2> Members;
    NewArchiveMember AsmMember(MemoryBufferRef(AsmText, AsmName));
    AsmMember.MemberName = AsmName;
    Members.push_back(std::move(AsmMember));

    NewArchiveMember JsonMember(MemoryBufferRef(ManifestText, JsonName));
    JsonMember.MemberName = JsonName;
    Members.push_back(std::move(JsonMember));

    if (Error E = writeArchive(EmitArchive, Members,
                               SymtabWritingMode::NormalSymtab,
                               object::Archive::K_GNU,
                               /*Deterministic=*/true,
                               /*Thin=*/false)) {
      errs() << argv[0] << ": writeArchive '" << EmitArchive
             << "': " << toString(std::move(E)) << "\n";
      return ExitToolError;
    }
  }

  // c2go #446: the #437(a) ship-gate moved upstream (before manifest /
  // .s / archive emit) so a violation refuses to land any artifact on
  // disk. The gate is no longer here.

  // Non-zero exit when escapes were found, so the tool can gate a build.
  return (EscapeClean || EscapeNonFatal) ? ExitOK : ExitEscapesFound;
}
