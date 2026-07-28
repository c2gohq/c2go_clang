//===- C2GoSafepoint.cpp - Insert llvm.experimental.stackmap at safepoints ===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Walks every function in the module. For each call to a Go-runtime
// safepoint-bearing helper (legacy managed-alloca seed/default list below),
// emits a
// `@llvm.experimental.stackmap(<id>, 0, <managed_alloca>...)` intrinsic
// call immediately before the call site, listing every alloca in the
// current function that carries `!c2go.ptr.managed` metadata as a
// stack-map operand. The LLVM backend then records the (PC, slot) list
// in the `__llvm_stackmaps` section.
//
// Constraints:
//   - Lightweight: no gc.statepoint, no GCStrategy, no addrspace change.
//   - Allocas-as-operand: LLVM's stackmap intrinsic treats an alloca
//     operand as a "Direct" location entry (FP+offset of the slot), which
//     is exactly what the GC walker needs.
//   - Module-monotonic ID: a single counter local to this pass instance.
//     IDs across multiple invocations on the same module are kept
//     non-overlapping by initialising the counter from a module-flag
//     watermark (`c2go.safepoint.next.id`) that the pass updates on exit.
//
// Functions without any `!c2go.ptr.managed` allocas get no stackmap
// emission — a stackmap with zero live pointers carries no useful GC
// information.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoSafepoint.h"

#include "llvm/Transforms/C2Go/C2GoGCMaskUtils.h"
#include "llvm/Transforms/C2Go/C2GoMorestackUtils.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "c2go-safepoint"

