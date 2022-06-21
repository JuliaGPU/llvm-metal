add_llvm_component_library(LLVMSPIRVerifier
  SpirIterators.cpp
  SpirTables.cpp
  SpirErrors.cpp
  SpirValidation.cpp
  
  DEPENDS
  intrinsics_gen
  )
