//===- X86C2GoFrameEmitter.cpp - c2go (Plan 9) frame meta staging (X86) -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #298 Wave AA Track B (X86 minimal staged-meta producer).
//
// See X86C2GoFrameEmitter.h header comment for the design overview / scope.
// This TU implements the MachineFunctionPass that, for every CC-flipped
// internal leaf the Wave V X86 leaf-ABI IR pass already turned into a
// `c2goabiinternalcc` callee, stages a baseline `C2GoFunctionMetadata` onto
// X86MachineFunctionInfo. The Wave Z Track B consumer
// (`X86AsmPrinter::emitFunctionEntryLabel`) republishes the staged value
// into the live `MCPlan9AsmStreamer` via `publishC2GoFunction` before the
// function label is emitted, which then lights up the streamer's Stage-1
// `TEXT name(SB), NOSPLIT|NOFRAME, $0-M` directive (instead of the Stage-4
// fallback `TEXT name(SB), NOFRAME, $0`).
//
//===----------------------------------------------------------------------===//

#include "X86C2GoFrameEmitter.h"
#include "X86.h"
#include "X86FrameLowering.h"
#include "X86InstrInfo.h"
#include "X86MachineFunctionInfo.h"
#include "X86RegisterInfo.h"
#include "X86Subtarget.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/MCC2GoFunctionMetadata.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/C2Go/C2GoGCMaskUtils.h"
#include "llvm/Transforms/C2Go/C2GoLeafEligibility.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

#define DEBUG_TYPE "x86-c2go-frame-meta-stager"

using namespace llvm;

//===----------------------------------------------------------------------===//
// c2go #298 Wave AB.1 / AB.2 — X86 c2go FrameInfo cache + Plan 9 prologue /
// epilogue emit. Mirrors `llvm::c2go::C2GoFrameInfo` (AArch64) at the
// X86-specific contract documented in X86C2GoFrameEmitter.h §3:
//   * no LR register — return address pushed by `CALL`, no software spill;
//   * RBP-conditional FP — `push rbp; mov rbp, rsp` only when hasFP(MF);
//   * `roundup(Locals, 16)` (no-BP) vs `roundup(Locals + 8, 16) - 8` (BP).
//===----------------------------------------------------------------------===//

// isX86C2GoMode — true when the function lives in a c2go-mode module
// (`c2go.goabi` module flag present). File-local (anonymous-namespace
// static) to avoid colliding with AArch64's `llvm::c2go::isC2GoMode`
// linker symbol (both share namespace `llvm::c2go::` and would otherwise
// duplicate-define). Wave AB header §5 notes a future shared
// `llvm/Transforms/C2Go/C2GoModeQuery.h` once a third caller appears.
namespace {
bool isX86C2GoMode(const MachineFunction &MF) {
  return MF.getFunction().getParent()->getModuleFlag(
             llvm::c2go::kGoabiModuleFlag) != nullptr;
}
} // anonymous namespace