namespace {

// Built-in legacy managed-alloca seed/default list of Go-runtime
// safepoint-bearing helpers. Kept as the always-on default so this pass
// stays self-contained for modules that do not declare an explicit list.
//
// Calls to any of these may trigger GC / preemption, so the live managed
// pointers must be discoverable from the stack at the call PC.
//
// #420: this set used to be the SOLE source of truth, with the in-source
// comment self-flagging `runtime.typedmemmove` as "kept for completeness,
// may drift". Modules can now extend the set via the named metadata
// `c2go.safepoint.callees` (see `getSafepointCallees(Module &)` below), so
// downstream tools (c2go-lto, ad-hoc opt invocations, test fixtures) can
// add helpers without recompiling this pass.
//
// #463: the canonical list now lives in
// `llvm/Transforms/C2Go/C2GoProtocol.h` so the clang frontend's stamping
// site (CodeGenModule.cpp) and this pass cannot drift textually. The
// returned `StringSet<>` is materialised once at first call; the textual
// payload is sourced from the protocol header's `ArrayRef`.
//
// Per-entry rationale (kept from the pre-#463 in-source comments):
//   * runtime.mallocgc           — heap allocator, may run GC.
//   * runtime.typedmemmove       — //go:nosplit upstream; kept for
//                                  completeness; see #465 follow-up.
//   * _c2go_typedMemmoveArray    — ARRAY 变体与 singleton 同为
//                                  gc-leaf-function + //go:nosplit;RS4GC 会
//                                  bypass 该调用;保留在该名单仅为 legacy
//                                  C2GoSafepoint managed-alloca 路径双保险;
//                                  后续 #465 探讨是否去除。
//   * runtime.gcWriteBarrier     — barrier slow-path; nosplit leaf;
//                                  see #465 follow-up.
//   * runtime.morestack          — switches to g0, scans current frame.
//   * runtime.newproc            — goroutine spawn safepoint.
//   * runtime.gopanic            — unwind, GC may run during recovery.
//   * runtime.systemstack        — runs `fn` on g0, full safepoint.
//
// #214 removed `_c2go_union_write_barrier` / `_c2go_write_barrier`
// entries that were added by Round 2 P0 B. The barrier emit itself is
// gone (CGExpr.cpp). If §A2 (task #215) introduces a new barrier
// function name it must add itself to C2GoProtocol.h.
static const StringSet<> &getBuiltinSafepointCallees() {
  static const StringSet<> S = [] {
    StringSet<> Init;
    for (StringRef N : llvm::c2go::getBuiltinSafepointCalleeNames())
      Init.insert(N);
    return Init;
  }();
  return S;
}

// Named metadata key used to extend the legacy managed-alloca seed list
// from outside this pass. Each operand is an `!{!"symbol"}` MDNode listing
// one additional callee name. Read at the start of every `run()`; missing /
// empty metadata is treated as "no extras" so behaviour matches the
// built-in default verbatim. (#288 per-call-site stackmap path does not
// depend on this list — it covers every non-no-op call regardless.)
//
// #420: replaces the stale "may drift" comment on the built-in list — the
// frontend (CodeGenModule) now writes the explicit set into the .bc so the
// .bc is self-describing and downstream tools can amend it without
// patching this file. #463: the spelling lives in C2GoProtocol.h alongside
// the legacy default list.
static constexpr StringRef kSafepointCalleesMD =
    llvm::c2go::kSafepointCalleesMDName;

// getSafepointCallees returns the per-module effective legacy seed list:
// the always-on built-in set unioned with any names supplied via the
// `c2go.safepoint.callees` named metadata. Module-supplied names that
// duplicate a built-in entry are silently absorbed (the StringSet dedup
// handles it).
static StringSet<> getSafepointCallees(const Module &M) {
  StringSet<> Result = getBuiltinSafepointCallees();
  const NamedMDNode *NMD = M.getNamedMetadata(kSafepointCalleesMD);
  if (!NMD)
    return Result;
  for (const MDNode *Op : NMD->operands()) {
    if (Op->getNumOperands() == 0)
      continue;
    if (const auto *S = dyn_cast<MDString>(Op->getOperand(0).get()))
      if (!S->getString().empty())
        Result.insert(S->getString());
  }
  return Result;
}

// Module-flag name used to persist the next-stackmap-ID watermark
// across multiple invocations of this pass on the same module (e.g.
// LTO pipelines that re-run module passes).
static constexpr StringRef kNextIdFlag = "c2go.safepoint.next.id";

// readNextIdWatermark returns the value of the module-flag watermark,
// defaulting to 0 when absent.
static uint64_t readNextIdWatermark(Module &M) {
  if (auto *MD = M.getModuleFlag(kNextIdFlag))
    if (auto *CAM = dyn_cast<ConstantAsMetadata>(MD))
      if (auto *CI = dyn_cast<ConstantInt>(CAM->getValue()))
        return CI->getZExtValue();
  return 0;
}

// writeNextIdWatermark records the next-ID value so future runs on the
// same module continue the sequence without collision.
//
// The flag uses Module::Max (not Override) so that per-TU bitcode can be
// merged by c2go-lto (the unified/whole-library build path): each TU carries
// its own watermark, and taking the max is exactly the right merged value —
// a re-run of the safepoint pass on the combined module then starts above
// every existing ID. (Override would demand identical watermarks across TUs,
// which never holds for differently-sized translation units, so it aborted
// the link.) The per-TU stackmap IDs themselves may repeat across TUs in the
// merged module, but that is benign: the pass does not re-run inside c2go-lto,
// and each ID lowers to that function's own per-frame FUNCDATA/PCDATA, never a
// module-global key.
static void writeNextIdWatermark(Module &M, uint64_t Next) {
  // Module flags are write-once; if it already exists we must rewrite the
  // metadata operand in place.
  NamedMDNode *MFs = M.getModuleFlagsMetadata();
  if (MFs) {
    for (MDNode *Op : MFs->operands()) {
      if (Op->getNumOperands() < 3)
        continue;
      auto *Name = dyn_cast<MDString>(Op->getOperand(1).get());
      if (!Name || Name->getString() != kNextIdFlag)
        continue;
      LLVMContext &Ctx = M.getContext();
      Op->replaceOperandWith(
          2, ConstantAsMetadata::get(
                 ConstantInt::get(Type::getInt64Ty(Ctx), Next)));
      return;
    }
  }
  M.addModuleFlag(Module::Max, kNextIdFlag, Next);
}

// collectManagedAllocas scans the entry block (where clang puts its
// alloca prelude) for instructions carrying `!c2go.ptr.managed`
// metadata. We deliberately limit the scan to the entry block because
// that's where CGDecl emits the tag — dynamic / inline allocas would not
// have a stable home frame slot anyway.
static void collectManagedAllocas(Function &F,
                                  SmallVectorImpl<AllocaInst *> &Out) {
  if (F.empty())
    return;
  for (Instruction &I : F.getEntryBlock()) {
    auto *AI = dyn_cast<AllocaInst>(&I);
    if (!AI)
      continue;
    if (AI->getMetadata(llvm::c2go::kPtrManagedMD))
      Out.push_back(AI);
  }
}

// #287 (Option 3): collect every pointer-typed local slot — scalar pointer
// locals AND parameter `<arg>.addr` spill slots (matched on allocated type, so
// the clang `c2go.ptr.slot` tag is not required; params are not tagged). These
// hold UNMANAGED C pointers that may point into the movable Go stack, so
// copystack must relocate them. Marking a pointer slot is always sound:
// copystack range-checks each value and skips non-stack (heap/NULL/global)
// pointers. Entry-block only. (Pointer FIELDS of aggregate locals are a
// separate, backend-computed follow-up — see collectPointerFieldOffsets.)
static void collectPtrSlotAllocas(Function &F,
                                  SmallVectorImpl<AllocaInst *> &Out) {
  if (F.empty())
    return;
  for (Instruction &I : F.getEntryBlock()) {
    auto *AI = dyn_cast<AllocaInst>(&I);
    if (!AI)
      continue;
    // #665: FUNCTION-pointer slots (kFnPtrAddrSpace) never enter the tracked
    // set — their values are code addresses or POSIX sentinel integers, and a
    // marked slot holding SIG_IGN==1 makes copystack throw. (clang already
    // stopped tagging them with kPtrSlotMD in #654c-b; this covers the bare
    // isPointerTy() arm for the new address space.)
    if (AI->getAllocatedType()->isPointerTy() &&
        AI->getAllocatedType()->getPointerAddressSpace() ==
            c2go::kFnPtrAddrSpace)
      continue;
    if (AI->getAllocatedType()->isPointerTy() ||
        AI->getMetadata(c2go::kPtrSlotMD))
      Out.push_back(AI);
  }
}

// isSafepointCall returns true when the call targets one of the legacy
// managed-alloca seed Go-runtime safepoint helpers (matched by callee
// name). Indirect / inline-asm calls and calls without a named callee are
// not considered safepoints by this pass. #420: the seed list is now a
// per-module snapshot taken once per `run()` and threaded in here so the
// `c2go.safepoint.callees` named metadata can extend it.
static bool isSafepointCall(CallBase *CB, const StringSet<> &SeedList) {
  Function *Callee = CB->getCalledFunction();
  if (!Callee)
    return false;
  StringRef Name = Callee->getName();
  if (Name.empty())
    return false;
  return SeedList.contains(Name);
}

// emitStackmapBefore inserts a `@llvm.experimental.stackmap` call
// immediately before `CB`. Returns the freshly-emitted CallInst*.

// #287/#288 (Option 3): collect the byte offsets of every pointer-typed field
// within an aggregate (struct/array) type. Used HERE to decide whether a
// struct/array stack local needs zero-init (it holds pointer fields that the
// locals map will scan — e.g. SQLite's `yyParser sEngine` whose `.pParse`
// holds `&sParse`). The matching locals-map BITS are emitted in the BACKEND
// (AArch64FrameLowering) from MachineFrameInfo + the alloca type — NOT via
// clang-IR GEP-per-field stackmap operands (that exhausts the register
// allocator at -O0).
//
// #432: thin shim over the shared `c2go::walkPointerFields` helper. The
// recursive walk previously duplicated here / in C2GoGCSetup / in
// AArch64/C2GoFrameEmitter has been lifted to C2GoGCMaskUtils.h so the three
// sites cannot silently drift on the same aggregate.
static void collectPointerFieldOffsets(Type *Ty, uint64_t Base,
                                       const DataLayout &DL,
                                       SmallVectorImpl<uint64_t> &Out) {
  c2go::walkPointerFields(Ty, Base, DL,
                          [&](uint64_t Off) { Out.push_back(Off); });
}

static CallInst *emitStackmapBefore(CallBase *CB, uint64_t Id,
                                    ArrayRef<AllocaInst *> ManagedAllocas,
                                    Function *StackmapFn) {
  IRBuilder<> B(CB);
  SmallVector<Value *, 16> Args;
  // operand 0: ID — i64 immediate. The intrinsic requires ImmArg.
  Args.push_back(B.getInt64(Id));
  // operand 1: shadow byte count — 0 for our use; we do not need to
  // reserve a code-region after the stackmap.
  Args.push_back(B.getInt32(0));
  // operand 2..n: live values to record. Allocas are emitted as Direct
  // (FP+offset) entries by the stackmap lowering — exactly the form the
  // post-process tool expects.
  for (AllocaInst *AI : ManagedAllocas)
    Args.push_back(AI);
  return B.CreateCall(StackmapFn, Args);
}

// #288 (Option A): is `CB` a call that may trigger morestack / copystack?
// ANY ordinary call (libc malloc, runtime helper, indirect call, …) may grow
// the goroutine stack and relocate it, so the locals pointer map must be valid
// at EVERY call's return PC — not just the legacy managed-alloca seed list
// (SQLite reaches morestack through libc malloc, never runtime.mallocgc).
//
// Excluded (#436, via c2go::mayReachMorestack): our own stackmap intrinsic
// (no PC of its own / not a real call), side-effect-free debug / lifetime /
// pseudo intrinsics that lower to nothing, and InlineAsm CallBases. LLVM uses
// CallBase to model inline asm operands and effects, but c2go's supported
// inline-asm contract is call-free, so the asm node itself cannot reach a
// callee morestack prologue. The shared predicate keeps the stackmap, RS4GC,
// and loop-poll paths in lockstep.
static bool isPotentialMorestackCall(CallBase *CB) {
  return c2go::mayReachMorestack(*CB);
}

// #307 — idempotency guard. C2GoSafepointPass runs TWICE in c2go-mode: once
// at PipelineStart (before inlining) and once at OptimizerLast (after the
// inliner has spliced callee bodies — and their already-emitted stackmap
// intrinsics — into callers). On the second run we must ADD coverage for
// genuinely-uncovered call sites while NOT re-emitting a stackmap in front of
// a site the first run already instrumented.
//
// `emitStackmapBefore` inserts the `llvm.experimental.stackmap` call
// IMMEDIATELY before `CB`, with nothing in between. So a site is "already
// covered" exactly when `CB`'s immediate predecessor is that intrinsic.
// Inlining preserves this adjacency (it splices the callee's stackmap and the
// call it anchors as a contiguous block), so the direct-predecessor test is a
// precise check for run-1 output and never matches a site we have not yet
// instrumented. Mirrors the `Intrinsic::experimental_stackmap` exclusion in
// `isPotentialMorestackCall`.
//
// First-run effect: on the first run no site has a preceding stackmap, so this
// always returns false and emission is byte-identical to today.
//
// #420 IR-enforce: the adjacency invariant alone leaves room for a HUMAN-
// inserted stackmap intrinsic (or one synthesized by an unrelated pass) to be
// mis-detected as "ours". We now also validate that the preceding stackmap's
// ID operand is strictly LESS than the current-run starting watermark — only
// IDs emitted by a previous pass invocation can satisfy that, because every
// stackmap we emit in THIS run is assigned an ID ≥ watermark. A stackmap
// authored externally (test fixtures, hand-written IR) carries an arbitrary ID
// that almost always violates this contract, so we treat it as "not ours" and
// proceed to emit our own stackmap; the resulting IR is still well-formed
// (two adjacent stackmaps record the same PC twice, harmless).
//
// #462 watermark-radius: bare `getPrevNode()` only matches if the stackmap is
// LITERALLY the previous instruction. Between run 1 and run 2 the inliner /
// SROA / lifetime-marker emission can splice `llvm.lifetime.{start,end}` or
// `llvm.dbg.*` between the prior-run stackmap and its anchor call, breaking
// pure adjacency and causing a re-emission at run 2 (two stackmaps for the
// same PC). We extend the search radius backward past debug-only and
// lifetime-marker intrinsics — neither has runtime semantics that could
// invalidate the "stackmap-immediately-before-CB" invariant the backend cares
// about — and stop at the first real instruction. If that first real
// instruction is the prior-run stackmap, we treat the site as covered.
//
// #467: the LLVM inliner emits `llvm.experimental.noalias.scope.decl` at the
// inlined call's insertion point (InlineFunction.cpp ~96-100/1167-1172) to
// announce alias-scope identities for the cloned body. These intrinsics carry
// only metadata (no runtime effect, no PC), and the inliner can interleave
// them between the prior-run stackmap and its anchor call exactly the same way
// it interleaves lifetime markers. They must be transparent to the watermark
// walk, otherwise the second run double-stamps the site. We extend the named
// skip list to cover this case. We deliberately do NOT skip
// `assume` / `fake_use` / `launder` / `strip` — those have semantics that the
// backend or other passes are permitted to observe.
static bool isAlreadyStackmapped(CallBase *CB, uint64_t Watermark) {
  Instruction *Cursor = CB->getPrevNode();
  while (Cursor) {
    if (Cursor->isDebugOrPseudoInst() || Cursor->isLifetimeStartOrEnd()) {
      Cursor = Cursor->getPrevNode();
      continue;
    }
    if (auto *II = dyn_cast<IntrinsicInst>(Cursor)) {
      if (II->getIntrinsicID() == Intrinsic::experimental_noalias_scope_decl) {
        Cursor = Cursor->getPrevNode();
        continue;
      }
    }
    break;
  }
  auto *Prev = dyn_cast_or_null<IntrinsicInst>(Cursor);
  if (!Prev || Prev->getIntrinsicID() != Intrinsic::experimental_stackmap)
    return false;
  // operand 0 is the stackmap ID — i64 ImmArg. A run-1 emission always uses
  // IDs in [prior-watermark, watermark), so any well-formed prior-run stackmap
  // has `ID < watermark`. An ID ≥ watermark indicates the stackmap is NOT one
  // we previously emitted (external / synthesized) and must not suppress our
  // own emission at this site.
  auto *IdC = dyn_cast<ConstantInt>(Prev->getArgOperand(0));
  if (!IdC)
    return false;
  return IdC->getZExtValue() < Watermark;
}

// #307 — zero-init idempotency marker. The first run emits a `store ptr null,
// %AI` for each scalar pointer slot and a `memset(%AI, 0, …)` for each
// aggregate with pointer fields, in the function's entry block, and tags each
// with this metadata kind. On the second run the inliner has spliced inlined
// callees' allocas — together with the (tagged) zero-inits the first run
// already emitted for them — into the caller's entry block, so (re-)emitting
// would DOUBLE the inits.
//
// We key idempotency off OUR OWN metadata tag (not "any null store to the
// slot"), so a user-source `p = NULL;` store in the entry block never counts
// as already-inited. That keeps first-run IR byte-identical (no run-1 site is
// tagged yet → every slot is initialized exactly as today) while letting the
// second run precisely skip only the slots we previously initialized.
static constexpr StringRef kZeroInitTag = "c2go.zeroinit";

// markZeroInit tags an init instruction so a later run recognises it as ours.
static void markZeroInit(Instruction *I) {
  I->setMetadata(kZeroInitTag,
                 MDNode::get(I->getContext(), {}));
}

// #311 — strip `llvm.lifetime.start/end` markers off a tracked alloca,
// returning true if any were removed.
//
// Why: the entry zero-init we emit for a pointer-bearing aggregate (above) is
// placed at function ENTRY, but after inlining the inlined aggregate carries
// its own lifetime.start LATER in the body (e.g. SQLite's Walker `%w.i`:
// lifetime.start is hundreds of instructions past entry). A memset that writes
// the slot BEFORE its lifetime.start is, by the lifetime contract, a write to
// dead memory — so StackColoring / DSE legitimately DELETE it, and the slot
// reaches copystack holding stale garbage while the backend's agg mask still
// marks its pointer fields → "bad pointer in frame ... 0x80" (the #311 crash).
//
// The backend agg mask marks an aggregate's pointer fields at EVERY body PC
// (unconditionally), so the slot must be valid (zeroed-then-used) for the whole
// body anyway — there is no liveness window to exploit. Dropping the lifetime
// markers makes the slot whole-function-live, which (a) keeps the entry
// zero-init valid so it survives to the prologue, and (b) stops StackColoring
// from reusing the slot for an unrelated non-pointer local (consistent with the
// #305 pointer/non-pointer no-merge rule). Applied to c2go-tracked aggregates
// and (#654c) to address-captured SCALAR pointer slots — those are forced
// whole-function live in the per-PC stackmaps, so their slot must likewise
// never be recycled for a non-pointer. #312 tracks the per-PC-field-liveness
// root fix that would restore slot reuse.
static bool stripAllocaLifetimes(AllocaInst *AI) {
  SmallVector<IntrinsicInst *, 4> ToErase;
  for (User *U : AI->users()) {
    if (auto *II = dyn_cast<IntrinsicInst>(U)) {
      Intrinsic::ID Id = II->getIntrinsicID();
      if (Id == Intrinsic::lifetime_start || Id == Intrinsic::lifetime_end)
        ToErase.push_back(II);
    }
  }
  for (IntrinsicInst *II : ToErase)
    II->eraseFromParent();
  return !ToErase.empty();
}

// hasEntryZeroInit reports whether `AI`'s entry block already holds a c2go
// zero-init (tagged) store/memset to `AI` from a prior run.
static bool hasEntryZeroInit(AllocaInst *AI) {
  BasicBlock &Entry = AI->getFunction()->getEntryBlock();
  for (Instruction &I : Entry) {
    if (!I.getMetadata(kZeroInitTag))
      continue;
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (SI->getPointerOperand() == AI)
        return true;
    } else if (auto *MS = dyn_cast<MemSetInst>(&I)) {
      if (MS->getRawDest() == AI)
        return true;
    }
  }
  return false;
}

