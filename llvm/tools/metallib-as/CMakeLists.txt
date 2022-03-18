set(LLVM_LINK_COMPONENTS
  Core
  Support
  BitReader
  Target
  AllTargetsInfos
  AllTargetsCodeGens
  )

add_llvm_tool(metallib-as
  metallib-as.cpp

  DEPENDS
  intrinsics_gen
  )