namespace llvm {
namespace c2go {

// c2go #298 Wave AC.1 — GC precision triplet helpers. These are direct
// ports of the inline computation embedded in AArch64's `emitC2GoPrologue`
// (llvm/lib/Target/AArch64/C2GoFrameEmitter.cpp). The shared
// `c2go::walkPointerFields` walker is target-agnostic — the only X86 delta
// is the `FrameReg == X86::RSP` filter (vs AArch64::SP) and the lack of a
// `-8` adjustment on `Nbit` (X86's SavedLinkSize=0 contract: no saved-LR
// top-word to exclude).
//
// File-static helpers (not anonymous-namespace) so the late-running
// X86C2GoFrameMetaStager fallback path can call them without crossing
// nested-anonymous-namespace boundaries. The names are TU-local thanks to
// `static`; AArch64's free function `c2goMarkPtrFieldBits` lives in its
// own TU so there is no link-time collision.

// X86 mirror of AArch64's `c2goMarkPtrFieldBits`. Same delegation to
// `c2go::walkPointerFields` (#432 lift) + identical SkipBytes scrub
// semantics (#312).
static void x86MarkPtrFieldBits(llvm::Type *Ty, uint64_t Base,
                                const llvm::DataLayout &DL, uint64_t Nbit,
                                std::vector<uint8_t> &Bits, bool &Any,
                                const llvm::DenseSet<uint64_t> &SkipBytes) {
  llvm::c2go::walkPointerFields(Ty, Base, DL, [&](uint64_t Off) {
    if (SkipBytes.contains(Off))
      return; // union-ambiguous word — leave unmarked (#312)
    uint64_t W = Off / 8;
    if (W < Nbit) {
      Bits[W / 8] |= uint8_t(1) << (W % 8);
      Any = true;
    }
  });
}

// computeX86LocalsAggMaskBytes / computeX86LocalsAmbigMaskBytes — joint
// pass over MFI allocas. We compute both masks in one walk because the
// per-alloca `!c2go.union.ambig.words` MD must be read to build
// `SkipBytes` for the aggregate scan AND to OR into the ambig scrub at
// the same time (mirror of the AArch64 inner loop body in
// `emitC2GoPrologue` lines ~341-399).
//
// Returns (LocalsAggMaskBytes, LocalsAmbigMaskBytes) — both empty when no
// aggregate stack object carries pointer fields or has union-ambig MD.
//
// Strict-leaf functions (FrameSize == 0 or < 8) short-circuit to empty
// masks (no body locals → nothing to scan; same gate AArch64 uses with
// `FrameSize >= 16` since AArch64's locals region starts above the saved
// LR+FP pair, so the AArch64 number-line happens to be 16 = 8+8 while X86
// is just 8 = ptr-size). For X86 SavedLinkSize=0, the meaningful gate is
// `FrameSize >= 8` (at least one pointer-word slot must exist).
static std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
computeX86LocalsMasks(const llvm::MachineFunction &MF, uint64_t FrameSize) {
  std::vector<uint8_t> AggBits, AmbigBits;
  if (FrameSize < 8)
    return {AggBits, AmbigBits};

  // c2go #327 mirror (AArch64 C2GoFrameEmitter.cpp:327-328 / 360-361):
  // this static, all-PCs aggregate-field mask + union-ambig scrub is part of
  // the LIGHTWEIGHT alloca-only path and is UNSOUND under the statepoint GC
  // path. It marks a pointer field of an aggregate at EVERY PC, but
  // stack-slot coloring lets that field's word hold a non-pointer (e.g. a
  // union's integer member, or a reused slot) at some safepoints — copystack
  // then reads the int as a pointer and aborts ("bad pointer in frame").
  // When the function uses the "c2go-gc" strategy (set by C2GoGCSetupPass
  // when -c2go-statepoint-gc is on), RewriteStatepointsForGC already tracks
  // every live pointer per-PC and LowerSTATEPOINT records them, which is the
  // SOUND source of truth. Suppress this static OR entirely in statepoint
  // mode — let per-PC liveness take over and avoid the silent over-mark vs
  // stack-slot coloring conflict (Wave Z F1-same-shape silent corruption).
  const llvm::Function &F = MF.getFunction();
  const bool C2GoStatepointGC = F.hasGC() && F.getGC() == "c2go-gc";
  if (C2GoStatepointGC)
    return {AggBits, AmbigBits}; // both empty — per-PC liveness owns it

  const llvm::MachineFrameInfo &MFI = MF.getFrameInfo();
  const llvm::DataLayout &DL = MF.getDataLayout();
  const llvm::TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();

  // X86 amd64 contract: SavedLinkSize=0 → no top-word exclusion.
  // Nbit = FrameSize / 8 (vs AArch64's `(FrameSize - 8) / 8`).
  uint64_t Nbit = FrameSize / 8;
  AggBits.assign((Nbit + 7) / 8, 0);
  AmbigBits.assign((Nbit + 7) / 8, 0);
  bool Any = false, AnyAmbig = false;

  for (int FI = MFI.getObjectIndexBegin(), E = MFI.getObjectIndexEnd();
       FI < E; ++FI) {
    if (MFI.isDeadObjectIndex(FI))
      continue;
    const llvm::AllocaInst *AI = MFI.getObjectAllocation(FI);
    if (!AI)
      continue;
    llvm::Type *ATy = AI->getAllocatedType();
    if (!ATy->isAggregateType()) // scalars handled by the stackmap path
      continue;
    llvm::Register FrameReg;
    llvm::StackOffset Off = TFL->getFrameIndexReference(MF, FI, FrameReg);
    // X86 delta from AArch64: SP → RSP.
    if (FrameReg != llvm::X86::RSP)
      continue;
    int64_t SPoff = Off.getFixed();
    if (SPoff < 0)
      continue;

    // Union-ambig scrub (#312): read alloca-attached MD and translate
    // to SP-relative byte offsets. Use the same `SkipBytes` set for the
    // aggregate scan (so ambig words are NOT marked in AggBits) AND OR
    // into AmbigBits the streamer applies downstream.
    llvm::DenseSet<uint64_t> SkipBytes;
    if (const llvm::MDNode *MD =
            AI->getMetadata(llvm::c2go::kUnionAmbigWordsMD)) {
      for (const llvm::MDOperand &MDOp : MD->operands()) {
        if (auto *CMD = llvm::dyn_cast<llvm::ConstantAsMetadata>(MDOp.get()))
          if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(CMD->getValue())) {
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

    x86MarkPtrFieldBits(ATy, (uint64_t)SPoff, DL, Nbit, AggBits, Any,
                        SkipBytes);
  }

  if (!Any)
    AggBits.clear();
  if (!AnyAmbig)
    AmbigBits.clear();
  return {std::move(AggBits), std::move(AmbigBits)};
}

// Decode the `c2go-argptrmask` IR fn attribute (hex byte sequence) into a
// raw byte vector. Empty when the attribute is missing OR the decoded hex
// is empty. Mirror of the AArch64 inline decode (C2GoFrameEmitter.cpp
// :287-302); kept anonymous-namespace.
static std::vector<uint8_t>
computeX86ArgPtrMaskBytes(const llvm::MachineFunction &MF) {
  std::vector<uint8_t> Out;
  const llvm::Function &F = MF.getFunction();
  if (!F.hasFnAttribute("c2go-argptrmask"))
    return Out;
  llvm::StringRef HexMask =
      F.getFnAttribute("c2go-argptrmask").getValueAsString();
  auto Nibble = [](char C) -> int {
    if (C >= '0' && C <= '9')
      return C - '0';
    if (C >= 'a' && C <= 'f')
      return C - 'a' + 10;
    if (C >= 'A' && C <= 'F')
      return C - 'A' + 10;
    return 0;
  };
  Out.reserve(HexMask.size() / 2);
  for (size_t I = 0; I + 1 < HexMask.size(); I += 2)
    Out.push_back(uint8_t((Nibble(HexMask[I]) << 4) | Nibble(HexMask[I + 1])));
  return Out;
}

// x86MakesRealCall — mirror of AArch64's c2goMakesRealCall (see
// C2GoFrameEmitter.cpp). Excludes STACKMAP/PATCHPOINT (#306 same-shape
// orphan-stackmap problem reappears on X86 once the C2GoSafepoint IR pass
// runs against the X86 path; the inline-call deletion symptom is
// architecture-independent). STATEPOINT is NOT excluded (#326 same rule —
// LowerSTATEPOINT emits a real CALL on X86 too).
static bool x86MakesRealCall(const MachineFunction &MF) {
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

// computeX86C2GoFrameInfo — public pure-function summary, idempotent.
// Callers reach this via X86MachineFunctionInfo::getOrComputeC2GoFI which
// memoises the result. See the header file §3.3 / §5 / §6 for the rationale
// behind the two FrameSize formulas (no-BP vs BP, both lifted verbatim from
// cmd/internal/obj/x86/obj6.go).
X86C2GoFrameInfo computeX86C2GoFrameInfo(const MachineFunction &MF) {
  X86C2GoFrameInfo FI;
  FI.MakesRealCall = x86MakesRealCall(MF);

  // AB5 GPT round-2 fix: do NOT read X86FrameLowering::hasFP(MF) here.
  // hasFP(MF) is the SysV decision face (it folds in stackmap/patchpoint/
  // EH/needFP — see X86FrameLowering.cpp:99-108) and is NOT congruent with
  // Go obj6.go's BP-emit predicate `!NOFRAME && !(autoffset==0 && !hasCall)`
  // (cmd/internal/obj/x86/obj6.go:621-635). On the c2go Plan-9 path obj6
  // owns the entire BP decision face based on `TEXT $framesize` plus the
  // NOSPLIT/NOFRAME flag word — letting LLVM also stamp `push rbp; mov rbp,
  // rsp` here would either double-emit (caught by the AB4 Plan-9 .s filter
  // in X86AsmPrinter::emitInstruction but still wrong for the .o path) or
  // race obj6's own injection. Pin NeedsFramePointer to false so the c2go
  // prologue/epilogue never emits a BP save; obj6 will inject one if (and
  // only if) its own predicate fires for the staged `$framesize`.
  FI.NeedsFramePointer = false;

  // NeedsFrame matches go-asm's "has frame" condition (mirror of the
  // AArch64 c2GoFrameSizeImpl predicate). With AB5's NeedsFramePointer=false
  // pin above, the BP contribution drops out — `NeedsFrame` is now purely
  // about whether locals/calls/var-sized objects force a SUB.
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  uint64_t Locals = MFI.getStackSize();
  bool NeedsFrame = FI.MakesRealCall || MFI.getMaxCallFrameSize() != 0 ||
                    MFI.hasVarSizedObjects() || Locals != 0;
  if (!NeedsFrame) {
    FI.FrameSize = 0;
    return FI;
  }

  // No-BP path is the only path on the c2go Plan-9 surface (AB5: obj6 owns
  // the BP decision; this emitter never emits `push rbp`). SUB amount =
  // roundup(Locals, 16) (≡ 0 mod 16). The CALL-pushed return address sits
  // at rsp+FrameSize+0 (above the declared `$framesize`, per obj6.go), so
  // the body's rsp stays 16-aligned without any BP adjustment.
  FI.FrameSize = (Locals + 15) & ~uint64_t{15};

  // c2go #298 Wave AC.1 — GC precision triplet (partial: args mask only
  // here; locals masks land in emitX86C2GoPrologue after setStackSize so
  // `getFrameIndexReference` returns the SP-relative offset the actual
  // emitted SUB will produce — mirror of the AArch64 sequencing).
  //
  // Why split here: `computeX86LocalsMasks` reads SPoff via the
  // TargetFrameLowering resolver, whose StackPtr-relative arithmetic
  // depends on `MFI.getStackSize()` being the FINAL post-prologue value.
  // PEI calls X86FrameLowering::emitPrologue (which dispatches to
  // emitX86C2GoPrologue) BEFORE finishing its setStackSize round-up; the
  // c2go prologue then calls setStackSize(FrameSize) itself to publish
  // the rounded value. That single setStackSize call must precede any
  // FrameRegister-relying mask scan. The args mask has no such dep — it
  // is a pure hex decode of an IR Fn attribute — so we compute it here
  // for symmetry with the existing per-MF cache.
  FI.ArgPtrMaskBytes = computeX86ArgPtrMaskBytes(MF);
  return FI;
}

// emitX86C2GoPrologue — X86 mirror of AArch64's emitC2GoPrologue.
// Mirrors the call-site contract exactly: returns true when the c2go path
// handled emission (caller skips standard X86 prologue), false otherwise.
//
// Plan 9 prologue instruction shape (header §3.2, AB5-updated):
//   single path:  SUB rsp, FrameSize                 (FrameSetup)
//
// No BP save/restore is emitted from this path — obj6.go owns the entire
// BP decision face based on the staged `TEXT $framesize` + flag word (see
// AB5 GPT round-2 fix on computeX86C2GoFrameInfo above).
bool emitX86C2GoPrologue(MachineFunction &MF, MachineBasicBlock &MBB) {
  // Triple-gate (header §5): c2go.goabi flag + Triple::isX86 + CC =
  // C2GoABIInternal. ANY off → return false so the base X86 SysV
  // prologue runs. This preserves byte-identical SysV emission for every
  // production X86 module that did not opt into the c2go pipeline AND
  // for every non-flipped function inside a c2go module (NOSPLIT budget
  // overflow / address-taken / cross-TU callee etc — Wave V's
  // eligibility predicate, not our concern here).
  if (!isX86C2GoMode(MF))
    return false;
  Triple TT(MF.getFunction().getParent()->getTargetTriple());
  if (!TT.isX86())
    return false;
  if (MF.getFunction().getCallingConv() != CallingConv::C2GoABIInternal)
    return false;

  auto *X86FI = MF.getInfo<X86MachineFunctionInfo>();
  const X86C2GoFrameInfo &FI = X86FI->getOrComputeC2GoFI(MF);
  uint64_t FrameSize = FI.FrameSize;

  // Mirror the AArch64 path: publish the rounded value back through MFI
  // so frame-index resolution sees the same SP delta the prologue used,
  // AND stage it onto X86MachineFunctionInfo so the epilogue (which
  // doesn't recompute) reads the exact same number.
  MF.getFrameInfo().setStackSize(FrameSize);
  X86FI->setC2GoFrameSize(FrameSize);

  // Build the staged metadata aggregate the AsmPrinter consumer will
  // republish into the Plan-9 streamer. Extends Wave AA Track B's
  // FrameSize=0 strict-leaf form by setting the real FrameSize and
  // letting the streamer pick up the no-frame vs frame branch.
  C2GoFunctionMetadata Meta;
  Meta.Name = std::string(MF.getName());
  Meta.FrameSize = (int)FrameSize;
  // X86 amd64 contract (Wave AA Track A — locked):
  //   SavedLinkSize  = 0  (CALL pushes return addr but obj6.go does NOT
  //                        count it in `$framesize`)
  //   FrameAlignment = 0  (obj6.go does NOT add extra padding on top of
  //                        declared `$framesize`)
  Meta.SavedLinkSize = 0;
  Meta.FrameAlignment = 0;

  // NoSplit decision — mirror of the AArch64 prologue's NOSPLIT-eligibility
  // check (#297 L0 / change-B). A function carrying the register ABI
  // (CallingConv::C2GoABIInternal) was flipped by the leaf-abi pass, which
  // already proved its whole in-TU nosplit chain fits the budget. It MUST be
  // NOSPLIT: morestack does not preserve incoming argument registers, so a
  // splittable register-ABI function would lose its register args across
  // copystack. This is the single source of truth shared with the
  // leaf-eligibility analysis.
  {
    if (MF.getFunction().getCallingConv() == CallingConv::C2GoABIInternal) {
      // fail-closed: the eligibility pass must never flip a function whose
      // frame exceeds the linker's NOSPLIT budget. If it did, emitting
      // NOSPLIT would silently violate the stackcheck contract — abort.
      unsigned Budget = llvm::c2go::getNosplitBudgetForTriple(
          MF.getTarget().getTargetTriple().str()); // amd64 = 792
      if (FrameSize > Budget)
        report_fatal_error(
            "c2go: C2GoABIInternal function frame exceeds nosplit budget");
      Meta.NoSplit = true;
    } else {
      // A strict leaf with frame ≤ NOSPLIT budget gets the NOSPLIT bit so the
      // Go assembler omits the morestack stack-growth check at entry;
      // non-leaves keep NoSplit=false so the linker's stackcheck graph
      // injects a splittable prologue header. Same budget (128) as the
      // AArch64 path.
      bool MakesCall =
          FI.MakesRealCall || MF.getFrameInfo().hasVarSizedObjects();
      const uint64_t kNoSplitFrameBudget = 128;
      if (!MakesCall && FrameSize <= kNoSplitFrameBudget)
        Meta.NoSplit = true;
    }
  }

  // Forward c2go-argsize from the IR fn attribute (same plumbing the
  // Wave AA Track B stager already used). Same Wave AA GPT Fix-3 logic:
  // the loud-fail when the attribute is missing-AND-CC-flipped lives on
  // the publish path in X86AsmPrinter::emitFunctionEntryLabel, not here,
  // so LIT tests that don't go through the Plan-9 streamer aren't broken.
  if (MF.getFunction().hasFnAttribute("c2go-argsize")) {
    StringRef S =
        MF.getFunction().getFnAttribute("c2go-argsize").getValueAsString();
    int Parsed = 0;
    if (!S.getAsInteger(10, Parsed))
      Meta.ArgSize = Parsed;
  }

  // c2go #298 Wave AC.1 — GC precision triplet. ArgPtrMaskBytes is a
  // pure attribute decode (cached on FrameInfo). Locals masks must be
  // recomputed HERE because `getFrameIndexReference` reads MFI.getStackSize()
  // and we just bumped it above; running the mask scan inside
  // computeX86C2GoFrameInfo (before the setStackSize) would land every
  // SP-relative offset off by `(FrameSize - PEI's pre-roundup StackSize)`
  // and the `SPoff < 0` filter would drop every aggregate alloca. This
  // sequencing mirrors AArch64::emitC2GoPrologue (which also computes
  // its `Bits` / `AmbigBits` vectors after setStackSize) — see comment
  // on computeX86C2GoFrameInfo above for the full rationale.
  Meta.ArgPtrMaskBytes = FI.ArgPtrMaskBytes;
  auto LocalsMasks = computeX86LocalsMasks(MF, FrameSize);
  Meta.LocalsAggMaskBytes = std::move(LocalsMasks.first);
  Meta.LocalsAmbigMaskBytes = std::move(LocalsMasks.second);
  // Note: do NOT write the locals masks back into the cached FrameInfo via
  // const_cast. The only consumer of `LocalsAgg/AmbigMaskBytes` is the
  // staged Meta we just populated (republished by the AsmPrinter via
  // MCPlan9AsmStreamer::publishC2GoFunction at emitFunctionEntryLabel).
  // The cached `X86C2GoFrameInfo` is documented as "computed by
  // computeX86C2GoFrameInfo" — writing the masks back here would violate
  // that contract (computeX86C2GoFrameInfo is called BEFORE setStackSize,
  // so it cannot legitimately produce locals masks itself) AND would write
  // through a const reference. AC1.2 GPT round-2 sharpen: drop the dead
  // write-back; revisit if/when a future Wave AB.4 stkobj harvester needs
  // a cache reader (re-add cleanly through a non-const accessor then).

  X86FI->setC2GoStagedMeta(std::move(Meta));

  // Strict-leaf frameless case — nothing to subtract from rsp. Wave AA
  // Track B's earlier minimal stager produced byte-identical output here
  // (the standard X86 SysV prologue is also no-op for a true leaf with
  // zero locals + no FP). We still return true to mark that the c2go
  // path owns the prologue decision for this function. AB.3+ locals
  // mask scans will hang off this same FrameSize==0 short-circuit.
  if (FrameSize == 0)
    return true;

  // Non-leaf path: emit the X86 prologue sequence per header §3.2 (AB5:
  // single-path SUB only — no BP save here; obj6 owns BP).
  const X86Subtarget &STI = MF.getSubtarget<X86Subtarget>();
  const TargetInstrInfo *TII = STI.getInstrInfo();
  MachineBasicBlock::iterator MBBI = MBB.begin();
  DebugLoc DL;

  // SUB rsp, FrameSize. Use the 32-bit-immediate form unconditionally —
  // it covers every realistic FrameSize (encoded imm-32 sign-extended to
  // 64 bits, max 2GiB; far above NOSPLIT budget and any plausible Go
  // frame). Matches the standard X86 prologue's choice (getSUBriOpcode
  // returns SUB64ri32 for LP64; the imm8 variant is opportunistic and
  // not required for correctness).
  BuildMI(MBB, MBBI, DL, TII->get(X86::SUB64ri32), X86::RSP)
      .addReg(X86::RSP)
      .addImm(static_cast<int64_t>(FrameSize))
      .setMIFlag(MachineInstr::FrameSetup);

  return true;
}

// emitX86C2GoEpilogue — symmetric mirror. Reads the published FrameSize
// (NOT MFI.getStackSize) so it can't desync with the prologue across
// intervening passes that might re-write the stack-size side channel.
bool emitX86C2GoEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) {
  if (!isX86C2GoMode(MF))
    return false;
  Triple TT(MF.getFunction().getParent()->getTargetTriple());
  if (!TT.isX86())
    return false;
  if (MF.getFunction().getCallingConv() != CallingConv::C2GoABIInternal)
    return false;

  // #438 mirror: any c2go-mode function reaching this epilogue MUST have
  // run the prologue (which always sets C2GoFrameSizeValid before
  // returning, even on the FrameSize==0 fast path). A missing channel
  // means an upstream pass split the prologue/epilogue invariant — fall
  // back silently to MFI.getStackSize() would re-introduce the AArch64
  // #277-equivalent RET-to-0 corruption on X86. Hard-fail instead.
  auto *X86FI = MF.getInfo<X86MachineFunctionInfo>();
  if (!X86FI->hasC2GoFrameSize()) {
    report_fatal_error(Twine("c2go x86 epilogue without prologue-staged "
                             "framesize (MF='") +
                           MF.getName() +
                           "'): emitX86C2GoPrologue must run before "
                           "emitX86C2GoEpilogue in c2go-mode (#438 mirror)",
                       /*GenCrashDiag=*/false);
  }
  uint64_t FrameSize = X86FI->getC2GoFrameSize();

  // Strict-leaf fast path — same short-circuit as the prologue. The
  // terminator (RET) is emitted by the X86 backend's standard terminator
  // path; we don't touch it.
  if (FrameSize == 0)
    return true;

  const X86Subtarget &STI = MF.getSubtarget<X86Subtarget>();
  const TargetInstrInfo *TII = STI.getInstrInfo();
  MachineBasicBlock::iterator MBBI = MBB.getFirstTerminator();
  DebugLoc DL;
  if (MBBI != MBB.end())
    DL = MBBI->getDebugLoc();

  // ADD rsp, FrameSize  (undoes the prologue's SUB). AB5: no BP POP here —
  // obj6.go owns the BP teardown to match its own injection at the entry
  // (see computeX86C2GoFrameInfo NeedsFramePointer=false pin).
  BuildMI(MBB, MBBI, DL, TII->get(X86::ADD64ri32), X86::RSP)
      .addReg(X86::RSP)
      .addImm(static_cast<int64_t>(FrameSize))
      .setMIFlag(MachineInstr::FrameDestroy);

  return true;
}

} // namespace c2go
} // namespace llvm

namespace {

#define PASS_NAME                                                              \
  "X86 c2go staged-meta producer (Plan 9 TEXT directive forwarder)"

class X86C2GoFrameMetaStager : public MachineFunctionPass {
public:
  static char ID;
  X86C2GoFrameMetaStager() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return PASS_NAME; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  bool runOnMachineFunction(MachineFunction &MF) override;
};

} // namespace

char X86C2GoFrameMetaStager::ID = 0;

INITIALIZE_PASS(X86C2GoFrameMetaStager, "x86-c2go-frame-meta-stager", PASS_NAME,
                false, false)

FunctionPass *llvm::createX86C2GoFrameMetaStagerPass() {
  return new X86C2GoFrameMetaStager();
}

bool X86C2GoFrameMetaStager::runOnMachineFunction(MachineFunction &MF) {
  // Production-byte-identity gate. The pass must be a strict no-op on any
  // module that did not opt into the c2go pipeline; non-c2go X86 builds
  // never run a non-default code path here.
  const Function &F = MF.getFunction();
  const Module *M = F.getParent();
  if (!M->getModuleFlag(llvm::c2go::kGoabiModuleFlag))
    return false;

  // Defensive: only X86 targets are wired by X86PassConfig — but the legacy
  // PM hook is callable directly, so re-check.
  Triple TT(M->getTargetTriple());
  if (!TT.isX86())
    return false;

  // c2go #298 Wave AH.1 — staging path decoupled from CC flip. The earlier
  // Wave AA Track B / AB gating required `F.getCallingConv() ==
  // CallingConv::C2GoABIInternal` here (the single CC-level marker the
  // X86C2GoLeafABI IR pass produces). That coupling was the root the Wave
  // AG diagnosis bottomed out on:
  //
  //   * Functions the LeafABI pass DECLINED to flip (NOSPLIT-budget
  //     overflow, address-taken, cross-TU callee, non-leaf — every
  //     eligibility filter Wave V applies) still need a Plan-9 TEXT
  //     directive when the Plan-9 streamer is the active OutStreamer.
  //     With the CC-gate in place those functions fell back to the
  //     streamer's Stage-4 TU-local `NOFRAME, $0` shape — argsize-less
  //     and NoSplit-less — and runtime probes (`mallocgc` /
  //     `gcAssistAlloc`) crashed in copystack because the caller had no
  //     accurate FUNCDATA $1 / $0 to point at.
  //
  //   * Mirror discipline: AArch64's `emitC2GoPrologue` (which is the
  //     producer-time equivalent of this stager) is single-gated on
  //     `isC2GoMode(MF)` only — no CC re-check. The X86 path should be
  //     symmetric.
  //
  // c2go #298 Wave AJ.2 — gate ALSO accepts `c2go-boundary` so the X86
  // stager publishes a TEXT framesize for `c2go_extern` functions. Before
  // Wave AJ.2 boundary functions fell through this gate (clang stamps
  // `c2go-boundary-argsize` for them, NOT `c2go-argsize`; see
  // CodeGenModule.cpp:2906-2929 + 2946-2952) and the Plan-9 streamer
  // ended up publishing the manifest-side `FrameSize=0` shipped by
  // `enqueueC2GoBoundary` at CodeGenAction.cpp:1125-1128. But the LLVM
  // X86 SysV body lowers outgoing args as `MOVQ … N(SP)` after a
  // `pushq %rbp; subq $N,%rsp` prologue. The Plan-9 `X86MCInstLower`
  // suppresses the FrameSetup/FrameDestroy MIs (X86MCInstLower.cpp:2603-
  // 2608) so the `.s` body carries the SP-relative writes WITHOUT the
  // matching SP decrement. With `$framesize=0` in the TEXT directive the
  // Go assembler `obj6.go` injects no frame either → the callee's
  // `MOVQ <arg>, 0(SP)` writes land on the caller's retPC slot →
  // `unexpected return pc` (the stress_init+0x24 crash observed at
  // Wave AI). Publishing the real LLVM-computed frame size lets
  // `obj6.go` inject the right SP adjustment + RBP save.
  //
  // The decoupled gate now reads:
  //   (a) module is c2go-mode  (above)
  //   (b) triple is X86        (above)
  //   (c) the function carries `c2go-argsize` (internal GoABI0, clang or
  //       c2go-lto stamped) OR `c2go-boundary` (c2go_extern boundary).
  //       Functions in neither bucket (runtime helpers living in the
  //       same module, c2go-internal helpers not yet on the boundary
  //       table) keep the standard X86 path.
  if (!F.hasFnAttribute("c2go-argsize") &&
      !F.hasFnAttribute("c2go-boundary"))
    return false;

  // Wave AB.2 / AH.1: when `emitX86C2GoPrologue` (the FrameLowering-time
  // producer) has already populated the staged metadata aggregate with
  // its precise FrameSize / NoSplit / locals masks decision, defer to
  // it — re-staging here would overwrite the prologue's authoritative
  // numbers with this pass's strict-leaf-only baseline. `hasC2GoStagedMeta`
  // is true precisely when the CC was flipped AND the prologue hook ran;
  // in the AH.1 CC-not-flipped case it is false and we stage the SysV-
  // fallback baseline ourselves (FrameSize=0 / NoSplit=false, see below).
  auto *X86FIEarly = MF.getInfo<X86MachineFunctionInfo>();
  if (X86FIEarly->hasC2GoStagedMeta())
    return false;

  // AH.1 / AJ.4 — track whether this function went through the LeafABI
  // CC flip so the baseline below can pick the right NoSplit / FrameSize
  // values. CC-flipped MFs that reach this fallback path (prologue ran
  // but failed to publish — defensive, should not happen post-AB.2)
  // keep the Wave V strict-leaf invariant: NoSplit=true / FrameSize=0.
  // CC-not-flipped MFs get the SysV-fallback baseline: NoSplit=false
  // (LLVM-emitted SysV prologue carries the SUB, obj6 may layer more)
  // and FrameSize read from MFI.getStackSize() alone (NOT plus
  // getMaxCallFrameSize). This stager runs in `addPreEmitPass2`, AFTER
  // PrologEpilogInserter has already folded MaxCallFrameSize into
  // getStackSize() and alignTo'd the sum (PrologEpilogInserter.cpp:
  // 1122-1165). Adding it a second time was the double-count that broke
  // the 16B alignment invariant (see the long comment at the SysV-
  // fallback branch below for the symptom-moved-one-frame trace).
  // Note this is NOT a "cross-arch symmetric to AArch64
  // c2GoFrameSizeImpl + 16" computation — AArch64's `+ 16` is a hand-
  // rolled prologue contract on a pre-PEI hook (saved LR slot + align);
  // X86's stager hooks post-PEI and consumes the final autosize. Each
  // arch reads the final MFI.getStackSize() appropriate for its own
  // stager timing.
  const bool CCFlipped =
      F.getCallingConv() == CallingConv::C2GoABIInternal;
  const bool IsBoundary = F.hasFnAttribute("c2go-boundary");

  // Build the minimal Stage-1 metadata aggregate. Wave AB will extend this
  // to non-leaf functions (real FrameSize, LocalsAggMaskBytes, etc.) once
  // the hand-rolled X86 prologue / epilogue mirror lands.
  C2GoFunctionMetadata Meta;
  Meta.Name = std::string(F.getName());

  // AH.1 / AJ.2: branch on CC-flipped vs SysV-fallback. The CC-flipped
  // path preserves the Wave AA Track B strict-leaf shape (NoSplit=true /
  // FrameSize=0); the SysV-fallback path reads the actual stack size +
  // outgoing-args reservation so the TEXT directive matches the
  // SP-relative offsets `obj6.go` must inject for the suppressed LLVM
  // prologue.
  if (CCFlipped) {
    // Wave V flipped only NOSPLIT-eligible leaves, so every function
    // reaching this CC-flipped stage has been certified strict-leaf by
    // the shared eligibility analysis. NoSplit=true tells the streamer
    // to emit the NOSPLIT bit in the TEXT flag word so the Go assembler
    // omits the morestack stack-growth check at entry — a strict leaf
    // is its own entire nosplit chain, so the linker's stackcheck graph
    // terminates at it and the all-pairs explosion that afflicts "every
    // function NOSPLIT" never happens here.
    Meta.FrameSize = 0;
    Meta.NoSplit = true;
  } else {
    // SysV-fallback path: LeafABI declined this function (internal
    // GoABI0 with c2go-argsize, or c2go_extern boundary). The LLVM X86
    // SysV PrologueEmitter would have emitted:
    //   pushq %rbp                         ; SP -= 8
    //   movq  %rsp, %rbp                   ; (no SP delta)
    //   subq  $autosize, %rsp              ; SP -= N
    // … but those FrameSetup MIs are SUPPRESSED in Plan-9 mode by
    // X86MCInstLower.cpp:2603-2608. The body still contains the
    // SP-relative outgoing-arg `MOVQ … N(SP)` writes that assume the
    // SP delta is in place, so we MUST publish that exact delta as the
    // declared `$framesize` for obj6.go to re-inject the SP adjustment
    // + RBP save (its NOFRAME branch is gated off when framesize != 0
    // — see obj6.go:608-635).
    //
    // Wave AJ.4 fix (F1 GPT round-1 — symptom-moved-one-frame): this
    // pass runs in `addPreEmitPass2`, which is AFTER
    // `PrologEpilogInserter` (TargetPassConfig.cpp: PEI at line 1185,
    // addPreEmitPass2 at line 1317, X86TargetMachine.cpp:713 schedules
    // the stager inside addPreEmitPass2). PEI has already folded
    // `getMaxCallFrameSize()` into `getStackSize()` in the reserved-CF
    // mode we run in (PrologEpilogInserter.cpp:1122-1123 adds the
    // outgoing-args reservation, line 1141 alignTo(StackAlign), line
    // 1165 setStackSize). So `MFI.getStackSize()` IS the post-prologue
    // autosize the SysV PrologueEmitter is about to encode in the
    // suppressed `subq $N, %rsp` — exactly what obj6.go's `$framesize`
    // wants.
    //
    // The earlier draft added `getMaxCallFrameSize()` on top, which
    // double-counted the outgoing-args reservation. Because that
    // reservation is the only part of the frame that isn't already
    // 16B-aligned in isolation (PEI re-aligns the *sum*), adding it
    // again broke the 16B invariant — observed values 100/84/124/140
    // all satisfied `framesize mod 16 == 4`. The Go assembler's
    // alignment check (obj6.go) then rejected the frame, and even when
    // it slipped through the GC stackmap layout in obj/runtime was
    // shifted relative to the actual SP delta — surfacing later as a
    // nil-deref in writeBarrier / `internal/abi.(*Type).Pointers` (an
    // address that "looks like" the writeBarrier global but is actually
    // 4 bytes off, on a stack frame the GC cannot scan correctly).
    //
    // RBP save is also already accounted for: X86FrameLowering accounts
    // for the PUSH RBP slot inside getStackSize when frame pointer is
    // required; obj6.go adds bpsize internally for the framepointer
    // case so we don't add it here either.
    Meta.FrameSize = static_cast<int>(MF.getFrameInfo().getStackSize());
    Meta.NoSplit = false;
  }
  // Wave AJ.2 — silence the unused-variable warning when IsBoundary is
  // only consulted in the argsize forward block below.
  (void)IsBoundary;

  // c2go #298 Wave AA Track A (F3 contract lock). AArch64 production
  // values are SavedLinkSize=8 / FrameAlignment=16 (the Go assembler's
  // obj7.go preprocess injects a `MOVD.W R30,-autosize(RSP)` LR spill
  // and re-adds 16 bytes of padding on top of declared `$framesize`).
  // For X86 amd64 BOTH are zero — `CALL` pushes the return address onto
  // rsp+0 but it is NOT subtracted from the Go-assembler-declared
  // `$framesize` (cmd/internal/obj/x86/obj6.go) and there is no extra
  // padding. The Wave AA Track A streamer branch consumes these to
  // (a) skip the "FrameSize - FrameAlignment" subtraction and (b) drive
  // the locals-bitmap Nbit via `(FrameSize - SavedLinkSize) / PtrSize`.
  // For the strict-leaf FrameSize=0 path these values don't change the
  // emitted bytes — they just establish the per-arch contract for the
  // Wave AB extension to non-zero frames.
  Meta.SavedLinkSize = 0;
  Meta.FrameAlignment = 0;

  // Forward the manifest-provided argsize from the c2go-argsize IR fn
  // attribute clang stamps on every internal GoABI0 function (see
  // clang/lib/CodeGen/CodeGenModule.cpp:2871-2886).
  //
  // Wave AA GPT NEEDS_FIX Fix 3 (B4 X86 stager loud-fail): if the CC has
  // been flipped to C2GoABIInternal but the `c2go-argsize` attribute is
  // MISSING, the clang / c2go-lto plumbing has skipped this function —
  // and emitting `$0-0` from the Plan-9 streamer would be Plan-9 legal
  // but semantically wrong for any callee with non-empty parameter list
  // (callers would corrupt the outgoing arg frame).
  //
  // SCOPE NOTE: the loud-fail lives on the publish path (downstream of
  // this stager — see X86AsmPrinter::emitFunctionEntryLabel) rather
  // than here on the MFPass, because the MFPass runs unconditionally
  // for every X86 MF in c2go mode regardless of whether the asm
  // streamer is the Plan-9 one. Non-Plan-9 X86 codegen paths (the
  // normal `.o` round-trip, or LLVM-IR-driven LIT tests that just
  // exercise CC-table dispatch without going through the Plan-9
  // streamer) never reach a publishC2GoFunction call site, so they
  // would gain nothing from a fatal-error here AND a fatal here would
  // break those tests (e.g. CodeGen/X86/c2go-abiinternal-reglower.ll
  // which is a hand-written IR ISel canary not a streamer test). The
  // single source-of-truth check sits where bad data would actually
  // miscompile — see X86AsmPrinter::emitFunctionEntryLabel for the
  // fatal report_fatal_error.
  if (F.hasFnAttribute("c2go-argsize")) {
    StringRef S = F.getFnAttribute("c2go-argsize").getValueAsString();
    int Parsed = 0;
    if (!S.getAsInteger(10, Parsed))
      Meta.ArgSize = Parsed;
  } else if (F.hasFnAttribute("c2go-boundary-argsize")) {
    // c2go #298 Wave AJ.2 — boundary functions (c2go_extern) carry their
    // argsize on `c2go-boundary-argsize` rather than `c2go-argsize`
    // (clang CodeGenModule.cpp:2949). Forward that into Meta so the
    // streamer's TEXT directive matches what the manifest-side
    // `enqueueC2GoBoundary` would have published (the two are computed
    // by the same `clang::c2go::computeC2GoArgSize` helper, so they
    // agree byte-for-byte; we forward through the stager instead of
    // letting the boundary enqueue overwrite us so the FrameSize we
    // computed above is preserved — `publishC2GoFunction`'s partial-
    // update semantics keep nullopt fields unchanged).
    StringRef S =
        F.getFnAttribute("c2go-boundary-argsize").getValueAsString();
    int Parsed = 0;
    if (!S.getAsInteger(10, Parsed))
      Meta.ArgSize = Parsed;
  }

  // c2go #298 Wave AC.1 — strict-leaf fallback path still needs the args
  // pointer-word mask (locals masks are empty under FrameSize=0). Read
  // through the same `c2go-argptrmask` decode the prologue uses; sharing
  // the helper keeps the two stagers byte-identical on the args side
  // when the prologue hook is not yet wired. The helper lives in
  // `llvm::c2go::` (file-static within that namespace).
  Meta.ArgPtrMaskBytes = llvm::c2go::computeX86ArgPtrMaskBytes(MF);

  auto *X86FI = MF.getInfo<X86MachineFunctionInfo>();
  X86FI->setC2GoStagedMeta(std::move(Meta));
  return false; // staged metadata is consumed at AsmPrinter; no MIR edits.
}
