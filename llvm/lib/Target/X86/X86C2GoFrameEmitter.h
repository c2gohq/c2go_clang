//===- X86C2GoFrameEmitter.h - c2go (Plan 9) frame meta staging (X86) -===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go #298 Wave AA Track B (X86 minimal staged-meta producer).
//
// Mirrors the AArch64 c2go producer (C2GoFrameEmitter.{h,cpp}) at a much
// smaller scope: an X86 MachineFunctionPass that, for every function the
// Wave V X86 leaf-ABI IR pass already flipped to CallingConv::C2GoABIInternal,
// stages a baseline `C2GoFunctionMetadata` onto X86MachineFunctionInfo so
// that X86AsmPrinter::emitFunctionEntryLabel republishes it into the live
// MCPlan9AsmStreamer (Wave Z Track B's #376 plumbing) before the function
// label is emitted.
//
// SCOPE OF THIS WAVE (Wave AA Track B, minimal viable):
//   * Strict-leaf X86 functions only (FrameSize = 0; the X86 backend's
//     standard SysV prologue is NOT bypassed — Wave V's NOSPLIT-budget
//     filter keeps these functions truly frameless from the GoABI side).
//   * Per-arch frame contract: SavedLinkSize = 0, FrameAlignment = 0
//     (Wave AA Track A locked these in MCC2GoFunctionMetadata.h; the
//     streamer's Stage-1 branch uses them to skip the AArch64-specific
//     "$framesize = FrameSize - 16" fixup and to drive `Nbit` correctly
//     for the locals bitmap). X86 amd64: CALL pushes the return address
//     onto rsp+0 but it is NOT subtracted from the Go-assembler-declared
//     `$framesize` (cmd/internal/obj/x86/obj6.go), so both contract
//     fields are zero.
//   * NoSplit = true (Wave V flipped only NOSPLIT-eligible leaves into
//     C2GoABIInternal — by construction the CC is the strict-leaf marker
//     for the X86 path until Wave AB extends to near-leaves).
//   * ArgSize forwarded from the `c2go-argsize` IR fn attribute the
//     manifest / clang -fc2go pipeline stamps on every internal GoABI0
//     function (same producer-side convention the AArch64 path uses).
//
// DEFERRED to Wave AB:
//   * Non-leaf / near-leaf staging (real FrameSize, LocalsAggMaskBytes,
//     LocalsAmbigMaskBytes — the AArch64 C2GoFrameEmitter equivalents).
//   * Hand-rolled Plan-9 prologue/epilogue emit (X86 mirror of
//     emitC2GoPrologue / emitC2GoEpilogue). Wave AA Track B does NOT
//     bypass X86FrameLowering's standard prologue; the strict-leaf
//     functions Wave V flips have no frame to emit anyway.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//
// Wave AB (Track C scout + design): X86 full producer architecture.
//
// This section is a PURE DESIGN DOC. No implementation lives in this file
// beyond the existing Wave AA Track B minimal stager (the createX86…Pass /
// initialize… symbols declared at the bottom). Wave AB implements every
// API outlined below in a new TU pair (suggested filenames:
//   * llvm/lib/Target/X86/X86C2GoFrameEmitter.cpp — extend existing TU,
//     add the computeX86C2GoFrameInfo() + emitX86C2GoPrologue / Epilogue
//     bodies and the per-MF locals-mask scan.
//   * llvm/lib/Target/X86/X86C2GoPtrSlotLiveness.cpp — new TU, mirror of
//     AArch64C2GoPtrSlotLivenessPass (M5; deferred until X86 statepoint GC
//     is staged).
// )
//
// ----------------------------------------------------------------------------
// 1. Architecture diagram (ASCII, mirror of the AArch64 producer chain)
// ----------------------------------------------------------------------------
//
//   IR pipeline ─────────────────────────────────────────────────────────────
//
//     clang -fc2go              c2go-lto BackendUtil
//          │                          │
//          ▼                          ▼
//     IR fn attributes set on every internal GoABI0 function:
//       * c2go-argsize           (decimal bytes)
//       * c2go-argptrmask        (hex packed bitmask)
//       * c2go-reg-return        (enables C2GoABIInternal eligibility)
//       * !c2go.union.ambig.words on AllocaInst (union pointer/int overlap)
//
//          │
//          ▼
//     X86C2GoLeafABIPass (Wave V Track A, IR ModulePass, addIRPasses):
//       * eligibility analysis (shared C2GoLeafEligibility helper)
//       * for each eligible callee: setCallingConv(C2GoABIInternal)
//         + lockstep flip on every direct call site
//
//   MachineFunction pipeline ────────────────────────────────────────────────
//
//          │ (ISel + the rest of the codegen passes run here; X86 currently
//          │  drops the strict-leaf functions onto the SysV prologue; Wave
//          │  AB introduces the hand-rolled Plan-9 prologue replacement.)
//          ▼
//     X86FrameLowering::emitPrologue (Wave AB hook):
//        if (c2go::emitX86C2GoPrologue(MF, MBB))
//          return;            // bypass standard SysV prologue
//        … standard X86PrologueEmitter …
//
//     X86FrameLowering::emitEpilogue (Wave AB hook): symmetric.
//
//          │
//          ▼
//     X86C2GoFrameMetaStager (Wave AA Track B, MFPass, addPreEmitPass2):
//       * Wave AA Track B path  (strict-leaf, FrameSize=0):
//           Meta = { Name, FrameSize=0, NoSplit=true,
//                    SavedLinkSize=0, FrameAlignment=0,
//                    ArgSize=fnAttr("c2go-argsize") }
//       * Wave AB path          (full, FrameSize possibly >0):
//           Meta += LocalsAggMaskBytes  (aggregate-field pointer scan)
//           Meta += LocalsAmbigMaskBytes (union-ambig scrub)
//           Meta += ArgPtrMaskBytes     (decode hex fnAttr)
//
//          │
//          ▼
//     X86AsmPrinter::emitFunctionEntryLabel (already wired Wave Z Track B):
//       * takeC2GoStagedMeta() → MCPlan9AsmStreamer::publishC2GoFunction()
//
//     X86AsmPrinter::emitFunctionBodyEnd (Wave AB only):
//       * publishC2GoStackObjects() — stkobj harvest pass over MFI allocas.
//
//
// ----------------------------------------------------------------------------
// 2. Relationship with X86C2GoLeafABI pass (clarifies "two-stage" design)
// ----------------------------------------------------------------------------
//
//   X86C2GoLeafABI is an IR ModulePass (addIRPasses):
//     * Decides WHICH functions are eligible to use the internal GoABI.
//     * MUTATES IR: flips CallingConv on callee + every direct call site.
//     * Output: F->getCallingConv() == CallingConv::C2GoABIInternal.
//
//   X86C2GoFrameEmitter is an MFPass (addPreEmitPass2):
//     * Reads the CC decision LeafABI made and the IR fn attributes.
//     * COMPUTES META: FrameSize, NoSplit, locals masks, arg masks.
//     * Output: X86MachineFunctionInfo::C2GoStagedMeta (consumed at
//       AsmPrinter::emitFunctionEntryLabel time).
//     * Wave AB ALSO mutates MBB at FrameLowering time via
//       emitX86C2GoPrologue / Epilogue (separate hook from the stager
//       pass; same TU for code locality).
//
//   Why two passes / not one:
//     * The CC flip MUST happen before ISel so X86 CC tables route args
//       through the GoABIInternal register list.
//     * The meta computation MUST happen after PEI so MFI.getStackSize()
//       and getFrameIndexReference() return final SP-relative offsets.
//     * The prologue/epilogue emit MUST happen at FrameLowering time so
//       MFI.setStackSize() can publish the rounded-up FrameSize before
//       any other pass observes it. (Mirrors AArch64.)
//
// ----------------------------------------------------------------------------
// 3. X86-specific deltas from the AArch64 reference impl
// ----------------------------------------------------------------------------
//
//   3.1 No LR register — return address lives in memory.
//       * AArch64: BL pushes LR=x30; emitC2GoPrologue STRs LR to [sp+0]
//         using `STR LR, [SP, #-framesize]!` then SUBs FP to sp-8.
//       * X86 amd64: CALL pushes the 8-byte return address onto rsp+0
//         BEFORE the callee runs. No software spill needed; obj6.go does
//         NOT subtract it from the declared $framesize.
//
//       Consequence for c2GoFrameSize on X86:
//         Locals = MFI.getStackSize();
//         FrameSize = align16(Locals);  // NO `+ 16` for saved-LR
//                                       // (vs AArch64 which adds 16 for
//                                       // saved-LR + go FP-slot pair)
//       And SavedLinkSize=0, FrameAlignment=0 (already locked by Track A).
//
//   3.2 BP ownership: obj6.go owns it, NOT this emitter (AB5 GPT round-2).
//       * AArch64: emitC2GoPrologue STURs FP to sp-8 (red zone) then sets
//         FP=sp-8. Go's traceback.go reads saved-FP at fp-0 (= sp-8).
//       * X86 amd64: Go obj6.go's BP-emit predicate is
//         `!NOFRAME && !(autoffset==0 && !hasCall)`
//         (cmd/internal/obj/x86/obj6.go:621-635). When that fires obj6
//         injects `push rbp; mov rbp, rsp` ahead of the SUB at .s assemble
//         time, based purely on the staged `TEXT $framesize` + flag word.
//         This emitter therefore MUST NOT also emit BP save/restore — if
//         it did, the two would race on the .o path and the .s path would
//         either double-frame (filter bypass) or stay silent (filter
//         drops both, but then the .o path still corrupts).
//
//       AB5 design decision: NeedsFramePointer is pinned to false in
//       computeX86C2GoFrameInfo. The c2go emitter never reads
//       X86FrameLowering::hasFP(MF) — that predicate is the SysV decision
//       face (stackmap/patchpoint/EH/needFP — X86FrameLowering.cpp:99-108)
//       and is not congruent with obj6's predicate. The emitter emits a
//       single SUB only; obj6 layers BP on top of it if its own predicate
//       fires.
//
//       Consequence for emitX86C2GoPrologue:
//         if (FrameSize == 0):
//           emit nothing (true leaf, return-address only on stack).
//         else:
//           SUB rsp, FrameSize        ; allocate locals
//           // NO explicit ret-addr spill (CALL did it).
//           // NO BP save here — obj6 owns the BP decision face.
//           // FUNCDATA / locals scan use SP-relative offsets directly.
//
//       Concrete instruction sequences (single path, AB5):
//         prologue:  SUB64ri{8,32}  rsp, FrameSize    (FrameSetup flag)
//         epilogue:  ADD64ri{8,32}  rsp, FrameSize    (FrameDestroy flag)
//
//   3.3 Locals alignment.
//       * AArch64: SP must remain 16-aligned at all times → align16.
//       * X86 amd64: SysV requires rsp ≡ 8 (mod 16) at function entry
//         (after CALL push). After the prologue, rsp must be 16-aligned
//         in the body (so callees can XMM/AVX-spill at rsp+0..N). The
//         declared `$framesize` is the amount the prologue subtracts
//         from rsp, and TWO Go-assembler paths apply per obj6.go
//         (cmd/internal/obj/x86/obj6.go:621-632 hasFP add, :712-720
//         localoffset placement):
//
//         AB5 GPT round-2 fix: single-path FrameSize formula. obj6.go
//         owns BP (see §3.2), so the emitter never adds a saved-BP word
//         to FrameSize. The only path is:
//
//             FrameSize = roundup(Locals, 16)            (≡ 0 mod 16)
//
//         — return address sits at rsp+FrameSize+0 (CALL-pushed, OUTSIDE
//         the declared `$framesize` per obj6.go); the body observes rsp
//         16-aligned after the SUB. If obj6 layers a `push rbp` ahead of
//         the SUB based on its own predicate, it accounts for the BP
//         word in its own offset arithmetic — this emitter's FrameSize
//         remains the locals-only ≡0 mod 16 value.
//
//   3.4 Real-call detection.
//       * AArch64: c2goMakesRealCall scans for BL/BLR (excluding STACKMAP
//         / PATCHPOINT) using TII->get(Opc).isCall().
//       * X86: same shape, same TII->get(Opc).isCall(). The X86 opcode
//         space includes CALL64pcrel32, CALL64r, CALL64m, TCRETURN*,
//         JMP_4/JMP64m for tail-calls — all of which the TII isCall bit
//         covers. STATEPOINT exclusion ALSO matches (when X86 statepoint
//         GC eventually lands; today X86 statepoint path is unused).
//
//   3.5 Locals pointer scan (aggregate-field mask, union-ambig scrub).
//       * AArch64: walks MFI allocas with FrameReg == AArch64::SP and
//         calls c2go::walkPointerFields (already in C2GoGCMaskUtils).
//       * X86: identical algorithm; only the FrameReg check changes:
//         AArch64::SP  →  X86::RSP (or X86::ESP for i386 — defer i386
//         until amd64 lands and gets a -O2 SQLite e2e gate).
//       * The shared helper c2go::walkPointerFields is target-agnostic
//         (operates on llvm::Type / DataLayout) → zero duplication.
//
//   3.6 Bit-width: Nbit = (FrameSize - SavedLinkSize) / PtrSize.
//       * AArch64: (FrameSize - 8) / 8 → matches the `varp = fp - 8`
//         exclusion of the top word.
//       * X86: (FrameSize - 0) / 8 = FrameSize / 8. The full FrameSize
//         is the locals region; there is no saved-LR / top-FP word to
//         exclude (return-addr lives in the caller's frame from Go's
//         POV — rsp+framesize+0, OUTSIDE [rsp, rsp+framesize)).
//       The MCPlan9AsmStreamer Stage-1 branch already reads SavedLinkSize
//       from the metadata aggregate (Track A) and computes Nbit
//       correctly — Wave AB just has to set FrameSize honestly.
//
//   3.7 Spill-slot pointer tags (Approach B Milestones M2/M3/M5).
//       * AArch64: storeRegToStackSlot tags spill slots "ptr" via
//         AFI->setC2GoSpillSlotTag (#426); the prologue ORs them into
//         the body locals mask when M5 is disabled; M5 (per-PC live set)
//         consumes them at the AsmPrinter STATEPOINT/STACKMAP lowering.
//       * X86: ported in #298 (A4). storeRegToStackSlot tags spill slots
//         "ptr" via X86InstrInfo (x86C2GoIsPtrDerived def-chain classifier);
//         C2GoSpillSlotTags / C2GoLiveSpillSlotsAtCall live on
//         X86MachineFunctionInfo; X86C2GoPtrSlotLiveness (M5) computes the
//         per-PC live set, consumed at X86MCInstLower STATEPOINT/STACKMAP
//         lowering and converted through x86C2GoBitmapOffFromAnchor.
//
//   3.8 Variadic outgoing-call frame.
//       * AArch64: AArch64C2GoFunctionState::HasVariadicOutgoingCall +
//         hasReservedCallFrame (#125). X86's variadic-call story differs
//         (no fixed register window; everything via rsp+0..N). Wave AB
//         deferred — landed only if the X86 e2e SQLite gate exposes a
//         retval-read regression mirroring #125.
//
// ----------------------------------------------------------------------------
// 4. Coordination with Wave AA Track A (per-arch metadata fields)
// ----------------------------------------------------------------------------
//
//   Track A locked these defaults in MCC2GoFunctionMetadata:
//     SavedLinkSize  = 8  (AArch64 default) — X86 producers MUST set 0.
//     FrameAlignment = 16 (AArch64 default) — X86 producers MUST set 0.
//
//   The minimal viable X86 stager (Wave AA Track B, already landed)
//   sets both to 0 unconditionally. Wave AB MUST preserve this — every
//   code path that constructs a C2GoFunctionMetadata for an X86 function
//   sets both fields to 0 BEFORE handing off to setC2GoStagedMeta.
//
//   Recommended pattern (Wave AB):
//     static C2GoFunctionMetadata makeX86C2GoMetaBase(const MachineFunction &MF) {
//       C2GoFunctionMetadata M;
//       M.Name = std::string(MF.getName());
//       M.SavedLinkSize = 0;      // X86 amd64 contract (Track A)
//       M.FrameAlignment = 0;     // X86 amd64 contract (Track A)
//       return M;
//     }
//   Both emitX86C2GoPrologue (FrameLowering-time) and X86C2GoFrameMetaStager
//   (PreEmit-time) consume this base, then fill in their respective fields.
//
// ----------------------------------------------------------------------------
// 5. API signatures (Wave AB, mirror of llvm::c2go::AArch64 surface)
// ----------------------------------------------------------------------------
//
//   namespace llvm::c2go {
//     // Pure-function summary; cached on X86MachineFunctionInfo.
//     struct X86C2GoFrameInfo {
//       // AB5 GPT round-2 fix: single-path FrameSize formula. The emitter
//       // never emits BP save here (obj6 owns it — see §3.2), so the
//       // no-BP formula is the only path. Reference: cmd/internal/obj/x86
//       // /obj6.go:621-635 (hasFP/BP slot — obj6's responsibility) and
//       // obj6.go:712-720 (localoffset stack-pointer placement).
//       //   FrameSize = roundup(Locals, 16)            (≡ 0 mod 16)
//       uint64_t FrameSize    = 0;
//       bool MakesRealCall    = false;
//       // AB5: pinned to false. Kept for ABI mirror symmetry with the
//       // AArch64 struct; the c2go path does not consult hasFP(MF).
//       bool NeedsFramePointer = false;
//     };
//
//     // Module-flag check (already present in C2GoFrameEmitter.cpp as
//     // a free function; reuse via #include "C2GoFrameEmitter.h" or
//     // duplicate locally — recommendation: lift to a shared header
//     // llvm/Transforms/C2Go/C2GoModeQuery.h once both targets need it).
//     bool isC2GoMode(const MachineFunction &MF);
//
//     // Compute X86 frame summary. Pure. Idempotent.
//     X86C2GoFrameInfo computeX86C2GoFrameInfo(const MachineFunction &MF);
//
//     // Emit the X86 Plan-9 / Go ABI0 prologue. Returns true when the
//     // c2go path handled emission (caller skips standard X86 prologue).
//     bool emitX86C2GoPrologue(MachineFunction &MF, MachineBasicBlock &MBB);
//
//     // Mirror for epilogue.
//     bool emitX86C2GoEpilogue(MachineFunction &MF, MachineBasicBlock &MBB);
//   }
//
//   X86MachineFunctionInfo additions (mirror of AArch64C2GoFunctionState):
//     std::optional<c2go::X86C2GoFrameInfo> C2GoFI;
//     uint64_t C2GoFrameSize = 0;     // value the prologue actually used
//     bool C2GoFrameSizeValid = false;
//     // C2GoStagedMeta — already present (Wave Z Track B).
//
//     const c2go::X86C2GoFrameInfo &getOrComputeC2GoFI(const MachineFunction &MF);
//     bool hasC2GoFrameSize() const;
//     uint64_t getC2GoFrameSize() const;          // asserts valid
//     void setC2GoFrameSize(uint64_t Size);
//
//   X86FrameLowering hook insertion (mirror of the AArch64 dispatch):
//     void X86FrameLowering::emitPrologue(MachineFunction &MF,
//                                         MachineBasicBlock &MBB) const {
//       if (llvm::c2go::emitX86C2GoPrologue(MF, MBB))
//         return;
//       // … existing X86 prologue …
//     }
//     // symmetric for emitEpilogue.
//
// ----------------------------------------------------------------------------
// 6. Implementation order (Wave AB phases)
// ----------------------------------------------------------------------------
//
//   Phase AB.1 — Foundation cache (no behavior change):
//     * Add X86C2GoFrameInfo struct + computeX86C2GoFrameInfo() helper
//       to X86C2GoFrameEmitter.cpp (extend existing TU, do not split).
//     * Add C2GoFI / C2GoFrameSize members + accessors to
//       X86MachineFunctionInfo. Mirror the AArch64C2GoFunctionState
//       names so a future cross-target unification stays mechanical.
//     * Verify: opt -passes=x86-c2go-leaf-abi + llc on the abitest_amd64
//       suite produces byte-identical output (cache is unused yet).
//
//   Phase AB.2 — Hand-rolled prologue / epilogue (behavior change,
//                FrameSize=0 path stays identical to Wave AA Track B):
//     * Implement emitX86C2GoPrologue / Epilogue per §3.2 sequences.
//     * Wire the dispatch in X86FrameLowering::emitPrologue / Epilogue.
//     * Set MFI.setStackSize(FrameSize) + AFI->setC2GoFrameSize(FrameSize)
//       so subsequent passes see the rounded value.
//     * Verify: abitest_amd64 8/8 PASS; new lit test for FrameSize>0
//       function asserting the SUB/ADD shape.
//
//   Phase AB.3 — Non-leaf staged meta (full mirror of AArch64 prologue's
//                meta block, §3.5 algorithm):
//     * Compute LocalsAggMaskBytes via shared c2go::walkPointerFields
//       (FrameReg == X86::RSP filter).
//     * Compute LocalsAmbigMaskBytes from !c2go.union.ambig.words MD on
//       each alloca (identical to AArch64 loop).
//     * Decode ArgPtrMaskBytes from c2go-argptrmask hex attr.
//     * Stage onto X86MachineFunctionInfo::C2GoStagedMeta (extend the
//       Wave AA Track B base) — the existing AsmPrinter consumer needs
//       no change.
//     * Verify: SQLite-min.c WF1 -O0 amd64 path emits FUNCDATA $1 with
//       expected aggregate-field bits set (byte-compare against an
//       AArch64 reference run for a hand-curated set of functions).
//
//   Phase AB.4 — Stack objects (FUNCDATA $2 / stkobj):
//     * X86AsmPrinter::emitFunctionBodyEnd: add publishC2GoStackObjects()
//       mirror — iterates MFI allocas, builds StkObjEntry vector, calls
//       MCPlan9AsmStreamer::publishC2GoStackObjects.
//     * Verify: SQLite-min.c WF1 -O0 amd64 path stkobj table matches the
//       AArch64 reference run shape (the entries are SP-relative byte
//       offsets, so they are arch-independent in content).
//
//   Phase AB.5 — End-to-end gate:
//     * Drive an amd64 SQLite e2e (mirror of the AArch64 release_gate
//       sqlite_e2e_gate.sh) at -O0 and -O2.
//     * The 8/8 abitest_amd64 baseline (#485) stays the unit gate.
//
// ----------------------------------------------------------------------------
// 7. Wave AB work checklist (deferred — flagged as such in StructuredOutput)
// ----------------------------------------------------------------------------
//
//   [ ] AB.1  X86C2GoFrameInfo + computeX86C2GoFrameInfo (this header)
//   [ ] AB.1  X86MachineFunctionInfo cache members + accessors
//   [ ] AB.2  emitX86C2GoPrologue / emitX86C2GoEpilogue impl
//   [ ] AB.2  X86FrameLowering::emitPrologue / Epilogue dispatch hooks
//   [ ] AB.2  -O0 / -O2 abitest_amd64 8/8 byte-identical regression
//   [ ] AB.3  LocalsAggMaskBytes scan (RSP-FI filter)
//   [ ] AB.3  LocalsAmbigMaskBytes scan (!c2go.union.ambig.words MD)
//   [ ] AB.3  ArgPtrMaskBytes hex decode + stage
//   [ ] AB.4  publishC2GoStackObjects in X86AsmPrinter::emitFunctionBodyEnd
//   [ ] AB.5  amd64 SQLite e2e gate (mirror sqlite_e2e_gate.sh)
//   [ ] AB.deferred  M2/M3/M5 spill-slot tags + per-PC liveness (only
//                    when an X86 statepoint GC consumer lands)
//   [x] AB.deferred  HasVariadicOutgoingCall + hasReservedCallFrame port —
//                    CLOSED as PERMANENT FAIL-CLOSED by the Wave AM.2
//                    assessment (2026-06-10, supersedes the Wave AL.3
//                    "still correctly deferred" status). NOTE the [x]
//                    closes a *guard policy decision*, not an
//                    implementation: the non-reserved-CF shape itself
//                    stays UNSUPPORTED and loud-fails — it is not
//                    "handled" (Wave AM GPT round-1 finding 5 wording
//                    guard). The AArch64 s5
//                    PreCallSeqRetvals machinery (AArch64ISelLowering.cpp:
//                    10582-10623) is NOT ported and no port is planned,
//                    because no X86 c2go producer can reach the
//                    non-reserved-CF shape. Evidence (all verified against
//                    HEAD e36b63aea471):
//                      (1) hasVarSizedObjects — c2go Sema hard-rejects ALL
//                          dynamic stack allocation: VLAs via
//                          err_vla_unsupported (SemaType.cpp, VLASupport=
//                          false set by applyC2GoLangDefaults,
//                          CompilerInvocation.cpp) and __builtin_alloca /
//                          _uninitialized / _with_align* via
//                          err_c2go_dynamic_stack (SemaChecking.cpp).
//                          Empirically re-verified: 3/3 dyn-stack forms
//                          rejected under -fc2go, accepted without it.
//                      (2) getHasPushSequences — sole setter is
//                          X86CallFrameOptimization.cpp:625, a post-ISel
//                          pass (always false when the ISel bail-outs
//                          check it) that additionally bails out for c2go
//                          functions (Wave AJ.1, :147-173).
//                      (3) hasPreallocatedCall — sole setter is the
//                          PREALLOCATED_SETUP lowering
//                          (X86ISelLowering.cpp); clang never emits
//                          llvm.call.preallocated bundles (0 producers in
//                          clang/lib), so unreachable from c2go C source.
//                      (4) The AArch64-only s5 producer does not exist on
//                          X86: AArch64 forces non-reserved-CF for
//                          functions with AAPCS variadic outgoing calls
//                          because its saved-LR-at-sp+0 contract clashes
//                          with AAPCS variadic args at sp+0
//                          (AArch64FrameLowering.cpp:664-681 +
//                          hasC2GoVariadicOutgoingCall). On X86 the
//                          hardware CALL-pushed RA takes that slot (§3.1),
//                          variadic outgoing calls stay reserved-CF, and
//                          no HasC2GoVariadicOutgoingCall mechanism exists.
//                      (5) Production error spectrum re-run (stress.c +
//                          sqlite-min.c × -O0/-O2, x86_64-apple-darwin):
//                          0 reportGoABI0NonReservedCFBailout hits, 0
//                          fatal-crash signatures.
//                      (6) Fail-closed defense verified LIVE: synthetic
//                          dyn-alloca goabi0cc IR (bypassing Sema via llc)
//                          trips the LowerReturn loud-fail
//                          (c2go-goabi0-non-reserved-cf-bailout.ll).
//                    The three Wave AK Fix 2 loud-fails
//                    (reportGoABI0NonReservedCFBailout in LowerCall /
//                    LowerCallResult / LowerReturn) are therefore the
//                    PERMANENT guard for this shape: the only way to reach
//                    it is hand-written IR or a future new producer, and
//                    in both cases loud-fail (not silent miscompile) is
//                    the desired behaviour. Re-open ONLY if a new X86
//                    producer of non-reserved-CF c2go functions is
//                    deliberately introduced; in that case note that even
//                    a full PreCallSeqRetvals port must keep the
//                    LowerReturn bail-out — it guards a *callee-side*
//                    CreateFixedObject+FrameIndex base-pointer concern
//                    that is orthogonal to the PreCallSeqRetvals
//                    *caller-side* CALLSEQ_END race.
//   [ ] AB.deferred  i386 (32-bit) port — defer until amd64 lands
//
//   ── Wave AD producer-gap deferred items (release-gate stage 2b skip
//      drivers, observed 2026-06-09 against HEAD = 72e0c402dbb7 / Wave AC):
//   [ ] AD.deferred  LoopVectorize.cpp:7271 VPlan cost-model assert
//                    triggered by `stress.c` (c2go-stress merged TU) at
//                    -O2 on `--target=x86_64-apple-darwin`. AArch64 path
//                    at the same -O2 + same source is clean, so the
//                    delta is X86-target-specific (likely VPlanCostModel
//                    BestFactor disagreement on a loop X86 vectorizes
//                    differently from AArch64). Blocks stage 2b real
//                    signal. Tracking sentinel: run_amd64.sh probe[stress-O2]:
//                    PRODUCER CRASH at preflight.
//   [ ] AD.deferred  X86Plan9InstPrinter SmallVector OOB in
//                    `tryPrintArithReg` (SmallVector.h:301 idx<size())
//                    triggered by `stress_ascast.c` at -O0. The Wave Y
//                    minimal leaf-syntax X86Plan9InstPrinter dispatch
//                    has a missing-fragment case the stress_ascast.c
//                    workload exercises that the abitest 8-case kit
//                    does not. Blocks stage 2b real signal even for the
//                    -O0 fallback path. Tracking sentinel: run_amd64.sh
//                    probe[ascast-O0]: PRODUCER CRASH at preflight.
//                    Once closed (and the LoopVectorize assert above is
//                    closed), stage 2b run_amd64.sh promotes from honest-
//                    SKIP to real signal with zero release_gate.sh
//                    changes; success criteria = `STRESS OK iters=` +
//                    `STRESS ASCAST OK depth=2000 reached=2000` + within-
//                    arch cksum stability (NOT cross-arch cksum match
//                    with aarch64 — each arch has its own cksum).
//
//   ── Wave AD GPT round-2 fixes (narrative drift + infra-vs-producer
//      distinction, 2026-06-09):
//   [x] AD.fix1     run_amd64.sh narrative scrubbed of `linux/amd64 ELF`
//                   + `docker linux/amd64` + `--rosetta-only` fallback
//                   claims that the preflight (hard-bound to
//                   `xcrun --show-sdk-path` + `--target=x86_64-apple-darwin`)
//                   never actually exercised. Driver is now documented as
//                   Rosetta darwin/amd64 only.
//   [x] AD.fix2     run_amd64.sh preflight separates INFRA_FAIL (SDK /
//                   clang / include paths missing → rc=5 hard-fail, NEVER
//                   silenced by --skip-if-impossible) from PRODUCER_CRASH
//                   (stderr matches
//                   `Assertion failed|Stack dump|SmallVectorTemplateCommon|
//                   UNREACHABLE|llvm::report_fatal_error|Segmentation fault`
//                   → rc=6 strict, honest SKIP under
//                   --skip-if-impossible). Closes the silent-skip path
//                   where a misconfigured worker would masquerade as a
//                   producer gap on the default 3-stage release_gate
//                   path.
//
//   ── Wave AI Track A LIVE follow-ups (2026-06-09 — Wave AJ scope):
//   [x] AJ.1+AJ.2    X86 Plan-9 streamer frame contract + WORD→LONG raw-byte
//                    fallback — LANDED 2026-06-10. The Wave AI-emitted
//                    `asm: unbalanced PUSH/POP` had THREE roots, all closed
//                    in one wave:
//                      (a) AJ.2 — X86C2GoFrameMetaStager gated boundary
//                          functions (`c2go-boundary` attr) out of the
//                          staging path because it only accepted
//                          `c2go-argsize`. The Plan-9 streamer then
//                          published the manifest-side `framesize=0`
//                          shipped by clang's `enqueueC2GoBoundary`, but
//                          the LLVM SysV body still emitted `MOVQ <arg>,
//                          N(%rsp)` outgoing-args writes (FrameSetup MIs
//                          are suppressed in Plan-9 mode by
//                          X86MCInstLower.cpp:2603-2608). With
//                          `$framesize=0` obj6.go injects no SP
//                          adjustment → the writes overwrote the
//                          caller's retPC slot
//                          (`stress_init+0x24 unexpected return pc`).
//                          Fix: gate also accepts `c2go-boundary`,
//                          forwards `c2go-boundary-argsize` into Meta.
//                          ArgSize, and reads `MFI.getStackSize()` for
//                          FrameSize (which after PEI already includes
//                          the outgoing-args reservation aligned to
//                          16B; see AJ.4 narrative below — an earlier
//                          draft added MaxCallFrameSize on top, which
//                          double-counted and broke the alignment
//                          invariant).
//                      (b) AJ.1 — `X86CallFrameOptimization` rewrote
//                          `SUBQ $N,%rsp + MOVQ outgoing` into PUSHQ
//                          sequences at -O2+, setting
//                          `HasPushSequences=true` → non-reserved-CF.
//                          obj6.go's deltasp tracker counted the PUSHQs
//                          but never decremented for the raw `ADDQ $N,
//                          %rsp` sweep (only AADJSP-pseudos / POPQ
//                          decrement it), so RET-time `autoffset !=
//                          deltasp` → `asm: unbalanced PUSH/POP`. Fix:
//                          `X86CallFrameOptimization::isLegal` bails
//                          out for c2go-mode functions in either bucket
//                          (`c2go-argsize` internal, `c2go-boundary`
//                          boundary), forcing reserved-CF for the
//                          Plan-9 path.
//                      (c) AJ.1 — `MCPlan9AsmStreamer::emitRawBytesOrFail`
//                          emitted four-byte chunks as `WORD
//                          $0xXXXXXXXX`. AArch64 Go asm treats AWORD as
//                          4 bytes (arm64/asm7.go:465); X86 Go asm
//                          treats AWORD as 2 bytes (x86/asm6.go:1519,
//                          `{AWORD, ybyte, Px, opBytes{2}}`). Result:
//                          every raw-byte X86 instruction was truncated
//                          to its low 2 bytes (SIGILL trap at
//                          `48 c7 40 10 …` →
//                          assembled as `48 c7` + disjoint data → CPU
//                          mid-instruction decode failed). Fix: emit
//                          `LONG $0xXXXXXXXX` on x86 (asm6.go:1176
//                          `{ALONG, ybyte, Px, opBytes{4}}`); AArch64
//                          keeps `WORD` to preserve the byte-identical
//                          cross-arch invariant.
//                    Verification:
//                      * `asm: unbalanced PUSH/POP` count = 0 at -O0
//                        and -O2 (`c2go-stress/run_amd64.sh`).
//                      * `stress_init+0x24 unexpected return pc` gone
//                        at -O0.
//                      * AArch64 SQLite WF1+WF2 -O2 e2e PASS, abitest
//                        8/8 PASS, cksum=0x7a20f stable
//                        (`.build_status/sqlite_e2e_gate.sh --both`).
//                      * Locked by
//                        `llvm/test/CodeGen/X86/c2go-plan9-boundary-frame-aj.ll`.
//                    Residual: amd64 stress runtime SIGSEGV/SIGSEGV in
//                    `writeBarrier` nil-deref / `internal/abi.(*Type).
//                    Pointers` — tracked separately as Plan-9
//                    InstPrinter GOTPCREL-load → direct-load rewrite gap
//                    (boundary `MOVQ runtime·writeBarrier(SB), AX` is
//                    Go-load not LEA; LLVM's GOTPCREL semantics need
//                    deref). DEFERRED — orthogonal to AJ.1/AJ.2 frame
//                    contract.
//   [x] AJ.3        X86 GoABI0 `c2go-reg-return` (SysV-fallback) — LANDED
//                   Wave AJ.3. Mirrors AArch64 CanLowerReturn /
//                   LowerReturn / LowerCall / LowerCallResult bypass to
//                   `RetCC_AArch64_AAPCS` via the x86 equivalent
//                   `RetCC_X86_64_C` (SysV reg file). Internal
//                   goabi0cc-with-`c2go-reg-return` funcs now return via
//                   RAX / XMM0 (etc.) on x86-64 as the SQLite WF1 -O2
//                   path and any sqlite3-min internal helpers require.
//                   Boundary GoABI0 callees (no `c2go-reg-return` attr)
//                   keep the stack-result contract. Mirror reference:
//                   AArch64ISelLowering.cpp:10706-10718 + 10730-10788.
//   [x] AJ.4        Framesize double-count fix (GPT round-1 F1) — LANDED
//                   Wave AJ.4. AJ.2's first draft computed
//                   `Meta.FrameSize = MFI.getStackSize() +
//                   MFI.getMaxCallFrameSize()` in the SysV-fallback
//                   branch. BUT this stager runs in `addPreEmitPass2`
//                   (TargetPassConfig.cpp:1317), AFTER
//                   PrologEpilogInserter (PrologEpilogInserter.cpp:
//                   1122-1123 adds MaxCallFrameSize, 1141 alignTo
//                   StackAlign, 1165 setStackSize). So getStackSize()
//                   already IS the post-PEI 16B-aligned final
//                   autosize. Adding MaxCallFrameSize again was a
//                   double-count: observed values 100/84/124/140 all
//                   satisfied `framesize mod 16 == 4` — the exact
//                   signature of adding a not-yet-aligned amount back
//                   onto an already-aligned base. obj6.go's alignment
//                   check then surfaced as either Go-asm reject or, on
//                   the slip-through, a GC stackmap shift that
//                   manifested as a nil-deref in writeBarrier /
//                   `internal/abi.(*Type).Pointers` one frame later —
//                   the "symptom-moved-one-frame" GPT correctly flagged
//                   as a symptom NOT a typeinfo gap. Fix: read
//                   `MFI.getStackSize()` alone. Locked by the upgraded
//                   `c2go-plan9-boundary-frame-aj.ll` positive
//                   framesize-mod-16==0 check on a 7-arg boundary
//                   callee.
//   [ ] AJ.5 audit  X86CallFrameOptimization bail (Wave AJ.1) is a
//                   workaround, not the root. The real invariant — any
//                   LLVM pass emitting a raw `ADDQ $N, %rsp` /
//                   `SUBQ $N, %rsp` in Plan-9 mode is invisible to
//                   obj6.go's deltasp tracker (which only counts
//                   APUSHQ / AADJSP / APOPQ) — applies to every X86
//                   backend pass that might lower a call-frame
//                   adjustment to raw SP arithmetic. Possible higher-
//                   level fix points: a Plan-9 lookahead pass in
//                   MCPlan9AsmStreamer that systematically rewrites
//                   raw SP adjustments to AADJSP pseudos before
//                   emission, or a per-pass audit that enforces "no
//                   raw SP arithmetic in c2go-mode". Wave AK follow-up:
//                   sweep the X86 backend for all emit sites of
//                   `ADDQ/SUBQ $imm, %rsp` and either route through
//                   AADJSP or add the corresponding per-pass bail.
//   [x] AI.followup  X86 GoABI0 non-reserved-CF MemLoc lowering port —
//                    CLOSED WITHOUT PORT by the Wave AM.2 permanent
//                    fail-closed verdict (2026-06-10): no X86 c2go
//                    producer reaches the non-reserved-CF shape, so the
//                    AArch64 `GoABI0CallNumBytes` + PreCallSeqRetvals
//                    plumbing would be dead machinery. The
//                    `reportGoABI0NonReservedCFBailout` loud-fails
//                    (locked by
//                    `llvm/test/CodeGen/X86/c2go-goabi0-non-reserved-cf-bailout.ll`)
//                    are the permanent guard. Full assessment + evidence:
//                    see the "AB.deferred HasVariadicOutgoingCall" entry
//                    above (§7).
//
//   ── Wave AK Fix 1 / Fix 3 / Fix 4 follow-up (2026-06-10):
//   [x] AK.1.followup-A ★ BLOCKER-1 -O2 BX stale typeinfo nil-deref —
//                    CLOSED by Wave AL.1 (mask/SaveList flip) + Wave
//                    AM.1 real-root correction (2026-06-10). The Wave
//                    AK framing ("RBX is also the platform BasePtr, so
//                    the flip needs a BasePtr relocate first") turned
//                    out NOT to be the production trigger:
//                    `hasBasePointer(MF)` is false for every production
//                    c2go function (fixed-size 8-aligned frame objects;
//                    no VLA / realign / preallocated). The PEI
//                    `Interference usage of base pointer/frame
//                    pointer.` loud-fail was instead caused by the RBP
//                    bit in CSR_NoRegs_RegMask: it set
//                    FPClobberedByCall on every c2go call and PEI's
//                    spillFPBP/checkInterferedAccess rejected any
//                    call-sequence with a frame-index access. Wave AM.1
//                    fix: CSR_NoRegs → CSR_64_NoneRegs (= {RBP};
//                    truthful — Go amd64 ABI keeps BP callee-saved via
//                    obj6.go BP push/pop) on both getCalleeSavedRegs
//                    (GoABI0) and getCallPreservedMask
//                    (GoABI0+C2GoABIInternal), plus an RBP
//                    reserved-pin; the AL.1 RBX/R12-R15 broad pin is
//                    narrowed away (mask alone models BLOCKER-1).
//                    No BasePtr relocate needed FOR PRODUCTION — but
//                    the BasePtr=RBX overlap is NOT refuted as a
//                    corner (Wave AM GPT round-1 finding 3): a
//                    hasBasePointer(MF)=true c2go function (only
//                    reachable today via hand-written IR with stack
//                    realignment + opaque SP adjustment, or the
//                    varsized/preallocated shapes that already
//                    loud-fail at ISel) still has RBX in the
//                    CSR_64_NoneRegs clobber set → BPClobberedByCall →
//                    spillFPBP. Unsupported, fail-closed (spillFPBP
//                    machinery or its loud Interference diagnostic) —
//                    a BasePtr relocate off RBX is the eventual fix if
//                    a real producer appears. Locked by
//                    c2go-goabi0-no-fp-interference.ll (negative-
//                    verified: pre-fix llc FAILs it) +
//                    c2go-goabi0-csr-noregs-blocker1.ll (incl. SYSV
//                    byte-identity RUN, AM round-1 finding 6) +
//                    c2go-abiinternal-mask-no-preserve.ll.
//   [ ] AK.1.followup-B runtime.* / type.* global LEA-vs-LOAD semantic gap —
//                    typeinfo rewrite (X86C2GoFrameEmitter / Plan-9
//                    streamer typeinfo path) only matches the
//                    `c2go.typeinfo.` / `type:` prefix family. Other
//                    runtime-owned globals (notably runtime.writeBarrier,
//                    runtime.gcController, runtime.sched) have the same
//                    "LEA emitted as 8-byte LOAD that interprets the
//                    struct head as a pointer" hazard and cause a -O0
//                    nil-deref on `runtime.writeBarrier.enabled` reads
//                    inside c2go-bucketed callers. Wave AL real fix:
//                    either extend `tryPrintRuntimeGlobal` (X86Plan9-
//                    InstPrinter) so every `runtime.*` and `type.*`
//                    8-byte symbol load is force-rewritten as `LEA + then
//                    LOAD`, or push the LEA-vs-LOAD distinction into the
//                    X86 ISel layer so the IR emits an explicit 2-step
//                    access for any runtime global referenced from c2go
//                    bucket. GPT round-1 BLOCKER-3 audit; AK.1's
//                    GOTPCREL hypothesis was disproved as the root, but
//                    the sibling gap remains real.
//   [x] AK.2.followup raw-SP-arith fatal coverage extension (Wave AL.3
//                    landed). The Wave AK.2 fatal in
//                    X86MCInstLower.cpp:emitInstruction only matched direct
//                    ADD/SUB-immediate-to-RSP forms.
//                    X86FrameLowering::BuildStackAdjustment selects
//                    `LEA64r SP, [SP + imm]` instead of ADD/SUB when
//                    `STI.useLeaForSP()` is true (X86FrameLowering.cpp:
//                    378-393), which the original matcher did not cover.
//                    Wave AL.3 extended the matcher to also catch
//                    `LEA{64,32}r SP, [SP + imm]` (Scale=1, Idx=NoReg,
//                    Disp=imm), plus a negative MIR LIT
//                    (c2go-plan9-raw-sp-arith-lea-loud-fail.mir) that
//                    actually triggers the fatal (handcrafted MIR via
//                    `-start-before=x86-asm-printer`). The Wave AK.2
//                    positive .ll LIT continues to cover the
//                    no-fatal-expected production path. Choice notes:
//                      * blanket `MI->definesRegister(RSP)` over-triggers on
//                        PUSH/POP/CALL/RET implicit-defs — kept an explicit
//                        opcode whitelist (ADD/SUB-imm + LEA-from-SP) instead.
//                      * future X86 passes that materialise a `def %rsp` MI
//                        outside ADD/SUB-imm / LEA-from-SP / PUSH/POP/CALL/
//                        RET would still slip past; covered by the
//                        `MI->definesRegister(RSP)` audit recipe in this
//                        comment block (no production trigger today; tracked
//                        as Wave AM follow-up if a real producer emerges).
//
//   ── Wave AF.3 operand-access systematic audit (2026-06-09):
//   [x] AF.3         X86Plan9InstPrinter.cpp full sweep of every
//                    `MI->getOperand(N)` call site. Wave Y C6 +
//                    Wave AE.2 closed two leaf gaps (tryPrintSPAdjust /
//                    tryPrintArithReg); AF.3 systematic audit closed
//                    three more (tryPrintIndirectCall mem `< 5`,
//                    tryPrintUnconditionalBranch JMP64r `< 1`,
//                    tryPrintUnconditionalBranch JMP64m `< 5`). All
//                    remaining dispatchers were verified head-guarded
//                    and remain byte-identical for well-formed
//                    operands. Locked by
//                    `llvm/test/CodeGen/X86/c2go-plan9-instprinter-oob.ll`
//                    (extended with `call_through_mem_repro` and
//                    `indir_jmp_repro` repros). AArch64 path 0 byte
//                    changes (read-only mirror constraint honoured).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86C2GOFRAMEEMITTER_H
#define LLVM_LIB_TARGET_X86_X86C2GOFRAMEEMITTER_H

