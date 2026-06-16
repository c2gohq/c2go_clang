//===- C2GoCommon.h - Shared CC/attr enforcement for C2Go passes -*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Helpers shared by every C2Go module pass that declares (or reuses) a
// GoABI0 helper symbol. Two invariants every pass must enforce on its
// helpers:
//
//   (1) Declared CC must be GoABI0 — the Go linker generates the ABI0
//       entry that reads args from the stack; a default-CC declaration
//       would let the entry read garbage (failure mode #229).
//
//   (2) The "gc-leaf-function" attribute must match the Go-side
//       //go:nosplit decoration. IsLeaf=true mirrors a //go:nosplit
//       shim (RS4GC skips statepoint wrap). IsLeaf=false strips the
//       attribute if it has been mis-attached, so RS4GC wraps real
//       non-leaf libc paths (failure mode #415).
//
// The companion enforceCallSiteCC sweep (Wave CD #419 audit Finding #6,
// task #429) defends against the asymmetry between
// `Function::setCallingConv` (which only updates the declaration) and
// pre-existing CallInsts (which retain whatever CC they were created
// with). Without the sweep, release-tier builds silently let a
// call-site CC mismatch through to the linker.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOCOMMON_H
#define LLVM_TRANSFORMS_C2GO_C2GOCOMMON_H

namespace llvm {

class Function;

namespace c2go {

// Normalises `F`'s declaration: forces GoABI0 CC, then either adds or
// strips `gc-leaf-function` based on `IsLeaf`. See header comment for
// the two invariants this enforces.
//
// #453: returns true iff the declaration was actually mutated (CC was
// not already GoABI0, or the gc-leaf-function attr toggled), so callers
// can OR the result into a pass-level `Changed` flag and return
// `PreservedAnalyses::none()` only when retrofit actually touched IR.
bool enforceGoABI0AndOptLeaf(Function *F, bool IsLeaf);

// Sweeps every direct CallBase whose called Function == `F`. If a call
// site CC does NOT match `F`'s declared CC, the call site CC is
// rewritten to match AND a one-line diagnostic is emitted to errs() so
// the offending upstream emitter is visible in CI / human review.
// `Function::setCallingConv` only updates the declaration; pre-existing
// CallInsts retain their original CC, and the IR verifier accepts the
// mismatch silently — without this sweep a release-tier mismatch slips
// through to the Go linker (failure mode #229 / #399 / #429).
// A nullptr `F` is a no-op (the caller may pass the result of
// `M.getFunction(...)` directly).
//
// #453: returns true iff at least one call site CC was rewritten, so
// the caller can OR the result into a pass-level `Changed`.
bool enforceCallSiteCC(Function *F);

} // namespace c2go

} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOCOMMON_H
