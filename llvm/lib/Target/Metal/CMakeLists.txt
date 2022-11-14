add_llvm_component_group(Metal)

add_llvm_target(MetalCodeGen
  Metal.cpp
  MetalTargetMachine.cpp
  MetalPasses.cpp

  bzip2/blocksort.c
  bzip2/bzlib.c
  bzip2/compress.c
  bzip2/crctable.c
  bzip2/decompress.c
  bzip2/huffman.c
  bzip2/randtable.c
  tar/microtar.c

  LINK_COMPONENTS
  Core
  CodeGen
  MetalInfo
  Scalar
  Support
  Target
  MC
  TransformUtils
  SelectionDAG
  BitWriter50
  Analysis
  InstCombine
  IPO
  Passes

  ADD_TO_COMPONENT
  Metal
  )

add_subdirectory(TargetInfo)
