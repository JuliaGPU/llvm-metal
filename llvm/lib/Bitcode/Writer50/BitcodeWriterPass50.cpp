//===- BitcodeWriterPass50.cpp - Bitcode 5.0 writing pass -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// BitcodeWriterPass50 implementation.
//
//===----------------------------------------------------------------------===//

#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/Analysis/ModuleSummaryAnalysis.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
using namespace llvm;

PreservedAnalyses BitcodeWriterPass50::run(Module &M, ModuleAnalysisManager &AM) {
  const ModuleSummaryIndex *Index =
      EmitSummaryIndex ? &(AM.getResult<ModuleSummaryIndexAnalysis>(M))
                       : nullptr;
  WriteBitcode50ToFile(&M, OS, ShouldPreserveUseListOrder, Index, EmitModuleHash);
  return PreservedAnalyses::all();
}

namespace {
  class WriteBitcodePass50 : public ModulePass {
    raw_ostream &OS; // raw_ostream to print on
    bool ShouldPreserveUseListOrder;
    bool EmitSummaryIndex;
    bool EmitModuleHash;

  public:
    static char ID; // Pass identification, replacement for typeid
    WriteBitcodePass50() : ModulePass(ID), OS(dbgs()) {
      initializeWriteBitcodePass50Pass(*PassRegistry::getPassRegistry());
    }

    explicit WriteBitcodePass50(raw_ostream &o, bool ShouldPreserveUseListOrder,
                              bool EmitSummaryIndex, bool EmitModuleHash)
        : ModulePass(ID), OS(o),
          ShouldPreserveUseListOrder(ShouldPreserveUseListOrder),
          EmitSummaryIndex(EmitSummaryIndex), EmitModuleHash(EmitModuleHash) {
      initializeWriteBitcodePass50Pass(*PassRegistry::getPassRegistry());
    }

    StringRef getPassName() const override { return "Bitcode 5.0 Writer"; }

    bool runOnModule(Module &M) override {
      const ModuleSummaryIndex *Index =
          EmitSummaryIndex
              ? &(getAnalysis<ModuleSummaryIndexWrapperPass>().getIndex())
              : nullptr;
      WriteBitcode50ToFile(&M, OS, ShouldPreserveUseListOrder, Index,
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

char WriteBitcodePass50::ID = 0;
INITIALIZE_PASS_BEGIN(WriteBitcodePass50, "write-bitcode50", "Write Bitcode50", false,
                      true)
INITIALIZE_PASS_DEPENDENCY(ModuleSummaryIndexWrapperPass)
INITIALIZE_PASS_END(WriteBitcodePass50, "write-bitcode50", "Write Bitcode50", false,
                    true)

ModulePass *llvm::createBitcode50WriterPass(raw_ostream &Str,
                                            bool ShouldPreserveUseListOrder,
                                            bool EmitSummaryIndex, bool EmitModuleHash) {
  return new WriteBitcodePass50(Str, ShouldPreserveUseListOrder,
                              EmitSummaryIndex, EmitModuleHash);
}
