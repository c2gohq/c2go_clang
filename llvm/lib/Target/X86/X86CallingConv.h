//=== X86CallingConv.h - X86 Custom Calling Convention Routines -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains the custom routines for the X86 Calling Convention that
// aren't done by tablegen.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86CALLINGCONV_H
#define LLVM_LIB_TARGET_X86_X86CALLINGCONV_H

#include "MCTargetDesc/X86MCTargetDesc.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/IR/CallingConv.h"

namespace llvm {

bool RetCC_X86(unsigned ValNo, MVT ValVT, MVT LocVT,
               CCValAssign::LocInfo LocInfo, ISD::ArgFlagsTy ArgFlags,
               Type *OrigTy, CCState &State);

bool CC_X86(unsigned ValNo, MVT ValVT, MVT LocVT, CCValAssign::LocInfo LocInfo,
            ISD::ArgFlagsTy ArgFlags, Type *OrigTy, CCState &State);

// c2go #298 / Wave X Track A: hand-written x86-64 register-assignment for
// `CallingConv::C2GoABIInternal`. Mirrors AArch64C2GoABIInternal — Go-style
// integer (AX, BX, CX, DI, SI, R8-R11) and FP (XMM0-XMM14) reg lists with
// stack overflow. The dispatch is wired from X86CallingConv.td via CCCustom;
// see also X86CallingConv.cpp for the call/return entry points and rationale
// (the recursive aggregate "all-or-nothing per top-level arg" semantics are
// not expressible in the table-gen DSL, matching the AArch64 implementation).
bool CC_X86_64_C2GoABIInternal_TD(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                                  CCValAssign::LocInfo &LocInfo,
                                  ISD::ArgFlagsTy &ArgFlags, CCState &State);
bool RetCC_X86_64_C2GoABIInternal_TD(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                                     CCValAssign::LocInfo &LocInfo,
                                     ISD::ArgFlagsTy &ArgFlags, CCState &State);

// c2go #298 Wave AI Track A — hand-written x86-64 stack-only convention for
// `CallingConv::GoABI0` (LIVE). Mirrors `CC_AArch64_GoABI0` (Go stack-based
// ABI0 boundary CC used at c2go_extern call sites and Go linkname targets).
// Slot rules per project_298_abitest_amd64_baseline_2026_06_07:
//   * outgoing-args block starts at SP+0 (amd64; aarch64's SP+8 saved-LR
//     slot has no amd64 equivalent — `CALL` pushes retPC into the caller's
//     frame, NOT the callee's outgoing-arg region).
//   * incoming-args block on the callee side starts at SP+8 *automatically*
//     because hardware `call` pushed retPC to (caller_sp - 8); X86 MFI
//     resolves LocMemOffset 0 to the slot directly above retPC. So no
//     equivalent of AArch64's c2goReserveCallerLRSlot is needed here.
//   * i1/i8/i16/i32   → 4-byte slot, 4-byte align (Go zero-extends).
//   * i64 / pointer   → 8-byte slot, 8-byte align.
//   * f32             → 4-byte slot.
//   * f64             → 8-byte slot.
//   * 128-bit SIMD    → 16-byte slot, 16-byte align (mirror of
//     AArch64 GoABI0 indirect-pass — amd64 contract here is direct
//     16-byte stack slot because amd64 already aligns on 16).
//
// Wave AI landed the LowerReturn / LowerCallResult MemLoc paths in
// X86ISelLoweringCall.cpp and flipped the td stub to a live
// CCCustom dispatch (X86CallingConv.td CC_X86 / RetCC_X86 entries
// just above the SysV fall-through). The previous Wave AH.2 DORMANT
// canary at llvm/test/CodeGen/X86/c2go-goabi0-cc-table-dormant-canary.ll
// is now superseded by the live stack-result canary in
// c2go-goabi0-cc-table-live-canary.ll (same path); see also
// c2go-goabi0-isel-dispatch.ll for end-to-end ISel dispatch CHECKs
// (FormalArguments / Return / Call / CallResult).
//
// Register-return for internal goabi0cc functions (`c2go-reg-return`
// attr — AArch64 §2.0.2 fallback to RetCC_AArch64_AAPCS in
// CanLowerReturn / LowerReturn) landed in Wave AJ.3. On x86-64 the
// SysV-equivalent of AAPCS is `RetCC_X86_64_C` (RAX / XMM0 / etc.).
// `RetCC_X86_64_C` is `static` in the td-generated inc (no `let Entry=1`
// on its def), so X86ISelLoweringCall.cpp cannot reference it directly.
// The bypass is implemented by constructing a CCState whose CallConv is
// `CallingConv::X86_64_SysV` and running it through the Entry-marked
// `RetCC_X86` — that dispatch arm (`CCIfCC<"X86_64_SysV", CCDelegateTo<
// RetCC_X86_64_C>>`) delegates to the static SysV table. The detection
// hooks live in CanLowerReturn / LowerReturn / LowerCall /
// LowerCallResult: a function flagged `c2go-reg-return` (callee side)
// or a call site flagged the same (caller side; read via
// CLI.hasCallSiteFnAttr to survive RS4GC statepoint rewrites) flips
// EffectiveCC to X86_64_SysV before constructing the CCState. Boundary
// GoABI0 callees keep CallConv=GoABI0 and the stack-result contract via
// the td dispatch below.
//
// The table itself is hand-written (mirror of the C2GoABIInternal pattern)
// because the consecutive-regs aggregate rule cannot be expressed in
// CCIfType alone — GoABI0 has no register branch but the table layout is
// kept hand-written for symmetry with the AArch64 RetCC_AArch64_GoABI0
// mirror.
bool CC_X86_64_GoABI0_TD(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                         CCValAssign::LocInfo &LocInfo,
                         ISD::ArgFlagsTy &ArgFlags, CCState &State);
bool RetCC_X86_64_GoABI0_TD(unsigned &ValNo, MVT &ValVT, MVT &LocVT,
                            CCValAssign::LocInfo &LocInfo,
                            ISD::ArgFlagsTy &ArgFlags, CCState &State);

} // End llvm namespace

#endif

