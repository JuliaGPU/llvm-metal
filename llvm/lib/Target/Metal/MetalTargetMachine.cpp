//===-- MetalTargetMachine.cpp - TargetMachine for the Metal backend -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MetalTargetMachine.h"
#include "Metal.h"

#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/Support/TargetRegistry.h"

using namespace llvm;

extern "C" LLVM_EXTERNAL_VISIBILITY void LLVMInitializeMetalTarget() {
  // Register the target.
  RegisterTargetMachine<MetalTargetMachine> X(getTheMetalTarget());

  PassRegistry *PR = PassRegistry::getPassRegistry();
  initializeMetalFinalPass(*PR);
  initializeMetalFinalModuleCleanupPass(*PR);
}

namespace {
/// Pass Configuration Options.
class MetalPassConfig : public TargetPassConfig {
public:
  MetalPassConfig(MetalTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  MetalTargetMachine &getMetalTargetMachine() const {
    return getTM<MetalTargetMachine>();
  }

  void addIRPasses() override;
};
} // namespace

TargetPassConfig *MetalTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new MetalPassConfig(*this, PM);
}

void MetalPassConfig::addIRPasses() {
  // run backend final passes at the very end, no IR should change after this point!
  addPass(createMetalFinalPass());
  addPass(createMetalFinalModuleCleanupPass());

  // cleanup
  addPass(createTailCallEliminationPass());
  addPass(createCFGSimplificationPass());
  addPass(createInstructionCombiningPass());
  addPass(createGVNPass());
  addPass(createAggressiveDCEPass());

  TargetPassConfig::addIRPasses();
}

bool MetalTargetMachine::addPassesToEmitFile(PassManagerBase &PM,
                                             raw_pwrite_stream &Out,
                                             raw_pwrite_stream *DwoOut,
                                             CodeGenFileType FileType,
                                             bool DisableVerify,
                                             MachineModuleInfoWrapperPass *MMI) {
  // we don't want to call addPassesToGenerateCode since we're not actually generating code,
  // so manually call the necessary passes that would otherwise prepare the IR.
  TargetPassConfig *PassConfig = createPassConfig(PM);
  // Set PassConfig options provided by TargetMachine.
  PassConfig->setDisableVerify(DisableVerify);
  PM.add(PassConfig);
  PassConfig->addIRPasses();

  if (FileType == CodeGenFileType::CGFT_AssemblyFile)
    PM.add(createPrintModulePass(Out, ""));
  else
    PM.add(createMetalLibWriterPass(Out));

  return false;
}

const TargetSubtargetInfo *
MetalTargetMachine::getSubtargetImpl(const Function &) const {
  return &SubtargetInfo;
}

bool MetalTargetSubtargetInfo::enableAtomicExpand() const { return true; }

const TargetLowering *MetalTargetSubtargetInfo::getTargetLowering() const {
  return &Lowering;
}
