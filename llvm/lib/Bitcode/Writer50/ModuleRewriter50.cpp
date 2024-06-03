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
#include "PointerTypeAnalysis.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
using namespace llvm;

void ModuleRewriter50::run() {
  removeFreeze();
  convertPointers();
}

bool ModuleRewriter50::removeFreeze() {
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

static Value *maybeGenerateBitcast(IRBuilder<> &Builder,
                                    PointerTypeMap &PointerTypes,
                                    Instruction &Inst, Value *Operand,
                                    Type *ElTy) {
  // Omit bitcasts if the incoming value matches the expected operand type.
  auto It = PointerTypes.find(Operand);
  if (It != PointerTypes.end())
    if (cast<TypedPointerType>(It->second)->getElementType() == ElTy)
      return nullptr;
  // Insert bitcasts where we are removing the instruction.
  Builder.SetInsertPoint(&Inst);
  // This code only gets hit in opaque-pointer mode, so the type of the
  // pointer doesn't matter.
  PointerType *PtrTy = cast<PointerType>(Operand->getType());
  return Builder.Insert(
      CastInst::Create(Instruction::BitCast, Operand,
                        Builder.getInt8PtrTy(PtrTy->getAddressSpace())));
}

bool ModuleRewriter50::convertPointers() {
  // Only insert bitcasts if the IR is using opaque pointers.
  if (M.getContext().supportsTypedPointers())
    return false;

  PointerTypeMap PointerTypes = PointerTypeAnalysis::run(M);
  for (auto &F : M.functions()) {
    for (auto &BB : F) {
      IRBuilder<> Builder(&BB);
      for (auto &I : make_early_inc_range(BB)) {
        // Emtting NoOp bitcast instructions allows the ValueEnumerator to be
        // unmodified as it reserves instruction IDs during contruction.
        if (auto LI = dyn_cast<LoadInst>(&I)) {
          if (Value *NoOpBitcast = maybeGenerateBitcast(
                  Builder, PointerTypes, I, LI->getPointerOperand(),
                  LI->getType())) {
            LI->replaceAllUsesWith(
                Builder.CreateLoad(LI->getType(), NoOpBitcast));
            LI->eraseFromParent();
            PointerTypes.erase(LI);
          }
          continue;
        }
        if (auto SI = dyn_cast<StoreInst>(&I)) {
          if (Value *NoOpBitcast = maybeGenerateBitcast(
                  Builder, PointerTypes, I, SI->getPointerOperand(),
                  SI->getValueOperand()->getType())) {

            SI->replaceAllUsesWith(
                Builder.CreateStore(SI->getValueOperand(), NoOpBitcast));
            SI->eraseFromParent();
            PointerTypes.erase(SI);
          }
          continue;
        }
        if (auto GEP = dyn_cast<GetElementPtrInst>(&I)) {
          if (Value *NoOpBitcast = maybeGenerateBitcast(
                  Builder, PointerTypes, I, GEP->getPointerOperand(),
                  GEP->getResultElementType()))
            GEP->setOperand(0, NoOpBitcast);
          continue;
        }
      }
    }
  }

  return true;
}
