//===- ValueEnumerator50.cpp - Rewrite IR for LLVM 5.0 ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the ModuleRewriter50 class.
//
//===----------------------------------------------------------------------===//

#include "ModuleRewriter50.h"
#include "PointerRewriter.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
using namespace llvm;

static bool removeFreeze(Module &M) {
    // Find freeze instructions
    SmallVector<FreezeInst *, 8> Worklist;
    for (Function &F : M) {
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                if (auto *FI = dyn_cast<FreezeInst>(&I)) {
                    Worklist.push_back(FI);
                }
            }
        }
    }
    if (Worklist.empty())
        return false;

    // Replace freeze instructions by their operand
    for (FreezeInst *FI : Worklist) {
        FI->replaceAllUsesWith(FI->getOperand(0));
        FI->eraseFromParent();
    }
    return true;
}

bool ModuleRewriter50::run() {
  bool Changed = removeFreeze(M);

  PointerRewriter PR(M);
  Changed |= PR.run();

  return Changed;
}
