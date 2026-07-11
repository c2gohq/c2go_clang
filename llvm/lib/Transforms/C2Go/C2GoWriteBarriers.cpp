//===- C2GoWriteBarriers.cpp - Write-barrier insertion --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go-write-barriers: insert Go hybrid write barriers for direct stores of a
// managed Go-heap pointer into managed Go-heap memory. A managed pointer is one
// in addrspace(1) (the c2go-gc discriminator; see CodeGenTypes AS1 lowering and
// the "c2go-gc" GCStrategy). For every
//
//   store ptr addrspace(1) %new, ptr addrspace(1) %slot
//
// whose destination is NOT a current-frame root slot (a local alloca), the
// store is rewritten to Go's pattern:
//
//   %enabled = load i32, ptr @runtime.writeBarrier
//   br i1 (%enabled != 0), label %wb_slow, label %wb_fast
// wb_slow:                         ; GC on: shim does buffer-then-store
//   call void @_c2go_writePtr(ptr addrspace(1) %slot, ptr addrspace(1) %new)
//   br label %wb_done
// wb_fast:                         ; GC off: plain store
//   store ptr addrspace(1) %new, ptr addrspace(1) %slot
//   br label %wb_done
// wb_done:
//
// _c2go_writePtr is a GoABI0 c2go-libc shim that forwards to runtime.atomicstorep
// (which performs the hybrid barrier — atomicwb shades the old+new pointer — and
// the store). Its LLVM prototype keeps addrspace(1) parameters so that
// RewriteStatepointsForGC sees the managed call operands and keeps them live
// across the (safepointing) shim call.
//
// Runs late (after the optimizer, before RewriteStatepointsForGC), analogous to
// Go's cmd/compile/internal/ssa/writebarrier.go.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoWriteBarriers.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/C2Go/C2GoCommon.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

#define DEBUG_TYPE "c2go-write-barriers"

static constexpr unsigned kManagedAS = 1;
static constexpr char kWriteBarrierGlobal[] = "runtime.writeBarrier";
static constexpr char kWritePtrShim[] = "_c2go_writePtr";
// Idempotency marker: the fast-path store emitted by this pass carries
// `!c2go.wb.done` so a subsequent run (BackendUtil + c2go-lto post-inliner)
// does not re-wrap an already-instrumented store. See #371.
static constexpr char kBarrierDoneMD[] = "c2go.wb.done";

// A store needs a barrier iff it deposits a managed (addrspace(1)) pointer and
// its destination is not provably the current frame (a local alloca root slot).
// "Prove stack-local to skip; otherwise barrier" — matches Go's rule (writes to
// the current frame need no barrier, but a pointer reachable through a param,
// load, global, phi/select, or opaque result is barriered conservatively).
static bool storeNeedsBarrier(const StoreInst *SI) {
  // v0: only plain stores. A volatile/atomic store's ordering can't be
  // preserved once the slow path becomes a GoABI0 shim call, so leave those
  // alone until a matching atomic barrier shim exists.
  if (SI->isVolatile() || SI->isAtomic())
    return false;
  // Idempotency: skip stores this pass has already wrapped (#371).
  if (SI->getMetadata(kBarrierDoneMD))
    return false;
  const Value *Val = SI->getValueOperand();
  if (!Val->getType()->isPointerTy() ||
      Val->getType()->getPointerAddressSpace() != kManagedAS)
    return false;
  // The destination: managed (AS1) memory, or — #646 hole-2 fix — an AS0 slot
  // that is not a current-frame alloca. C GLOBALS (data/bss pointer slots,
  // including the Go-owned ceded ones) are AS0 addresses: they are GC roots
  // re-scanned only at mark start, so a store during concurrent mark must
  // shade the value exactly like a heap write (the Go compiler barriers ITS
  // global pointer stores; C must too). Other AS0 slots reached through
  // opaque/boundary-cast pointers are barriered conservatively — the same
  // "prove safe or barrier" philosophy as the alloca rule below: an extra
  // barrier only over-marks, never under-marks. (The former AS1-only slot
  // filter existed to avoid mistyping the AS1 shim call; insertBarrier now
  // addrspacecasts an AS0 slot for the call instead.)
  unsigned SlotAS =
      SI->getPointerOperand()->getType()->getPointerAddressSpace();
  if (SlotAS != kManagedAS && SlotAS != 0)
    return false;
  const Value *Obj = getUnderlyingObject(SI->getPointerOperand());
  // A store straight into a current-frame alloca slot is a root-slot write; the
  // GC scans that slot via the stack map, so no heap barrier is required.
  return !isa<AllocaInst>(Obj);
}

// External i32 view of runtime.writeBarrier; its first word is the `enabled`
// flag (nonzero when write barriers are on).
static Constant *getWriteBarrierFlag(Module &M) {
  return M.getOrInsertGlobal(kWriteBarrierGlobal, Type::getInt32Ty(M.getContext()));
}

