//===- C2GoLoopPoll.cpp - Cooperative loop preemption poll ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See C2GoLoopPoll.h. Picks every natural loop in a c2go-managed function
// (`c2go.goabi` module flag + `c2go-c-name` fn-attr, non-boundary) that has
// NO real call in its body and injects a periodic
// `call void @runtime.Gosched()` at the latch. The call is a normal non-leaf
// call: the lightweight C2GoSafepoint and the SOUND C2GoGCSetup/RS4GC paths
// both recognise every non-noop call as a safepoint (mirrors
// `isPotentialMorestackCall`), so no additional attr is needed.
//
// Iteration period M is chosen so the poll fires roughly every
// `c2go-loop-poll-target-ns` (default 10 ms), using TTI cycle cost of the
// loop body. M is clamped to [1, 1<<16] and rounded down to a power of two
// so the mod check is a single AND. Loops with a known small trip count
// (TC < M) are skipped — the counter would never trip and we'd just leave
// dead code behind.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoLoopPoll.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/C2Go/C2GoCommon.h"
#include "llvm/Transforms/C2Go/C2GoMorestackUtils.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

#define DEBUG_TYPE "c2go-loop-poll"

// Default ON (#362). The pass injects calls to c2go-libc's Gosched bridge
// (`github.com/c2gohq/c2go_libc.Gosched`, a Go-side thin wrapper for
// `runtime.Gosched` with a `//go:linkname` push so the ABI0 entry is
// generated). `runtime.Gosched` itself has no linkname push and is
// ABIInternal-only, so c2go .s cannot target it directly. The c2go-libc
// bridge sits at a multi-segment import path; the Plan9 streamer renders
// `.`/`/` via U+00B7 / U+2215 Unicode escapes (MCPlan9AsmStreamer::
// symbolToPlan9 path-a), so no per-user-package trampoline is needed.
static cl::opt<bool>
    ClEnableLoopPoll("c2go-loop-poll", cl::init(true), cl::Hidden,
                     cl::desc("c2go #252: enable cooperative loop poll pass "
                              "(BL c2go-libc.Gosched every M iterations). "
                              "Set to 0 to disable."));

// Target wall-clock period between polls, in nanoseconds. 0 = DISABLED (the
// default: cooperative loop-poll preemption is opt-in). Set to N to enable —
// Go's sysmon forcePreemptNS tick is 10 ms (10000000).
static cl::opt<unsigned> ClTargetNs(
    "c2go-loop-poll-target-ns", cl::init(0), cl::Hidden,
    cl::desc("Target nanoseconds between c2go cooperative loop polls "
             "(0 = disable the pass)"));

// Approximate cycles per nanosecond assuming a ~3 GHz core. TTI returns cost
// in abstract cycles; multiplying by NS_PER_CYCLE recovers a rough wall-clock
// estimate. Conservative (over-estimates body cost → smaller M → polls more
// often) is the safer side: an extra Gosched is harmless, a missed safepoint
// re-introduces the 8.5 s stall.
static constexpr double kNsPerCycle = 0.33;

// Maximum M (clamp). 1 << 16 = 65536 iterations. Going higher risks pushing
// GC pause beyond the 10 ms budget for very tight bodies; we'd rather poll a
// bit more often than blow the budget.
static constexpr uint64_t kMaxM = 1ULL << 16;

// Fallback M for loops whose trip count SCEV cannot compute. 1024 is the
// design-doc value (§4.10.6); a power of two for cheap `and` mod.
static constexpr uint64_t kFallbackM = 1024;

// hasAnyRealCallInBody returns true if any BB in L (including sub-loop
// blocks) contains a CallBase that can reach a callee morestack prologue.
// c2go::mayReachMorestack is shared with the lightweight stackmap and RS4GC
// paths, so a call-free InlineAsm node neither suppresses a loop poll nor
// acquires a fake safepoint.
static bool hasAnyRealCallInBody(const Loop &L) {
  for (const BasicBlock *BB : L.blocks())
    for (const Instruction &I : *BB) {
      const auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      if (c2go::mayReachMorestack(*CB))
        return true;
    }
  return false;
}

// Compute the body cost of L in abstract cycles. Sums TTI getInstructionCost
// over every instruction in L's blocks. Sub-loops are included (they're part
// of the latch->header round trip).
static uint64_t computeBodyCycles(const Loop &L,
                                  const TargetTransformInfo &TTI) {
  uint64_t Cost = 0;
  for (const BasicBlock *BB : L.blocks())
    for (const Instruction &I : *BB) {
      InstructionCost C =
          TTI.getInstructionCost(&I, TargetTransformInfo::TCK_Latency);
      if (C.isValid())
        Cost += C.getValue();
      else
        Cost += 1;
    }
  if (Cost == 0)
    Cost = 1;
  return Cost;
}

