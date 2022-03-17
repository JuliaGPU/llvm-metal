set(LLVM_LINK_COMPONENTS
  Core
  Support
  BitReader
  Target
  MetalInfo
  MetalCodeGen
  )

add_llvm_tool(metallib-as
  metallib-as.cpp

  DEPENDS
  intrinsics_gen
  )
