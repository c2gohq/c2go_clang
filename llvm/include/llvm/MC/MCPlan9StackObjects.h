//===- MCPlan9StackObjects.h - c2go FUNCDATA $2 stkobj table -----*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #284 (S2-S5): shared POD describing one entry in the per-function
// stack-objects table (FUNCDATA $2). Lives in its own header so
// AArch64AsmPrinter (collector) and MCPlan9AsmStreamer (emitter) can share
// the type without entangling header dependencies.
//
// Wire format spec: docs/c2go_design.md §4.10.4.1 (S1 normative).
//
// NB: `frameOffset` is `varp`/`argp`-relative per the Go runtime
// stackObjectRecord convention — NOT SP-relative. The collector in
// AArch64AsmPrinter performs the conversion when populating entries (see
// §4.10.4.1 point 2). The streamer trusts the field as-is.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCPLAN9STACKOBJECTS_H
#define LLVM_MC_MCPLAN9STACKOBJECTS_H

#include <cstdint>
#include <string>

namespace llvm {

/// One row of the per-function FUNCDATA $2 table (16 bytes on the wire).
///
/// Layout mirrors Go runtime `stackObjectRecord` (go/src/runtime/stack.go):
///   +0  int32  frameOffset  // varp/argp-relative; locals < 0, args >= 0
///   +4  uint32 size         // total object bytes
///   +8  uint32 ptrBytes     // pointer-containing prefix length
///   +12 uint32 gcdataoff    // SymPtrOff to gcdata symbol (Go linker fills)
///
/// `gcdataSymName` is the **symbol name** of the GC bitmap blob (e.g.
/// `c2go.gcbitmap.struct.foo` or `c2go.gcbitmap.ptr`). The Plan 9 `.s`
/// emitter writes
///   `DATA <stkobj>+12(SB)/4, $<gcdataSymName>(SB)`
/// so the Go linker performs the SymPtrOff relocation (see §4.10.4.1 pt 1).
struct StkObjEntry {
  int32_t frameOffset;
  uint32_t size;
  uint32_t ptrBytes;
  std::string gcdataSymName;
};

} // end namespace llvm

#endif // LLVM_MC_MCPLAN9STACKOBJECTS_H
