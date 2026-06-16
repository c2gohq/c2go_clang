//===- C2GoMemcpyTyping.h - Auto-type memcpy/memmove ------------*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go-memcpy-typing LLVM pass.
//
// For every `memcpy(dst, src, n)` / `memmove(dst, src, n)` call, the
// pass inspects:
//   - The static type of `dst` (from Clang AST metadata `!c2go.elem.type`).
//   - The shape of `n` (constant `sizeof(T)`, runtime `sizeof(T) * N`,
//     or arbitrary).
//
// Decision table:
//
//   dst type        | n shape         | T contains ptrs | lower to
//   ----------------+-----------------+-----------------+-----------------------
//   T*              | sizeof(T)       | yes             | runtime.typedmemmove
//   T*              | sizeof(T)*N     | yes             | _c2go_typedMemmoveArray
//   T*              | sizeof(T)[*N]   | no              | @llvm.memcpy intrinsic
//   T*              | arbitrary       | any             | @llvm.memcpy intrinsic
//   void*/char*/u8* | any             | n/a             | @llvm.memcpy intrinsic
//
// For the noscan / no-barrier path, the pass leaves the call as the
// LLVM @llvm.memcpy / @llvm.memmove intrinsic. The LLVM backend then
// picks the best lowering: inline SIMD for small fixed sizes, libc-
// style call (resolved through c2go-libc → runtime.memmove) for large
// or unknown sizes. This is preferable to always emitting a
// runtime.memmove call, since Clang's builtin path can inline small
// fixed-size copies into a few SIMD instructions.
//
// Only the pointer-bearing managed case requires runtime calls
// (typedmemmove / _c2go_typedMemmoveArray) so the bulk write barrier
// fires correctly.
//
// See docs/c2go_design.md §6.5.
//
// Status: implemented. Rewrites managed memcpy/memmove calls to
// runtime.typedmemmove / _c2go_typedMemmoveArray per the table above and
// leaves the noscan path as the LLVM @llvm.memcpy / @llvm.memmove intrinsic.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_C2GO_C2GOMEMCPYTYPING_H
#define LLVM_TRANSFORMS_C2GO_C2GOMEMCPYTYPING_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class C2GoMemcpyTypingPass : public PassInfoMixin<C2GoMemcpyTypingPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_C2GO_C2GOMEMCPYTYPING_H
