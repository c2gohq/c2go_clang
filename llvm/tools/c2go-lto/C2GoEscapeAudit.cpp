//===- C2GoEscapeAudit.cpp - WF2 stack->heap escape audit -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #289 client B (#478 split): the Andersen-lite stack-address escape
// audit, lifted verbatim from c2go-lto.cpp:234-781. Refactor-only — no
// semantic change relative to the prior single-TU shape.
//
// Algorithm: a flavored, field-insensitive, constraint-based Andersen-lite
// points-to with a memory model, solved online to a fixpoint with a worklist.
// See c2go-lto.cpp's file header for the per-cell / per-constraint table —
// this TU keeps the implementation; the design narrative stays at the tool
// entry-point so a reader entering at `main` sees the full algorithm before
// it dives into the split.
//
// Diagnostic streams kept identical to the pre-split path:
//   * `c2go-lto: stack->heap in <fn> at <loc>` lines → caller-supplied Out
//     (used to be the global outs())
//   * `c2go-lto: <N> stack-address escape point(s)` summary → caller-supplied
//     Out (used to be inline in main)
//   * `c2go-lto: solved in <S> worklist step(s), …` → errs() (unchanged;
//     debug-only, gated by --c2go-print-stats)
//
//===----------------------------------------------------------------------===//

#include "C2GoEscapeAudit.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <utility>
#include <vector>

#define DEBUG_TYPE "c2go-lto"

using namespace llvm;

// #295 precision: when the value side of an escape store points to the same
// Andersen cell across many store instructions inside one function (typical
// pattern: same alloca address written to a heap field on multiple control-
// flow paths), reporting each store is noise — the underlying issue is a
// single source-level alloca. Default on collapses each (function, value-node)
// to one line; --c2go-escape-dedup=false restores the per-store stream for
// when fine-grained location is desired (e.g. patching individual stores).
static cl::opt<bool>
    EscapeDedup("c2go-escape-dedup",
                cl::desc("Collapse escape reports to one line per (function, "
                         "value-cell) pair (#295)."),
                cl::init(true));

// #295 — field-sensitive Andersen. When set, the analysis materializes a
// separate cell per (base-object, byte-offset) instead of collapsing every
// field of an object onto one cell. Phase 1 only landed the data structures
// and the cellAt() helper; Phase 2 (live now) reads this flag in the GEP /
// store / load paths and switches between the field-insensitive and
// field-sensitive cell-allocation strategies (see PointsTo::cellAt /
// PointsTo::wireGEPField). Default off — turn on for finer-grained
// (function, value-cell) escape diagnostics at the cost of analysis time.
static cl::opt<bool>
    FieldSensitive("c2go-andersen-field-sensitive",
                   cl::desc("Enable field-sensitive Andersen analysis "
                            "(per (base-object, byte-offset) cells)"),
                   cl::init(false));

// `--c2go-print-stats` / `-v` — solver-stats verbosity. Used to live in
// c2go-lto.cpp; moved here together with the only reader (PointsTo::solve).
static cl::opt<bool> Verbose("c2go-print-stats",
                             cl::desc("Print solver statistics"),
                             cl::init(false));
static cl::alias VerboseShort("v", cl::desc("Alias for --c2go-print-stats"),
                              cl::aliasopt(Verbose));

namespace {

// Recognised libc heap allocators (mirrors C2GoEscapeAudit::isHeapAllocatorName;
// kept in sync deliberately, not shared, to avoid coupling the tool to the
// transform library).
bool isHeapAllocatorName(StringRef Name) {
  // c2go GC allocator: c2go_gc_malloc routes to runtime.mallocgc and is emitted
  // with a c2go_linkname, so the IR symbol is the Go path (with a leading \01
  // no-mangle byte): "...c2go_libc.GCMalloc". A GC-heap object is non-moving,
  // but a stack address stored into one still dangles when the goroutine stack
  // moves, so GC allocations are "heap" for escape purposes.
  if (Name.ends_with("c2go_libc.GCMalloc"))
    return true;
  return Name == "malloc" || Name == "calloc" || Name == "realloc" ||
         Name == "aligned_alloc" || Name == "reallocf" || Name == "valloc";
}

enum CellKind : unsigned char { CK_Stack, CK_Heap, CK_Global, CK_HeapUnknown };

class PointsTo {
public:
  explicit PointsTo(Module &M, raw_ostream &Out) : M(M), Out(Out) {}

