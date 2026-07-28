//===- C2GoGCSetup.cpp - Attach the c2go-gc GC strategy -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See C2GoGCSetup.h. Two jobs, both prerequisites for the statepoint GC path
// (#326 Stage II/III):
//
//  (1) Tag every function that has ANY pointer with gc "c2go-gc", so
//      RewriteStatepointsForGC (RS4GC) processes it. (Widened from the old
//      AS1-only test: under the Go movable-stack model every pointer is a
//      potential stack root — see C2GoGC::isGCManagedPointer, Stage I.)
//
//  (2) Mark each call as either a SAFEPOINT (RS4GC wraps it in a gc.statepoint,
//      spilling every live pointer to a tracked slot) or GC-LEAF (RS4GC leaves
//      it alone). RS4GC wraps every non-leaf call that carries a deopt bundle
//      (RewriteStatepointsForGC.cpp NeedsRewrite / callsGCLeafFunction), so we:
//        - add an empty `[ "deopt"() ]` bundle to every SAFEPOINT call, and
//        - add the `"gc-leaf-function"` call-site attribute to every GC-LEAF
//          call.
//      The safepoint set MATCHES the lightweight C2GoSafepointPass's active
//      morestack-call set (isPotentialMorestackCall: every ordinary call is a
//      safepoint; no-op intrinsics and call-free inline asm are not) — NOT the
//      legacy managed-alloca
//      fallback seed list from getSafepointCallees (SQLite reaches morestack
//      through libc malloc, not
//      runtime.mallocgc, so a runtime-name-only list would miss the real
//      safepoints and reproduce the -O2 copystack bug). The only ADDITIONAL
//      gc-leaf exclusion is the RS4GC structural limitation: gc.statepoint
//      cannot wrap a NON-VOID VARARG callee (Verifier rule: gc.statepoint
//      cannot wrap a non-void vararg callee), so those are forced gc-leaf.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoGCSetup.h"
#include "llvm/Transforms/C2Go/C2GoGCMaskUtils.h"
#include "llvm/Transforms/C2Go/C2GoMorestackUtils.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

using namespace llvm;

#define DEBUG_TYPE "c2go-gc-setup"

static constexpr char kStrategy[] = "c2go-gc";

// hasAnyPointer reports whether F references a pointer-typed DATA value
// anywhere (argument, instruction result, or instruction operand). Under the Go
// movable stack model every such pointer is a potential stack root, so the
// function must be processed by RS4GC. (Widened from the old AS1-only
// predicate.) The called-operand of a call/invoke is excluded: it is always a
// `ptr` (the callee address) but is not a relocatable data pointer, and
// counting it would tag every function that merely makes a call.
static bool hasAnyPointer(const Function &F) {
  for (const Argument &A : F.args())
    if (A.getType()->isPointerTy())
      return true;
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB) {
      if (I.getType()->isPointerTy())
        return true;
      const auto *CB = dyn_cast<CallBase>(&I);
      for (const Use &U : I.operands()) {
        if (CB && U.get() == CB->getCalledOperand())
          continue; // callee address, not a data pointer
        if (U.get()->getType()->isPointerTy())
          return true;
      }
    }
  return false;
}

// isUnwrappableVararg returns true for callees RS4GC structurally cannot wrap:
// a gc.statepoint cannot wrap a NON-VOID VARARG callee (Verifier rule:
// gc.statepoint cannot wrap a non-void vararg callee). Such calls are forced
// gc-leaf. (A void vararg callee is fine to wrap.)
static bool isUnwrappableVararg(const CallBase *CB) {
  FunctionType *FTy = CB->getFunctionType();
  return FTy->isVarArg() && !FTy->getReturnType()->isVoidTy();
}

// markGCLeaf adds the "gc-leaf-function" call-site attribute (callsGCLeafFunction
// checks Call->hasFnAttr) so RS4GC's NeedsRewrite leaves the call alone.
static void markGCLeaf(CallBase *CB) {
  CB->addFnAttr(Attribute::get(CB->getContext(), "gc-leaf-function"));
}

