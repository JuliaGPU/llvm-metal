add_llvm_component_library(LLVMMetalInfo
  MetalTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  Metal
)