  // Solve to a fixpoint and report escapes. Returns the number of escapes.
  unsigned run();

private:
  Module &M;
  raw_ostream &Out; // destination for per-escape lines (was outs()).

  // ---- abstract cells -----------------------------------------------------
  SmallVector<CellKind, 1024> CellKinds;
  // contents(cell) is itself a node; map cell id -> node id (created lazily).
  SmallVector<int, 1024> CellContentsNode; // -1 until materialized
  unsigned HeapUnknownCell = 0;

  // #295 field-sensitive Andersen. With --c2go-andersen-field-sensitive=false
  // (default), cellAt() returns the single CollapsedCell[Base] — behaviour is
  // byte-identical to the pre-Phase-1 field-insensitive solver. With
  // --c2go-andersen-field-sensitive=true, GEPs with a constant byte offset
  // materialise a separate cell per (Base, Off) in FieldCell, and the GEP
  // edge in solve() propagates points-to into a *different* field cell than
  // its base. Cells outside [0, ObjSize[Base]) and GEPs with a non-constant
  // offset / dynamic index fall back to CollapsedCell[Base] (sound — heap /
  // global / unknown sentinel still flag the escape).
  //   CellBaseObj[c] = base Value that owns cell c (nullptr for synthetic
  //                    sentinels such as HeapUnknown).
  //   CellOffset[c]  = byte offset within the base object.
  //   CollapsedCell[V] = the field-insensitive cell for base V (offset
  //                       ignored); also the fallback used by cellAt() on a
  //                       dynamic / out-of-range / unknown-base offset.
  //   FieldCell[{V,O}] = the (lazily materialised) per-offset cell for base V
  //                       (only populated when FieldSensitive is on).
  //   ObjSize[V]       = byte size of the storage for base V (used to gate
  //                       lazy field-cell creation against out-of-range).
  SmallVector<const Value *, 1024> CellBaseObj;
  SmallVector<int64_t, 1024> CellOffset;
  DenseMap<const Value *, unsigned> CollapsedCell;
  DenseMap<std::pair<const Value *, int64_t>, unsigned> FieldCell;
  DenseMap<const Value *, uint64_t> ObjSize;
  const DataLayout *Layout = nullptr;

  unsigned newCell(CellKind K, const Value *Base = nullptr, int64_t Off = 0) {
    CellKinds.push_back(K);
    CellContentsNode.push_back(-1);
    CellBaseObj.push_back(Base);
    CellOffset.push_back(Off);
    return CellKinds.size() - 1;
  }

  // Resolve (Base, Off) to a cell id.
  //
  //   * Flag off: ignore Off, return CollapsedCell[Base] (Phase 1 path; the
  //     CollapsedCell lookup must succeed for every base reachable from the
  //     solver — every Stack/Heap/Global cell seeded by seedObjects has one).
  //   * Flag on : check FieldCell.lookup({Base, Off}); on miss, if Off is in
  //     [0, ObjSize[Base]) lazily create a same-Kind cell for that offset
  //     (self-pointing for non-Stack so loads keep yielding heap/global —
  //     mirrors the global / heap-call cell wiring in seedObjects). Out of
  //     range or unknown size falls back to CollapsedCell[Base] (sound).
  unsigned cellAt(const Value *Base, int64_t Off) {
    auto CIt = CollapsedCell.find(Base);
    assert(CIt != CollapsedCell.end() && "cellAt() on un-seeded base");
    if (!FieldSensitive)
      return CIt->second;
    auto FIt = FieldCell.find({Base, Off});
    if (FIt != FieldCell.end())
      return FIt->second;
    auto SIt = ObjSize.find(Base);
    if (SIt == ObjSize.end() || Off < 0 || (uint64_t)Off >= SIt->second)
      return CIt->second; // sound fallback: collapsed cell.
    CellKind K = CellKinds[CIt->second];
    unsigned NewC = newCell(K, Base, Off);
    FieldCell[{Base, Off}] = NewC;
    if (K != CK_Stack)
      addCell(contentsNode(NewC), NewC); // self-point: loads keep heap/global.
    return NewC;
  }

