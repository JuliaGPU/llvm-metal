set(LLVM_LINK_COMPONENTS
  BitReader
  Core
  Support
  )

add_llvm_tool(metallib-dis
  metallib-dis.cpp
  
  DEPENDS
  intrinsics_gen
  )
