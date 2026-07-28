# c2go-clang

[English](README.md) | [LLVM 上游 README](README.llvm.md)

`c2go-clang` 是一个实验性的 LLVM/Clang fork。它把 C 编译为 Go 工具链可以使用的 Plan 9 汇编和 JSON 导出 manifest。它与 `c2go-bind`、`c2go-libc` 配合，使选定的 C 代码可以运行在 Go 程序中，并接入 Go 运行时的调度和托管内存模型。

本仓库是独立 fork，不是 LLVM 官方项目或官方发行版。

## 工具链组成

完整的 c2go 流程由三个独立版本化的组件组成：

```text
C 源码
  │
  ▼
c2go-clang + c2go-lto       Apache-2.0 WITH LLVM-exception
  │  Plan 9 .s + JSON manifest
  ▼
c2go-bind                   AGPL-3.0-only 或商业许可
  │  生成的 Go 包
  ▼
c2go-libc + Go 工具链       第三方代码与 c2go 自有代码的组合
```

本仓库包含第一部分：Clang driver、C2Go LLVM passes 和后端、`c2go-lto` 以及 `<c2go.h>` resource header。编译器可执行文件目前仍叫 `clang`，并不叫 `c2go-clang`。`c2go-bind` 和 `c2go-libc` 独立发布，使用时必须选择相互兼容的版本。

## 当前范围

- 仅支持 C，不支持 C++。
- 仅支持 64 位目标。
- X86-64 和 AArch64 已有实现，但代码生成支持与发布认证状态会分别记录。
- 生成代码与 Go runtime 之间有版本化契约，必须匹配同一发行系列的 `c2go-bind` 和 `c2go-libc`。
- 项目仍处于 1.0 之前；接口、manifest 字段及 ABI epoch 可能在次版本间变化。
- 本仓库目前不宣称已经达到 production-ready。

更多 fork 专属设计文档仍在复核中，不属于本次首批仓库文档。统一 toolchain release 会列出真正通过发布认证的平台与 Go 版本组合，不能仅从已有 codegen 推断。

## 构建

需要 CMake、Ninja、受支持的宿主 C++ 编译器、Python 3，以及 LLVM 的常规构建依赖。

```sh
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DCLANG_INCLUDE_TESTS=ON \
  -DLLVM_ENABLE_PROJECTS=clang \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64"

ninja -C build c2go-toolchain
```

聚合 target 会构建 `clang`、`c2go-lto` 和 Clang resource headers。`clang` 与 `c2go-lto` 必须处于同一个 `bin/` 目录，因为 driver 会从该目录查找 linker。

## 最小编译示例

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

`.s` 和 JSON manifest 才是交给 `c2go-bind` 的 c2go 产物；`.o` 在该流程中只是附带输出。完整的消费者构建还需要兼容版本的 `c2go-bind`、`c2go-libc` 和 Go 工具链。

## 测试

先构建受影响的 target，再从配置好的 build 目录运行相关 LLVM/Clang 测试：

最低限度应重新构建：

```sh
ninja -C build c2go-toolchain
ninja -C build check-llvm check-clang

build/bin/llvm-lit -sv \
  --filter='c2go|C2Go' \
  build/test \
  build/tools/clang/test
```

跨仓绑定测试必须显式传入真实的 `c2go-bind`：

```sh
C2GOBIND=/absolute/path/to/c2go-bind \
  build/bin/llvm-lit -sv build/test/tools/c2go-bind
```

编译器成功构建或 LLVM LIT 通过，不能替代统一 release 中包含 `c2go-libc` 和 `go build` 的端到端矩阵。

## 许可

LLVM 代码及集成在该 fork 中的 C2Go 修改统一按 `Apache-2.0 WITH LLVM-exception` 发布；明确标有其他上游兼容许可的文件除外。权威条款仍是 [LICENSE.TXT](LICENSE.TXT)；fork 归属与修改概要见 [NOTICE](NOTICE)。

仅仅使用 `c2go-clang` 不会把编译器许可证自动施加到用户输入或编译器输出上。生成包中实际嵌入或链接的、独立发布的 C2Go 支持代码适用其自身许可。详见 [C2GO-LICENSING.zh-CN.md](C2GO-LICENSING.zh-CN.md) 以及 `c2go-bind`、`c2go-libc` 随附的许可文件。

## 贡献与安全

LLVM 集成代码遵循 LLVM 的代码与审查规范。提交到本仓库的贡献必须能够按 `Apache-2.0 WITH LLVM-exception` 提供。请勿提交你无权按该许可提供的代码。

本 fork 自己的公开 issue、安全报告和贡献渠道尚未启用。在这些渠道发布前，请勿把 c2go 问题当作 LLVM 官方功能问题提交给 LLVM 上游。