// #288 (Option A) — per-function backward liveness of pointer-slot allocas.
//
// Goal: for each potential-morestack call, know which tracked allocas hold a
// value that may still be READ afterwards. A *dead* alloca (value never read
// again) whose slot stack-coloring later reuses for a non-pointer (the 0x7e
// bug) must NOT be listed at that PC; a *live* one MUST (copystack relocates
// stack-resident pointers).
//
// This is textbook backward liveness over the CFG, done ONCE per function
// (O(insts × slots)) — NOT a per-(alloca, site) reachability/MemorySSA query
// (that OOM'd at 16 GB on SQLite, see BackendUtil.cpp:1224).
//
// gen (USE):  a load through the alloca address, or the alloca-derived pointer
//             escaping as a value (store-as-value / call arg / ret / cmp / …) —
//             i.e. anything that can OBSERVE the slot's current contents.
// kill (DEF): a *direct, full-width* `store <ptr>, %alloca` that provably
//             overwrites the whole slot. Conservative: only a store straight to
//             the alloca base kills; stores through GEP/bitcast/select/phi do
//             NOT kill (they may be partial / aliased). Never killing a still-
//             readable pointer is the safe direction.
//
// We track each alloca by an index into `Allocas`. `Idx` maps an alloca to its
// bit position. The result is `LiveBefore[CB]`: the bitset of allocas live at
// the program point immediately BEFORE the call (the program point the
// stackmap/return PC observes).
struct AllocaLiveness {
  // For each tracked alloca, the set of load/escape "use" instructions and the
  // set of full-overwrite "kill" instructions. Classification is done up front
  // by walking each alloca's transitive ptr users once.
  DenseMap<Instruction *, BitVector> Gen;  // per-inst: allocas used here
  DenseMap<Instruction *, BitVector> Kill; // per-inst: allocas fully overwritten