// Compute the iteration period M for L. Returns 0 if the loop should be
// skipped (e.g. known small trip count).
static uint64_t computeM(Loop &L, ScalarEvolution &SE,
                         const TargetTransformInfo &TTI) {
  uint64_t Cycles = computeBodyCycles(L, TTI);
  double BodyNs = double(Cycles) * kNsPerCycle;
  if (BodyNs < 1.0)
    BodyNs = 1.0;
  double Raw = double(ClTargetNs) / BodyNs;
  uint64_t M;
  if (Raw <= 1.0)
    M = 1;
  else if (Raw >= double(kMaxM))
    M = kMaxM;
  else
    M = uint64_t(Raw);
  // Round down to the nearest power of two so we can use `cnt & (M-1)`.
  // M is always >= 1 here (the branches above set M to 1, kMaxM, or
  // uint64_t(Raw) with Raw > 1.0, and `1ULL << Log2_64(M)` preserves >= 1).
  M = 1ULL << Log2_64(M);

  // If trip count is known and < M, no point inserting — the counter would
  // never trip. Fall back to kFallbackM for unknown trip counts (the design
  // default).
  unsigned MaxTC = SE.getSmallConstantMaxTripCount(&L);
  if (MaxTC != 0) {
    if (uint64_t(MaxTC) < M)
      return 0;
  } else {
    // Unknown trip count — use the fallback M instead of the TTI-derived
    // one. This keeps the period bounded for hot pure-arith loops where
    // SCEV can't see the exit count.
    M = std::min(M, kFallbackM);
    // M stays >= 1 (kFallbackM is 1024 and the incoming M is >= 1).
    M = 1ULL << Log2_64(M);
  }
  return M;
}

// Inject the poll counter + cond call into L. Returns true on success.
static bool injectPoll(Loop &L, uint64_t M, Function &F) {
  BasicBlock *Preheader = L.getLoopPreheader();
  BasicBlock *Latch = L.getLoopLatch();
  if (!Preheader || !Latch)
    return false; // shouldn't happen — caller checks isLoopSimplifyForm.

  LLVMContext &Ctx = F.getContext();
  Type *I64 = Type::getInt64Ty(Ctx);

  // Counter alloca lives at function entry so it survives backedge restarts
  // without needing a phi. (Putting it in the preheader works too but a
  // function-entry alloca is simpler and is what mem2reg would have done
  // anyway; mem2reg has already run by OptimizerLast.)
  IRBuilder<> EB(&*F.getEntryBlock().getFirstInsertionPt());
  AllocaInst *Cnt = EB.CreateAlloca(I64, nullptr, "c2go.lp.cnt");
  Cnt->setAlignment(Align(8));

  // Initialize counter in the preheader (so re-entry zeros it; nested-loop
  // safe).
  IRBuilder<> PB(Preheader->getTerminator());
  PB.CreateStore(ConstantInt::get(I64, 0), Cnt);

  // At the latch (just before the terminator) bump the counter and check
  // (cnt & (M-1)) == 0.
  IRBuilder<> LB(Latch->getTerminator());
  Value *Old = LB.CreateLoad(I64, Cnt, "c2go.lp.cnt.old");
  Value *New = LB.CreateAdd(Old, ConstantInt::get(I64, 1), "c2go.lp.cnt.new");
  LB.CreateStore(New, Cnt);
  Value *Mask = ConstantInt::get(I64, M - 1);
  Value *Masked = LB.CreateAnd(New, Mask, "c2go.lp.cnt.mod");
  Value *IsZero = LB.CreateICmpEQ(Masked, ConstantInt::get(I64, 0),
                                  "c2go.lp.fire");

  // Split out a then-block and emit the call there. The latch terminator is
  // re-rooted onto the merge BB by SplitBlockAndInsertIfThen.
  Instruction *ThenTerm = SplitBlockAndInsertIfThen(
      IsZero, Latch->getTerminator(), /*Unreachable=*/false);
  IRBuilder<> TB(ThenTerm);
  FunctionType *VoidFT = FunctionType::get(Type::getVoidTy(Ctx), false);
  // c2go-libc bridge: `runtime.Gosched` has no //go:linkname push and is
  // ABIInternal-only, so c2go .s cannot target it directly. c2go-libc's
  // `Gosched` is a Go-side thin wrapper marked `//go:linkname Gosched` so
  // an ABI0 entry is emitted. The Plan9 streamer encodes `.`/`/` in this
  // multi-segment path via U+00B7 / U+2215 Unicode escapes (symbolToPlan9
  // path-a), producing
  //   BL github·com∕c2gohq∕c2go_libc·Gosched(SB)
  // which `go tool asm` resolves to the c2go-libc ABI0 wrapper.
  FunctionCallee Gosched = F.getParent()->getOrInsertFunction(
      "github.com/c2gohq/c2go_libc.Gosched", VoidFT);
  // #455(c): the per-injection `enforceGoABI0AndOptLeaf(GoschedF, false)`
  // retrofit was deleted — the pass-end sweep below (run unconditionally
  // on every pass invocation) already retrofits the helper declaration
  // before the matching `enforceCallSiteCC` walks the call sites. The
  // injection-time `setCallingConv(GoABI0)` on the NEW CallInst stays so
  // the pass-end `enforceCallSiteCC` sees a matching CC for this fresh
  // call and does NOT trip the #437 ship-gate's `c2go.cc.violations`
  // counter on a clean release build.
  CallInst *GoschedCall = TB.CreateCall(Gosched, {});
  GoschedCall->setCallingConv(CallingConv::GoABI0);

  // Inserting a real call invalidates any leaf-cc fast-path; drop the attr so
  // a later resolver doesn't keep promoting F as a leaf.
  if (F.hasFnAttribute("c2go-leaf-cc"))
    F.removeFnAttr("c2go-leaf-cc");

  return true;
}

