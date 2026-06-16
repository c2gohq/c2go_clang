//===- C2GoFrameEmitter.cpp - c2go (Plan 9) frame emission ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #238 (Phase 1): extract the AArch64 c2go (Plan 9 / Go ABI0) prologue
// and epilogue emission helpers out of AArch64FrameLowering.cpp.
//
// Phase 1 is a pure refactor — the helpers below are moved verbatim from
// AArch64FrameLowering.cpp; semantic and byte-level output is unchanged.
// Phase 2/3 (cached C2GoFrameInfo + collapsed streamer side-channel) is
// described in .build_status/issue238_design_draft.md.
//
//===----------------------------------------------------------------------===//

#include "C2GoFrameEmitter.h"

#include "AArch64InstrInfo.h"
#include "AArch64MachineFunctionInfo.h"
#include "AArch64Subtarget.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCPlan9AsmStreamer.h"
#include "llvm/Support/C2GoEmergencyFlag.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/C2Go/C2GoGCMaskUtils.h"
#include "llvm/Transforms/C2Go/C2GoLeafEligibility.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace llvm;

// #377: was `extern cl::opt<bool>` references to two sibling-TU cl::opts
// (`-c2go-gc-spill-tags`, `-c2go-gc-ptrslot-liveness`). Both are now read
// through the unified llvm::c2go::isC2GoDisabled() emergency flag helper
// (subsystem tokens "spill-tags" / "ptrslot-liveness").

