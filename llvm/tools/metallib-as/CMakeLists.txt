set(LLVM_LINK_COMPONENTS
  Core
  Support
  IRReader
  Analysis
  Target
  AllTargetsInfos
  AllTargetsCodeGens
  MC
  )

add_llvm_tool(metallib-as
  metallib-as.cpp

  DEPENDS
  intrinsics_gen
  )
