add_llvm_library(LLVMBitWriter70
  BitWriter70.cpp
  BitcodeWriter70.cpp
  BitcodeWriterPass70.cpp
  ValueEnumerator70.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  MC
  Object
  Support
  )
