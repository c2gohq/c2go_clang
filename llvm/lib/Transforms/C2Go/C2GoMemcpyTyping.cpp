//===- C2GoMemcpyTyping.cpp - Auto-type memcpy/memmove --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go-memcpy-typing pass. For each call to one of c2go-libc's
// memcpy / memmove / memset stub functions, decide how to lower it based
// on the destination's per-call `!c2go.elem.type` metadata that clang's
// CodeGen attaches:
//
//   Metadata form:
//     !c2go.elem.type !N
//     !N = !{!"struct.<TypeName>", i64 <element_count>}
//
//   Decision table for @llvm.memcpy / @llvm.memmove (and raw/named
//   c2go-libc memcpy/memmove calls):
//
//     element_count=1 + typeinfo  → runtime.typedmemmove (write barrier)
//     element_count>1 + typeinfo  → _c2go_typedMemmoveArray
//     no typeinfo, small const n  → @llvm.memcpy/memmove intrinsic
//                                   (backend inlines with ldp/stp)
//     no typeinfo, large/unknown  → matching libc.Memcpy/Memmove call
//
// Raw memset/bzero calls are handled here as well. Small constant operations
// remain intrinsics for target inlining; dynamic/large operations become
// package-qualified libc.Memset calls before GC lowering. No raw memory
// symbol may survive into SelectionDAG because its platform C ABI does not
// match the GoABI0 implementation in c2go-libc.
//
// The size-threshold path (`canStayInline`) defends against the
// backend's libcall fallback emitting a reference to a bare memory symbol
// when the size isn't inline-able. Anything above the threshold gets routed
// to an explicit c2go-libc Go-side call so the ABI and GC boundary are
// explicit before instruction selection.
//
// The typed paths invoke Go runtime's typed copy primitives (the
// linkname `_c2go_typedmemmove` resolves to runtime.typedmemmove), which
// fire the bulk write barrier so GC-pointer fields are tracked
// correctly when copying managed structs. Without these, copying a
// struct containing managed pointers would lose the destination
// pointers from the GC's view, corrupting the live-pointer set under
// the next GC cycle.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/C2Go/C2GoMemcpyTyping.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/C2Go/C2GoCommon.h"
#include "llvm/Transforms/C2Go/C2GoProtocol.h"

using namespace llvm;

#define DEBUG_TYPE "c2go-memcpy-typing"