// makeSafepoint ensures CB carries a deopt operand bundle, which RS4GC's
// NeedsRewrite (AllowStatepointWithNoDeoptInfo==false → hasDeoptState()
// required) needs to wrap a non-leaf call. If the call already has a deopt
// bundle (e.g. front-end emitted), leave it. Otherwise clone the call with an
// empty `[ "deopt"() ]` bundle appended and replace the original. Returns the
// (possibly new) call.
static CallBase *makeSafepoint(CallBase *CB) {
  if (CB->getOperandBundle(LLVMContext::OB_deopt))
    return CB;
  // CallBrInst cannot be cloned with addOperandBundle the same way and never
  // appears in c2go output; guard defensively.
  if (isa<CallBrInst>(CB))
    return CB;
  OperandBundleDef Deopt("deopt", ArrayRef<Value *>{});
  CallBase *NewCB =
      CallBase::addOperandBundle(CB, LLVMContext::OB_deopt, Deopt, CB->getIterator());
  NewCB->copyMetadata(*CB);
  CB->replaceAllUsesWith(NewCB);
  CB->eraseFromParent();
  return NewCB;
}

PreservedAnalyses C2GoGCSetupPass::run(Module &M, ModuleAnalysisManager &) {
  if (!M.getModuleFlag(llvm::c2go::kGoabiModuleFlag))
    return PreservedAnalyses::all();

  unsigned NTagged = 0, NSafepoint = 0, NLeaf = 0, NStripped = 0;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (!F.hasGC() && hasAnyPointer(F)) {
      F.setGC(kStrategy);
      ++NTagged;
    }
    // Only mark calls inside tagged functions: RS4GC only rewrites functions
    // with the RS4GC GCStrategy, so calls elsewhere need no annotation.
    if (!F.hasGC() || F.getGC() != kStrategy)
      continue;

    // Collect first; makeSafepoint may replace a call (RAUW + erase), which
    // would invalidate an in-flight instruction iterator.
    SmallVector<CallBase *, 64> Calls;
    SmallVector<IntrinsicInst *, 64> Lifetimes;
    for (Instruction &I : instructions(F)) {
      if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
        Intrinsic::ID Id = II->getIntrinsicID();
        if (Id == Intrinsic::lifetime_start || Id == Intrinsic::lifetime_end) {
          Lifetimes.push_back(II);
          continue; // not a safepoint; don't also annotate it
        }
      }
      if (auto *CB = dyn_cast<CallBase>(&I))
        Calls.push_back(CB);
    }

    // #326: strip all llvm.lifetime.start/end markers in tagged functions
    // BEFORE RS4GC. Because we track AS0 pointers (Stage I), RS4GC relocates
    // alloca-address pointers and rewrites a following lifetime intrinsic's
    // operand to the relocated SSA value — which the verifier rejects
    // ("lifetime.start/end can only be used on alloca or poison"). Lifetime
    // markers are stack-coloring hints; stack-coloring has already run by
    // OptimizerLast, so dropping them here is correctness-neutral (and matches
    // the existing c2go aggregate-lifetime strip in C2GoSafepoint).
    for (IntrinsicInst *II : Lifetimes)
      II->eraseFromParent();
    NStripped += Lifetimes.size();

    for (CallBase *CB : Calls) {
      // gc.statepoint intrinsics (re-runs) and already-leaf calls: skip.
      if (auto *II = dyn_cast<IntrinsicInst>(CB))
        if (II->getIntrinsicID() == Intrinsic::experimental_gc_statepoint)
          continue;
      if (!c2go::mayReachMorestack(*CB) || isUnwrappableVararg(CB)) {
        markGCLeaf(CB);
        ++NLeaf;
        continue;
      }
      makeSafepoint(CB);
      ++NSafepoint;
    }
  }

  LLVM_DEBUG(dbgs() << "c2go-gc-setup: tagged " << NTagged << " function(s), "
                    << NSafepoint << " safepoint call(s), " << NLeaf
                    << " gc-leaf call(s), " << NStripped
                    << " lifetime marker(s) stripped in " << M.getName()
                    << "\n");
  return (NTagged || NSafepoint || NLeaf || NStripped)
             ? PreservedAnalyses::none()
             : PreservedAnalyses::all();
}

//===----------------------------------------------------------------------===//
// C2GoFoldAllocaRelocatesPass (#327)
//===----------------------------------------------------------------------===//

