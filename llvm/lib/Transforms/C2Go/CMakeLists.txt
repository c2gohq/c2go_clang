add_llvm_component_library(LLVMC2Go
  C2GoCommon.cpp
  C2GoWriteBarriers.cpp
  C2GoGCSetup.cpp
  C2GoGCMaskUtils.cpp
  C2GoLeafEligibility.cpp
  C2GoLibCallRouting.cpp
  C2GoLoopPoll.cpp
  C2GoMemcpyTyping.cpp
  C2GoPipeline.cpp
  C2GoSafepoint.cpp
  C2GoEscapeCheck.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/C2Go

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  Scalar
  Support
  TransformUtils
  )
