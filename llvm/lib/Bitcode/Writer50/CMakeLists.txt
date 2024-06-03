add_llvm_component_library(LLVMBitWriter50
  BitWriter50.cpp
  BitcodeWriter50.cpp
  BitcodeWriterPass50.cpp
  ValueEnumerator50.cpp
  ModuleRewriter50.cpp
  PointerTypeAnalysis.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Analysis
  Core
  MC
  Object
  Support
  TransformUtils
  )