// Is V rooted at an alloca, looking THROUGH gc.relocate and phi/select? The
// relocate chain for an alloca-based pointer is typically
// `%e -> phi(%e, %e.relocated) -> gc.relocate -> ...`, so a plain
// getUnderlyingObject (which stops at a phi) is not enough. We treat
// gc.relocate(alloca-rooted), and phi/select whose operands are ALL
// alloca-rooted, as alloca-rooted. A small visited set breaks the relocate
// <-> phi cycle.
static bool isAllocaRooted(Value *V, SmallPtrSetImpl<Value *> &Visited) {
  V = V->stripPointerCasts();
  if (!Visited.insert(V).second)
    return true; // assume rooted on a back-edge; confirmed by other operands
  if (isa<AllocaInst>(V))
    return true;
  if (auto *GEP = dyn_cast<GetElementPtrInst>(V))
    return isAllocaRooted(GEP->getPointerOperand(), Visited);
  if (auto *RI = dyn_cast<GCRelocateInst>(V))
    return isAllocaRooted(const_cast<Value *>(RI->getDerivedPtr()), Visited);
  if (auto *PN = dyn_cast<PHINode>(V)) {
    for (Value *In : PN->incoming_values())
      if (!isAllocaRooted(In, Visited))
        return false;
    return true;
  }
  if (auto *SI = dyn_cast<SelectInst>(V))
    return isAllocaRooted(SI->getTrueValue(), Visited) &&
           isAllocaRooted(SI->getFalseValue(), Visited);
  return false;
}

// Append the byte offsets of every pointer-typed field of `Ty` based at
// `Base`. #432 thin shim over the shared `c2go::walkPointerFields` helper;
// previously this duplicated the same recursive walk as
// C2GoSafepoint::collectPointerFieldOffsets and the backend's
// c2goMarkPtrFieldBits. The shared helper guarantees the three sites see the
// same field set on the same aggregate.
static void collectPtrFieldOffsets(Type *Ty, uint64_t Base, const DataLayout &DL,
                                   SmallVectorImpl<uint64_t> &Out) {
  c2go::walkPointerFields(Ty, Base, DL,
                          [&](uint64_t Off) { Out.push_back(Off); });
}

