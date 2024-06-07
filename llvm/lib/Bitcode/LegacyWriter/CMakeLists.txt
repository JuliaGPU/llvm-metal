add_llvm_component_library(LLVMLegacyBitWriter
  BitWriter50.cpp
  BitcodeWriter50.cpp
  BitcodeWriterPass50.cpp
  ValueEnumerator50.cpp
  ModuleRewriter50.cpp
  BitWriter70.cpp
  BitcodeWriter70.cpp
  BitcodeWriterPass70.cpp
  ValueEnumerator70.cpp
  ModuleRewriter70.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  MC
  Object
  Support
  )
