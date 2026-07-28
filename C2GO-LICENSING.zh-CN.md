# c2go-clang 许可说明

[English](C2GO-LICENSING.md)

本文解释 c2go-clang fork 的许可结构，不替代正式许可证文本，也不构成法律意见。

## 仓库源码

LLVM Project 按带 LLVM Exceptions 的 Apache License 2.0（`Apache-2.0 WITH LLVM-exception`）许可，但仍应遵守仓库内既有的逐文件声明和第三方声明。权威文本为 [LICENSE.TXT](LICENSE.TXT)。

集成进 LLVM/Clang 源码树的 C2Go 代码，除文件另有明确声明外，也按 `Apache-2.0 WITH LLVM-exception` 提供。作者仍然拥有其贡献的版权，同时授予该许可证规定的权利。

即使某个 C2Go 文件完全由你新建，只要它作为 LLVM fork 的集成部分贡献和发布，继续采用 LLVM 项目许可仍是最清晰的做法，也能避免把编译器源码树变成许可证不兼容的组合。真正独立、在其他仓库单独发布的程序可以选择不同的兼容许可证；因此 `c2go-bind` 与 `c2go-libc` 的原创部分可以采用另一套许可模式。

本 fork 不会移除或收窄上游 LLVM 材料已经授予的权利。再发布时必须保留适用的版权、许可、归属和修改说明，包括本 fork 的 [NOTICE](NOTICE)。

## 编译器输出

运行 c2go-clang 通常不会使用户输入或该输入的翻译形式自动适用编译器许可证。由输入派生的输出，其版权和许可跟随输入及适用法律。

最终 Go 包还可能包含 `c2go-bind` 实际生成的 C2Go 支持代码，或者链接 `c2go-libc` 的代码。这些真实存在的支持代码部分适用对应项目随附的许可证。仅仅在输出文件中插入一段注释，并不会取得用户输入代码的版权。

## 商业使用

本仓库中的 LLVM 和 C2Go 修改可以依照 `Apache-2.0 WITH LLVM-exception` 商业使用；仅仅使用本编译器 fork 不需要另购 c2go 商业许可证。

`c2go-bind` 或 `c2go-libc` 原创组件提供的商业许可，只覆盖对应版权持有人能够授权的权利，不会重新许可 LLVM 或其他第三方材料。

## 贡献

提交到该 LLVM 集成 fork 的代码必须能够按 `Apache-2.0 WITH LLVM-exception` 提供。未经明确的来源和兼容性审查，不得把其他许可证代码复制进本 fork。

## 名称与关联

本 fork 未获 LLVM Project 或 LLVM Foundation 赞助或背书。许可证不授予 LLVM 或 c2go 名称、标志、商标的使用权；法律允许的真实描述性引用除外。