#include <cstdint>
#include <vector>

namespace llvm {

class FunctionPass;
class MachineBasicBlock;
class MachineFunction;
class PassRegistry;

/// Legacy-PM entry point for the X86 c2go staged-meta producer.
/// Wired into `X86PassConfig::addPreEmitPass2` so the MachineFunction is
/// stable (post outliner, post BB sections) and the streamer pointer the
/// AsmPrinter will read is the same one this pass's downstream consumer
/// uses.
FunctionPass *createX86C2GoFrameMetaStagerPass();

/// Initialize the legacy MachineFunctionPass with the PassRegistry. Called
/// from `LLVMInitializeX86Target` alongside the other X86 pass initializers.
void initializeX86C2GoFrameMetaStagerPass(PassRegistry &);

namespace c2go {

/// Pure-function summary of the X86 c2go frame decisions. Mirrors
/// `llvm::c2go::C2GoFrameInfo` (AArch64). AB5 GPT round-2 fix: the c2go
/// emitter does NOT consult `X86FrameLowering::hasFP(MF)` — obj6.go owns
/// the BP decision face on the Plan-9 .s path based on the staged
/// `TEXT $framesize` + flag word (see §3.2 of this header).
struct X86C2GoFrameInfo {
  /// align16(Locals); 0 = strict leaf → emitX86C2GoPrologue no-op.
  /// NO `+ saved-LR` adjustment (CALL pushes the return address but
  /// obj6.go does NOT count it in `$framesize`). NO `+ saved-BP` either —
  /// if obj6 injects `push rbp` it accounts for it in its own offsets.
  uint64_t FrameSize = 0;