  // ---- points-to graph nodes ---------------------------------------------
  // A node is either a value node (SSA pointer) or a cell-contents node.
  struct Node {
    SparseBitVector<> Pts;             // cells this node may hold/set
    SparseBitVector<> CopyTo;          // node ids n s.t. pts(n) >= pts(this)
    // Complex-constraint indices: this node is the *pointer operand* p of …
    SmallVector<const Value *, 0> StoreVals; // … store q, p  (q values)
    SmallVector<unsigned, 0> LoadResults;    // … v = load p  (v node ids)
    // … field-sensitive GEP: src node of an `addr = gep base, Delta`. For
    //   each cell c in pts(this), addCell(DstNode, cellAt(base(c), off(c)+Delta)).
    SmallVector<std::pair<unsigned, int64_t>, 0> GEPDsts;
  };
  std::vector<Node> Nodes;
  DenseMap<const Value *, unsigned> ValNode; // SSA value -> node id

  SmallVector<unsigned, 256> Work; // node ids whose pts grew

  unsigned valNode(const Value *V) {
    auto It = ValNode.find(V);
    if (It != ValNode.end())
      return It->second;
    unsigned Id = Nodes.size();
    Nodes.emplace_back();
    ValNode[V] = Id;
    return Id;
  }
  unsigned contentsNode(unsigned Cell) {
    if (CellContentsNode[Cell] >= 0)
      return (unsigned)CellContentsNode[Cell];
    unsigned Id = Nodes.size();
    Nodes.emplace_back();
    CellContentsNode[Cell] = (int)Id;
    return Id;
  }

  // pts(node) >= {cell}; enqueue if grew.
  void addCell(unsigned NodeId, unsigned Cell) {
    if (Nodes[NodeId].Pts.test_and_set(Cell))
      Work.push_back(NodeId);
  }
  // add copy edge a -> b (pts(b) >= pts(a)); seed b with a's current pts.
  void addCopyEdge(unsigned A, unsigned B) {
    if (A == B)
      return;
    if (!Nodes[A].CopyTo.test_and_set(B))
      return; // edge already present
    if ((Nodes[B].Pts |= Nodes[A].Pts))
      Work.push_back(B);
  }

  bool isHeapCall(const CallBase *CB) const {
    const Function *F = CB->getCalledFunction();
    return F && isHeapAllocatorName(F->getName());
  }
  // A function that may be entered from outside the analyzed program.
  static bool mayBeExternalEntry(const Function *F) {
    return F->hasAddressTaken() || !F->hasLocalLinkage();
  }

  DenseMap<const Value *, unsigned> ObjCell; // alloca/heap/global -> its cell

  void seedObjects();
  void buildConstraints();
  void solve();
  unsigned reportEscapes();
  void printEscape(const StoreInst *SI);
  void wireConstantOperand(const Value *V); // const GEP/bitcast of a global