  // LiveBefore[CB] = allocas live immediately before CB.
  DenseMap<CallBase *, BitVector> LiveBefore;
};

// Classify one alloca's effect on each instruction (gen/kill), recording into
// the per-instruction bitsets at bit `BitIdx`.
static void classifyAllocaEffects(AllocaInst *A, unsigned BitIdx, unsigned NBits,
                                  AllocaLiveness &L) {
  // Walk transitive pointer-deriving users. `DirectToBase` tracks whether a
  // value is the alloca base itself (so a store to it is a full-width kill);
  // values reached through GEP/bitcast/etc. are NOT the base.
  SmallVector<std::pair<Value *, bool>, 16> Work;
  SmallPtrSet<Value *, 16> Seen;
  Work.push_back({A, true});
  Seen.insert(A);
  auto setBit = [&](DenseMap<Instruction *, BitVector> &Map, Instruction *I) {
    auto It = Map.try_emplace(I, NBits).first;
    It->second.set(BitIdx);
  };
  while (!Work.empty()) {
    auto [V, IsBase] = Work.pop_back_val();
    for (User *U : V->users()) {
      auto *I = dyn_cast<Instruction>(U);
      if (!I)
        continue;
      switch (I->getOpcode()) {
      case Instruction::BitCast:
      case Instruction::AddrSpaceCast:
      case Instruction::GetElementPtr:
      case Instruction::PHI:
      case Instruction::Select:
        // Pointer-deriving: chase, but the derived value is no longer the
        // base, so a later store through it cannot be proven full-width.
        if (Seen.insert(I).second)
          Work.push_back({I, false});
        // A derived pointer feeding a PHI/Select also *escapes* if that
        // PHI/Select value is then used as a value elsewhere — covered when
        // we visit those users. No gen here for the derive itself.
        break;
      case Instruction::Load:
        // Reading the slot's contents observes its current value → USE.
        setBit(L.Gen, I);
        break;
      case Instruction::Store: {
        auto *SI = cast<StoreInst>(I);
        if (SI->getValueOperand() == V) {
          // The alloca-derived POINTER is being stashed elsewhere (escape):
          // a future load through that other location can observe it → USE.
          // NOTE (#654c): this gen only extends liveness BACKWARD (toward
          // entry); the "future load" needs coverage at every PC AFTER the
          // escape too, which backward dataflow cannot express. That forward
          // half is handled in run(): captured slots (this store makes the
          // slot captured) are forced live at every site via CapturedSlots.
          setBit(L.Gen, I);
        } else if (IsBase && SI->getValueOperand()->getType()->isPointerTy()) {
          // store <ptr>, %alloca — direct, full pointer-width overwrite of the
          // whole slot → KILL. (Only when the destination is the alloca base
          // itself; partial/aliased stores fall through and do NOT kill.)
          setBit(L.Kill, I);
        }
        // store of a non-pointer to the base, or store through a derived ptr:
        // not a kill (conservative) and not a use of this alloca's value.
        break;
      }
      default:
        // call / invoke / ret / cmp / ptrtoint / atomic / … all may observe
        // the slot's address or contents → USE (conservative).
        setBit(L.Gen, I);
        break;
      }
    }
  }
}

