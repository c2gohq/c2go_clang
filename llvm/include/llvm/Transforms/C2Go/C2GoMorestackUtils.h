//===- C2GoMorestackUtils.h - Shared morestack-noop intrinsic set *- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #436 — single shared "can this IR call reach morestack" predicate.
//
// Three passes need an identical answer to the question "can this CallBase
// ever lower to a real call that reaches morestack/GC?":
//
//   * C2GoSafepoint::isPotentialMorestackCall  (LLVM stackmap path; the false
//     branch suppresses per-call-site stackmap emission)
//   * C2GoGCSetup                              (RewriteStatepointsForGC path;
//     a false answer downgrades the call to gc-leaf so RS4GC skips it)
//   * C2GoLoopPoll::hasAnyRealCallInBody       (a false answer means the loop
//     still needs its own cooperative-preemption poll)
//
// Before #436 each pass kept its own hand-maintained `switch (IntrinsicID)`
// over the same eight noop intrinsics. The two lists are SUPPOSED to be the
// same set: a noop for one path is a noop for the other (no PC, no real call,
// cannot reach the GC). Keeping two identical X-macros in sync by hand is a
// foot-gun — one side adding (or losing) an entry silently desynchronises GC
// coverage versus stackmap coverage.
//
// Implementation: retain the intrinsic-only helper for callers that need it,
// and expose `mayReachMorestack(CallBase)` as the shared policy entry point.
// Inline so neither the LLVMC2Go component (Transforms/C2Go) nor the target
// backends need a new link dependency.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOMORESTACKUTILS_H
#define LLVM_TRANSFORMS_C2GO_C2GOMORESTACKUTILS_H

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Intrinsics.h"

namespace llvm {
namespace c2go {

/// Return true when the intrinsic ID names a side-effect-free debug /
/// lifetime / pseudo intrinsic that lowers to NOTHING and can therefore never
/// reach morestack / GC.
///
/// Caller responsibility: invoke ONLY for intrinsic calls (i.e. on an
/// `IntrinsicInst`'s `getIntrinsicID()`). Use mayReachMorestack() for a
/// general CallBase.
inline bool isNoopMorestackIntrinsic(Intrinsic::ID Id) {
  switch (Id) {
  case Intrinsic::experimental_stackmap:
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_label:
  case Intrinsic::assume:
  case Intrinsic::donothing:
  // c2go #454: `experimental_noalias_scope_decl` is a pure scope marker
  // for the noalias analysis (no PC, no real call, lowers to nothing).
  // C2GoLoopPoll already excluded it from "real call in body"; mirror it
  // here so the stackmap path (C2GoSafepoint) and the RS4GC path
  // (C2GoGCSetup) cannot drift from the loop-poll path.
  case Intrinsic::experimental_noalias_scope_decl:
    return true;
  default:
    // memcpy/memset/etc. and any other intrinsic may lower to a real call
    // (or be left as a libcall) → may reach morestack/GC.
    return false;
  }
}

/// Return true when CB can lower to a real function call whose callee may run
/// a Go morestack prologue and therefore trigger copystack / GC.
///
/// LLVM represents inline asm as a CallBase even when the emitted instruction
/// stream contains no function call. c2go gives inline asm a call-free
/// contract: calls must remain visible as IR calls, while hiding CALL, BL, or
/// BLR inside an asm template is unsupported because it bypasses the Go calling
/// convention and per-return-PC stackmap machinery. Consequently an InlineAsm
/// CallBase is not itself a morestack site.
inline bool mayReachMorestack(const CallBase &CB) {
  if (CB.isInlineAsm())
    return false;
  if (Intrinsic::ID Id = CB.getIntrinsicID(); Id != Intrinsic::not_intrinsic)
    return !isNoopMorestackIntrinsic(Id);
  return true;
}

} // namespace c2go
} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOMORESTACKUTILS_H