  // Bind a copy edge for a pointer operand, resolving constant pointer
  // expressions (const GEP/bitcast of a global) to their base on the fly.
  void copyOperandTo(const Value *Src, unsigned DstNode);
};

// Phase 1: one cell per alloca / heap-call / mutable-global; seed the defining
// value's node; seed external-entry pointer params with HeapUnknown. #295
// also records ObjSize for use by cellAt() in field-sensitive mode (the
// CollapsedCell entry remains the offset-0 / fallback cell either way).
void PointsTo::seedObjects() {
  HeapUnknownCell = newCell(CK_HeapUnknown);
  // HeapUnknown points to itself: a load through an unknown-heap pointer must
  // keep yielding (unknown) heap — sound destination-side closure.
  addCell(contentsNode(HeapUnknownCell), HeapUnknownCell);

  for (GlobalVariable &GV : M.globals()) {
    if (GV.isConstant())
      continue; // read-only storage is not a GC heap root (sound to skip).
    unsigned C = newCell(CK_Global, &GV, 0);
    ObjCell[&GV] = C;
    CollapsedCell[&GV] = C;
    addCell(valNode(&GV), C);
    // self-point: loading out of a global yields a global, never transmits
    // the Stack-ness of addresses stored into it (kills cross-fn conflation).
    addCell(contentsNode(C), C);
    if (Type *VT = GV.getValueType(); VT && VT->isSized())
      ObjSize[&GV] = Layout->getTypeAllocSize(VT).getFixedValue();
  }

  for (Function &F : M) {
    if (!F.isDeclaration() && mayBeExternalEntry(&F))
      for (Argument &A : F.args())
        if (A.getType()->isPointerTy())
          addCell(valNode(&A), HeapUnknownCell);
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F)) {
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        unsigned C = newCell(CK_Stack, AI, 0);
        ObjCell[AI] = C;
        CollapsedCell[AI] = C;
        addCell(valNode(AI), C);
        if (std::optional<TypeSize> Sz = AI->getAllocationSize(*Layout);
            Sz && !Sz->isScalable())
          ObjSize[AI] = Sz->getFixedValue();
      } else if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (isHeapCall(CB)) {
          unsigned C = newCell(CK_Heap, CB, 0);
          ObjCell[CB] = C;
          CollapsedCell[CB] = C;
          addCell(valNode(CB), C);
          // self-point (see Global): loads yield Heap, no Stack transmission.
          addCell(contentsNode(C), C);
          // malloc/realloc/aligned_alloc/reallocf/valloc/calloc each carry
          // the byte size in a different arg slot. Picking the largest
          // ConstantInt arg covers all of them; if no arg is a ConstantInt
          // (size is dynamic), ObjSize is left unset and cellAt() falls back
          // to CollapsedCell — sound, just less precise.
          uint64_t Best = 0;
          for (const Use &U : CB->args())
            if (auto *CI = dyn_cast<ConstantInt>(U.get()))
              if (CI->getZExtValue() > Best)
                Best = CI->getZExtValue();
          if (Best)
            ObjSize[CB] = Best;
        }
      }
    }
  }
}

// Resolve a constant pointer expression (const GEP/bitcast/addrspacecast/
// inttoptr of a global) so that referencing it as an operand carries the base
// object's pts. A plain global / function / null contributes its own (or no)
// cell, already handled by valNode + ObjCell.
void PointsTo::wireConstantOperand(const Value *V) {
  auto *CE = dyn_cast<ConstantExpr>(V);
  if (!CE)
    return;
  if (CE->getOpcode() == Instruction::GetElementPtr ||
      CE->getOpcode() == Instruction::BitCast ||
      CE->getOpcode() == Instruction::AddrSpaceCast ||
      CE->getOpcode() == Instruction::IntToPtr) {
    const Value *Base = CE->getOperand(0);
    if (Base->getType()->isPointerTy()) {
      wireConstantOperand(Base);
      addCopyEdge(valNode(Base), valNode(CE));
    }
  }
}

void PointsTo::copyOperandTo(const Value *Src, unsigned DstNode) {
  if (!Src->getType()->isPointerTy())
    return;
  wireConstantOperand(Src);
  addCopyEdge(valNode(Src), DstNode);
}

// Phase 2: emit all constraints as graph edges / complex indices.
void PointsTo::buildConstraints() {
  auto isPtrCopy = [](const Instruction *I) {
    return isa<GetElementPtrInst>(I) || isa<BitCastInst>(I) ||
           isa<AddrSpaceCastInst>(I) || isa<PHINode>(I) || isa<SelectInst>(I) ||
           isa<IntToPtrInst>(I);
  };

  // Pre-collect address-taken candidate callees (for indirect calls), grouped
  // by ascending param count for a cheap arity filter.
  SmallVector<const Function *, 32> AddrTaken;
  for (Function &F : M)
    if (!F.isDeclaration() && F.hasAddressTaken())
      AddrTaken.push_back(&F);

  auto bindCall = [&](const CallBase *CB, const Function *Callee) {
    unsigned N = std::min<unsigned>(Callee->arg_size(), CB->arg_size());
    for (unsigned i = 0; i < N; ++i) {
      const Argument *Formal = Callee->getArg(i);
      if (Formal->getType()->isPointerTy())
        copyOperandTo(CB->getArgOperand(i), valNode(Formal));
    }
    if (CB->getType()->isPointerTy())
      for (const BasicBlock &BB : *Callee)
        if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
          if (const Value *RV = RI->getReturnValue())
            if (RV->getType()->isPointerTy())
              copyOperandTo(RV, valNode(CB));
  };

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F)) {
      if (auto *SI = dyn_cast<StoreInst>(&I)) {
        const Value *Q = SI->getValueOperand();
        if (!Q->getType()->isPointerTy())
          continue;
        wireConstantOperand(Q);
        wireConstantOperand(SI->getPointerOperand());
        unsigned PNode = valNode(SI->getPointerOperand());
        Nodes[PNode].StoreVals.push_back(Q);
        // Fire for cells already known in pts(p). Snapshot: addCopyEdge may
        // reallocate Nodes, invalidating a reference into Nodes[PNode].Pts.
        SparseBitVector<> Seeded = Nodes[PNode].Pts;
        for (unsigned C : Seeded)
          if (CellKinds[C] == CK_Stack) // only Stack-slot memory keeps precise
            addCopyEdge(valNode(Q), contentsNode(C)); // contents; heap/global/
                                                      // unknown self-point.
      } else if (auto *LI = dyn_cast<LoadInst>(&I)) {
        if (!LI->getType()->isPointerTy())
          continue;
        wireConstantOperand(LI->getPointerOperand());
        unsigned PNode = valNode(LI->getPointerOperand());
        unsigned VNode = valNode(LI);
        Nodes[PNode].LoadResults.push_back(VNode);
        SparseBitVector<> Seeded = Nodes[PNode].Pts;
        for (unsigned C : Seeded)
          addCopyEdge(contentsNode(C), VNode);
      } else if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (isHeapCall(CB))
          continue; // object cell, not a copy
        if (const Function *Direct = CB->getCalledFunction()) {
          if (!Direct->isDeclaration())
            bindCall(CB, Direct);
        } else {
          // Indirect: sound over-approximation — bind every address-taken
          // function with compatible arity (a superset of the true callees).
          for (const Function *Cand : AddrTaken)
            if (Cand->getFunctionType()->getNumParams() <= CB->arg_size())
              bindCall(CB, Cand);
        }
      } else if (I.getType()->isPointerTy() && isPtrCopy(&I)) {
        unsigned D = valNode(&I);
        if (auto *Phi = dyn_cast<PHINode>(&I)) {
          for (const Value *In : Phi->incoming_values())
            copyOperandTo(In, D);
        } else if (auto *Sel = dyn_cast<SelectInst>(&I)) {
          copyOperandTo(Sel->getTrueValue(), D);
          copyOperandTo(Sel->getFalseValue(), D);
        } else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
          // Field-sensitive (#295 Phase 2): when the GEP has a constant byte
          // offset, install a GEP edge that propagates pts(src) cell-by-cell
          // into pts(dst) at a shifted (base, off+Delta). Non-constant /
          // dynamic-index GEPs fall back to a plain copy edge on the
          // collapsed cell (sound — Stack-ness still flows through).
          const Value *Src = GEP->getPointerOperand();
          bool Handled = false;
          if (FieldSensitive) {
            unsigned BW = Layout->getIndexTypeSizeInBits(GEP->getType());
            APInt Off(BW, 0);
            if (cast<GEPOperator>(GEP)->accumulateConstantOffset(*Layout,
                                                                 Off)) {
              wireConstantOperand(Src);
              Nodes[valNode(Src)].GEPDsts.push_back(
                  {D, Off.getSExtValue()});
              // Seed pts(D) for cells already in pts(src). Snapshot Pts:
              // cellAt() may newCell and reallocate Nodes.
              SparseBitVector<> Seeded = Nodes[valNode(Src)].Pts;
              for (unsigned C : Seeded)
                if (const Value *Base = CellBaseObj[C])
                  addCell(D, cellAt(Base, CellOffset[C] + Off.getSExtValue()));
              Handled = true;
            }
          }
          if (!Handled)
            copyOperandTo(Src, D);
        } else {
          copyOperandTo(I.getOperand(0), D);
        }
      }
    }
  }
}

// Phase 3: online worklist propagation to fixpoint.
void PointsTo::solve() {
  uint64_t Steps = 0;
  while (!Work.empty()) {
    unsigned N = Work.pop_back_val();
    ++Steps;
    // Snapshot everything read from Nodes[N] up front: addCopyEdge below may
    // create new nodes and reallocate the Nodes vector, invalidating any
    // reference into it. New pts bits arriving later just re-enqueue N.
    SparseBitVector<> NPts = Nodes[N].Pts;
    SparseBitVector<> NCopyTo = Nodes[N].CopyTo;
    SmallVector<const Value *, 4> NStoreVals(Nodes[N].StoreVals.begin(),
                                             Nodes[N].StoreVals.end());
    SmallVector<unsigned, 4> NLoadResults(Nodes[N].LoadResults.begin(),
                                          Nodes[N].LoadResults.end());
    SmallVector<std::pair<unsigned, int64_t>, 2> NGEPDsts(
        Nodes[N].GEPDsts.begin(), Nodes[N].GEPDsts.end());

    // (1) plain copy edges.
    for (unsigned M2 : NCopyTo)
      if ((Nodes[M2].Pts |= NPts))
        Work.push_back(M2);

    // (2) complex: this node is a store/load pointer operand. For each cell in
    // its pts, wire the corresponding contents edge (idempotent).
    for (const Value *Q : NStoreVals)
      for (unsigned C : NPts)
        if (CellKinds[C] == CK_Stack) // only Stack-slot memory keeps precise
          addCopyEdge(valNode(Q), contentsNode(C)); // contents (see build).
    for (unsigned VNode : NLoadResults)
      for (unsigned C : NPts)
        addCopyEdge(contentsNode(C), VNode);

    // (3) field-sensitive GEP edge: this node is the source of one or more
    // `dst = gep src, Delta`. For each cell c in pts(this), pts(dst) gains
    // cellAt(base(c), off(c)+Delta). Snapshot NPts already (cellAt may
    // newCell → reallocate Nodes; iterate by id, not by ref).
    for (auto [DstNode, Delta] : NGEPDsts)
      for (unsigned C : NPts)
        if (const Value *Base = CellBaseObj[C])
          addCell(DstNode, cellAt(Base, CellOffset[C] + Delta));
  }
  if (Verbose)
    errs() << "c2go-lto: solved in " << Steps << " worklist step(s), "
           << CellKinds.size() << " cells, " << Nodes.size() << " nodes\n";
}

void PointsTo::printEscape(const StoreInst *SI) {
  const Function *F = SI->getFunction();

  std::string Loc = "<no-dbg>";
  if (const DebugLoc &DL = SI->getDebugLoc()) {
    std::string S;
    raw_string_ostream OS(S);
    if (const auto *Scope = dyn_cast_or_null<DIScope>(DL.getScope()))
      OS << Scope->getFilename() << ":";
    else
      OS << "<unknown>:";
    OS << DL.getLine() << ":" << DL.getCol();
    Loc = S;
  }

  Out << "c2go-lto: stack->heap in " << F->getName() << " at " << Loc
      << "\n";
}

