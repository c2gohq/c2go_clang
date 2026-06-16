//===- C2GoBackendKnobsX86Test.cpp - X86 c2go BackendConfig hook smoke ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// c2go #298 / #435: smoke-test that `LLVMInitializeX86Target` registers an
// `applyX86C2GoConfig` hook for Triple::x86 + Triple::x86_64, so the
// arch-neutral `llvm::c2go::applyC2GoBackendConfig(TM, Cfg)` dispatcher
// resolves to a real applier on X86 instead of silently falling through as
// "no applier for this arch".
//
// Wave W Track B: the production applier is no longer a no-op — it writes
// the 3 booleans into the per-TM `C2Go*` fields (mirror of
// `applyAArch64C2GoConfig`). Tests (1) and (2) still spy the dispatcher
// path; the new test (3) replaces the prior "default applier is noop"
// observation with a direct `WritesX86TMFields` check that exercises the
// production applier and reads back each field via the X86 subclass.
//
//===---------------------------------------------------------------------===//

#include "X86TargetMachine.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/C2GoBackendKnobs.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

static std::unique_ptr<TargetMachine> createX86TM(const char *TripleStr) {
  Triple TT(TripleStr);
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget(TT, Error);
  if (!TheTarget)
    return nullptr;
  return std::unique_ptr<TargetMachine>(TheTarget->createTargetMachine(
      TT, "", "", TargetOptions(), std::nullopt, std::nullopt,
      CodeGenOptLevel::Default));
}

// Spy state — incremented every time SpyApplier is dispatched. Static
// because `ApplyC2GoConfigFn` is a plain C-style function pointer.
static unsigned SpyHitCount = 0;
static c2go::BackendConfig SpyLastCfg{};

static void SpyApplier(TargetMachine *TM, const c2go::BackendConfig &Cfg) {
  ++SpyHitCount;
  SpyLastCfg = Cfg;
  // Sanity: dispatcher should only route to us when the arch matches the key
  // we registered against.
  ASSERT_TRUE(TM != nullptr);
  ASSERT_TRUE(TM->getTargetTriple().isX86());
}

class C2GoBackendKnobsX86Test : public ::testing::Test {
protected:
  void SetUp() override {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    SpyHitCount = 0;
    SpyLastCfg = c2go::BackendConfig{};
  }
};

// (1) x86_64 hook resolves and dispatches. Default-registered applier is a
// no-op, so we re-register a spy to observe the dispatch.
TEST_F(C2GoBackendKnobsX86Test, DispatchesOnX86_64) {
  auto TM = createX86TM("x86_64-unknown-linux");
  ASSERT_TRUE(TM) << "x86_64 TargetMachine lookup failed";

  c2go::registerC2GoBackendConfigHook(Triple::x86_64, &SpyApplier);

  c2go::BackendConfig Cfg{/*ForceBlockAddressJumpTable=*/true,
                          /*DisableRegisterCoalescing=*/false,
                          /*DisableGlobalMerge=*/true};
  c2go::applyC2GoBackendConfig(TM.get(), Cfg);

  EXPECT_EQ(SpyHitCount, 1u);
  EXPECT_TRUE(SpyLastCfg.ForceBlockAddressJumpTable);
  EXPECT_FALSE(SpyLastCfg.DisableRegisterCoalescing);
  EXPECT_TRUE(SpyLastCfg.DisableGlobalMerge);
}

// (2) i386 (32-bit) hook resolves and dispatches — exercises the second
// registration in `LLVMInitializeX86Target`.
TEST_F(C2GoBackendKnobsX86Test, DispatchesOnI386) {
  auto TM = createX86TM("i386-unknown-linux");
  ASSERT_TRUE(TM) << "i386 TargetMachine lookup failed";

  c2go::registerC2GoBackendConfigHook(Triple::x86, &SpyApplier);

  c2go::BackendConfig Cfg{/*ForceBlockAddressJumpTable=*/false,
                          /*DisableRegisterCoalescing=*/true,
                          /*DisableGlobalMerge=*/false};
  c2go::applyC2GoBackendConfig(TM.get(), Cfg);

  EXPECT_EQ(SpyHitCount, 1u);
  EXPECT_FALSE(SpyLastCfg.ForceBlockAddressJumpTable);
  EXPECT_TRUE(SpyLastCfg.DisableRegisterCoalescing);
  EXPECT_FALSE(SpyLastCfg.DisableGlobalMerge);
}

// (3) Wave W Track B: production applier writes each Cfg bool into the
// per-TM `C2Go*` field. Verifies the real consumer wiring is live — a
// regression that left the applier as a (void)Cfg no-op would fail here.
TEST_F(C2GoBackendKnobsX86Test, WritesX86TMFields) {
  auto TM = createX86TM("x86_64-unknown-linux");
  ASSERT_TRUE(TM);
  auto *X86TM = static_cast<X86TargetMachine *>(TM.get());

  // Default state — every field must be false before any apply.
  EXPECT_FALSE(X86TM->C2GoForceBlockAddressJumpTable);
  EXPECT_FALSE(X86TM->C2GoDisableRegisterCoalescing);
  EXPECT_FALSE(X86TM->C2GoDisableGlobalMerge);

  // Don't re-register the spy — exercise the production applier directly.
  c2go::BackendConfig Cfg{/*ForceBlockAddressJumpTable=*/true,
                          /*DisableRegisterCoalescing=*/true,
                          /*DisableGlobalMerge=*/true};
  c2go::applyC2GoBackendConfig(TM.get(), Cfg);
  EXPECT_EQ(SpyHitCount, 0u);

  // All three fields must have flipped.
  EXPECT_TRUE(X86TM->C2GoForceBlockAddressJumpTable);
  EXPECT_TRUE(X86TM->C2GoDisableRegisterCoalescing);
  EXPECT_TRUE(X86TM->C2GoDisableGlobalMerge);

  // Flip back — applier is idempotent / re-writable.
  c2go::BackendConfig Off{};
  c2go::applyC2GoBackendConfig(TM.get(), Off);
  EXPECT_FALSE(X86TM->C2GoForceBlockAddressJumpTable);
  EXPECT_FALSE(X86TM->C2GoDisableRegisterCoalescing);
  EXPECT_FALSE(X86TM->C2GoDisableGlobalMerge);
}

} // namespace