// Run backward liveness and populate L.LiveBefore for every safepoint call.
static void computeAllocaLiveness(Function &F,
                                  ArrayRef<AllocaInst *> Allocas,
                                  ArrayRef<CallBase *> Sites,
                                  AllocaLiveness &L) {
  unsigned NBits = Allocas.size();
  for (unsigned I = 0; I < NBits; ++I)
    classifyAllocaEffects(Allocas[I], I, NBits, L);

  // Standard backward dataflow at basic-block granularity:
  //   LiveOut[BB] = ∪ LiveIn[succ]
  //   LiveIn[BB]  = transfer backward through BB's instructions.
  // Iterate to fixpoint (loops). Block count is small per function.
  DenseMap<BasicBlock *, BitVector> LiveIn, LiveOut;
  for (BasicBlock &BB : F) {
    LiveIn.try_emplace(&BB, NBits);
    LiveOut.try_emplace(&BB, NBits);
  }
  bool Changed = true;
  // Process in reverse so most updates propagate in one sweep.
  SmallVector<BasicBlock *, 32> RPO;
  for (BasicBlock &BB : F)
    RPO.push_back(&BB);
  while (Changed) {
    Changed = false;
    for (BasicBlock *BB : reverse(RPO)) {
      BitVector Out(NBits);
      for (BasicBlock *Succ : successors(BB))
        Out |= LiveIn[Succ];
      // Transfer backward through the block.
      BitVector Cur = Out;
      for (Instruction &I : reverse(*BB)) {
        // kill before gen within the same instruction: a `store ptr,%a` both
        // kills and is not a use; a load is a gen. Apply kill then gen so a
        // self-store (impossible here) would still mark live.
        auto KIt = L.Kill.find(&I);
        if (KIt != L.Kill.end())
          Cur.reset(KIt->second);
        auto GIt = L.Gen.find(&I);
        if (GIt != L.Gen.end())
          Cur |= GIt->second;
      }
      BitVector &InRef = LiveIn[BB];
      if (Cur != InRef) {
        InRef = std::move(Cur);
        Changed = true;
      }
      LiveOut[BB] = std::move(Out);
    }
  }

  // Second pass: walk each block backward again, snapshotting the live set at
  // the program point immediately BEFORE each safepoint call. Pre-size every
  // site's entry so a caller's `.test()` is always in-bounds.
  SmallPtrSet<CallBase *, 32> SiteSet(Sites.begin(), Sites.end());
  for (CallBase *CB : Sites)
    L.LiveBefore.try_emplace(CB, NBits);
  for (BasicBlock &BB : F) {
    BitVector Cur(NBits);
    for (BasicBlock *Succ : successors(&BB))
      Cur |= LiveIn[Succ];
    for (Instruction &I : reverse(BB)) {
      auto KIt = L.Kill.find(&I);
      if (KIt != L.Kill.end())
        Cur.reset(KIt->second);
      auto GIt = L.Gen.find(&I);
      if (GIt != L.Gen.end())
        Cur |= GIt->second;
      // After applying this instruction's transfer, `Cur` is the live set
      // BEFORE `I`. That is exactly what the call's return PC observes.
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (SiteSet.count(CB))
          L.LiveBefore[CB] = Cur;
    }
  }
}

} // namespace

