//===- MCC2GoFunctionMetadata.h - c2go per-function metadata aggregate -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #376: shared POD aggregate describing per-function metadata that
// flows from the producer (AArch64FrameLowering's c2go prologue, AArch64
// AsmPrinter's stkobj collector, clang's manifest pass) to the consumer
// (MCPlan9AsmStreamer's TEXT-directive / FUNCDATA emit). Lives in its
// own header so MCContext and MCPlan9AsmStreamer can share the type
// without entangling header dependencies (MCContext.h needs the type
// for the pending-boundary queue used during the clang→streamer
// hand-off; MCPlan9AsmStreamer.h needs it for the publish API).
//
// Storage convention: this struct OWNS its variable-length payload
// (vector<uint8_t> mask bytes, vector<StkObjEntry>). The legacy
// ArrayRef<>-based shape was replaced as part of the
// thread_local-to-instance-member migration (#376) so callers no longer
// need to keep a parallel storage buffer alive until publish time.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCC2GOFUNCTIONMETADATA_H
#define LLVM_MC_MCC2GOFUNCTIONMETADATA_H

#include "llvm/MC/MCPlan9StackObjects.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace llvm {

/// Per-function metadata aggregate. A single struct collected at the
/// source-of-truth site then submitted via
/// MCPlan9AsmStreamer::publishC2GoFunction(). Collapses 6 separate
/// thread_local maps (framesize/argsize, NOSPLIT, args ptr-mask, locals
/// agg/ambig masks, stkobj table) into one entry per function.
///
/// Field semantics:
///   * Name             — Mach-O-mangled (or LLVM IR) function symbol;
///                         publishC2GoFunction strips the optional `\01`
///                         no-mangle marker.
///   * FrameSize        — -1 means "leave the previously registered
///                         framesize untouched". A numeric value publishes
///                         (creates the metadata entry on miss).
///   * ArgSize          — std::nullopt preserves the prior argsize (used
///                         when the prologue publishes framesize for a
///                         c2go_extern boundary whose argsize already came
///                         from the manifest). A numeric value overwrites.
///   * NoSplit          — true adds the function to the NOSPLIT set; false
///                         leaves the bit untouched.
///   * ArgPtrMaskBytes  — byte-packed args pointer-word mask. Non-empty
///                         publishes; empty preserves the prior value.
///   * LocalsAggMaskBytes / LocalsAmbigMaskBytes — same non-empty-publishes
///                         convention.
///   * StackObjects     — std::nullopt preserves; an empty vector erases
///                         the prior entry; a non-empty vector publishes.
///   * SavedLinkSize    — c2go #298 Wave AA Track A (F3 contract lock):
///                         size in bytes of the saved-LR slot inside the
///                         physical frame the producer reports as
///                         FrameSize. AArch64 = 8 (LR is a register; the
///                         Go assembler's obj7.go preprocess injects
///                         `MOVD.W R30,-autosize(RSP)` to spill it and
///                         the runtime's traceback reads it back at
///                         `varp = fp - 8`). X86 amd64 = 0 (the return
///                         address is pushed by CALL onto rsp+0 but is
///                         NOT counted in `$framesize` per obj6.go).
///                         Wave AA GPT NEEDS_FIX (Fix 1 / A3): the field
///                         is `std::optional<unsigned>` so that secondary
///                         publishers (e.g. AArch64AsmPrinter's stkobj
///                         harvest at body end, which constructs a fresh
///                         metadata aggregate to forward only the
///                         StackObjects payload) can leave it nullopt
///                         and the streamer-side aggregate PRESERVES the
///                         value the primary producer (FrameLowering /
///                         X86C2GoFrameMetaStager) published earlier.
///                         publishC2GoFunction has partial-update
///                         semantics: a `has_value()` field overwrites
///                         E.SavedLinkSize; nullopt preserves the prior
///                         publish. The Stage-1 FUNCDATA preamble
///                         consumer falls back to 8 when nothing has
///                         ever been published (AArch64-compatible
///                         default) — `Nbit = (FrameSize -
///                         SavedLinkSize) / PtrSize`.
///   * FrameAlignment   — c2go #298 Wave AA Track A (F3 contract lock):
///                         per-arch fixup the Go assembler adds back on
///                         top of the declared `$framesize` to recover
///                         the physical SP-decrement D. AArch64 = 16
///                         (saved-LR 8 + padding 8, because obj7.go's
///                         `autosize += 8` followed by `if autosize%16
///                         == 8: extrasize = 8` lands on D when we
///                         declare D-16). X86 amd64 = 0 (the declared
///                         `$framesize` equals the local frame size;
///                         saved-BP is added by obj6.go when needed but
///                         is NOT subtracted from the declared value).
///                         Wave AA GPT NEEDS_FIX (Fix 1 / A3): same
///                         `std::optional<unsigned>` preserve-on-nullopt
///                         semantics as SavedLinkSize above — keeps the
///                         X86 stkobj second-publish from silently
///                         resetting an X86 producer's `0` back to the
///                         AArch64 `16` default. The Stage-1 TEXT
///                         directive consumer falls back to 16 when
///                         nothing has been published; declared
///                         `$framesize = FrameSize - FrameAlignment`;
///                         if `FrameSize < FrameAlignment` we emit
///                         `NOFRAME, $0` instead.
struct C2GoFunctionMetadata {
  std::string Name;
  int FrameSize = -1;
  std::optional<int> ArgSize;
  bool NoSplit = false;
  std::vector<uint8_t> ArgPtrMaskBytes;
  std::vector<uint8_t> LocalsAggMaskBytes;
  std::vector<uint8_t> LocalsAmbigMaskBytes;
  std::optional<std::vector<StkObjEntry>> StackObjects;
  // c2go #298 Wave AA Track A (F3 contract lock) + Wave AA GPT NEEDS_FIX
  // Fix 1 (A3 publishC2GoFunction API foot-gun): per-arch frame layout
  // contract. nullopt = "preserve the prior published value" (partial-
  // update semantics, matching ArgSize / StackObjects / mask-byte fields
  // around it). The streamer-side AArch64 fallback uses 8 / 16 — the
  // pre-#NEEDS_FIX struct defaults — when no producer has ever set the
  // field on this function, keeping the AArch64 production path
  // byte-identical.
  std::optional<unsigned> SavedLinkSize;
  std::optional<unsigned> FrameAlignment;
};

} // end namespace llvm

#endif // LLVM_MC_MCC2GOFUNCTIONMETADATA_H
