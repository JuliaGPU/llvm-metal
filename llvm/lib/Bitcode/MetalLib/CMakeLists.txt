add_llvm_component_library(LLVMMetalLib
  MetalLibWriter.cpp
  MetalLibWriterPass.cpp
  bzip2/blocksort.c
  bzip2/bzlib.c
  bzip2/compress.c
  bzip2/crctable.c
  bzip2/decompress.c
  bzip2/huffman.c
  bzip2/randtable.c
  tar/microtar.c

  DEPENDS
  intrinsics_gen
  LLVMBitWriter50

  LINK_COMPONENTS
  Analysis
  Core
  MC
  Object
  Support
  )
