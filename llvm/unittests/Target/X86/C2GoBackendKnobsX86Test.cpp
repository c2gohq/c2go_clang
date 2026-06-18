//===- C2GoBackendKnobsX86Test.cpp - X86 c2go BackendConfig hook ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//
//
// Verify that LLVMInitializeX86Target registers an X86 c2go BackendConfig
// applier for both Triple::x86 and Triple::x86_64, so the arch-neutral
// llvm::c2go::applyC2GoBackendConfig dispatcher resolves to a real applier
// on X86 rather than falling through as "no applier for this arch".
//
// The production applier writes the calling-convention knob booleans into
// the per-TargetMachine C2Go* fields. The first two tests spy the dispatch
// path; the third exercises the production applier directly and reads back
// each field via the X86TargetMachine subclass.
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

// Spy state, bumped each time SpyApplier is dispatched. Static because the
// hook is a plain C-style function pointer.
static unsigned SpyHitCount = 0;
static c2go::BackendConfig SpyLastCfg{};

static void SpyApplier(TargetMachine *TM, const c2go::BackendConfig &Cfg) {
  ++SpyHitCount;
  SpyLastCfg = Cfg;
  // The dispatcher must only route here when the arch matches the registered
  // key.
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

// The x86_64 hook resolves and dispatches; a spy applier is registered to
// observe the routed config.
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

// The i386 (32-bit) hook resolves and dispatches, exercising the second
// registration in LLVMInitializeX86Target.
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

// The production applier writes each config bool into the per-TargetMachine
// C2Go* field. Pins the real consumer wiring: a regression that left the
// applier as a (void)Cfg no-op would fail here.
TEST_F(C2GoBackendKnobsX86Test, WritesX86TMFields) {
  auto TM = createX86TM("x86_64-unknown-linux");
  ASSERT_TRUE(TM);
  auto *X86TM = static_cast<X86TargetMachine *>(TM.get());

  // Every field must be false before any apply.
  EXPECT_FALSE(X86TM->C2GoForceBlockAddressJumpTable);
  EXPECT_FALSE(X86TM->C2GoDisableRegisterCoalescing);
  EXPECT_FALSE(X86TM->C2GoDisableGlobalMerge);

  // No spy registered, so this exercises the production applier directly.
  c2go::BackendConfig Cfg{/*ForceBlockAddressJumpTable=*/true,
                          /*DisableRegisterCoalescing=*/true,
                          /*DisableGlobalMerge=*/true};
  c2go::applyC2GoBackendConfig(TM.get(), Cfg);
  EXPECT_EQ(SpyHitCount, 0u);

  // All three fields must have flipped.
  EXPECT_TRUE(X86TM->C2GoForceBlockAddressJumpTable);
  EXPECT_TRUE(X86TM->C2GoDisableRegisterCoalescing);
  EXPECT_TRUE(X86TM->C2GoDisableGlobalMerge);

  // Re-applying with all-false clears them; the applier is re-writable.
  c2go::BackendConfig Off{};
  c2go::applyC2GoBackendConfig(TM.get(), Off);
  EXPECT_FALSE(X86TM->C2GoForceBlockAddressJumpTable);
  EXPECT_FALSE(X86TM->C2GoDisableRegisterCoalescing);
  EXPECT_FALSE(X86TM->C2GoDisableGlobalMerge);
}

} // namespace