// #221 — MemorySSA-backed liveness query.
//
// For each managed alloca A and safepoint S, A is "live at S" iff some
// LOAD (or address-escaping use) of A's memory is reachable from S
// without an intervening store that completely clobbers A. This is
// exactly the question MemorySSA answers: each load is a MemoryUse
// whose `defining access` is the most recent MemoryDef that may have
// written the loaded bytes.
//
//   Query: "Is alloca A's value at safepoint S observable later?"
//   Answer: ∃ MemoryUse U of A in F such that:
//     1. U's CFG position is reachable from S (CFG forward-reach), AND
//     2. The walker's clobbering access for U is on a path that S
//        passes through (i.e., S could *be* that clobbering def, or
//        precedes it without a redefining store).
//
// In practice (1) alone over-approximates conservatively (it's the
// previous forward-reach algorithm), and (2) adds precision by
// removing allocas whose value is provably re-written between S and
// every reachable use. For c2go-mode where managed allocas hold GC
// pointers, the over-approximation is correct (an extra live entry
// causes a redundant scan but not a GC bug), so this pass falls back
// to (1) when MemorySSA is unavailable. With MemorySSA we get the
// tighter set out-of-the-box, integrated with the rest of the LLVM
// optimisation pipeline's cache invalidation.
static bool isAllocaLiveAtSafepointViaMSSA(
    AllocaInst *A, CallBase *S, MemorySSA &MSSA, DominatorTree &DT) {
  MemorySSAWalker *Walker = MSSA.getWalker();
  // Iterate all memory-accessing users of A (transitively through GEP
  // / bitcast). A user is "live at S" if its CFG position is reachable
  // from S AND the walker thinks A's value at S could survive to it.
  SmallVector<Value *, 8> Work{A};
  SmallPtrSet<Value *, 8> Seen{A};
  while (!Work.empty()) {
    Value *V = Work.pop_back_val();
    for (User *U : V->users()) {
      auto *I = dyn_cast<Instruction>(U);
      if (!I)
        continue;
      switch (I->getOpcode()) {
      case Instruction::BitCast:
      case Instruction::AddrSpaceCast:
      case Instruction::GetElementPtr:
      case Instruction::PHI:
      case Instruction::Select:
        if (Seen.insert(I).second)
          Work.push_back(I);
        continue;
      case Instruction::Store:
        // Store-into-alloca: only counts as a use of A's value if
        // alloca-derived ptr is the VALUE operand (escape).
        if (cast<StoreInst>(I)->getValueOperand() != V)
          continue;
        break;
      default:
        break;
      }
      // CFG reachability check: is I downstream of S?
      if (I->getParent() == S->getParent()) {
        // Same BB: I must come after S in instruction order.
        bool After = false;
        for (Instruction *Cur = S->getNextNode(); Cur;
             Cur = Cur->getNextNode()) {
          if (Cur == I) {
            After = true;
            break;
          }
        }
        if (!After)
          continue;
      } else if (!isPotentiallyReachable(S->getParent(), I->getParent(),
                                          /*ExclusionSet=*/nullptr, &DT)) {
        continue;
      }
      // I is reachable from S. Now check the MemorySSA refinement:
      // if I is a LOAD, ask MemorySSA whether S's defining access
      // (or earlier) could be I's clobbering def. If so, A's value
      // at S can flow to I → live.
      if (auto *LI = dyn_cast<LoadInst>(I)) {
        MemoryAccess *Clobber = Walker->getClobberingMemoryAccess(LI);
        MemoryAccess *MaS = MSSA.getMemoryAccess(S);
        if (!MaS || !Clobber) {
          // No MemorySSA info — fall back to "reachable → live".
          return true;
        }
        // Live iff S's memory state dominates the clobber, meaning
        // the clobber could be S itself or something between S and
        // the load. MemorySSA's MemoryDef/Phi form a SSA chain;
        // dominance checks correspond to reaching-def queries.
        if (MSSA.dominates(MaS, Clobber))
          return true;
        // Clobber is independent of S → S's value already overwritten
        // before the load. Not live via this load.
        continue;
      }
      // Non-load user (call / invoke / ret / store-escape / etc.):
      // these may observe A's address or contents in an opaque way
      // — treat as live conservatively. MemorySSA doesn't model
      // call-side memory effects precisely enough to refine these.
      return true;
    }
  }
  return false;
}

