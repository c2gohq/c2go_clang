set(LLVM_LINK_COMPONENTS
  AArch64CodeGen
  AArch64AsmParser
  AArch64Desc
  AArch64Disassembler
  AArch64Info
  AArch64Utils
  AsmPrinter
  BitReader
  BitWriter
  C2Go
  CodeGen
  Core
  IPO
  IRReader
  Linker
  MC
  MCParser
  Object
  Passes
  Scalar
  Support
  Target
  TargetParser
  TransformUtils
  X86AsmParser
  X86CodeGen
  X86Desc
  X86Disassembler
  X86Info
  )

add_llvm_tool(c2go-lto
  c2go-lto.cpp
  C2GoArCli.cpp
  C2GoEscapeAudit.cpp
  C2GoManifestRebuilder.cpp

  DEPENDS
  intrinsics_gen
  )

# c2go: a convenience aggregate that builds exactly the in-tree c2go toolchain a
# user needs and nothing else — the clang driver, this c2go-lto bitcode linker,
# and the clang resource headers (so `#include <c2go.h>` resolves). `ninja
# c2go-toolchain` is the minimal "build me the c2go compiler" command. Defined
# here (the c2go-lto tool is configured after the clang project, so the clang
# targets are visible) and guarded so a build without the clang project still
# configures cleanly.
if(TARGET clang AND TARGET clang-resource-headers)
  add_custom_target(c2go-toolchain)
  add_dependencies(c2go-toolchain clang c2go-lto clang-resource-headers)
endif()