  /// Has a real CALL / TCRETURN (excludes STACKMAP/PATCHPOINT — same
  /// rule as AArch64 #306/#326).
  bool MakesRealCall = false;

  /// AB5: pinned to false. Kept for ABI mirror symmetry with the AArch64
  /// struct; the c2go path does not consult hasFP(MF) and never emits a
  /// BP save/restore.
  bool NeedsFramePointer = false;

  /// c2go #298 Wave AC.1 — GC precision triplet (mirror of the AArch64
  /// C2GoFrameEmitter inline computation in `emitC2GoPrologue`). Only the
  /// `ArgPtrMaskBytes` field is populated by `computeX86C2GoFrameInfo`
  /// (pure attribute decode, no FI dependency). The two `Locals*MaskBytes`
  /// fields are NOT populated by `computeX86C2GoFrameInfo`; they are
  /// [prologue-populated, AC1.2] computed inside `emitX86C2GoPrologue`
  /// AFTER `MFI.setStackSize(FrameSize)` so `getFrameIndexReference`
  /// returns the final SP-relative offsets. They are staged directly
  /// onto `C2GoFunctionMetadata` from there — the cached `X86C2GoFrameInfo`
  /// is NOT a reader of locals masks (kept here only as ABI mirror with
  /// AArch64's `C2GoFrameInfo` for a future Wave AB.4 stkobj harvester
  /// that may grow a cache reader; today they remain empty in the cache).
  /// Empty = "nothing to publish" (the staging path stages an empty
  /// std::vector<uint8_t> into `C2GoFunctionMetadata`, which the streamer
  /// treats as a no-op for the corresponding FUNCDATA field). Mirror
  /// algorithm is documented in the cpp; the X86-only deltas vs AArch64 are:
  ///   * Aggregate-field locals: `Nbit = FrameSize / 8` (SavedLinkSize=0
  ///     contract — no saved-LR top-word exclusion), FrameReg filter
  ///     swaps `AArch64::SP` → `X86::RSP`.
  ///   * Union-ambig scrub: identical algorithm (alloca-attached
  ///     `!c2go.union.ambig.words` MD), only the FrameReg differs.
  ///   * Args mask: identical hex-decode of `c2go-argptrmask` Fn attr.
  ///
  /// AC1.1 mirror (#327, statepoint GC soundness): when the function
  /// uses the "c2go-gc" GC strategy, BOTH locals masks are returned EMPTY
  /// by `computeX86LocalsMasks` (the lightweight static all-PCs OR is
  /// unsound under per-PC liveness — see AArch64 C2GoFrameEmitter.cpp
  /// :315-328 / 360-361). The streamer then emits no FUNCDATA $1 bits
  /// from this path; LowerSTATEPOINT owns the per-PC pointer-slot record.

