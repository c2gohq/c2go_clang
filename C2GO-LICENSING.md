# c2go-clang licensing

[简体中文](C2GO-LICENSING.zh-CN.md)

This document explains the licensing layout of the c2go-clang fork. It does not replace the license texts and is not legal advice.

## Repository source

The LLVM Project is licensed under the Apache License 2.0 with LLVM Exceptions (`Apache-2.0 WITH LLVM-exception`), subject to the per-file and third-party notices already present in the repository. The canonical text is [LICENSE.TXT](LICENSE.TXT).

C2Go code integrated into the LLVM/Clang source tree is offered under the same `Apache-2.0 WITH LLVM-exception` terms unless a file explicitly states otherwise. Its authors retain copyright in their contributions while granting the permissions in that license.

This applies even when a C2Go file was newly created: once it is contributed to and distributed as an integrated part of this LLVM fork, keeping the LLVM project license is the clearest policy and avoids creating a license-incompatible compiler tree. A genuinely independent program distributed in another repository may use a different compatible license; that is why `c2go-bind` and the original portions of `c2go-libc` have their own licensing model.

The fork does not remove or narrow any permission granted for upstream LLVM material. Redistribution must preserve the applicable copyright, license, attribution, and modification notices, including the fork [NOTICE](NOTICE).

## Compiler output

Running c2go-clang does not ordinarily make user input or the translated form of that input subject to the compiler's license. Copyright and licensing of input-derived output follow the input and applicable law.

A resulting Go package may also contain separately copyrighted C2Go support code emitted by `c2go-bind` or linked from `c2go-libc`. Those actual support-code portions are governed by the licenses shipped with those projects. A comment inserted into an output file does not, by itself, change ownership of user-authored input.

## Commercial use

LLVM and the C2Go changes in this repository may be used commercially under `Apache-2.0 WITH LLVM-exception`; no separate c2go commercial license is required merely to use this compiler fork.

Commercial licensing offered for `c2go-bind` or original `c2go-libc` components covers only rights controlled by their respective copyright holders. It does not relicense LLVM or other third-party material.

## Contributions

Code contributed to this LLVM-integrated fork must be available under `Apache-2.0 WITH LLVM-exception`. Code under a different license must not be copied into the fork without an explicit provenance and compatibility review.

## Names and affiliation

This fork is not sponsored or endorsed by the LLVM Project or LLVM Foundation. The licenses do not grant rights to use LLVM or c2go names, logos, or marks beyond accurate descriptive reference as allowed by law.
