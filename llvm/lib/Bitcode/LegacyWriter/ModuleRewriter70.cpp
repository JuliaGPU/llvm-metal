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
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
using namespace llvm;

void ModuleRewriter70::run() {
  removeFreeze();
}

bool ModuleRewriter70::removeFreeze() {
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