PreservedAnalyses C2GoSafepointPass::run(Module &M, ModuleAnalysisManager &MAM) {
  LLVM_DEBUG(dbgs() << "c2go-safepoint: scanning " << M.getName() << "\n");

  // Resolve the intrinsic declaration once; getOrInsertDeclaration
  // creates it lazily on first use.
  Function *StackmapFn = Intrinsic::getOrInsertDeclaration(
      &M, Intrinsic::experimental_stackmap);

  uint64_t NextId = readNextIdWatermark(M);
  // #420: capture the run-start watermark so the per-site idempotency check
  // can distinguish OUR prior-run stackmaps (ID < Watermark) from external
  // / human-authored intrinsics (any other ID).
  const uint64_t StartWatermark = NextId;
  // #420: snapshot the effective callee seed list (built-in + named-metadata
  // extensions) once per `run()`; used by the legacy managed-alloca path.
  const StringSet<> SeedList = getSafepointCallees(M);
  bool Changed = false;
  unsigned EmittedSites = 0;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    // Skip the function entirely when nothing in the frame is managed —
    // a stackmap with zero live operands records nothing useful.
    SmallVector<AllocaInst *, 8> ManagedAllocas;
    collectManagedAllocas(F, ManagedAllocas);

    // #288 (Option A): the locals pointer map is now PER-CALL-SITE, not one
    // static full mask. Entry bitmap 0 stays EMPTY (the entry morestack PC,
    // locals still uninitialized via PCDATA $1,$-1 → entry-0 fallback). At each
    // call site we emit a stackmap listing only the pointer-slot allocas that
    // are LIVE at that PC (their value may still be read), so stack-coloring is
    // free to reuse a *dead* slot for a non-pointer without it being scanned as
    // a root (the -O2 `bad pointer 0x7e` bug). copystack can fire at ANY
    // callee's morestack, so we cover EVERY call (not the old runtime-only
    // seed list — SQLite reaches morestack through libc malloc).
    //
    // Two slot kinds:
    //   (1) SCALAR pointer allocas (`T* p` locals + param `<arg>.addr` spills):
    //       zero-inited at entry + listed in per-call-site stackmaps when live.
    //   (2) Pointer FIELDS inside AGGREGATE (struct/array) stack locals (e.g.
    //       SQLite's `yyParser sEngine.pParse = &sParse`): their locals-map
    //       bits are computed in the BACKEND (AArch64FrameLowering, from
    //       MachineFrameInfo + the alloca type) and OR'd by MCPlan9AsmStreamer
    //       into every body bitmap (index >= 1) — NOT via stackmap operands
    //       (GEP-per-field exhausts the register allocator). Here we only
    //       ZERO-INIT such aggregates so an unset pointer field reads NULL at a
    //       body call before the C code initializes the struct.
    SmallVector<AllocaInst *, 16> SlotAllocas;
    collectPtrSlotAllocas(F, SlotAllocas);
    const DataLayout &DL = M.getDataLayout();
    SmallVector<AllocaInst *, 8> AggAllocas;
    for (Instruction &I : F.getEntryBlock()) {
      auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI || !AI->getAllocatedType()->isAggregateType())
        continue;
      SmallVector<uint64_t, 8> Offs;
      collectPointerFieldOffsets(AI->getAllocatedType(), 0, DL, Offs);
      if (!Offs.empty())
        AggAllocas.push_back(AI);
    }
    if (!SlotAllocas.empty() || !AggAllocas.empty()) {
      // #311: drop each tracked aggregate's lifetime markers FIRST — before
      // computing the zero-init insertion point — so the entry zero-init is not
      // treated as a write to dead memory (which DSE/StackColoring would delete;
      // see stripAllocaLifetimes). Done every run (the inliner re-introduces
      // markers on the second run's freshly-inlined copies).
      //   #314: this strip MUST precede the IP computation below. A
      // `llvm.lifetime.start/end` intrinsic can be the first non-alloca entry
      // instruction (the inliner hoists inlined-callee lifetime markers up at
      // -O2); if IP were computed first it could point AT a marker that this
      // strip then erases, leaving the IRBuilder insertion point dangling and
      // crashing the next CreateMemSet (an insertBefore on a freed iterator).
      // Stripping first guarantees IP lands on a live instruction. (Previously
      // masked because the removed #314 aggregate stackmap operands kept the
      // markers from being hoisted to the entry head.)
      for (AllocaInst *AI : AggAllocas)
        if (stripAllocaLifetimes(AI))
          Changed = true;
      // #654c: a pointer slot whose ADDRESS is captured (stored out as a
      // value / handed to a callee that may stash it) can be read later
      // through pointer chains the backward AllocaLiveness below cannot see —
      // its escape handling is a one-off `gen`, which only protects PCs
      // BEFORE the escape, while the promised "future load through that other
      // location" needs every PC AFTER it. Force captured slots live at every
      // site (their entry null-init makes the pre-store window read NULL,
      // which copystack skips) and strip their lifetime markers — same
      // #305/#311 no-recycle rationale as aggregates, and same #314 ordering
      // constraint: strip BEFORE the zero-init IP is computed below.
      BitVector CapturedSlots(SlotAllocas.size());
      for (unsigned I = 0, N = SlotAllocas.size(); I < N; ++I)
        if (c2go::isAllocaAddressCaptured(SlotAllocas[I])) {
          CapturedSlots.set(I);
          if (stripAllocaLifetimes(SlotAllocas[I]))
            Changed = true;
        }
      Instruction *IP = &*F.getEntryBlock().getFirstInsertionPt();
      while (IP && isa<AllocaInst>(IP))
        IP = IP->getNextNode();
      if (IP) {
        IRBuilder<> ZB(IP);
        for (AllocaInst *AI : SlotAllocas) {
          // #307: skip slots the prior run already zero-inited (incl. those
          // inlined together with their run-1 init) so we don't double-init.
          if (hasEntryZeroInit(AI))
            continue;
          markZeroInit(ZB.CreateStore(
              ConstantPointerNull::get(cast<PointerType>(AI->getAllocatedType())),
              AI));
          Changed = true;
        }
        for (AllocaInst *AI : AggAllocas) {
          // #307: skip slots the prior run already zero-inited (incl. those
          // inlined together with their run-1 init) so we don't double-init.
          if (hasEntryZeroInit(AI))
            continue;
          markZeroInit(ZB.CreateMemSet(
              AI, ZB.getInt8(0),
              ZB.getInt64(
                  DL.getTypeAllocSize(AI->getAllocatedType()).getFixedValue()),
              AI->getAlign()));
          Changed = true;
        }

        // Per-call-site emission: anchor a stackmap to every potential-
        // morestack call, listing the pointer slots live at that PC. A strict
        // NOSPLIT leaf (no calls) gets zero sites here — correct, it never hits
        // copystack-at-call. Aggregate-only functions (no scalar pointer slots)
        // still need a body bitmap (index >= 1) for the backend to OR
        // aggregate-field bits into, so we emit an empty-operand stackmap at
        // their call sites too.
        //
        // #311 keeps an aggregate's entry zero-init alive via
        // stripAllocaLifetimes (above): with no lifetime markers the slot is
        // whole-function-live, so DSE/StackColoring can no longer prove the
        // zero-init dead. The backend OR's the aggregate's pointer-field bits
        // into every body bitmap (FUNCDATA $1) using the REAL field offsets, so
        // a never-set field reads NULL and copystack skips it.
        //
        // #314 (regression from #311): the original #311 fix ALSO appended each
        // aggregate alloca to the per-PC stackmap operand list here, as a second
        // "anchor". That was harmful and is removed. An aggregate operand lowers
        // to one Direct(SP, base) entry (AArch64AsmPrinter::LowerSTACKMAP), and
        // recordC2GoStackmapSite (MCPlan9AsmStreamer) marks the BASE WORD
        // (offset 0). The #311 comment assumed "the agg mask already sets that
        // word, so the OR is idempotent" — FALSE when the aggregate's offset-0
        // field is a NON-pointer (e.g. SelectDest.eDest=u8). The agg mask marks
        // only the real pointer-field words; anchoring spuriously marked the
        // non-pointer base word, so copystack read eDest=SRT_Output=9 as a
        // pointer → "bad pointer in frame ... yy_reduce ... 0x9". The
        // zero-init anchoring is unnecessary (stripAllocaLifetimes already
        // keeps it live) and unsound (pollutes the base word), so aggregates are
        // NO LONGER per-PC stackmap operands. Empty-operand stackmaps are still
        // emitted at every aggregate-bearing site (see below) so the backend has
        // a body bitmap to OR the agg mask into. Full per-PC liveness of
        // aggregate FIELDS is the eventual root fix #313.
        if (!SlotAllocas.empty() || !AggAllocas.empty()) {
          SmallVector<CallBase *, 32> Sites;
          for (Instruction &I : instructions(F))
            if (auto *CB = dyn_cast<CallBase>(&I))
              if (isPotentialMorestackCall(CB))
                Sites.push_back(CB);
          if (!Sites.empty()) {
            AllocaLiveness L;
            if (!SlotAllocas.empty())
              computeAllocaLiveness(F, SlotAllocas, Sites, L);
            for (CallBase *CB : Sites) {
              // #307 idempotency: a site the prior (pre-inline) run already
              // instrumented carries its stackmap immediately before it; skip
              // it so the second run only fills genuinely-uncovered sites
              // (e.g. calls inside a freshly-inlined callee whose own run-1
              // stackmap was NOT spliced ahead of them). No-op on first run.
              if (isAlreadyStackmapped(CB, StartWatermark))
                continue;
              SmallVector<AllocaInst *, 8> LiveSlots;
              if (!SlotAllocas.empty()) {
                const BitVector &Live = L.LiveBefore[CB];
                // #654c: captured slots bypass the backward liveness — see
                // the CapturedSlots comment above.
                for (unsigned I = 0, N = SlotAllocas.size(); I < N; ++I)
                  if (Live.test(I) || CapturedSlots.test(I))
                    LiveSlots.push_back(SlotAllocas[I]);
              }
              // #314: aggregates are NO LONGER appended as stackmap operands
              // (their base-word mark polluted the locals bitmap; see above).
              // The backend agg mask marks their real pointer-field words.
              // Always emit (even when no slot is live) so a body bitmap entry
              // exists at this PC for the backend's aggregate-field OR; an
              // empty live set interns to bitmap index 0 only when the agg mask
              // is also empty, otherwise to a distinct >=1 entry.
              emitStackmapBefore(CB, NextId++, LiveSlots, StackmapFn);
              ++EmittedSites;
              Changed = true;
            }
          }
        }
      }
      continue; // pointer-slot path; skip the legacy per-safepoint emission
    }

    if (ManagedAllocas.empty())
      continue;

    // Collect call sites first so we don't re-visit the stackmap call
    // we just inserted on the very next iteration of the walker.
    SmallVector<CallBase *, 16> Sites;
    for (Instruction &I : instructions(F))
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (isSafepointCall(CB, SeedList))
          Sites.push_back(CB);

    if (Sites.empty())
      continue;

    LLVM_DEBUG(dbgs() << "c2go-safepoint: function " << F.getName() << " has "
                      << ManagedAllocas.size() << " managed alloca(s), "
                      << Sites.size() << " safepoint call(s)\n");

    // #221: MemorySSA-backed liveness. We need the per-function
    // FunctionAnalysisManager (FAM) wrapped inside our ModuleAnalysis.
    auto &FAM =
        MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
    MemorySSA *MSSA = nullptr;
    DominatorTree *DT = nullptr;
    // MemorySSA requires an extant AssumptionCache & DT. If any is
    // unavailable (e.g., new-PM was not configured for the module),
    // fall back to the previous forward-reach behaviour for safety.
    if (auto *MSSAA = FAM.getCachedResult<MemorySSAAnalysis>(F))
      MSSA = &MSSAA->getMSSA();
    else
      MSSA = &FAM.getResult<MemorySSAAnalysis>(F).getMSSA();
    DT = &FAM.getResult<DominatorTreeAnalysis>(F);

    for (CallBase *CB : Sites) {
      // #307 idempotency: skip a safepoint the prior run already instrumented
      // (its stackmap sits immediately before it). No-op on first run.
      if (isAlreadyStackmapped(CB, StartWatermark))
        continue;
      llvm::SmallVector<AllocaInst *, 8> LiveAllocas;
      for (AllocaInst *A : ManagedAllocas) {
        bool Live = (MSSA && DT)
                        ? isAllocaLiveAtSafepointViaMSSA(A, CB, *MSSA, *DT)
                        : true; // conservative fallback
        if (Live)
          LiveAllocas.push_back(A);
      }

      // Skip emitting a stackmap with zero live operands — Go runtime
      // will see PCDATA $1, $0 (empty bitmap), which is exactly what
      // an "unsafe point" looks like, but we don't need the metadata.
      if (LiveAllocas.empty())
        continue;

      emitStackmapBefore(CB, NextId++, LiveAllocas, StackmapFn);
      ++EmittedSites;
      Changed = true;
    }
  }

  if (Changed) {
    LLVM_DEBUG(dbgs() << "c2go-safepoint: emitted " << EmittedSites
                      << " stackmap intrinsic call(s); next ID = " << NextId
                      << "\n");
    writeNextIdWatermark(M, NextId);
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
