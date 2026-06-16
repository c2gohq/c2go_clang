//===- C2GoLeafEligibility.h - c2go NOSPLIT leaf eligibility ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cross-target NOSPLIT leaf / TU-internal near-leaf eligibility analysis for
// the c2go optimization layer (Wave W Track A / #298). Originally duplicated
// across `llvm/lib/Target/AArch64/AArch64C2GoLeafABI.{h,cpp}` and
// `llvm/lib/Target/X86/X86C2GoLeafABI.{h,cpp}`, the algorithm is pure-IR /
// callgraph / linkage / frame-budget so the two backends now share this TU.
// Each backend supplies a `TargetHooks` struct with target-specific frame
// estimate + NOSPLIT budget lambdas; the analysis + CC-flip worker live here.
//
// The eligibility rule (v0) — see clang/docs/c2go_design.md §2.0.1 / §4.2 /
// §4.2.1 and ABI_TEST_FINDINGS_2026-05-25.md §一/§六 — is:
//   (a) NOT a c2go_extern / GoABI0-boundary symbol (has `c2go-reg-return` attr);
//   (b) internal linkage (C `static`) — no cross-TU caller could mis-marshal;
//   (c) not address-taken (cannot be reached via indirect call);
//   (d) the entire downstream call subtree is in this TU, has no recursion,
//       no indirect/external calls, and its accumulated frame usage is within
//       the NOSPLIT budget for the target GOARCH/GOOS.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOLEAFELIGIBILITY_H
#define LLVM_TRANSFORMS_C2GO_C2GOLEAFELIGIBILITY_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <functional>

