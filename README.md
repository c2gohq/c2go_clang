# c2go-clang

[简体中文](README.zh-CN.md) | [Upstream LLVM README](README.llvm.md)

`c2go-clang` is an experimental LLVM/Clang fork that compiles C into Go-toolchain-compatible Plan 9 assembly and a JSON export manifest. Together with `c2go-bind` and `c2go-libc`, it lets selected C code run inside a Go program and participate in the Go runtime's scheduling and managed-memory model.

This repository is an independent fork, not an official LLVM project or release.

## Toolchain

The complete c2go flow has three separately versioned components:

```text
C source
   │
   ▼
c2go-clang + c2go-lto     Apache-2.0 WITH LLVM-exception
   │  Plan 9 .s + JSON manifest
   ▼
c2go-bind                 AGPL-3.0-only or commercial license
   │  generated Go package
   ▼
c2go-libc + Go toolchain  mixed third-party and c2go-owned code
```

This repository contains the first component: the Clang driver, C2Go LLVM passes and backends, `c2go-lto`, and the `<c2go.h>` resource header. The compiler executable is currently named `clang`, not `c2go-clang`. `c2go-bind` and `c2go-libc` are published separately and must be used at compatible release versions.

## Current scope

- C only; C++ is not supported.
- 64-bit targets only.
- Implementations exist for X86-64 and AArch64, with target support and release certification tracked separately.
- The generated Go/runtime contract is versioned and must match the accompanying `c2go-bind` and `c2go-libc` release.
- The project is pre-1.0. Interfaces, manifest fields, and ABI epochs may change between minor releases.
- This repository does not currently claim production readiness.

Additional fork-specific design documentation is still being reviewed and is not part of this initial repository documentation set. Release-certified platform and Go-version combinations will be listed by the unified toolchain release rather than inferred from code generation support alone.

## Build

Requirements include CMake, Ninja, a supported host C++ compiler, Python 3, and the normal LLVM build dependencies.

```sh
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCLANG_INCLUDE_TESTS=ON \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64"

ninja -C build c2go-toolchain
```

The aggregate target builds `clang`, `c2go-lto`, and Clang resource headers. Keep `clang` and `c2go-lto` in the same `bin/` directory because the driver locates the linker there.

## Minimal compilation example

```sh
build/bin/clang \
  --target=x86_64-unknown-linux-goabi \
  -fc2go \
  -fc2go-package=example.com/acme/mypkg \
  -O2 \
  -fc2go-emit-plan9-asm=out.s \
  -fc2go-emit-manifest=out.json \
  -c -o out.o input.c
```

The `.s` and JSON manifest are the c2go outputs consumed by `c2go-bind`; the `.o` file is incidental to this flow. A full consumer build also needs compatible `c2go-bind`, `c2go-libc`, and Go toolchain versions.

## Testing

Build the affected targets first, then run the relevant LLVM/Clang tests from the configured build directory:

```sh
ninja -C build c2go-toolchain
ninja -C build check-llvm check-clang

build/bin/llvm-lit -sv \
  --filter='c2go|C2Go' \
  build/test \
  build/tools/clang/test
```

Cross-repository binding tests must receive the actual `c2go-bind` binary explicitly:

```sh
C2GOBIND=/absolute/path/to/c2go-bind \
  build/bin/llvm-lit -sv build/test/tools/c2go-bind
```

A green compiler build or LLVM LIT run is not a substitute for the unified release's end-to-end `c2go-libc` and `go build` matrix.

## Licensing

The LLVM code and the C2Go changes integrated into this fork are distributed under `Apache-2.0 WITH LLVM-exception`, except for files that explicitly carry another upstream-compatible license. The canonical terms remain in [LICENSE.TXT](LICENSE.TXT); the fork attribution and modification summary are in [NOTICE](NOTICE).

Using `c2go-clang` does not, by itself, impose the compiler's license on user input or compiler output. Separately distributed C2Go support code embedded in or linked with generated packages has its own terms. See [C2GO-LICENSING.md](C2GO-LICENSING.md) and the licensing documents shipped by `c2go-bind` and `c2go-libc`.

## Contributing and security

This fork follows LLVM's coding conventions for LLVM-integrated code. Contributions intended for this repository must be available under `Apache-2.0 WITH LLVM-exception`; do not submit code that you do not have the right to contribute under those terms.

The fork-specific public issue, security-reporting, and contribution channels have not yet been opened. Until they are published, do not report c2go issues to LLVM's upstream trackers as though c2go were an official LLVM feature.
