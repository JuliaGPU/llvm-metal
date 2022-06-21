add_llvm_component_library(LLVMBitWriter32
  BitWriter32.cpp
  BitcodeWriter32.cpp
  BitcodeWriterPass32.cpp
  ValueEnumerator32.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  MC
  Object
  Support
  )