PreservedAnalyses C2GoFoldAllocaRelocatesPass::run(Module &M,
                                                   ModuleAnalysisManager &AM) {
  unsigned NFolded = 0, NZeroed = 0;
  for (Function &F : M) {
    if (F.isDeclaration() || !F.hasGC() || F.getGC() != kStrategy)
      continue;
    SmallVector<GCRelocateInst *, 16> ToFold;
    // #327: every pointer-bearing alloca in a c2go-gc function (except vararg
    // packs) is a candidate for entry null-init. RS4GC threads an address-taken
    // alloca's BASE through phi nodes into the gc-live set of many — often ALL —
    // statepoints in the function (e.g. SQLite resolveSelectStep's inlined
    // `Walker w;` flows %w.i -> %.0610 -> ... -> %.8618 across the whole body).
    // At each such statepoint LowerSTATEPOINT expands the alloca's pointer
    // FIELDS into the per-PC locals bitmap. That mark is sound ONLY if the slot
    // reads as either a live heap pointer or null at that PC. On a control path
    // that reaches a marking statepoint WITHOUT first storing the field (the
    // field is written on a different path, or only just before its own call),
    // the slot holds uninitialized stack garbage; a small-int garbage value
    // (1..4095) makes copystack abort ("bad pointer in frame ... 0x10"). This is
    // exactly Go's `needzero` obligation for address-taken pointer-containing
    // stack locals. We therefore null-init at entry regardless of whether the
    // alloca's relocate was folded below (the Walker's relocate is threaded
    // through plain phis, so isAllocaRooted never reaches %w.i and the fold —
    // and the old fold-coupled null-init — both miss it).
    SmallPtrSet<AllocaInst *, 8> PtrAllocas;
    for (Instruction &I : instructions(F)) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        if (AI->getMetadata(llvm::c2go::kVaPackMD))
          continue;
        SmallVector<uint64_t, 8> Offs;
        collectPtrFieldOffsets(AI->getAllocatedType(), 0, F.getDataLayout(),
                               Offs);
        if (!Offs.empty())
          PtrAllocas.insert(AI);
        continue;
      }
      auto *RI = dyn_cast<GCRelocateInst>(&I);
      if (!RI)
        continue;
      // Fold when the relocated DERIVED pointer is rooted at an alloca. An
      // alloca address is `SP + const`: it never moves within the IR, and after
      // a copystack the runtime relocates the alloca's stored pointer FIELDS via
      // the locals bitmap (not this SSA value). So the relocate is the identity
      // — replacing it with the original derived pointer lets the value
      // rematerialize as a frame-index at each safepoint → Direct stackmap
      // location → per-PC aggregate-field expansion in LowerSTATEPOINT.
      //
      // EXCEPTION: vararg-pack allocas (`!c2go.va.pack`). RS4GC keeps their
      // address live across unrelated safepoints, but their pointer content is
      // valid only right before their own vararg call, so they must NOT be
      // surfaced as Direct/field-expanded (that marks uninitialized words —
      // "bad pointer in frame ... 0x1"). Leaving the relocate in place keeps
      // the array address as an Indirect spill (marked only where actually
      // spilled = content-valid), and the field expansion never fires.
      Value *Derived = RI->getDerivedPtr();
      Value *Base = getUnderlyingObject(Derived);
      if (auto *AI = dyn_cast<AllocaInst>(Base))
        if (AI->getMetadata(llvm::c2go::kVaPackMD))
          continue;
      SmallPtrSet<Value *, 16> Visited;
      if (isAllocaRooted(const_cast<Value *>(Derived), Visited))
        ToFold.push_back(RI);
    }
    for (GCRelocateInst *RI : ToFold) {
      RI->replaceAllUsesWith(RI->getDerivedPtr());
      RI->eraseFromParent();
      ++NFolded;
    }
    // #327: null-initialize the pointer FIELDS of every pointer-bearing alloca
    // (collected above) at function entry. This makes a slot that is MARKED by
    // the per-PC bitmap (because RS4GC threaded the alloca base into this
    // statepoint's gc-live set) but not yet written on the current path read as
    // null — which copystack accepts — instead of stale stack garbage that a
    // small int would turn into a fatal "bad pointer in frame" abort. Covers
    // both the scalar OUT-PARAM case (sqlite3_exec's `pStmt`/`zLeftover` passed
    // by address, written by the callee) and the aggregate case whose base is
    // phi-threaded across paths that skip the field store (resolveSelectStep's
    // `Walker w;` / `NameContext sNC;`). (Reading a slot before it is written is
    // UB in C, so the zero-init is semantically safe; it runs at OptimizerLast,
    // after SROA/DSE, so it is not eliminated.)
    if (!PtrAllocas.empty()) {
      const DataLayout &DL = F.getDataLayout();
      PointerType *PtrTy = PointerType::get(F.getContext(), 0);
      Constant *Null = ConstantPointerNull::get(PtrTy);
      Type *I8Ty = Type::getInt8Ty(F.getContext());
      Type *I64Ty = Type::getInt64Ty(F.getContext());
      for (AllocaInst *AI : PtrAllocas) {
        SmallVector<uint64_t, 8> Offs;
        collectPtrFieldOffsets(AI->getAllocatedType(), 0, DL, Offs);
        if (Offs.empty())
          continue;
        // Insert right AFTER the alloca so it dominates the GEPs/stores.
        BasicBlock::iterator IP = std::next(AI->getIterator());
        for (uint64_t Off : Offs) {
          Value *GEP =
              Off == 0 ? cast<Value>(AI)
                       : GetElementPtrInst::CreateInBounds(
                             I8Ty, AI, ConstantInt::get(I64Ty, Off), "", IP);
          new StoreInst(Null, GEP, /*isVolatile=*/false, IP);
        }
        ++NZeroed;
      }
    }
  }
  LLVM_DEBUG(dbgs() << "c2go-fold-alloca-relocates: folded " << NFolded
                    << " alloca relocate(s), null-init'd " << NZeroed
                    << " aggregate(s) in " << M.getName() << "\n");
  return (NFolded || NZeroed) ? PreservedAnalyses::none()
                              : PreservedAnalyses::all();
}