// GoABI0 declaration of the c2go-libc barrier shim. Parameters stay in
// addrspace(1) so RewriteStatepointsForGC tracks them as managed roots.
//
// The shim is declared `gc-leaf-function` because the Go-side
// `_c2go_writePtr` is `//go:nosplit` (see c2go-bind/emit.go updated in this
// commit). Function entry has no morestack prelude, so there is no
// cooperative safepoint at call. The inline `gcWriteBarrier2` path is
// safepoint-isolated via `systemstack` (mwbbuf.go:157-160 +
// asm_arm64.s:1296), so the user goroutine's stack is not scanned/relocated
// while the barrier runs. RS4GC will therefore skip the statepoint wrap on
// calls to this shim.
static FunctionCallee getWritePtrShim(Module &M) {
  LLVMContext &Ctx = M.getContext();
  Type *ManagedPtr = PointerType::get(Ctx, kManagedAS);
  FunctionType *FT =
      FunctionType::get(Type::getVoidTy(Ctx), {ManagedPtr, ManagedPtr}, false);
  FunctionCallee C = M.getOrInsertFunction(kWritePtrShim, FT);
  if (auto *F = dyn_cast<Function>(C.getCallee())) {
    F->setCallingConv(CallingConv::GoABI0);
    F->addFnAttr("gc-leaf-function");
  }
  return C;
}

static void insertBarrier(StoreInst *SI, Constant *WBFlag,
                          FunctionCallee Shim) {
  Value *Slot = SI->getPointerOperand();
  Value *Val = SI->getValueOperand();

  IRBuilder<> B(SI);
  Value *Enabled =
      B.CreateLoad(Type::getInt32Ty(SI->getContext()), WBFlag, "wb.enabled");
  Value *Cond = B.CreateICmpNE(
      Enabled, ConstantInt::get(Type::getInt32Ty(SI->getContext()), 0),
      "wb.on");

  // Then = barrier-on (slow): the shim buffers old+new and performs the store.
  // Else = barrier-off (fast): a plain store.
  Instruction *ThenTerm = nullptr;
  Instruction *ElseTerm = nullptr;
  SplitBlockAndInsertIfThenElse(Cond, SI->getIterator(), &ThenTerm, &ElseTerm);

  IRBuilder<> SlowB(ThenTerm);
  // #646: an AS0 slot (a C/global address) is addrspacecast for the AS1-typed
  // shim call — same pointer word, and it never crosses a statepoint (the shim
  // is gc-leaf and the cast's only use is this call), so RS4GC never needs a
  // base for it.
  Value *SlotArg = Slot;
  if (Slot->getType()->getPointerAddressSpace() != kManagedAS)
    SlotArg = SlowB.CreateAddrSpaceCast(
        Slot, PointerType::get(SI->getContext(), kManagedAS), "wb.slot.as1");
  CallInst *Call = SlowB.CreateCall(Shim, {SlotArg, Val});
  Call->setCallingConv(CallingConv::GoABI0);

  IRBuilder<> FastB(ElseTerm);
  StoreInst *Fast = FastB.CreateStore(Val, Slot);
  Fast->setAlignment(SI->getAlign());
  Fast->setVolatile(SI->isVolatile());
  Fast->setOrdering(SI->getOrdering());
  Fast->setSyncScopeID(SI->getSyncScopeID());
  // Mark the fast-path store so a re-run of this pass leaves it alone (#371).
  Fast->setMetadata(kBarrierDoneMD,
                    MDNode::get(SI->getContext(), {}));

  SI->eraseFromParent();
}

PreservedAnalyses C2GoWriteBarriersPass::run(Module &M,
                                             ModuleAnalysisManager &) {
  // Only active in c2go-mode modules (those targeting the Go runtime).
  if (!M.getModuleFlag(llvm::c2go::kGoabiModuleFlag))
    return PreservedAnalyses::all();

  SmallVector<StoreInst *, 16> Worklist;
  for (Function &F : M)
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *SI = dyn_cast<StoreInst>(&I))
          if (storeNeedsBarrier(SI))
            Worklist.push_back(SI);

  // #455(b): the unconditional pre-store sweep was deleted — the
  // post-insertion sweep below already covers the brand-new slow-path
  // calls AND every pre-existing direct call site in the module (the
  // sweep iterates `F->users()`, not just the newly emitted ones).
  // Modules whose Worklist is empty AND have no _c2go_writePtr shim
  // decl at all are guaranteed clean on this helper; modules whose
  // Worklist is empty BUT pre-pollute a direct call to _c2go_writePtr
  // must declare a barrier-triggering store to exercise the sweep
  // (see `c2go-common-callsite-cc-sweep.ll`).
  if (Worklist.empty())
    return PreservedAnalyses::all();

  Constant *WBFlag = getWriteBarrierFlag(M);
  FunctionCallee Shim = getWritePtrShim(M);
  for (StoreInst *SI : Worklist)
    insertBarrier(SI, WBFlag, Shim);

  // #429: post-insertion sweep — covers (i) brand-new slow-path calls
  // (those are set goabi0cc explicitly above; the sweep is a safety
  // net against future regressions in insertBarrier) and (ii) any
  // pre-existing CallInst against the shim that an upstream emitter
  // left with the default C CC. The sweep walks `F->users()`, so it
  // catches both sets in one pass.
  (void)llvm::c2go::enforceCallSiteCC(M.getFunction(kWritePtrShim));

  LLVM_DEBUG(dbgs() << "c2go-write-barriers: inserted " << Worklist.size()
                    << " barrier(s) in " << M.getName() << "\n");
  return PreservedAnalyses::none();
}
