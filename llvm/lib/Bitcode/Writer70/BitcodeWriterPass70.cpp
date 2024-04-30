//===- BitcodeWriterPass70.cpp - Bitcode 7.0 writing pass -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// BitcodeWriterPass70 implementation.
//
//===----------------------------------------------------------------------===//

#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/Analysis/ModuleSummaryAnalysis.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
using namespace llvm;

PreservedAnalyses BitcodeWriterPass70::run(Module &M, ModuleAnalysisManager &AM) {
  const ModuleSummaryIndex *Index =
      EmitSummaryIndex ? &(AM.getResult<ModuleSummaryIndexAnalysis>(M))
                       : nullptr;
  WriteBitcodeToFile(M, OS, ShouldPreserveUseListOrder, Index, EmitModuleHash);
  return PreservedAnalyses::all();
}

namespace {
  class WriteBitcodePass70 : public ModulePass {
    raw_ostream &OS; // raw_ostream to print on
    bool ShouldPreserveUseListOrder;
    bool EmitSummaryIndex;
    bool EmitModuleHash;

  public:
    static char ID; // Pass identification, replacement for typeid
    WriteBitcodePass70() : ModulePass(ID), OS(dbgs()) {
      initializeWriteBitcodePass70Pass(*PassRegistry::getPassRegistry());
    }

    explicit WriteBitcodePass70(raw_ostream &o, bool ShouldPreserveUseListOrder,
                              bool EmitSummaryIndex, bool EmitModuleHash)
        : ModulePass(ID), OS(o),
          ShouldPreserveUseListOrder(ShouldPreserveUseListOrder),
          EmitSummaryIndex(EmitSummaryIndex), EmitModuleHash(EmitModuleHash) {
      initializeWriteBitcodePass70Pass(*PassRegistry::getPassRegistry());
    }

    StringRef getPassName() const override { return "Bitcode 7.0 Writer"; }

    bool runOnModule(Module &M) override {
      const ModuleSummaryIndex *Index =
          EmitSummaryIndex
              ? &(getAnalysis<ModuleSummaryIndexWrapperPass>().getIndex())
              : nullptr;
      WriteBitcode70ToFile(M, OS, ShouldPreserveUseListOrder, Index,
                           EmitModuleHash);
      return false;
    }
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
      if (EmitSummaryIndex)
        AU.addRequired<ModuleSummaryIndexWrapperPass>();
    }
  };
}

char WriteBitcodePass70::ID = 0;
INITIALIZE_PASS_BEGIN(WriteBitcodePass70, "write-bitcode70", "Write 7.0 Bitcode", false,
                      true)
INITIALIZE_PASS_DEPENDENCY(ModuleSummaryIndexWrapperPass)
INITIALIZE_PASS_END(WriteBitcodePass70, "write-bitcode70", "Write 7.0 Bitcode", false,
                    true)

ModulePass *llvm::createBitcode70WriterPass(raw_ostream &Str,
                                          bool ShouldPreserveUseListOrder,
                                          bool EmitSummaryIndex, bool EmitModuleHash) {
  return new WriteBitcodePass70(Str, ShouldPreserveUseListOrder,
                              EmitSummaryIndex, EmitModuleHash);
}
