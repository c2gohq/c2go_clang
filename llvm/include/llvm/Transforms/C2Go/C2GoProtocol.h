//===- C2GoProtocol.h - Shared c2go IR-level protocol constants -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #463 + #465 (tech-debt): cross-component single-source for IR-level
// protocol constants whose textual spelling must stay byte-identical between:
//
//   * the clang frontend (CodeGenModule.cpp + CGC2GoTypeInfo.cpp +
//     CGC2GoManifestHelpers.cpp + CGDecl.cpp) that STAMPS the contract into
//     the .bc as module flags, named metadata or instruction MD;
//   * the LLVM passes (C2GoSafepoint.cpp, C2GoCommon.cpp,
//     C2GoGCMaskUtils.cpp, C2GoMemcpyTyping.cpp...) and selected upstream
//     passes (LoopIdiomRecognize, MemCpyOptimizer, StackColoring) that
//     READ the contract back;
//   * the c2go-lto tool that STITCHES per-TU contracts at link time;
//   * any future c2go-lto / opt-driven LIT that asserts the contract.
//
// Pre-#463 the legacy managed-alloca seed/default list lived as a hard-coded
// `StringSet<>` inside C2GoSafepoint.cpp AND as a `static constexpr StringRef[]`
// inside CodeGenModule.cpp. Keeping the two in lock-step by hand was a
// foot-gun: a one-side edit silently drifts the contract. #465 widens the
// same single-source treatment to the rest of the c2go.* module-flag /
// named-MD / instruction-MD vocabulary.
//
// This header carries the canonical list as `constexpr StringLiteral`s,
// header-only so neither the frontend nor the pass picks up a new link-time
// dependency. The names are tiny and reside in .rodata; the duplication
// elimination is at the source level only.
//
// LIT files intentionally still use the raw spelling — IR text in LIT
// does not include C++ headers and the strings need to read naturally.
//
// NOTE (#463 follow-up #465): GPT review suggests dropping the two
// gc-leaf-function entries (`_c2go_typedMemmoveArray` and
// `runtime.gcWriteBarrier`) from the legacy managed-alloca seed list — they
// are nosplit leaves on the Go side, so flagging them here is a no-op at
// best and risks re-classifying them as safepoint-bearing if a future pass
// trusts the list verbatim. Out-of-scope for #463 (which is the
// single-source consolidation); tracked as #465 follow-up.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOPROTOCOL_H
#define LLVM_TRANSFORMS_C2GO_C2GOPROTOCOL_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace llvm {
namespace c2go {

//===----------------------------------------------------------------------===//
// Module flags (LLVM module-level metadata, attached via
// Module::addModuleFlag). Producer: clang CodeGenModule.cpp.
// Consumer: c2go-lto.cpp (option propagation + CC violation merge) and a
// few LLVM passes (e.g. C2GoCommon::recordCCViolation).
// Lifetime: stamped at end of frontend, survives through link; passes are
// free to update Module::Max-merged counters (cc.violations).
//===----------------------------------------------------------------------===//

/// Module flag (uint32_t 1) that marks a Module as compiled under the c2go
/// pipeline. Producer: CodeGenModule::Release (stamped once per TU when the
/// frontend selects GoABI0 emission). Consumer: every c2go-aware backend /
/// pass that must early-exit on non-c2go modules — AArch64 backend
/// (RegisterInfo / FrameLowering / ISelLowering / CallLowering / InstrInfo /
/// AsmPrinter / C2GoFrameEmitter / C2GoLeafABI / C2GoPtrSlotLiveness),
/// generic StackColoring, and the C2Go transform passes (C2GoLoopPoll,
/// C2GoEscapeCheck, C2GoGCSetup, C2GoWriteBarriers, ...).
/// Lifetime: stamped at end of frontend, never re-written; c2go-lto MUST
/// preserve it across the llvm-link merge (Module::Error merge enforces).
inline constexpr StringLiteral kGoabiModuleFlag = "c2go.goabi";

/// Module flag carrying the requested optimisation level (0/1/2/3).
/// Producer: CodeGenModule. Consumer: c2go-lto (rebuilds PipelineTuningOptions
/// when running its own opt+codegen passes).
inline constexpr StringLiteral kOptLevelFlag = "c2go.opt-level";

/// Module flag (uint32_t 1) carried only by the clang-driver c2go-lto route.
/// It says that the per-TU optimizer deliberately deferred LoopPoll, GCSetup,
/// RS4GC/FoldAllocaRelocates (or the lightweight safepoint pass), and the late
/// leaf passes. c2go-lto consumes the flag after linking and inlining, runs
/// that sequence exactly once on the combined module, then changes the value to
/// 0.
inline constexpr StringLiteral kLTOPreLinkFlag = "c2go.lto.prelink";

/// Module flag carrying the requested -mcpu= value as a string.
/// Producer: CodeGenModule. Consumer: c2go-lto.
inline constexpr StringLiteral kTargetCpuFlag = "c2go.target-cpu";

/// Module flag carrying the requested -target-feature= list as a string.
/// Producer: CodeGenModule. Consumer: c2go-lto.
inline constexpr StringLiteral kTargetFeaturesFlag = "c2go.target-features";

/// Module flag carrying the final Go import path. Besides manifest identity,
/// this resolves c2go_linkname targets in the same package to their local LLVM
/// and Plan 9 symbol suffix.
inline constexpr StringLiteral kPackagePathModuleFlag = "c2go.pkgpath";

/// Schema generation expected in the per-TU c2go.manifest.json metadata.
/// Schema v2 and later must never fall back to the lossy legacy IR
/// reconstruction path when that metadata is missing.
inline constexpr StringLiteral kManifestSchemaModuleFlag =
    "c2go.manifest.schema";
inline constexpr unsigned kManifestSchemaVersion = 2;

/// Module flag carrying the running count of CC (calling-convention)
/// violations detected by C2GoCommon::enforceCallSiteCC. Merged with
/// Module::Max across TUs at link time.
/// Producer + consumer: C2GoCommon, c2go-lto.
inline constexpr StringLiteral kCCViolationsFlag = "c2go.cc.violations";

/// Module flag that gates whether the @c2go.global.gcmask.* manifest has
/// already been collected for this Module. Producer + consumer: c2go-lto
/// (BuildManifest / RewriteGoOwnedGlobals).
inline constexpr StringLiteral kGcmaskCollectedFlag = "c2go.gcmask.collected";

//===----------------------------------------------------------------------===//
// Named metadata (Module-level NamedMDNode keyed by string).
// Producer: clang CodeGenModule.cpp + CGC2GoTypeInfo.cpp +
// CGC2GoManifestHelpers.cpp. Consumer: LLVM passes (C2GoSafepoint,
// C2GoGCMaskUtils...) and c2go-lto.
//===----------------------------------------------------------------------===//

/// Named-metadata key prefix used to attach per-function manifest blobs
/// emitted by CGC2GoManifestHelpers ("c2go.func.<C-name>"). c2go-lto walks
/// any named MD whose name starts with this prefix.
/// Producer: CGC2GoManifestHelpers. Consumer: c2go-lto.
inline constexpr StringLiteral kFuncMDPrefix = "c2go.func.";

/// Named-metadata key PREFIX for c2go-managed struct records. The base
/// `c2go.struct.<X>` NMD marks the struct as managed (presence-only); sibling
/// NMDs with suffixes carry the payload:
///   `.fields`            -> (offset, fieldname) pairs (Plan9 AsmPrinter
///                           consults to rewrite LDR/STR immediates into
///                           symbolic `<Rec>_<Field>` references);
///   `.meta`              -> (managed-str, scheme-str, ptr-offset, linkname);
///   `.godef`             -> Go-side struct text (one MDString);
///   `.union_alts`        -> scheme2 union alternatives (offset/size/flags/
///                           fieldname/alt-typeinfo-name);
///   `.union_alts_go_type`-> per-alt Go-side type expression.
/// Producer: CodeGenTypes (base + `.fields`), CGC2GoTypeInfo (`.union_alts`),
/// CGC2GoManifestHelpers (`.meta`, `.union_alts`, `.union_alts_go_type`,
/// `.godef`). Consumer: AArch64AsmPrinter (Plan9 symbolic offsets),
/// C2GoManifestRebuilder (c2go-lto WF2 types[] rebuild). Use
/// `(kStructMDPrefix + Name).str()` to construct.
inline constexpr StringLiteral kStructMDPrefix = "c2go.struct.";

/// Named-metadata key listing globals whose storage is owned by the Go side
/// (i.e. allocated as Go GC roots rather than C BSS). Each operand is an
/// `!{!"varname"}` MDNode. See #387 / #389.
/// Producer: CGC2GoTypeInfo. Consumer: C2GoGCMaskUtils + c2go-lto.
inline constexpr StringLiteral kGoOwnedGlobalsMDName = "c2go.go_owned_globals";

/// Named-metadata key the frontend stamps and the pass reads to extend the
/// legacy managed-alloca seed/default list. Each operand is an
/// `!{!"symbol"}` MDNode.
inline constexpr StringLiteral kSafepointCalleesMDName =
    "c2go.safepoint.callees";

/// Named metadata preserving c2go_linkname routes for calls LLVM may create
/// late in the middle-end or during code generation (for example a
/// byte-scanning loop -> `strlen`, or `llvm.sin` -> `sin`). Each operand is:
///
///   !{!"<C name>", !"<Go linkname>"}
///
/// Producer: clang's end-of-TU AST walk. C2GoLibCallRoutingPass consumes routes
/// before RS4GC; SelectionDAG only enforces that none escaped that boundary.
/// Only c2go_linkname functions carrying C2GO_GOABI0 are recorded. The consumer
/// resolves a target in kPackagePathModuleFlag to its current-package suffix;
/// cross-package targets retain their full linker path.
inline constexpr StringLiteral kLibCallRoutesMDName = "c2go.libcall.routes";

//===----------------------------------------------------------------------===//
// Global-variable name prefix (not metadata key) used as a side channel
// for GC mask payloads. The corresponding @c2go.global.gcmask.<varname> GV
// carries the gcmask bytes for managed globals.
// Producer: CGC2GoTypeInfo. Consumer: C2GoGCMaskUtils + c2go-lto.
//===----------------------------------------------------------------------===//

/// Prefix for @c2go.global.gcmask.<varname> global variables. Use
/// `(kGlobalGcmaskPrefix + Name).str()` to construct, or `consume_front`
/// to recover the var name.
inline constexpr StringLiteral kGlobalGcmaskPrefix = "c2go.global.gcmask.";

/// Prefix for @c2go.typeinfo.<Name> global variables — the per-record RTTI
/// descriptor symbol c2gobind exports via its `_typeinfo_<Name>` indirection
/// var (the InstPrinter rewrites the symbol load into
/// `MOVD ·_typeinfo_<Name>(SB)`; see #134 / #218 / #385).
/// Producer: CGC2GoTypeInfo (defines the descriptor body), CodeGenTypes (lazy
/// reference), SemaExpr (synthesises an extern VarDecl with this AsmLabel for
/// `__c2go_typeinfo(T)`). Consumer: CodeGenAction (re-binds anon-record GVs
/// to manifest entries), CGExpr (resolves the AsmLabel back to a typeinfo
/// reference), C2GoMemcpyTyping (looks up the descriptor before redirecting
/// to typedmemmove), AArch64Plan9InstPrinter / MCPlan9AsmStreamer (recognise
/// the prefix in symbol emission — these two MC-layer sites keep the literal
/// spelling rather than #include this header, since MC must not depend on
/// Transforms; CMakeLists.txt for llvm/lib/MC has no Transforms link
/// component. Any future re-spelling MUST update those two sites by hand —
/// the kTypeinfoGVPrefix value below is the SOLE source of truth), c2go-lto
/// (preserves through link).
/// Lifetime: emitted at clang CodeGen, must survive llvm-link + LTO.
inline constexpr StringLiteral kTypeinfoGVPrefix = "c2go.typeinfo.";

//===----------------------------------------------------------------------===//
// Instruction-level metadata (attached via Instruction::setMetadata).
// Producer: clang frontend (CGDecl / CGC2GoTypeInfo) + select LLVM
// passes that propagate. Consumer: GC / safepoint passes + a handful of
// upstream passes (LoopIdiomRecognize, MemCpyOptimizer, StackColoring)
// that must NOT drop the tag on the floor when rewriting calls/memcpys.
//===----------------------------------------------------------------------===//

/// Instruction MD carrying the static element type of a memcpy/memmove or
/// the load/store it was synthesised from. Lets C2GoMemcpyTyping reclassify
/// raw `llvm.memcpy` calls into typed `_c2go_typedMemmoveArray` calls when
/// the destination type is GC-relevant.
/// Producer: CGC2GoTypeInfo. Consumer: C2GoMemcpyTyping (must be preserved
/// across LoopIdiomRecognize / MemCpyOptimizer rewrites).
inline constexpr StringLiteral kElemTypeMD = "c2go.elem.type";

/// Instruction MD marking an alloca as a managed-pointer slot (one slot per
/// managed pointer in the frame). Drives C2GoSafepoint's per-PC pointer-
/// slot liveness table and StackColoring's "don't merge" predicate.
/// Producer: CGDecl. Consumer: C2GoSafepoint + StackColoring +
/// AArch64InstrInfo (Plan9 .s emission).
inline constexpr StringLiteral kPtrSlotMD = "c2go.ptr.slot";

/// Instruction MD on an alloca that carries a c2go-managed compound (struct
/// or scheme1/2 union). Operand 0 is an MDString naming the record
/// (`c2go.anon` for nameless). Tells the GC stackmap machinery to scan the
/// alloca according to the per-record bitmap and tells SROA / PromoteMemToReg
/// to leave the slot intact (its address must remain a single GC root).
/// Producer: CGDecl (local managed struct/union) + CGCall (vararg argptrs
/// fan-in array on the callee side).
/// Consumer: C2GoSafepoint (legacy managed-alloca path),
/// StackColoring (don't-merge predicate), SROA (preserve alloca),
/// PromoteMemoryToRegister (don't promote pointer slot).
inline constexpr StringLiteral kPtrManagedMD = "c2go.ptr.managed";

/// Instruction MD on a vararg-pack alloca (the synthetic argptrs[] array and
/// each fixed storage slot CGCall emits for `...` calls). Distinct from
/// `c2go.ptr.managed`: the argptrs[] fan-in is managed as an aggregate while
/// each value slot retains its real scalar/aggregate type. The late GC pass
/// entry-zeroes pointer-bearing fields and folds their relocates so per-PC
/// field expansion can relocate argptrs[]'s stack-interior `&slot` values.
/// Producer: CGCall (vararg lowering only). Consumers: C2GoGCSetup,
/// StackColoring, and the target Plan 9 statepoint emitters.
inline constexpr StringLiteral kVaPackMD = "c2go.va.pack";

/// Instruction MD on an alloca whose containing type has scheme2 union
/// fields with pointer-overlapping bytes. Operand list is an ordered tuple
/// of i64 byte offsets (relative to the alloca) of union-ambiguous pointer
/// words; the backend locals-map emitter (c2goMarkPtrFieldBits /
/// AArch64AsmPrinter Plan9 emission) SKIPS marking these as pointers so the
/// GC does not chase a non-pointer scheme2 alternative. Not attached when
/// the type has no ambiguous words.
/// Producer: CGDecl (attachC2GoUnionAmbigMetadata).
/// Consumer: C2GoFrameEmitter (frame pointer-bitmap), AArch64AsmPrinter
/// (Plan9 .s gclocals emission).
inline constexpr StringLiteral kUnionAmbigWordsMD = "c2go.union.ambig.words";

/// c2go #665 (#654c-b root fix): the address space FUNCTION-POINTER types are
/// lowered to. Model rule: a function pointer's run-time value is a code
/// address or a POSIX sentinel integer (SIG_IGN == 1) — never a stack or
/// GC-heap address — so it must never enter ANY GC map. Lowering the TYPE to
/// a dedicated address space makes the exclusion survive every optimization:
/// the "c2go-gc" GCStrategy answers isGCManagedPointer==false for it, so
/// RewriteStatepointsForGC never threads a function-pointer SSA value into a
/// gc-live set (the last marking surface — the type-level argptrmask /
/// kPtrSlotMD / union-ambig exclusions and the MIR spill-root filter cover
/// the rest). Function DEFINITIONS stay in AS0 (the program address space);
/// decay/indirect-call sites bridge with addrspacecast, which both targets
/// lower as a no-op (same 64-bit width; note X86's isNoopAddrSpaceCast
/// requires AS < 256, and AArch64 reserves 270-272 for __ptr32/__ptr64 —
/// hence 200).
/// Producer: clang CodeGenTypes (pointer-to-function lowering).
/// Consumer: BuiltinGCs C2GoGC strategy, C2GoGCMaskUtils::walkPointerFields,
/// C2GoSafepoint::collectPtrSlotAllocas.
inline constexpr unsigned kFnPtrAddrSpace = 200;

/// GlobalVariable MD attached to a c2go-managed (or unmanaged) module-level
/// VarDecl. Encodes the c2go view of the global: (name MDString,
/// go_type MDString, managed-bit i1). Used by c2go-lto WF2 to rebuild
/// `symbols[] kind=var` manifest entries when the per-TU .json sidecar is
/// not available (#389 Go-owned globals follow-up).
/// Producer: CodeGenModule::EmitGlobalVarDefinition.
/// Consumer: c2go-lto C2GoManifestRebuilder.
inline constexpr StringLiteral kVarGVMD = "c2go.var";

/// CallBase instruction MD attached to a managed-allocator call (e.g.
/// `runtime.mallocgc`) carrying the *target* LLVM struct type as a poison
/// ValueAsMetadata operand. Lets a later pass (the consumer is scheduled
/// to land with the post-malloc typing rewrite; currently producer-only)
/// recover the static target type via ValueAsMetadata->getType() without
/// re-parsing the call. Header-listed so the future consumer reuses the
/// same string and `consume_front` semantics.
/// Producer: CodeGenModule (post-mallocgc call setup).
/// Consumer: reserved (no in-tree reader yet; preserved across LTO so a
/// later pass / c2go-lto step can read it).
inline constexpr StringLiteral kAllocTargetTypeMD = "c2go.alloc.target.type";

//===----------------------------------------------------------------------===//
// Built-in legacy managed-alloca seed/default list (consumed by
// C2GoSafepoint to seed the per-Module call-PC set; #463).
//===----------------------------------------------------------------------===//

/// Legacy managed-alloca path seed/default list + NMD extension
/// default. #288 per-call-site path 不依赖该 list 覆盖完整性 — call-PC 级别
/// 的 pointer-slot liveness 由 RewriteStatepointsForGC / per-PC pcdata
/// 自行兜底,本列表仅供 legacy C2GoSafepoint managed-alloca 路径 + 模块通过
/// `c2go.safepoint.callees` NMD 扩展时的默认 seed 使用。
///
/// MUST stay byte-identical with the frontend's stamping site
/// (CodeGenModule.cpp) and the pass's run-start union
/// (C2GoSafepoint.cpp::getBuiltinSafepointCallees).
inline constexpr StringLiteral kBuiltinSafepointCallees[] = {
    // Heap allocator — every call may grow the heap and run GC.
    "runtime.mallocgc",
    // Typed copy helpers. The singleton `_c2go_typedmemmove` is //go:nosplit
    // on the Go side so RewriteStatepointsForGC skips it. `runtime.typedmemmove`
    // upstream is //go:nosplit; kept for completeness (see #465 follow-up).
    "runtime.typedmemmove",
    // ARRAY 变体与 singleton 同为 gc-leaf-function + //go:nosplit;RS4GC 会
    // bypass 该调用;保留在该名单仅为 legacy C2GoSafepoint managed-alloca 路径
    // 双保险;后续 #465 探讨是否去除。
    "_c2go_typedMemmoveArray",
    // gc-leaf-function + //go:nosplit (mwbbuf wbBufFlush nowritebarrierrec);
    // 同 _c2go_typedMemmoveArray 保留仅供 legacy C2GoSafepoint managed-alloca
    // 路径双保险。
    "runtime.gcWriteBarrier",
    // Stack-growth helper — switches to g0, scans the current frame.
    "runtime.morestack",
    // Goroutine spawn — synchronous safepoint while allocating g + stk.
    "runtime.newproc",
    // Panic — unwinds the stack, GC may run during recovery.
    "runtime.gopanic",
    // Stack-switching helper — runs `fn` on g0 stack, full safepoint.
    "runtime.systemstack",
};

/// View over the canonical built-in safepoint-callee list. Callers that want
/// to iterate or seed a StringSet should consume the ArrayRef rather than
/// re-typing the entries.
inline ArrayRef<StringLiteral> getBuiltinSafepointCalleeNames() {
  return ArrayRef<StringLiteral>(kBuiltinSafepointCallees);
}

//===----------------------------------------------------------------------===//
// File-local protocol constants — DELIBERATELY NOT promoted to the header.
//
// These spellings are part of the c2go IR-level vocabulary, but every
// producer AND every consumer lives inside a single .cpp file, so
// promoting them to the header would expose unused symbols (CLAUDE.md §2:
// no abstractions for single-use code). They are listed here only as a
// cross-reference so future readers grepping the protocol still find them.
//
//   * `c2go.safepoint.next.id`  — Module flag carrying the next stackmap
//       ID watermark, persisted across re-runs of C2GoSafepoint (e.g. an
//       LTO pipeline that re-runs the pass). Producer + consumer:
//       llvm/lib/Transforms/C2Go/C2GoSafepoint.cpp:kNextIdFlag.
//
//   * `c2go.zeroinit`  — Instruction MD on a synthetic entry-block
//       zero-init store, so a second run of C2GoSafepoint recognises its
//       own previous output and stays idempotent (avoids double zero-
//       initialisation when the pass re-runs on an inlined callee's
//       allocas). Producer + consumer:
//       llvm/lib/Transforms/C2Go/C2GoSafepoint.cpp:kZeroInitTag.
//
//   * `c2go.wb.done`  — Instruction MD on a fast-path store emitted by
//       C2GoWriteBarriers, marking that the store has already been wrapped
//       by the write-barrier rewrite. Idempotency guard for a second pass
//       run (BackendUtil + c2go-lto post-inliner; #371). Producer +
//       consumer:
//       llvm/lib/Transforms/C2Go/C2GoWriteBarriers.cpp:kBarrierDoneMD.
//
// If a future change adds a SECOND user of any of these spellings (e.g.
// a c2go-lto reader that needs `c2go.wb.done` to filter post-link
// barrier sites), MOVE the constant up to this header and delete the
// file-local copy — keeping two source-of-truth strings is precisely
// what #463 / #465 is trying to eliminate.
//===----------------------------------------------------------------------===//

} // namespace c2go
} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOPROTOCOL_H
