//===- ValueEnumerator70.cpp - Rewrite IR for LLVM 7.0 ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the ModuleRewriter70 class.
//
//===----------------------------------------------------------------------===//

#include "ModuleRewriter70.h"
#include "PointerRewriter.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
using namespace llvm;

static bool removeFreeze(Module &M) {
    // Find freeze instructions
    SmallVector<FreezeInst *, 8> Worklist;
    for (Function &F : M)
      for (BasicBlock &BB : F)
        for (Instruction &I : BB)
          if (auto *FI = dyn_cast<FreezeInst>(&I))
            Worklist.push_back(FI);
    if (Worklist.empty())
        return false;

    // Replace freeze instructions by their operand
    for (FreezeInst *FI : Worklist) {
        FI->replaceAllUsesWith(FI->getOperand(0));
        FI->eraseFromParent();
    }
    return true;
}

static bool replaceFNeg(Module &M) {
  // Find fneg instructions
  SmallVector<UnaryOperator *, 8> Worklist;
  for (Function &F : M)
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (auto *Op = dyn_cast<UnaryOperator>(&I))
          if (Op->getOpcode() == Instruction::FNeg)
            Worklist.push_back(Op);
  if (Worklist.empty())
    return false;

  // Replace fneg instructions by fsub instructions
  IRBuilder<> Builder(M.getContext());
  for (UnaryOperator *Op : Worklist) {
    Builder.SetInsertPoint(Op);
    Value *In = Op->getOperand(0);
    Value *Zero = ConstantFP::get(In->getType(), -0.0);
    Op->replaceAllUsesWith(Builder.CreateFSub(Zero, In));
    Op->eraseFromParent();
  }
  return true;
}

bool ModuleRewriter70::run() {
  bool Changed = removeFreeze(M);
  Changed |= replaceFNeg(M);

  PointerRewriter PR(M);
  Changed |= PR.run();

  return Changed;
}