namespace llvm {

class Function;
class Module;

namespace c2go {

/// Per-function result of the eligibility analysis (target-agnostic).
struct LeafEligibility {
  /// Whether the function may use c2go-ABIInternal + NOSPLIT.
  bool Eligible = false;
  /// Max accumulated stack usage over the function's downstream call subtree
  /// (own frame + deepest callee chain), in bytes. Only meaningful for
  /// functions whose subtree was fully analyzable in this TU.
  uint64_t SubtreeStackBytes = 0;
  /// True if this is a strict leaf (calls no other function at all).
  bool IsStrictLeaf = false;
  /// Human-readable reason the function was rejected (empty if Eligible).
  StringRef IneligibleReason;
};

/// Target-specific hooks for the shared eligibility analysis.
///
/// Each backend (AArch64 / X86 / ...) supplies its own frame estimate +
/// NOSPLIT budget callbacks. The analysis itself is target-agnostic.
struct LeafEligibilityTargetHooks {
  /// Returns a SAFE UPPER BOUND on the function's own frame size in bytes
  /// (saved-LR/FP or RA/BP cushion + static allocas + outgoing-arg cushion).
  /// Over-counting is fine — the linker's nosplit pass is the hard backstop;
  /// over-estimation only makes the analysis more conservative.
  std::function<uint64_t(const Function &)> FrameOf;
  /// Returns the NOSPLIT stack budget (in bytes) for this target. Mirrors
  /// Go's `cmd/link/internal/ld/stackcheck` + `objabi.StackNosplit`:
  ///   * base = 800; mult = 1, +1 for OpenBSD/AIX (race deliberately ignored).
  ///   * callSize = arch.RegSize on non-LR machines (amd64=8, i386=4); 0 on
  ///     LR machines (arm64).
  ///   * arm64 reserves an extra 8B for the saved frame pointer.
  unsigned Budget = 0;
};

/// Compute the NOSPLIT-leaf budget (in bytes) for a target triple, matching
/// Go's cmd/link/internal/ld/stackcheck computation:
///
///   limit = objabi.StackNosplit(race) - callSize        // base = 800 * mult
///   if GOARCH == arm64: limit -= 8                       // extra 8B for FP
///
/// where:
///   - StackNosplitBase = 800; mult = 1, +1 if GOOS in {aix, openbsd}, +1 if
///     race. We use race=false (the conservative / smaller budget) because the
///     compiler does not know the race setting; race only ever *increases* the
///     budget, so race=false is a safe lower bound.
///   - callSize = Arch.RegSize on non-LR machines (amd64 = 8); on LR machines
///     (arm64, HasLR) callSize = 0.
///
/// Default linux/darwin, non-race: amd64 = 800-8 = 792, arm64 = 800-0-8 = 792.
/// aix/openbsd: base doubles to 1600 -> 1592.
///
/// \p ArchName is the Go GOARCH ("arm64", "amd64", "386", ...); \p OSName is
/// the Go GOOS ("linux", "darwin", "openbsd", "aix", ...).
unsigned getNosplitBudget(StringRef ArchName, StringRef OSName);

/// Convenience overload that derives GOARCH/GOOS from an LLVM target-triple
/// string (e.g. "arm64-apple-darwin" / "x86_64-unknown-linux").
unsigned getNosplitBudgetForTriple(StringRef TripleStr);

/// Coarse IR-only per-function frame estimate. SAFE UPPER BOUND: sums the
/// static alloca sizes plus a per-target cushion. The cushion is
/// target-specific (saved-LR+FP on arm64; saved-RA+BP on x86) so backends
/// pass it in via \p LinkageCushionBytes (typically 16). Callers should
/// supply real (post-codegen) frame sizes when available via
/// `analyzeC2GoLeafEligibility`'s PerFuncFrameBytes parameter.
uint64_t estimateLeafFrameBytes(const Function &F,
                                uint64_t LinkageCushionBytes = 16);

/// True if any callsite in \p F is indirect (function pointer) or has any
/// dynamic-alloca / inline-asm hazard that defeats static subtree analysis.
/// Target-agnostic; lives here so backends share a single definition.
bool hasUnanalyzableCallOrAlloca(const Function &F);

/// Run the v0 eligibility analysis over an entire module (one TU). Returns
/// a map from every defined Function to its LeafEligibility.
///
/// \p Hooks supplies target-specific frame estimate + NOSPLIT budget.
/// \p PerFuncFrameBytes optionally supplies real (post-codegen) frame sizes
/// to OVERRIDE the hooks' FrameOf. When null, Hooks.FrameOf is used directly.
/// \p IgnoreAddressTaken, when true, SKIPS the address-taken reject gate so the
/// analysis answers "is F leaf-eligible MODULO being address-taken?". Used by
/// the clang #495 wrapper synthesizer, which must decide eligibility on
/// pre-RAUW IR where F is still address-taken; every other gate still applies.
DenseMap<const Function *, LeafEligibility>
analyzeC2GoLeafEligibility(const Module &M,
                           const LeafEligibilityTargetHooks &Hooks,
                           const DenseMap<const Function *, uint64_t>
                               *PerFuncFrameBytes = nullptr,
                           bool IgnoreAddressTaken = false);

/// Run the analysis + CC-flip worker shared by AArch64 + X86 backend passes.
///
/// Behavior:
///   1. If the c2go emergency flag disables "leaf-abi", returns false.
///   2. If the module lacks the `c2go.goabi` module flag (not a c2go-mode
///      module), returns false.
///   3. If \p ExtraGate is non-empty AND the module lacks that flag, returns
///      false. (Generic second-gate hook; currently unused — both AArch64
///      and X86 pass ExtraGate="" now that X86 is powered on. Retained for
///      future opt-in gating without an API change.)
///   4. Otherwise runs `analyzeC2GoLeafEligibility(M, Hooks)` and flips each
///      eligible function's CC + every direct call site to
///      CallingConv::C2GoABIInternal in lockstep.
///
/// Returns true iff the module was modified.
bool runC2GoLeafCCFlip(Module &M, const LeafEligibilityTargetHooks &Hooks,
                       StringRef ExtraGate = "");

} // namespace c2go
} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOLEAFELIGIBILITY_H
