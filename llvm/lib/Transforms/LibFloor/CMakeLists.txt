add_llvm_component_library(LLVMLibFloor
  AddressSpaceFix.cpp
  CFGStructurization.cpp
  CMakeLists.txt
  CUDAFinal.cpp
  CUDAImage.cpp
  FMACombiner.cpp
  FloorImage.cpp
  LibFloor.cpp
  MetalFinal.cpp
  MetalImage.cpp
  PropagateRangeInfo.cpp
  SPIRFinal.cpp
  SPIRImage.cpp
  StructuralAnalysis.cpp
  StructuralTransform.cpp
  VulkanFinal.cpp
  VulkanImage.cpp
  
  cfg/cfg_structurizer.cpp
  cfg/cfg_translator.cpp
  cfg/node.cpp

  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/LibFloor

  DEPENDS
  intrinsics_gen

  COMPONENT_NAME
  LibFloor

  LINK_COMPONENTS
  AggressiveInstCombine
  Analysis
  Core
  InstCombine
  Support
  TransformUtils
  )