  /// Aggregate-field pointer mask over locals (byte-packed bitmap; one bit
  /// per pointer-word slot). [prologue-populated, AC1.2] Filled by
  /// `emitX86C2GoPrologue` post-`setStackSize`, NOT by
  /// `computeX86C2GoFrameInfo`. Empty when no aggregate stack object
  /// carries pointer fields, in strict-leaf FrameSize=0 functions (the
  /// scan short-circuits with the same `FrameSize >= 8` gate the AArch64
  /// path uses; X86 NoSplit=0 contract drops the AArch64 `-8` LR
  /// exclusion), OR under the "c2go-gc" GC strategy (per-PC liveness
  /// owns it — see AC1.1 mirror above).
  std::vector<uint8_t> LocalsAggMaskBytes;

  /// Union-ambiguous word scrub mask (byte-packed bitmap; mirror of
  /// AArch64 #312). [prologue-populated, AC1.2] Filled by
  /// `emitX86C2GoPrologue` post-`setStackSize`, NOT by
  /// `computeX86C2GoFrameInfo`. Bits flag stack words where a union
  /// member is a pointer in one alternative and an integer in another —
  /// the streamer strips them from BOTH the aggregate-field mask and the
  /// per-PC stackmap bits to keep copystack from reading an int as a
  /// pointer. Empty under the "c2go-gc" GC strategy (AC1.1 mirror).
  std::vector<uint8_t> LocalsAmbigMaskBytes;