namespace llvm {
namespace c2go {

// isC2GoMode — true when the function lives in a c2go-mode module.
bool isC2GoMode(const MachineFunction &MF) {
  return MF.getFunction().getParent()->getModuleFlag(llvm::c2go::kGoabiModuleFlag) != nullptr;
}

// c2GoFrameSize — total bytes the c2go prologue subtracts from SP.
// Reserves two fixed words around the locals so the frame matches what
// `go tool asm` builds for `TEXT $framesize` (obj7.go preprocess):
//   * the saved-LR word at the bottom (sp+0), and
//   * the Go "frame-top FP slot" (obj7.go's `extrasize`) at the top
//     [sp+framesize-8, sp+framesize).
// The runtime's locals scan is [sp, varp) with varp = sp+framesize-8, so
// the top word is EXCLUDED. resolveFrameOffsetReference shifts SP-based
// locals down by 8 to keep them out of that top word, so every managed
// local stays GC-visible. (The saved FP itself physically lives at sp-8
// in the red zone per Go's arm64 convention; the reserved top word is the
// notional x86-style FP slot that the runtime's varp computation backs
// over — see traceback.go.) Rounded up to 16-byte alignment.
//
// c2goMakesRealCall — true iff the function contains a REAL call (BL/BLR/
// tail-call): the exact criterion `go tool asm` uses to decide LEAF/NoFrame
// (cmd/internal/obj/arm64/obj7.go). This deliberately does NOT use
// MFI.hasCalls()/adjustsStack(), and it EXCLUDES the STACKMAP/PATCHPOINT
// pseudos even though their MCInstrDesc sets `isCall`. Rationale (#306): when
// the inliner deletes a call, the llvm.experimental.stackmap that
// C2GoSafepoint emitted before it is orphaned. SelectionDAG wraps that
// surviving STACKMAP in a zero-size ADJCALLSTACK, which sets
// hasCalls()/adjustsStack() and is `isCall`-flagged — yet there is no real
// call and no SP adjustment. go-asm would (correctly) treat such a function
// as a leaf and inject no frame, so the compile-time frame decision MUST
// match this or the baked-in incoming-arg offsets desync from the physical
// frame (reads land 16 bytes too high → e.g. SQLite's malformed-schema).
//
// #326: STATEPOINT is NOT excluded. In the c2go-gc statepoint path
// (RewriteStatepointsForGC), a STATEPOINT pseudo wraps and EMITS a real
// BL/BLR (AArch64AsmPrinter::LowerSTATEPOINT) — it is a real call, so the
// function is NOT a leaf and must get a splittable (morestack) frame. Counting
// it as a leaf wrongly marks the caller NOSPLIT, which the Go linker rejects
// for recursive chains ("nosplit stack ... infinite cycle") and overflows the
// 792-byte nosplit budget on deep chains.
static bool c2goMakesRealCall(const MachineFunction &MF) {
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  for (const MachineBasicBlock &MBB : MF) {
    for (const MachineInstr &MI : MBB) {
      unsigned Opc = MI.getOpcode();
      if (Opc == TargetOpcode::STACKMAP || Opc == TargetOpcode::PATCHPOINT)
        continue;
      if (TII->get(Opc).isCall())
        return true;
    }
  }
  return false;
}

// Returns 0 for true leaf functions with no frame — the caller then skips
// prologue/epilogue emit and the Plan 9 TEXT directive uses NOFRAME, $0.
//
// c2go #238 (Phase 2): MakesRealCall is taken as an input so callers that
// already have it (computeC2GoFrameInfo, emitC2GoPrologue) don't have to
// rewalk every MI a second time. The old free function called
// c2goMakesRealCall(MF) inline, which got run twice per MF (once here, once
// in the NOSPLIT-eligibility check below).
static uint64_t c2GoFrameSizeImpl(const MachineFunction &MF,
                                  bool MakesRealCall) {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t Locals = MFI.getStackSize();
  // NeedsFrame mirrors go-asm's "has frame" condition: a real call, real
  // outgoing args, dynamic stack, or locals. NOT adjustsStack()/hasCalls()
  // — an orphan stackmap pollutes those (see c2goMakesRealCall / #306).
  bool NeedsFrame = MakesRealCall ||
                    MFI.getMaxCallFrameSize() != 0 ||
                    MFI.hasVarSizedObjects() || Locals != 0;
  if (!NeedsFrame)
    return 0;
  return (Locals + 16 + 15) & ~uint64_t{15};
}

// c2go #238 (Phase 2): public pure-function summary. No side effects on
// MF / MFI / streamer. Callers reach this via
// AArch64FunctionInfo::getOrComputeC2GoFI(), which memoises the result.
C2GoFrameInfo computeC2GoFrameInfo(const MachineFunction &MF) {
  C2GoFrameInfo FI;
  FI.MakesRealCall = c2goMakesRealCall(MF);
  FI.FrameSize = c2GoFrameSizeImpl(MF, FI.MakesRealCall);
  return FI;
}

// emitC2GoPrologue emits the Go ABI0 (Plan 9) frame setup matching
// what `go tool asm` auto-injects for `TEXT $framesize-argsize`:
//
//   STR  LR, [SP, #-framesize]!     ; pre-index: sp -= framesize, [sp+0] = LR
//   STUR FP, [SP, #-8]               ; [sp-8] = saved old FP (red zone, BELOW frame)
//   SUB  FP, SP, #8                  ; FP register = sp - 8
//
// Required by Go runtime stack-walking: runtime/traceback.go reads
// the saved LR via `lrPtr = frame.sp + 0`, so non-leaf functions MUST
// store their saved LR at their own `[sp+0]`. Standard ARM64
// `stp x29, x30, [sp, #-N]!` puts LR at `[sp+8]` — incompatible.
//
// FP register at `sp - 8` (in the red zone below the frame) is Go's
// unusual convention. See traceback.go ~line 405:
//   "we decided to write the FP link *below* the stack pointer
//    (with R29 = RSP - 8 in Go functions)"
// c2go #288 (Option 3): set locals-map bits for every pointer FIELD of an
// aggregate (struct/array) stack local at SP-relative byte base `Base`.
// bit W ↔ stack word SP+W*8 (same convention as the stackmap path). Emits
// bits directly (no IR values) so it does not perturb register allocation.
//
// c2go #312: `SkipBytes` holds SP-relative byte offsets of UNION-AMBIGUOUS
// pointer words (a union word that is a pointer in one member and an int in
// another) — computed in clang CodeGen and forwarded via the alloca's
// `!c2go.union.ambig.words` metadata. We do NOT mark those words: this static
// all-PCs mask would otherwise flag a word that, at some PC, legitimately
// holds an integer, making copystack abort ("bad pointer in frame ...").
// Conservative under-marking; sound for the SQLite workload (union words hold
// heap/source pointers, never `&stack_local`). Fully-sound per-PC marking is
// tracked as #313.
//
// #432: the recursive walk that previously lived here has been lifted to
// `c2go::walkPointerFields` (llvm/Transforms/C2Go/C2GoGCMaskUtils.h) so this
// site, C2GoSafepoint::collectPointerFieldOffsets and
// C2GoGCSetup::collectPtrFieldOffsets all share one definition. The local
// callback retains the bitmap / Any / SkipBytes specifics of this site.
static void c2goMarkPtrFieldBits(Type *Ty, uint64_t Base, const DataLayout &DL,
                                 uint64_t Nbit, std::vector<uint8_t> &Bits,
                                 bool &Any,
                                 const DenseSet<uint64_t> &SkipBytes) {
  c2go::walkPointerFields(Ty, Base, DL, [&](uint64_t Off) {
    if (SkipBytes.contains(Off))
      return; // union-ambiguous word — leave it unmarked (#312)
    uint64_t W = Off / 8;
    if (W < Nbit) {
      Bits[W / 8] |= uint8_t(1) << (W % 8);
      Any = true;
    }
  });
}

bool emitC2GoPrologue(MachineFunction &MF, MachineBasicBlock &MBB) {
  if (!isC2GoMode(MF))
    return false;

  auto &Subtarget = MF.getSubtarget<AArch64Subtarget>();
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineBasicBlock::iterator MBBI = MBB.begin();
  DebugLoc DL;

  // c2go #238 (Phase 2): pull FrameSize + MakesRealCall through the
  // AArch64FunctionInfo cache so they're computed exactly once per MF
  // (previously the MI walk ran twice — c2GoFrameSize and the NOSPLIT
  // check below both called c2goMakesRealCall directly).
  auto *AFI = MF.getInfo<AArch64FunctionInfo>();
  const C2GoFrameInfo &FI = AFI->getOrComputeC2GoFI(MF);
  uint64_t FrameSize = FI.FrameSize;
  // Update MFI to the actual amount we subtract from SP so the
  // frame-index resolver computes correct SP-based offsets.
  MF.getFrameInfo().setStackSize(FrameSize);
  // Also persist on AArch64FunctionInfo so downstream c2go consumers
  // (epilogue, resolveFrameOffsetReference) can read it without
  // depending on PEI-stage MFI bookkeeping order.
  AFI->setC2GoFrameSize(FrameSize);

  // c2go (§B1, #238 Phase 3, #376): collect every piece of per-function
  // metadata here into a single C2GoFunctionMetadata aggregate, then stage
  // it on the AFI (AArch64AsmPrinter::emitFunctionEntryLabel republishes
  // into the actual MCPlan9AsmStreamer instance at the AsmPrinter stage —
  // the producer doesn't have a streamer handle at PEI time).
  C2GoFunctionMetadata C2GoMeta;
  C2GoMeta.Name = std::string(MF.getName());
  C2GoMeta.FrameSize = (int)FrameSize;

  // c2go (ABI foundation): internal GoABI0 functions carry their true Go
  // ABI0 argsize on the `c2go-argsize` IR fn attribute (set by clang's
  // SetLLVMFunctionAttributes). Forward it so the GC scans the incoming
  // argument region (an internal function never went through the manifest,
  // so without this it emits a bogus `$N-0`). When the attribute is absent
  // (c2go_extern boundary symbols), leave C2GoMeta.ArgSize std::nullopt so
  // publishC2GoFunction preserves the manifest argsize.
  if (MF.getFunction().hasFnAttribute("c2go-argsize")) {
    StringRef S =
        MF.getFunction().getFnAttribute("c2go-argsize").getValueAsString();
    int Parsed = 0;
    if (!S.getAsInteger(10, Parsed))
      C2GoMeta.ArgSize = Parsed;
  }

  // c2go #297 (L0): mark strict-leaf functions NOSPLIT (always on, no flag /
  // -O2 gate — L0 is a low-level mechanism). A strict leaf makes NO call (no
  // BL/BLR/tail-call) AND its frame fits the Go NOSPLIT budget. With no callee,
  // the function is its own entire nosplit chain, so the linker's stackcheck
  // graph terminates at it — no all-pairs explosion (that only happens when
  // EVERY function is NOSPLIT). NOSPLIT lets the Go assembler omit the
  // morestack stack-growth check at entry.
  //
  // Eligibility (conservative v0): scan every MI for a call (covers BL, the
  // indirect BLR, and tail-calls — MFI.hasCalls() can miss late-inserted
  // calls), require no dynamic stack adjustment, and cap the frame well below
  // the 800-byte StackNosplit limit using StackSmall (128). near-leaf
  // (callees within budget) is deferred to #283/#295.
  {
    // Use the same real-call criterion as c2GoFrameSize (#306): an orphan
    // stackmap left by inlining sets hasCalls()/adjustsStack() and is
    // isCall-flagged, but is NOT a real call — so a true leaf carrying one
    // (e.g. sqlite3_stricmp) is still NOSPLIT-eligible.
    //
    // c2go #238 (Phase 2): read the MakesRealCall decision from the cache
    // populated above — saves a second whole-MI walk.
    // A function carrying the register ABI (CallingConv::C2GoABIInternal) was
    // flipped by the leaf-abi pass, which already proved its whole in-TU
    // nosplit chain fits the budget. It MUST be NOSPLIT: morestack does not
    // preserve incoming argument registers, so a splittable register-ABI
    // function would lose its register args across copystack. This is the
    // single source of truth shared with the leaf-eligibility analysis.
    if (MF.getFunction().getCallingConv() == CallingConv::C2GoABIInternal) {
      // fail-closed: the eligibility pass must never flip a function whose
      // frame exceeds the linker's NOSPLIT budget. If it did, emitting NOSPLIT
      // would silently violate the stackcheck contract — abort instead.
      unsigned Budget = llvm::c2go::getNosplitBudgetForTriple(
          MF.getTarget().getTargetTriple().str());
      if (FrameSize > Budget)
        report_fatal_error(
            "c2go: C2GoABIInternal function frame exceeds nosplit budget");
      C2GoMeta.NoSplit = true;
    } else {
      bool MakesCall =
          FI.MakesRealCall || MF.getFrameInfo().hasVarSizedObjects();
      // StackSmall (objabi/internal abi) = 128: a conservative cap far under
      // the 800-byte StackNosplit hard limit the linker's stackcheck.go
      // enforces.
      const uint64_t kNoSplitFrameBudget = 128;
      if (!MakesCall && FrameSize <= kNoSplitFrameBudget)
        C2GoMeta.NoSplit = true;
    }
  }

  // c2go #287 (Option 3): forward the args pointer-word mask (hex, set by
  // clang's SetLLVMFunctionAttributes on internal GoABI0 functions) so the
  // streamer populates FUNCDATA $0 with pointer bits. Lets Go's copystack
  // relocate pointer args into the moving goroutine stack (fixes the
  // cross-Exec dangling-`&local` bug). Boundary symbols (c2go_extern /
  // c2go_linkname) lack the attribute and keep the all-zeros args map.
  //
  // Decode hex → bytes here and keep ArgPtrMaskStorage alive until the
  // publishC2GoFunction call at the end of this function (ArrayRef into the
  // local vector — must outlive the publish).
  std::vector<uint8_t> ArgPtrMaskStorage;
  if (MF.getFunction().hasFnAttribute("c2go-argptrmask")) {
    StringRef HexMask =
        MF.getFunction().getFnAttribute("c2go-argptrmask").getValueAsString();
    auto Nibble = [](char C) -> int {
      if (C >= '0' && C <= '9') return C - '0';
      if (C >= 'a' && C <= 'f') return C - 'a' + 10;
      if (C >= 'A' && C <= 'F') return C - 'A' + 10;
      return 0;
    };
    ArgPtrMaskStorage.reserve(HexMask.size() / 2);
    for (size_t I = 0; I + 1 < HexMask.size(); I += 2)
      ArgPtrMaskStorage.push_back(
          uint8_t((Nibble(HexMask[I]) << 4) | Nibble(HexMask[I + 1])));
    if (!ArgPtrMaskStorage.empty())
      C2GoMeta.ArgPtrMaskBytes = std::move(ArgPtrMaskStorage);
  }

  // c2go #288 (Option 3): aggregate-field LOCALS pointer mask. Pointer FIELDS
  // inside struct/array stack locals (e.g. SQLite's `yyParser sEngine.pParse =
  // &sParse`) must be relocated by copystack, but cannot be marked via clang
  // stackmap operands (GEP-per-field exhausts the -O0 register allocator).
  // Compute their locals-map bits HERE from MachineFrameInfo + the IR alloca
  // type (no live values) and forward to the streamer, which ORs them into the
  // FUNCDATA $1 body bitmaps. Offsets come from getFrameIndexReference (same
  // resolver the stackmap path uses, whose scalar bits are proven correct), so
  // we only take SP-relative slots (FrameReg == SP) — matching the stackmap
  // harvest filter. Verified against the emitted .s.
  //
  // #327: this static, all-PCs aggregate mask is part of the LIGHTWEIGHT
  // alloca-only path and is UNSOUND under the statepoint GC path. It marks a
  // pointer field of an aggregate at EVERY PC, but stack-slot coloring lets
  // that field's word hold a non-pointer (e.g. a union's integer member, or a
  // reused slot) at some safepoints — copystack then reads the int as a
  // pointer and aborts ("bad pointer in frame ... 0xa": sqlite3Prepare word 21
  // held 0xa). When the function uses the "c2go-gc" strategy (set by
  // C2GoGCSetupPass when -c2go-statepoint-gc is on), RewriteStatepointsForGC
  // already tracks every live pointer — including aggregate-field pointers it
  // loads/derives — and LowerSTATEPOINT records them per-PC, which is the SOUND
  // source of truth. So suppress this static OR entirely in statepoint mode;
  // the union-ambig scrub (its companion #312 hack) is likewise unneeded.
  bool C2GoStatepointGC = MF.getFunction().hasGC() &&
                          MF.getFunction().getGC() == "c2go-gc";
  // c2go #238 Phase 3: locals masks (`Bits` / `AmbigBits`) and the storage
  // they point to via C2GoMeta.LocalsAgg/AmbigMaskBytes must outlive the
  // publishC2GoFunction call at the END of this function. Hoist them to
  // function scope so that ArrayRef remains valid.
  std::vector<uint8_t> Bits, AmbigBits;
  bool Any = false, AnyAmbig = false;
  // c2go GC Approach B (#330): the pointer-spill-slot marking runs under BOTH
  // GC paths. The aggregate-FIELD mask + union-ambig scrub remain gated to the
  // lightweight path (they are unsound / unneeded under statepoint per #327),
  // but the anonymous-spill "ptr" tags are the missing-derived-pointer fix the
  // statepoint path needs too (LowerSTATEPOINT only records the gc-live set,
  // which omits derived pointers regalloc spilled to anonymous slots — #329).
  if (FrameSize >= 16) {
    const MachineFrameInfo &MFI = MF.getFrameInfo();
    const DataLayout &DL = MF.getDataLayout();
    const TargetFrameLowering *TFL = Subtarget.getFrameLowering();
    uint64_t Nbit = (FrameSize - 8) / 8;
    Bits.assign((Nbit + 7) / 8, 0);
    // c2go #312: union-ambiguous word bitmask (same width). A bit here means
    // "this stack word overlaps a union member that is a pointer in one
    // alternative and a non-pointer in another". It is scrubbed from EVERY
    // locals bitmap by the streamer — covering BOTH the aggregate-field mask
    // (below) AND the per-PC stackmap path. The stackmap path can mark a union
    // word because stack-slot coloring reuses the union's slot for a managed
    // pointer local at other PCs; the static (all-PCs) FUNCDATA $1 cannot tell
    // them apart, so at a PC where the slot holds the union's integer member
    // copystack would misread it as a pointer (#312: yy_reduce ... 0x9).
    AmbigBits.assign((Nbit + 7) / 8, 0);
    // Aggregate pointer-FIELD mask + union-ambig scrub: lightweight path only
    // (#327: unsound/unneeded under the statepoint path, which marks aggregate
    // fields per-PC via LowerSTATEPOINT).
    for (int FI = MFI.getObjectIndexBegin(), E = MFI.getObjectIndexEnd();
         !C2GoStatepointGC && FI < E; ++FI) {
      if (MFI.isDeadObjectIndex(FI))
        continue;
      const AllocaInst *AI = MFI.getObjectAllocation(FI);
      if (!AI)
        continue;
      Type *ATy = AI->getAllocatedType();
      if (!ATy->isAggregateType()) // scalars handled by the stackmap path
        continue;
      Register FrameReg;
      StackOffset Off = TFL->getFrameIndexReference(MF, FI, FrameReg);
      if (FrameReg != AArch64::SP)
        continue;
      int64_t SPoff = Off.getFixed();
      if (SPoff < 0)
        continue;
      // c2go #312: read the union-ambiguous pointer-word offsets clang
      // attached to this alloca (relative to the alloca origin) and translate
      // them to SP-relative byte offsets. Used to (a) skip marking them in the
      // aggregate-field mask, and (b) accumulate the AmbigBits scrub mask the
      // streamer applies to the per-PC stackmap bits as well.
      DenseSet<uint64_t> SkipBytes;
      if (const MDNode *MD = AI->getMetadata(llvm::c2go::kUnionAmbigWordsMD)) {
        for (const MDOperand &Op : MD->operands()) {
          if (auto *CMD = dyn_cast<ConstantAsMetadata>(Op.get()))
            if (auto *CI = dyn_cast<ConstantInt>(CMD->getValue())) {
              uint64_t Abs = (uint64_t)SPoff + CI->getZExtValue();
              SkipBytes.insert(Abs);
              uint64_t W = Abs / 8;
              if (W < Nbit) {
                AmbigBits[W / 8] |= uint8_t(1) << (W % 8);
                AnyAmbig = true;
              }
            }
        }
      }
      c2goMarkPtrFieldBits(ATy, (uint64_t)SPoff, DL, Nbit, Bits, Any,
                           SkipBytes);
    }
    // c2go GC Approach B (#330) Milestone 3: mark RegAlloc-introduced
    // ANONYMOUS spill slots that hold a GC pointer. These have NO backing
    // alloca but M2 (InlineSpiller) tagged them "ptr" via setC2GoSpillSlotTag.
    //
    // M5 promotion: this BODY-wide OR-in is now only used when M5
    // (`-c2go-gc-ptrslot-liveness`, default on) is explicitly disabled.
    // M5 runs LATER than this prologue emit (addPreEmitPass2), so we cannot
    // gate via AFI.isC2GoPtrSlotLivenessValid() here; we read the cl::opt
    // directly. When M5 is on, the AsmPrinter LowerSTATEPOINT / LowerSTACKMAP
    // paths consult the per-call set and we MUST NOT also OR the slots into
    // the body mask — that would re-introduce the all-PCs over-mark M5
    // exists to eliminate (SQLite `deep`/`all` crashes).
    // #377: both subsystems now read through llvm::c2go::isC2GoDisabled().
    bool M5Enabled = !llvm::c2go::isC2GoDisabled("ptrslot-liveness");
    if (!llvm::c2go::isC2GoDisabled("spill-tags") && !M5Enabled) {
      // #375 slice 3: spill-slot tags now live on AArch64FunctionInfo (AFI
      // captured above at the entry of emitC2GoPrologue).
      for (const auto &KV : AFI->getC2GoSpillSlotTags()) {
        int FI = KV.first;
        if (KV.second != "ptr")
          continue;
        if (FI < MFI.getObjectIndexBegin() || FI >= MFI.getObjectIndexEnd() ||
            MFI.isDeadObjectIndex(FI))
          continue;
        if (MFI.getObjectAllocation(FI))
          continue;
        Register FrameReg;
        StackOffset Off = TFL->getFrameIndexReference(MF, FI, FrameReg);
        if (FrameReg != AArch64::SP)
          continue;
        int64_t SPoff = Off.getFixed();
        if (SPoff < 0)
          continue;
        uint64_t W = (uint64_t)SPoff / 8;
        if (W < Nbit) {
          Bits[W / 8] |= uint8_t(1) << (W % 8);
          Any = true;
        }
      }
    }
  }

  // c2go #238 Phase 3, #376: byte-vectors are populated above; the
  // C2GoFunctionMetadata struct now OWNS its payload (was ArrayRef pre-#376),
  // so move the vectors in rather than aliasing.
  if (Any)
    C2GoMeta.LocalsAggMaskBytes = std::move(Bits);
  if (AnyAmbig)
    C2GoMeta.LocalsAmbigMaskBytes = std::move(AmbigBits);

  // c2go #238 Phase 3, #376: stage the metadata aggregate on the AFI. The
  // AsmPrinter consumer (AArch64AsmPrinter::emitFunctionEntryLabel) will
  // republish into the actual MCPlan9AsmStreamer instance once it has a
  // streamer handle. Must come before the FrameSize==0 early-return below —
  // strict-leaf frameless functions still need their TEXT directive metadata
  // published (NOSPLIT bit, argsize, etc).
  AFI->setC2GoStagedMeta(std::move(C2GoMeta));

  // True leaf with no locals: skip prologue emit entirely. The Plan 9
  // streamer will see framesize=0 and emit `NOSPLIT|NOFRAME, $0`. We
  // still return true to mark that we've handled c2go-mode framing
  // (so emitPrologue's standard path is bypassed).
  if (FrameSize == 0)
    return true;

  // STR LR, [SP, #-framesize]!   (pre-index)
  BuildMI(MBB, MBBI, DL, TII->get(AArch64::STRXpre))
      .addReg(AArch64::SP, RegState::Define)
      .addReg(AArch64::LR)
      .addReg(AArch64::SP)
      .addImm(-static_cast<int64_t>(FrameSize))
      .setMIFlags(MachineInstr::FrameSetup);

  // STUR FP, [SP, #-8]   (unscaled signed-offset store, no writeback)
  BuildMI(MBB, MBBI, DL, TII->get(AArch64::STURXi))
      .addReg(AArch64::FP)
      .addReg(AArch64::SP)
      .addImm(-8)
      .setMIFlags(MachineInstr::FrameSetup);

  // SUB FP, SP, #8   (FP register = address of saved-FP slot = sp - 8)
  BuildMI(MBB, MBBI, DL, TII->get(AArch64::SUBXri), AArch64::FP)
      .addReg(AArch64::SP)
      .addImm(8)
      .addImm(0) // shift
      .setMIFlags(MachineInstr::FrameSetup);

  return true;
}

// emitC2GoEpilogue mirror:
//
//   LDUR FP, [SP, #-8]
//   LDR  LR, [SP], #framesize       ; post-index: load LR, sp += framesize
//   RET
bool emitC2GoEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) {
  if (!isC2GoMode(MF))
    return false;

  auto &Subtarget = MF.getSubtarget<AArch64Subtarget>();
  const TargetInstrInfo *TII = Subtarget.getInstrInfo();
  MachineBasicBlock::iterator MBBI = MBB.getFirstTerminator();
  DebugLoc DL;
  if (MBBI != MBB.end())
    DL = MBBI->getDebugLoc();

  // Read the framesize the prologue published on AArch64FunctionInfo —
  // robust against intervening passes that may modify MFI.getStackSize().
  //
  // c2go #438: in c2go-mode the prologue is REQUIRED to have run first
  // (emitC2GoPrologue calls AFI->setC2GoFrameSize before returning). A
  // missing C2GoFrameSize while still in c2go-mode means an upstream
  // refactor split the prologue/epilogue invariant — the silent
  // MFI.getStackSize() fallback would let the epilogue ship with a
  // mismatched frame and re-introduce the #277 RET-to-0 corruption.
  // Hard-fail instead of fall back.
  //
  // True-leaf functions (FrameSize == 0) take the prologue's fast path
  // which also sets hasC2GoFrameSize(true) — they reach the FrameSize==0
  // early-return below, NOT this assertion.
  const auto *AFI = MF.getInfo<AArch64FunctionInfo>();
  if (!AFI->hasC2GoFrameSize()) {
    report_fatal_error(Twine("c2go epilogue without prologue-staged framesize "
                             "(MF='") + MF.getName() +
                       "'): emitC2GoPrologue must run before emitC2GoEpilogue "
                       "in c2go-mode (#438)",
                       /*GenCrashDiag=*/false);
  }
  uint64_t FrameSize = AFI->getC2GoFrameSize();

  // Mirror the prologue's leaf-no-locals fast path: nothing to undo.
  if (FrameSize == 0)
    return true;

  // LDUR FP, [SP, #-8]   (unscaled signed-offset load)
  BuildMI(MBB, MBBI, DL, TII->get(AArch64::LDURXi), AArch64::FP)
      .addReg(AArch64::SP)
      .addImm(-8)
      .setMIFlags(MachineInstr::FrameDestroy);

  // LDR LR, [SP], #framesize   (post-index)
  BuildMI(MBB, MBBI, DL, TII->get(AArch64::LDRXpost))
      .addReg(AArch64::SP, RegState::Define)
      .addReg(AArch64::LR, RegState::Define)
      .addReg(AArch64::SP)
      .addImm(FrameSize)
      .setMIFlags(MachineInstr::FrameDestroy);

  return true;
}

} // namespace c2go
} // namespace llvm