unsigned PointsTo::reportEscapes() {
  auto has = [&](unsigned NodeId, std::initializer_list<CellKind> Kinds) {
    for (unsigned C : Nodes[NodeId].Pts)
      for (CellKind K : Kinds)
        if (CellKinds[C] == K)
          return true;
    return false;
  };

  unsigned N = 0;
  // #295: dedup per (function, source-alloca). Andersen value-nodes are very
  // fine-grained (every GEP/cast/bitcast produces a fresh node) — deduping at
  // value-node granularity barely moved 1203 → 1200 on SQLite. The user's
  // mental model is "this alloca leaks": collapse all stores reachable from
  // the same alloca cell to one report. Build cell→owner-Value map once
  // (mirrors the LLVM_DEBUG path below) so the inner loop is O(|Pts|).
  DenseMap<unsigned, const Value *> CellOwner;
  if (EscapeDedup)
    for (auto &KV : ObjCell)
      CellOwner[KV.second] = KV.first;
  DenseSet<std::pair<const Function *, const Value *>> SeenEscape;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F)) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI || !SI->getValueOperand()->getType()->isPointerTy())
        continue;
      auto QIt = ValNode.find(SI->getValueOperand());
      auto PIt = ValNode.find(SI->getPointerOperand());
      if (QIt == ValNode.end() || PIt == ValNode.end())
        continue;
      // value side stays precise: a real Stack cell.
      if (!has(QIt->second, {CK_Stack}))
        continue;
      // destination over-approximated: heap, global, or unknown-heap.
      if (!has(PIt->second, {CK_Heap, CK_Global, CK_HeapUnknown}))
        continue;
      // #295: collapse multi-store escapes that all flow from the same
      // source-level alloca inside one function. Walk the value-node's Pts
      // set, dedup per (F, alloca-instruction) tuple. If every alloca in
      // Pts is a repeat-seen for this function, suppress; otherwise mark
      // any new ones and emit one line. Toggle with --c2go-escape-dedup=false.
      if (EscapeDedup) {
        bool AnyNew = false;
        for (unsigned C : Nodes[QIt->second].Pts) {
          if (CellKinds[C] != CK_Stack)
            continue;
          const Value *Alloca = CellOwner.lookup(C);
          if (!Alloca)
            continue; // shouldn't happen for stack cells, be defensive.
          if (SeenEscape.insert({&F, Alloca}).second)
            AnyNew = true;
        }
        if (!AnyNew)
          continue;
      }
      LLVM_DEBUG({
        if (N < 6) {
          DenseMap<unsigned, const Value *> CellOwner;
          for (auto &KV : ObjCell)
            CellOwner[KV.second] = KV.first;
          dbgs() << "store in " << F.getName()
                 << ": value-side Stack cells:\n";
          for (unsigned C : Nodes[QIt->second].Pts)
            if (CellKinds[C] == CK_Stack) {
              const Value *Ow = CellOwner.lookup(C);
              dbgs() << "    cell#" << C << " <- ";
              if (Ow) {
                if (auto *I2 = dyn_cast<Instruction>(Ow))
                  dbgs() << I2->getFunction()->getName() << "::";
                Ow->printAsOperand(dbgs(), false);
              }
              dbgs() << "\n";
            }
        }
      });
      printEscape(SI);
      ++N;
    }
  }
  return N;
}

unsigned PointsTo::run() {
  // Pre-grow the Nodes pool. Andersen-lite typically needs O(1k-10k)
  // value/contents nodes for SQLite-sized modules; reserving up front
  // avoids the worst-case `vector` realloc cost and matches the small-size
  // hint already used on CellKinds / CellContentsNode.
  Nodes.reserve(1024);
  Layout = &M.getDataLayout();
  seedObjects();
  buildConstraints();
  solve();
  return reportEscapes();
}

} // namespace

namespace llvm {
namespace c2go {

bool runAndersenEscapeAudit(Module &M, raw_ostream &Out) {
  PointsTo PT(M, Out);
  unsigned N = PT.run();
  Out << "c2go-lto: " << N << " stack-address escape point(s)\n";
  return N == 0;
}

} // namespace c2go
} // namespace llvm
