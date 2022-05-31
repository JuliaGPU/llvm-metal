add_llvm_component_group(Metal)

add_llvm_target(MetalCodeGen
  Metal.cpp
  MetalTargetMachine.cpp
  MetalPasses.cpp

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
