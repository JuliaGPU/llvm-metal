//===-- MetalTargetMachine.h - TargetMachine for the C backend ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef METALTARGETMACHINE_H
#define METALTARGETMACHINE_H

#include "MetalTargetObjectFile.h"

#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {

class MetalTargetLowering : public TargetLowering {
public:
  explicit MetalTargetLowering(const TargetMachine &TM) : TargetLowering(TM) {
    setMaxAtomicSizeInBitsSupported(0);
  }
};

class MetalTargetSubtargetInfo : public TargetSubtargetInfo {
public:
  MetalTargetSubtargetInfo(const TargetMachine &TM, const Triple &TT, StringRef CPU,
                       StringRef TuneCPU,StringRef FS)
      : TargetSubtargetInfo(TT, CPU,TuneCPU, FS, ArrayRef<SubtargetFeatureKV>(),
                            ArrayRef<SubtargetSubTypeKV>(), nullptr, nullptr,
                            nullptr, nullptr, nullptr, nullptr),
        Lowering(TM) {
  }
  bool enableAtomicExpand() const override;
  const TargetLowering *getTargetLowering() const override;
  const MetalTargetLowering Lowering;
};

class MetalTargetMachine : public LLVMTargetMachine {
  const MetalTargetSubtargetInfo SubtargetInfo;
  std::unique_ptr<TargetLoweringObjectFile> TLOF;

public:
  MetalTargetMachine(const Target &T, const Triple &TT, StringRef CPU, StringRef FS,
                 const TargetOptions &Options, Optional<Reloc::Model> RM,
                 Optional<CodeModel::Model> CM, CodeGenOpt::Level OL,
                 bool /*JIT*/)
      : LLVMTargetMachine(T, "", TT, CPU, FS, Options,
                          RM.hasValue() ? RM.getValue() : Reloc::Static,
                          CM.hasValue() ? CM.getValue() : CodeModel::Small, OL),
        SubtargetInfo(*this, TT, CPU,"", FS),
        TLOF(std::make_unique<MetalTargetObjectFile>()) {
      }

  /// Add passes to the specified pass manager to get the specified file
  /// emitted.  Typically this will involve several steps of code generation.
  bool addPassesToEmitFile(PassManagerBase &PM, raw_pwrite_stream &Out,
                           raw_pwrite_stream *DwoOut,
                           CodeGenFileType FileType, bool DisableVerify = true,
                           MachineModuleInfoWrapperPass *MMI = nullptr
                           ) override;

  // Pass Pipeline Configuration
  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;

  const TargetSubtargetInfo *getSubtargetImpl(const Function &) const override;
  TargetLoweringObjectFile *getObjFileLowering() const override {
    return TLOF.get();
  }
};

Target &getTheMetalTarget();

} // namespace llvm

#endif