static bool shouldProcess(Function &F) {
  if (F.isDeclaration())
    return false;
  if (!F.getParent()->getModuleFlag(llvm::c2go::kGoabiModuleFlag))
    return false;
  if (!F.hasFnAttribute("c2go-c-name"))
    return false;
  // Boundary symbols are GoABI0 bridges; they're not the c2go-managed body we
  // want to poll inside. (They're also normally tiny shims with a real call.)
  if (F.hasFnAttribute("c2go-boundary"))
    return false;
  return true;
}

PreservedAnalyses C2GoLoopPollPass::run(Module &M, ModuleAnalysisManager &AM) {
  // ClTargetNs == 0 disables the pass (the default — cooperative preemption is
  // opt-in); ClEnableLoopPoll is the legacy on/off knob.
  if (!ClEnableLoopPoll || ClTargetNs == 0)
    return PreservedAnalyses::all();
  if (!M.getModuleFlag(llvm::c2go::kGoabiModuleFlag))
    return PreservedAnalyses::all();

  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  unsigned NInjected = 0;
  for (Function &F : M) {
    if (!shouldProcess(F))
      continue;

    LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
    if (LI.empty())
      continue;
    ScalarEvolution &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
    const TargetTransformInfo &TTI =
        FAM.getResult<TargetIRAnalysis>(F);

    // Outer-most-first walk. For each loop nest we inject AT MOST ONE poll —
    // at the outermost eligible loop. The outermost loop's body subsumes its
    // sub-loops, so its body-cost / TTI naturally accounts for them. Once we
    // inject into an outer loop, every contained sub-loop sees that call in
    // its body too (via hasAnyRealCallInBody, since L.blocks() of a nested
    // loop is a subset of the outer's), but we skip the sub-loops eagerly to
    // avoid double-injection.
    SmallPtrSet<const Loop *, 8> AncestorInjected;
    for (Loop *L : LI.getLoopsInPreorder()) {
      // Skip if any ancestor already got a poll.
      bool Skip = false;
      for (Loop *P = L->getParentLoop(); P; P = P->getParentLoop())
        if (AncestorInjected.count(P)) {
          Skip = true;
          break;
        }
      if (Skip)
        continue;
      if (!L->isLoopSimplifyForm())
        continue;
      if (hasAnyRealCallInBody(*L))
        continue;
      uint64_t M_ = computeM(*L, SE, TTI);
      if (M_ == 0)
        continue;
      if (injectPoll(*L, M_, F)) {
        ++NInjected;
        AncestorInjected.insert(L);
      }
    }
    if (NInjected)
      FAM.invalidate(F, PreservedAnalyses::none());
  }

  // #429 (Wave CD #419 audit Finding #6): pass-end CC sweep on the
  // Gosched helper. Run unconditionally — a pre-existing CallInst
  // (from an earlier pass, IR fixture, or a future rewrite) against
  // the helper could carry the default-C CC; without this sweep a
  // release-tier mismatch slips through silently. Mirrors the
  // C2GoMemcpyTyping / C2GoWriteBarriers / C2GoEscapeCheck defences.
  //
  // Retrofit the helper declaration first so the sweep has a GoABI0
  // declared CC to compare against — when no NoCall loop was injected
  // above, injectPoll never ran and the declaration's CC was never
  // touched.
  //
  // #453: OR the retrofit + sweep returns into Changed so a release
  // mismatch fixed in a no-inject pass run still surfaces as
  // PreservedAnalyses::none().
  bool SweepChanged = false;
  if (auto *GoschedF =
          M.getFunction("github.com/c2gohq/c2go_libc.Gosched"))
    SweepChanged |=
        llvm::c2go::enforceGoABI0AndOptLeaf(GoschedF, /*IsLeaf=*/false);
  SweepChanged |= llvm::c2go::enforceCallSiteCC(
      M.getFunction("github.com/c2gohq/c2go_libc.Gosched"));

  LLVM_DEBUG(dbgs() << "c2go-loop-poll: injected " << NInjected
                    << " poll(s) in " << M.getName() << "\n");
  return (NInjected || SweepChanged) ? PreservedAnalyses::none()
                                     : PreservedAnalyses::all();
}