  /// Args pointer-word mask (byte-packed; decoded from the
  /// `c2go-argptrmask` IR fn attribute clang stamps on every internal
  /// GoABI0 function). Empty when the attribute is missing OR when the
  /// callee has no pointer-typed args (the producer stamps `"00"` in the
  /// latter case so the streamer still emits FUNCDATA $0 — same #330
  /// contract the AArch64 path observes).
  std::vector<uint8_t> ArgPtrMaskBytes;
};

/// Compute the X86 c2go frame summary for MF. Pure. Safe to call
/// repeatedly. Wave AB callers go through
/// `X86MachineFunctionInfo::getOrComputeC2GoFI()` which memoises.
///
/// DEFERRED: declared here for Wave AB; the implementation lands in
/// X86C2GoFrameEmitter.cpp alongside the existing stager body.
X86C2GoFrameInfo computeX86C2GoFrameInfo(const MachineFunction &MF);

/// Emit the X86 Go ABI0 (Plan 9) prologue when MF is in c2go mode and
/// the Wave V LeafABI pass flipped it to CallingConv::C2GoABIInternal.
/// Returns true when the c2go path handled prologue emission (caller
/// — X86FrameLowering::emitPrologue — must skip the standard X86
/// PrologueEmitter). False otherwise.
///
/// DEFERRED to Wave AB.
bool emitX86C2GoPrologue(MachineFunction &MF, MachineBasicBlock &MBB);

/// Mirror of emitX86C2GoPrologue for the epilogue. Returns true when the
/// c2go path handled epilogue emission.
///
/// DEFERRED to Wave AB.
bool emitX86C2GoEpilogue(MachineFunction &MF, MachineBasicBlock &MBB);

} // namespace c2go
} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_X86C2GOFRAMEEMITTER_H