namespace {

// c2go-libc byte-blob fallback symbols the pass routes @llvm.mem* to. These
// are the c2go-libc mem* implementations' own (lowercase) ABI0 symbols — the
// natural libc names, since the C2GoExportName casing only capitalises the
// generated .go binding, never the .s/IR symbol. c2go-libc provides them
// (mem_funcs.go today; a C source/mem.c is the intended replacement).
constexpr StringRef kMemcpyName  = "github.com/c2gohq/c2go_libc.memcpy";
constexpr StringRef kMemmoveName = "github.com/c2gohq/c2go_libc.memmove";
constexpr StringRef kMemsetName  = "github.com/c2gohq/c2go_libc.memset";

// Runtime typed-copy helpers — provided by c2gobind's runtimeHelpers.
//   _c2go_typedmemmove(typ *_type, dst, src unsafe.Pointer)
//   _c2go_typedMemmoveArray(typ *_type, dst, src unsafe.Pointer, n uintptr)
constexpr StringRef kTypedMemmoveName      = "_c2go_typedmemmove";
constexpr StringRef kTypedMemmoveArrayName = "_c2go_typedMemmoveArray";

// Threshold above which we route non-typed @llvm.memcpy/memmove/memset
// intrinsics to an explicit c2go-libc Go-side call rather than leaving
// them for the backend. The backend's libcall fallback would emit a
// reference to the bare `memcpy`/`memset` symbol — which doesn't exist
// in c2go-mode link layout (c2go-libc exports `libc.Memcpy` etc.).
// Anything ≤ this stays as an intrinsic so the backend can inline it
// with ldp/stp pairs.
constexpr uint64_t kInlineByteThreshold = 64;

enum class StubKind { Memcpy, Memmove, Memset, Bzero };

struct Hit {
  CallInst *Call;
  StubKind  Kind;
};

// enforceGoABI0AndOptLeaf unifies the CC=GoABI0 + optional gc-leaf-function
// retrofit applied to every helper declaration in this pass. Both newly
// created and reused (pre-existing) declarations must go through this so
// IR fixtures / upstream passes that pre-declared a helper with the
// default C CC don't slip through with a CC mismatch at the call site
// (the call sites unconditionally set goabi0cc — declaration must match).
// IsLeaf=true is for the typed-copy helpers (`_c2go_typedmemmove` /
// `_c2go_typedMemmoveArray`) whose Go-side shims are `//go:nosplit`;
// IsLeaf=false is for libc.Memcpy/Memmove/Memset which are genuine
// non-leaf libc paths and MUST be statepoint-wrapped by RS4GC.
//
// #429 — implementation lifted to C2GoCommon so WriteBarriers,
// EscapeCheck and LoopPoll share the same retrofit + sweep. This file
// keeps a thin inline alias purely so the existing call cluster below
// reads naturally.
static inline bool enforceGoABI0AndOptLeaf(Function *F, bool IsLeaf) {
  return llvm::c2go::enforceGoABI0AndOptLeaf(F, IsLeaf);
}

// extractElemTypeMD pulls (typeName, count) from a call's
// `!c2go.elem.type` metadata, returning empty StringRef + count=0 when
// the metadata is missing or malformed.
static std::pair<StringRef, uint64_t> extractElemTypeMD(CallInst *CI) {
  MDNode *MD = CI->getMetadata(c2go::kElemTypeMD);
  if (!MD || MD->getNumOperands() < 2)
    return {StringRef(), 0};
  auto *MS = dyn_cast<MDString>(MD->getOperand(0).get());
  auto *CountMD = dyn_cast<ConstantAsMetadata>(MD->getOperand(1).get());
  if (!MS || !CountMD)
    return {StringRef(), 0};
  auto *CI2 = dyn_cast<ConstantInt>(CountMD->getValue());
  if (!CI2)
    return {StringRef(), 0};
  return {MS->getString(), CI2->getZExtValue()};
}

// getOrDeclareTypedMemmove returns the runtime typed-copy helper as a
// Function*, declaring it (external linkage) if not already present.
//
// Signature: `func _c2go_typedmemmove(typ, dst, src unsafe.Pointer)`
// Go-side ABI0; in IR this is `void(ptr, ptr, ptr)`. Declaration MUST
// be GoABI0 — the Go-linker generates the ABI0 entry that reads args
// from the stack; if the call site uses the default AAPCS register
// convention the entry reads garbage. Each call site must also set
// the CC explicitly (CallInst inherits FunctionType, not CC).
static Function *getOrDeclareTypedMemmove(Module &M) {
  if (auto *F = M.getFunction(kTypedMemmoveName)) {
    // GPT round 2 P1 C: pre-existing declarations (from earlier
    // frontend lowering, or IR fixtures) may still have the default C
    // CC. Force GoABI0 on reuse so the callee CC matches the
    // `goabi0cc` we set on every CallInst below. gc-leaf-function is
    // also retrofitted here — see #378 rationale in the create path.
    enforceGoABI0AndOptLeaf(F, /*IsLeaf=*/true);
    return F;
  }
  LLVMContext &Ctx = M.getContext();
  Type *Void = Type::getVoidTy(Ctx);
  Type *Ptr  = PointerType::getUnqual(Ctx);
  FunctionType *FT = FunctionType::get(Void, {Ptr, Ptr, Ptr}, /*isVarArg=*/false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, kTypedMemmoveName, &M);
  // #378 follow-up to round 24 Blocker #3 closure: mirror _c2go_writePtr.
  // Go runtime.typedmemmove is //go:nosplit (no morestack, no statepoint
  // needed). When MemcpyTyping runs in Layer 3 (c2go-lto post-inliner) it
  // emits new _c2go_typedmemmove calls AFTER RS4GC; without this attr,
  // RS4GC bookkeeping would treat them as safepoint-introducing on the
  // Layer 1 path too.
  enforceGoABI0AndOptLeaf(F, /*IsLeaf=*/true);
  return F;
}

// Signature: `func _c2go_typedMemmoveArray(typ, dst, src unsafe.Pointer, n uintptr)`
// Same GoABI0 contract as getOrDeclareTypedMemmove above.
static Function *getOrDeclareTypedMemmoveArray(Module &M) {
  if (auto *F = M.getFunction(kTypedMemmoveArrayName)) {
    enforceGoABI0AndOptLeaf(F, /*IsLeaf=*/true);
    return F;
  }
  LLVMContext &Ctx = M.getContext();
  Type *Void = Type::getVoidTy(Ctx);
  Type *Ptr  = PointerType::getUnqual(Ctx);
  Type *I64  = Type::getInt64Ty(Ctx);
  FunctionType *FT =
      FunctionType::get(Void, {Ptr, Ptr, Ptr, I64}, /*isVarArg=*/false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 kTypedMemmoveArrayName, &M);
  // Array helper is also //go:nosplit after the Round 23/24 push-forward
  // (c2go-bind emit.go marks it TEXTFLAG_NOSPLIT, mirroring the singleton
  // _c2go_typedmemmove). Mark gc-leaf-function so Layer-3 (c2go-lto
  // post-inliner) MemcpyTyping insertions stay GC-leaf on the Layer-1
  // path too — symmetric with getOrDeclareTypedMemmove above.
  enforceGoABI0AndOptLeaf(F, /*IsLeaf=*/true);
  return F;
}

// findTypeinfoGlobal looks up the `c2go.typeinfo.<typeName>` global emitted
// by C2GoMallocReplacement for c2go-managed struct types. Returns
// nullptr if absent (meaning we shouldn't dispatch to typedmemmove for
// this type — fall back to the intrinsic).
static GlobalVariable *findTypeinfoGlobal(Module &M, StringRef TypeName) {
  std::string GName = (llvm::c2go::kTypeinfoGVPrefix + TypeName).str();
  return M.getNamedGlobal(GName);
}

// canStayInline returns true when this size is safe to leave as an
// @llvm.mem* intrinsic — backend will inline it. Anything that may
// produce a backend libcall must be redirected to an explicit c2go-libc
// symbol because the bare libc name (`memcpy`, `memset`) isn't part of
// the c2go-mode link surface.
static bool canStayInline(Value *NArg) {
  auto *CI = dyn_cast<ConstantInt>(NArg);
  if (!CI)
    return false; // unknown size → backend libcall risk → fallback
  return CI->getZExtValue() <= kInlineByteThreshold;
}

// adaptArgAS bridges the address-space mismatch between an AS1 (managed)
// pointer at a call site and an AS0 helper-parameter declaration.
//
// Why AS0 cast is safe here — two distinct cases:
//
//   (a) Typed helpers `_c2go_typedmemmove` and `_c2go_typedMemmoveArray`
//       carry `gc-leaf-function` (RS4GC skip-wrap) AND are `//go:nosplit`
//       on the Go side (no morestack, no statepoint needed). The AS1→AS0
//       cast at the boundary loses no load-bearing info because no GC
//       relocate happens across the call.
//
//   (b) `libc.Memcpy/Memmove/Memset` are NOT `gc-leaf-function` — they
//       are genuinely non-leaf (real libc copy paths) and RS4GC DOES
//       statepoint-wrap them. The AS1→AS0 cast is still sound because
//       the #384 c2go-gc carve-out (C2GoGC::isGCManagedPointer / the
//       RS4GC base-pointer search) walks across the addrspacecast and
//       still recognizes the AS1 root, so the spilled-and-relocated
//       pointer on the caller side stays valid across the safepoint.
//       See c2go-libc-memmove-statepoint.ll for the invariant.
//
// Pattern mirrors C2GoWriteBarriers' slow-path: helper signatures stay
// AS0 (libc.Memmove is genuinely unmanaged), and only the managed-record
// callers pay the addrspacecast.
static Value *adaptArgAS(IRBuilder<> &B, Value *V, Type *DeclaredTy) {
  return V->getType() == DeclaredTy ? V : B.CreateAddrSpaceCast(V, DeclaredTy);
}

// Helper classification (two distinct buckets — do not conflate):
//
//   (1) Typed-copy helpers `_c2go_typedmemmove` (3-arg) and
//       `_c2go_typedMemmoveArray` (4-arg) ARE `gc-leaf-function`.
//       After the Round 23/24 push-forward in c2go-bind emit.go both
//       carry `//go:nosplit` (TEXTFLAG_NOSPLIT) on the Go side — they
//       tail-call runtime.typedmemmove / runtime.typedArrayClear-style
//       nosplit helpers, emit no morestack prelude, and so cannot
//       grow / relocate the stack across the call. Marking them
//       `gc-leaf-function` here makes RS4GC skip the statepoint wrap on
//       both the Layer-1 (post-clang) and Layer-3 (c2go-lto
//       post-inliner) entries to MemcpyTyping. This mirrors
//       C2GoWriteBarriers' fast-path `_c2go_writePtr`.
//
//   (2) `libc.Memcpy/Memmove/Memset` are NOT `gc-leaf-function`. They
//       are real non-leaf libc copy paths and RS4GC MUST statepoint-wrap
//       them so the caller's managed roots survive the call. AS1→AS0
//       boundary safety is preserved by the #384 c2go-gc carve-out
//       (C2GoGC::isGCManagedPointer / the RS4GC base-pointer search
//       walks across the addrspacecast and tracks the AS1 root). This is
//       symmetric with C2GoWriteBarriers' slow-path entry — splittable
//       callees do go through the statepoint machinery.
//
// Compile-time invariant pinning: c2go-libc-memmove-statepoint.ll runs
// `opt -passes=c2go-memcpy-typing,c2go-gc-setup,rewrite-statepoints-for-gc`
// on an untyped large AS1 memcpy and asserts (a) libc.Memmove IS wrapped
// in `gc.statepoint` and (b) _c2go_typedmemmove is NOT wrapped — so an
// accidental future attribute swap fails the LIT before it ships.
//
// getOrDeclareLibcMemcpy returns an external declaration for
// c2go-libc.Memcpy with GoABI0 calling convention. Direct memcpy calls must
// retain memcpy semantics here: routing them through libc.Memmove makes
// musl's memmove -> memcpy fast path recurse back into memmove.
//   libc.Memcpy signature: ptr(ptr, ptr, i64)
static Function *getOrDeclareLibcMemcpy(Module &M) {
  if (auto *F = M.getFunction(kMemcpyName)) {
    enforceGoABI0AndOptLeaf(F, /*IsLeaf=*/false);
    return F;
  }
  LLVMContext &Ctx = M.getContext();
  Type *Ptr = PointerType::getUnqual(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  FunctionType *FT = FunctionType::get(Ptr, {Ptr, Ptr, I64}, false);
  Function *F =
      Function::Create(FT, GlobalValue::ExternalLinkage, kMemcpyName, &M);
  enforceGoABI0AndOptLeaf(F, /*IsLeaf=*/false);
  return F;
}

// getOrDeclareLibcMemmove returns an external declaration for
// c2go-libc.Memmove with GoABI0 calling convention — same as if it
// had been declared in a c2go-libc header with c2go_linkname. GoABI0
// makes call sites push args to stack so the Go-linker-generated ABI0
// wrapper entry on libc.Memmove finds them. Without this CC the call
// would use default AAPCS reg-args and the ABI0 wrapper would read
// garbage from stack, crashing on first dereference.
//   libc.Memmove signature: ptr(ptr, ptr, i64)
static Function *getOrDeclareLibcMemmove(Module &M) {
  if (auto *F = M.getFunction(kMemmoveName)) {
    // #399 aux audit #4: symmetric reuse-path CC retrofit. Pre-existing
    // declarations (from earlier passes or hand-written IR fixtures)
    // could still hold the default C CC; without this enforce, the call
    // site below would emit `call goabi0cc @libc.Memmove` against a
    // default-CC declaration → Go-linker ABI0 wrapper reads garbage
    // from stack on entry (same failure mode as #229 documents).
    // IsLeaf=false: libc.Memmove is a real non-leaf libc copy path and
    // RS4GC MUST statepoint-wrap it (see c2go-libc-memmove-statepoint.ll).
    enforceGoABI0AndOptLeaf(F, /*IsLeaf=*/false);
    return F;
  }
  LLVMContext &Ctx = M.getContext();
  Type *Ptr = PointerType::getUnqual(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  FunctionType *FT = FunctionType::get(Ptr, {Ptr, Ptr, I64}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 kMemmoveName, &M);
  enforceGoABI0AndOptLeaf(F, /*IsLeaf=*/false);
  return F;
}

// getOrDeclareLibcMemset returns an external declaration for
// c2go-libc.Memset (GoABI0). Used to rewrite @llvm.memset intrinsics
// BEFORE SelectionDAG sees them, which prevents the SelectionDAG
// memset→bzero substitution (#229) — that substitution emits a raw
// `bzero` external decl with AAPCS CC, mismatching the Go-side
// `sqlitepkg.bzero` shim's auto-generated ABI0 entry → SIGBUS.
//   libc.Memset signature: ptr(ptr, i32, i64)
static Function *getOrDeclareLibcMemset(Module &M) {
  if (auto *F = M.getFunction(kMemsetName)) {
    // #399 aux audit #4: symmetric reuse-path CC retrofit. Same rationale
    // as getOrDeclareLibcMemmove above — pre-declared default-CC must be
    // upgraded so the goabi0cc CallInst we emit matches.
    enforceGoABI0AndOptLeaf(F, /*IsLeaf=*/false);
    return F;
  }
  LLVMContext &Ctx = M.getContext();
  Type *Ptr = PointerType::getUnqual(Ctx);
  Type *I32 = Type::getInt32Ty(Ctx);
  Type *I64 = Type::getInt64Ty(Ctx);
  FunctionType *FT = FunctionType::get(Ptr, {Ptr, I32, I64}, false);
  Function *F = Function::Create(FT, GlobalValue::ExternalLinkage,
                                 kMemsetName, &M);
  enforceGoABI0AndOptLeaf(F, /*IsLeaf=*/false);
  return F;
}

static CallInst *emitLibcMemsetCall(IRBuilder<> &B, Module &M, Value *Dst,
                                    Value *Val, Value *N) {
  Function *F = getOrDeclareLibcMemset(M);
  FunctionType *FT = F->getFunctionType();
  Value *DstC = adaptArgAS(B, Dst, FT->getParamType(0));
  Value *ValI32 = B.CreateZExtOrTrunc(Val, B.getInt32Ty());
  Value *NCast = B.CreateZExtOrTrunc(N, B.getInt64Ty());
  CallInst *Call = B.CreateCall(F, {DstC, ValI32, NCast});
  Call->setCallingConv(llvm::CallingConv::GoABI0);
  return Call;
}

// emitTypedOrLibcCopyCall emits a single GoABI0 call to the typed-copy
// helper (when Typeinfo is non-null) or the requested libc fallback (when
// Typeinfo is null). Three-way dispatch:
//   Typeinfo + Count<=1  → _c2go_typedmemmove(typ, dst, src)
//   Typeinfo + Count>1   → _c2go_typedMemmoveArray(typ, dst, src, count)
//   Typeinfo == nullptr  → libc.Memcpy/Memmove(dst, src, n)
// AS1↔AS0 boundary handled via adaptArgAS for each declared param type.
// Callers retain control of the "inline intrinsic" path (canStayInline)
// and of per-call inline/erase/return-value cleanup — this helper only
// owns the dispatch+call emission shared between rewriteMemcpyMemmove
// and rewriteIntrinsicMemTransfer.
static void emitTypedOrLibcCopyCall(IRBuilder<> &B, Module &M,
                                    GlobalVariable *Typeinfo, uint64_t Count,
                                    Value *Dst, Value *Src, Value *N,
                                    StubKind FallbackKind) {
  if (Typeinfo) {
    Function *F = (Count <= 1) ? getOrDeclareTypedMemmove(M)
                               : getOrDeclareTypedMemmoveArray(M);
    FunctionType *FT = F->getFunctionType();
    Value *DstC = adaptArgAS(B, Dst, FT->getParamType(1));
    Value *SrcC = adaptArgAS(B, Src, FT->getParamType(2));
    SmallVector<Value *, 4> Args{Typeinfo, DstC, SrcC};
    if (Count > 1)
      Args.push_back(ConstantInt::get(B.getInt64Ty(), Count));
    B.CreateCall(F, Args)->setCallingConv(llvm::CallingConv::GoABI0);
    return;
  }
  Function *F = FallbackKind == StubKind::Memcpy ? getOrDeclareLibcMemcpy(M)
                                                 : getOrDeclareLibcMemmove(M);
  FunctionType *FT = F->getFunctionType();
  Value *DstC = adaptArgAS(B, Dst, FT->getParamType(0));
  Value *SrcC = adaptArgAS(B, Src, FT->getParamType(1));
  Value *NCast = B.CreateZExtOrTrunc(N, B.getInt64Ty());
  B.CreateCall(F, {DstC, SrcC, NCast})
      ->setCallingConv(llvm::CallingConv::GoABI0);
}

// rewriteMemcpyMemmove handles c2go-libc.Memcpy and c2go-libc.Memmove.
// Decision tree:
//   1. has typeinfo → runtime.typedmemmove (single) / Array (multi)
//   2. else, small constant size → LLVM intrinsic (backend inlines)
//   3. else (unknown or large size) → matching package-qualified
//      libc.Memcpy/Memmove Go-side call
static void rewriteMemcpyMemmove(Hit H) {
  CallInst *CI = H.Call;
  IRBuilder<> B(CI);
  Module *M  = CI->getModule();
  Value *Dst = CI->getArgOperand(0);
  Value *Src = CI->getArgOperand(1);
  Value *N   = CI->getArgOperand(2);

  // Inspect `!c2go.elem.type` for a typed-dispatch hint.
  auto [TypeName, Count] = extractElemTypeMD(CI);
  GlobalVariable *Typeinfo = nullptr;
  if (!TypeName.empty())
    Typeinfo = findTypeinfoGlobal(*M, TypeName);

  if (!Typeinfo && canStayInline(N)) {
    if (H.Kind == StubKind::Memcpy)
      B.CreateMemCpy(Dst, MaybeAlign(), Src, MaybeAlign(), N, false);
    else
      B.CreateMemMove(Dst, MaybeAlign(), Src, MaybeAlign(), N, false);
  } else {
    // Typed (single/array) or non-inlinable byte blob → shared helper.
    // Preserve the original operation for the untyped fallback. In
    // particular, musl memmove deliberately calls memcpy on its non-overlap
    // fast path; redirecting that call back to memmove causes recursion.
    emitTypedOrLibcCopyCall(B, *M, Typeinfo, Count, Dst, Src, N, H.Kind);
  }

  // ISO C: memcpy/memmove return dst.
  if (!CI->getType()->isVoidTy()) {
    Value *RV = Dst;
    if (RV->getType() != CI->getType())
      RV = B.CreateBitCast(RV, CI->getType());
    CI->replaceAllUsesWith(RV);
  }
  CI->eraseFromParent();
}

// Rewrite direct calls that escaped builtin folding. Public c2go headers keep
// memset/bzero plain so LLVM may form intrinsics, but -fno-builtin and libc's
// own implementations can still leave raw calls. Those raw names cannot use
// the platform C ABI because their c2go-libc implementations are GoABI0.
static void rewriteMemsetBzero(Hit H) {
  CallInst *CI = H.Call;
  IRBuilder<> B(CI);
  Module *M = CI->getModule();
  Value *Dst = CI->getArgOperand(0);
  Value *Val = H.Kind == StubKind::Bzero ? ConstantInt::get(B.getInt8Ty(), 0)
                                         : CI->getArgOperand(1);
  Value *N =
      H.Kind == StubKind::Bzero ? CI->getArgOperand(1) : CI->getArgOperand(2);

  if (canStayInline(N)) {
    Value *ValI8 = B.CreateZExtOrTrunc(Val, B.getInt8Ty());
    B.CreateMemSet(Dst, ValI8, N, MaybeAlign(), false);
  } else {
    emitLibcMemsetCall(B, *M, Dst, Val, N);
  }

  // ISO C memset returns dst; BSD bzero is void.
  if (!CI->getType()->isVoidTy()) {
    Value *RV = Dst;
    if (RV->getType() != CI->getType())
      RV = B.CreateBitCast(RV, CI->getType());
    CI->replaceAllUsesWith(RV);
  }
  CI->eraseFromParent();
}

} // namespace

// rewriteIntrinsicMemTransfer handles a @llvm.memcpy / @llvm.memmove
// intrinsic. Three outcomes:
//   1. has c2go typeinfo → runtime.typedmemmove (write-barrier-aware)
//   2. small constant size → leave intrinsic (backend inlines)
//   3. else → libc.Memmove Go-side call (avoid backend libcall to a
//      bare `memcpy` symbol that c2go-mode link doesn't carry)
static bool rewriteIntrinsicMemTransfer(MemTransferInst *MI) {
  Module *M = MI->getModule();
  Value *Dst = MI->getRawDest();
  Value *Src = MI->getRawSource();
  Value *N   = MI->getLength();

  // Decide typed-dispatch up front so we can share the inline/libc gate.
  auto [TypeName, Count] = extractElemTypeMD(MI);
  GlobalVariable *Typeinfo = nullptr;
  if (!TypeName.empty())
    Typeinfo = findTypeinfoGlobal(*M, TypeName);

  // No typeinfo + small size → leave intrinsic for backend to inline.
  if (!Typeinfo && canStayInline(N))
    return false;

  IRBuilder<> B(MI);
  // Keep the historical conservative fallback for IR intrinsics. Explicit
  // source-level memcpy calls are distinguished in rewriteMemcpyMemmove.
  emitTypedOrLibcCopyCall(B, *M, Typeinfo, Count, Dst, Src, N,
                          StubKind::Memmove);
  MI->eraseFromParent();
  return true;
}

PreservedAnalyses C2GoMemcpyTypingPass::run(Module &M,
                                            ModuleAnalysisManager &) {
  LLVM_DEBUG(dbgs() << "c2go-memcpy-typing: scanning " << M.getName() << "\n");

  // Collect both:
  //   (a) direct raw calls and calls to c2go-libc's named memory stubs — for
  //       the path where clang did NOT fold to an intrinsic (e.g. via
  //       -fno-builtin or while compiling libc itself).
  //   (b) @llvm.memcpy / @llvm.memmove intrinsic calls — for the common
  //       path where clang folds `memcpy(...)` directly to the
  //       intrinsic. These get the typed-dispatch treatment only when
  //       they carry `!c2go.elem.type` metadata.
  //
  SmallVector<Hit, 32> Hits;
  SmallVector<MemTransferInst *, 32> Transfers;
  SmallVector<MemSetInst *, 32> Sets; // #229
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (Instruction &I : instructions(F)) {
      if (auto *MI = dyn_cast<MemTransferInst>(&I)) {
        // Skip @llvm.memcpy.inline: the inline form is a hard constraint
        // from the frontend ("must lower as inline, no libcall"), and
        // rewriting it into a `runtime.typedmemmove` call would both
        // violate that contract and break MemCpyOpt invariants
        // downstream. (LLVM has no llvm.memmove.inline, so checking
        // memcpy_inline alone is sufficient.)
        if (MI->getIntrinsicID() == Intrinsic::memcpy_inline)
          continue;
        Transfers.push_back(MI);
        continue;
      }
      if (auto *MS = dyn_cast<MemSetInst>(&I)) {
        Sets.push_back(MS);
        continue;
      }
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      Function *Callee = CI->getCalledFunction();
      if (!Callee)
        continue;
      StringRef Name = Callee->getName();
      if (Name == kMemcpyName || Name == "memcpy")
        Hits.push_back({CI, StubKind::Memcpy});
      else if (Name == kMemmoveName || Name == "memmove")
        Hits.push_back({CI, StubKind::Memmove});
      else if (Name == kMemsetName || Name == "memset")
        Hits.push_back({CI, StubKind::Memset});
      else if (Name == "bzero")
        Hits.push_back({CI, StubKind::Bzero});
    }
  }

  bool Changed = false;
  if (!Hits.empty()) {
    LLVM_DEBUG(dbgs() << "c2go-memcpy-typing: rewriting " << Hits.size()
                      << " stub calls\n");
    for (Hit H : Hits) {
      if (H.Kind == StubKind::Memcpy || H.Kind == StubKind::Memmove)
        rewriteMemcpyMemmove(H);
      else
        rewriteMemsetBzero(H);
    }
    Changed = true;
  }
  for (MemTransferInst *MI : Transfers)
    if (rewriteIntrinsicMemTransfer(MI))
      Changed = true;

  // #229 — rewrite @llvm.memset intrinsics to direct c2go-libc.Memset
  // calls BEFORE SelectionDAG runs. The SelectionDAG memset/bzero
  // lowering would otherwise substitute `memset(p, 0, n)` →
  // `bzero(p, n)` when the value is zero (SelectionDAG.cpp:8890), and
  // the resulting raw `@bzero` external decl is emitted with AAPCS
  // register-args (no GoABI0 CC) — mismatching the Go-side bzero
  // shim's auto-generated ABI0 entry that reads args from the stack.
  // Routing through c2go-libc.Memset gets the GoABI0 CC applied here
  // and goes straight to the Go-side implementation, no bzero detour.
  for (MemSetInst *MS : Sets) {
    if (canStayInline(MS->getLength()))
      continue; // small const size → backend inlines (no libcall risk)
    IRBuilder<> B(MS);
    Value *Dst = MS->getRawDest();
    Value *Val = MS->getValue();      // i8
    Value *N   = MS->getLength();
    // Mirror memcpy/memmove: clang may lower `*managed = 0` (or a large
    // managed-pointer-clear) to `llvm.memset.p1.i64` with AS1 Dst, but
    // libc.Memset declaration is AS0. emitLibcMemsetCall performs the cast.
    emitLibcMemsetCall(B, M, Dst, Val, N);
    MS->eraseFromParent();
    Changed = true;
  }

  // #414 aux self-audit Track B (a): pass-end CC enforce.
  //
  // enforceGoABI0AndOptLeaf (#399 audit #4) only normalises the helper's
  // declared CC at decl-time. Call sites that were emitted by EARLIER
  // passes against the same helper (e.g. an upstream lowering, or a
  // hand-written IR fixture's pre-existing CallInst) are NOT retroactively
  // updated — `setCallingConv` on the Function does not propagate to
  // existing CallInst CCs. In release the IR verifier accepts a CC
  // mismatch silently, and the Go-linker-generated ABI0 entry on the
  // callee would then read garbage from the stack at runtime (same
  // failure mode as #229 / #399). Sweep every use of each helper
  // helpers this pass declares; on mismatch the sweep rewrites the
  // call site CC to match the declaration AND emits a one-line
  // diagnostic so the upstream emitter that produced the wrong CC is
  // visible.
  //
  // #429 — sweep impl lifted to C2GoCommon; WriteBarriers / EscapeCheck
  // / LoopPoll now share the same single source of truth.
  //
  // #453 — OR the sweep's "did I rewrite something?" return into the
  // pass-level Changed so a release-tier mismatch fix flips us to
  // PreservedAnalyses::none() rather than misreporting "no change".
  Changed |= llvm::c2go::enforceCallSiteCC(M.getFunction(kTypedMemmoveName));
  Changed |= llvm::c2go::enforceCallSiteCC(M.getFunction(kTypedMemmoveArrayName));
  Changed |= llvm::c2go::enforceCallSiteCC(M.getFunction(kMemcpyName));
  Changed |= llvm::c2go::enforceCallSiteCC(M.getFunction(kMemmoveName));
  Changed |= llvm::c2go::enforceCallSiteCC(M.getFunction(kMemsetName));

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
