//===- BitcodeWriterPass32.cpp - Bitcode 3.2 writing pass -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// BitcodeWriter32Pass implementation.
//
//===----------------------------------------------------------------------===//

#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
using namespace llvm;

PreservedAnalyses Bitcode32WriterPass::run(Module &M, ModuleAnalysisManager &) {
  WriteBitcode32ToFile(&M, OS);
  return PreservedAnalyses::all();
}

namespace {
  class WriteBitcode32Pass : public ModulePass {
    raw_ostream &OS; // raw_ostream to print on

  public:
    static char ID; // Pass identification, replacement for typeid
    explicit WriteBitcode32Pass(raw_ostream &o)
        : ModulePass(ID), OS(o) {}

    StringRef getPassName() const override { return "Bitcode 3.2 Writer"; }

    bool runOnModule(Module &M) override {
      WriteBitcode32ToFile(&M, OS);
      return false;
    }
  };
}

char WriteBitcode32Pass::ID = 0;

ModulePass *llvm::createBitcode32WriterPass(raw_ostream &Str) {
  return new WriteBitcode32Pass(Str);
}
