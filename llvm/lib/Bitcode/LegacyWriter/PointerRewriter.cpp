//===- PointerRewriter.cpp - Rewrite opaque pointers for typed IR ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the PointerRewriter class.
//
//===----------------------------------------------------------------------===//

// Old LLVM versions do not support opaque pointers, so we need to emit typed
// instructions when writing the bitcode. This is hard, as the element type
// information is lost. We deal with this by surrounding all known typed pointer
// uses and definitions with bitcasts to a custom opaque pointer type. Since we
// cannot represent typed pointers in IR (it is illegal to cast to
// TypedPointerTypes), these casts are emitted by the bitcode writer. However,
// to make that easier, we already emit no-op bitcasts here so that the
// ValueEnumerator reserves instruction IDs correctly.
//
// To expose the element type information to the bitcode writer, we provide a
// pointer map that maps values to their typed pointer types.
//
// All this is similar to LLVM's PointerTypeAnalysis pass for DXIL. That pass
// tries to infer the element type of opaque pointers by looking at the uses of
// a pointer, and subsequently the DXIL module writer tries to keep values typed
// for much longer time. This turns out to be fragile, breaking / requiring
// special handling for many more instructions (like select or phi), while also
// not correctly handling multiple (but differently typed) uses of the same
// opaque pointer. To avoid that complexity, we simply emit a bitcast
// surrounding every use or definition of a typed value, keeping pointers opaque
// for the rest of the function.
//
// We also support front-ends customizing element type information, i.e., to
// indicate that operands to certain function calls need to be typed, the
// analysis supports !arg_eltypes metadata on function declarations, containing
// pairs of operand indices and null values representing the element type of the
// operand. This is very useful for custom intrinsics whose type information
// cannot be inferred from the IR.

#include "PointerRewriter.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/TypedPointerType.h"
using namespace llvm;

// Demote all constant expressions that produce pointers, to their
// corresponding instructions so that we can more easily rewrite them.
static bool demotePointerConstexprs(Module &M) {
  SmallVector<std::pair<Instruction *, int>, 8> Worklist;
  for (Function &F : M)
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        for (const Use &Op : I.operands())
          if (auto *CE = dyn_cast<ConstantExpr>(Op))
            Worklist.push_back({&I, Op.getOperandNo()});
  if (Worklist.empty())
    return false;

  for (auto Item : Worklist) {
    Instruction *I = Item.first;
    int OpIdx = Item.second;
    ConstantExpr *CE = cast<ConstantExpr>(I->getOperand(OpIdx));
    Instruction *NewI = CE->getAsInstruction();
    NewI->insertBefore(I);
    I->setOperand(OpIdx, NewI);
  }
  return true;
}

// determine the typed function type based on !arg_eltypes metadata
static FunctionType *getTypedFunctionType(const Function *F) {
  auto *FTy = F->getFunctionType();
  MDNode *MD = F->getMetadata("arg_eltypes");
  if (!MD)
    return FTy;

  auto Args = FTy->params().vec();
  for (unsigned i = 0; i < MD->getNumOperands(); i += 2) {
    auto IdxConstant = cast<ConstantAsMetadata>(MD->getOperand(i))->getValue();
    int Idx = cast<ConstantInt>(IdxConstant)->getZExtValue();
    Type *ElTy =
        cast<ValueAsMetadata>(MD->getOperand(i + 1))->getValue()->getType();

    auto OpaquePtrTy = cast<PointerType>(Args[Idx]);
    auto TypedPtrTy =
        TypedPointerType::get(ElTy, OpaquePtrTy->getAddressSpace());
    Args[Idx] = TypedPtrTy;
  }
  return FunctionType::get(FTy->getReturnType(), Args, FTy->isVarArg());
}

static bool isNoopCast(const Value *V) {
  auto I = cast<Instruction>(V);
  if (I->getOpcode() != Instruction::BitCast)
    return false;
  return I->getOperand(0)->getType() == I->getType();
}

// prepend an instruction's pointer operand with a no-op bitcast
static void prependBitcast(Module &M, Instruction *I, int Idx) {
  Value *V = I->getOperand(Idx);
  assert(V->getType()->isPointerTy() && "Expected a pointer operand");

  // Create no-op bitcast
  auto *Cast = CastInst::Create(Instruction::BitCast, V, V->getType());

  if (auto *PHI = dyn_cast<PHINode>(I)) {
    // we can't insert before phis, so rewrite in the incoming block instead
    auto *BB = PHI->getIncomingBlock(Idx);
    Cast->insertBefore(BB->getTerminator());
  } else {
    Cast->insertBefore(I);
  }

  I->setOperand(Idx, Cast);
}

// replace all uses of a value with no-op bitcasts
static void replaceWithBitcast(Module &M, Value *V) {
  assert(V->getType()->isPointerTy() && "Expected a pointer value");

  // Find all uses
  SmallVector<std::pair<Instruction *, unsigned>, 8> Worklist;
  for (Use &Use : V->uses()) {
    auto User = Use.getUser();
    if (auto *I = dyn_cast<Instruction>(User))
      Worklist.push_back({I, Use.getOperandNo()});
  }

  // Insert no-op bitcasts
  for (auto Item : Worklist) {
    Instruction *I = Item.first;
    int Idx = Item.second;
    prependBitcast(M, I, Idx);
  }
}

// append a single instruction's pointer return value with a no-op bitcast
static void appendBitcast(Module &M, Instruction *I) {
  assert(I->getType()->isPointerTy() &&
         "Expected a pointer-returning instruction");

  // Insert no-op bitcast
  Instruction *Cast = CastInst::Create(Instruction::BitCast, I, I->getType(),
                                       "", I->getNextNode());
  I->replaceAllUsesWith(Cast);
  // HACK: undo the RAUW which messed with our input argument too
  Cast->setOperand(0, I);
}

// bitcast uses of globals, for which we can infer the element type based on the
// global's type
bool bitcastGlobals(Module &M) {
  // Find all globals
  SmallVector<GlobalVariable *, 8> Worklist;
  for (GlobalVariable &GV : M.globals())
    Worklist.push_back(&GV);
  if (Worklist.empty())
    return false;

  // Insert bitcasts
  for (GlobalVariable *GV : Worklist) {
    replaceWithBitcast(M, GV);
  }

  return true;
}

// bitcast operands to instructions, by infering the element type by inspecting
// the instruction
bool bitcastInstructionOperands(Module &M) {
  // Find all instructions with pointer inputs or outputs
  SmallVector<Instruction *, 8> Worklist;
  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *LI = dyn_cast<LoadInst>(&I))
          Worklist.push_back(LI);
        else if (auto *SI = dyn_cast<StoreInst>(&I))
          Worklist.push_back(SI);
        else if (auto *AI = dyn_cast<AtomicRMWInst>(&I))
          Worklist.push_back(AI);
        else if (auto *AI = dyn_cast<AtomicCmpXchgInst>(&I))
          Worklist.push_back(AI);
        else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
          Worklist.push_back(GEP);
        else if (auto *AI = dyn_cast<AllocaInst>(&I))
          Worklist.push_back(AI);
      }
    }
  }
  if (Worklist.empty())
    return false;

  // Add no-op bitcasts
  for (Instruction *I : Worklist) {
    if (auto *LI = dyn_cast<LoadInst>(I)) {
      prependBitcast(M, LI, LI->getPointerOperandIndex());
    } else if (auto *SI = dyn_cast<StoreInst>(I)) {
      prependBitcast(M, SI, SI->getPointerOperandIndex());
    } else if (auto *AI = dyn_cast<AtomicRMWInst>(I)) {
      prependBitcast(M, AI, AI->getPointerOperandIndex());
    } else if (auto *AI = dyn_cast<AtomicCmpXchgInst>(I)) {
      prependBitcast(M, AI, AI->getPointerOperandIndex());
    } else if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
      prependBitcast(M, GEP, GEP->getPointerOperandIndex());
      appendBitcast(M, GEP);
    } else if (auto *AI = dyn_cast<AllocaInst>(I)) {
      appendBitcast(M, AI);
    } else
      llvm_unreachable("Unhandled instruction");
  }

  return true;
}

// bitcast operands to calls, whose type can be altered by metadata attached to
// the function
bool bitcastFunctionOperands(Module &M) {
  for (Function &F : M) {
    auto *FTy = F.getFunctionType();
    auto *NewFTy = getTypedFunctionType(&F);
    if (FTy == NewFTy)
      continue;

    // convert calls to this function
    for (User *U : F.users()) {
      if (auto *CI = dyn_cast<CallInst>(U)) {
        for (unsigned Idx = 0; Idx < CI->arg_size(); Idx++) {
          auto OldTy = FTy->getParamType(Idx);
          auto NewTy = NewFTy->getParamType(Idx);
          if (OldTy == NewTy)
            continue;

          prependBitcast(M, CI, Idx);
        }
      }
    }
  }

  return false;
}

// build a map of values to typed pointer types
PointerTypeMap PointerRewriter::buildPointerMap(const Module &M) {
  PointerTypeMap PointerMap;

  // globals
  for (const GlobalVariable &GV : M.globals()) {
    Type *ElTy = GV.getValueType();
    unsigned AS = GV.getAddressSpace();
    auto TypedPtrTy = TypedPointerType::get(ElTy, AS);
    PointerMap[&GV] = TypedPtrTy;
  }

  // instructions
  for (const Function &F : M) {
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        if (auto *LI = dyn_cast<LoadInst>(&I)) {
          assert(isNoopCast(LI->getPointerOperand()));
          PointerMap[LI->getPointerOperand()] = TypedPointerType::get(
              LI->getType(), LI->getPointerAddressSpace());
        } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
          assert(isNoopCast(SI->getPointerOperand()));
          PointerMap[SI->getPointerOperand()] = TypedPointerType::get(
              SI->getValueOperand()->getType(), SI->getPointerAddressSpace());
        } else if (auto *AI = dyn_cast<AtomicRMWInst>(&I)) {
          assert(isNoopCast(AI->getPointerOperand()));
          PointerMap[AI->getPointerOperand()] = TypedPointerType::get(
              AI->getValOperand()->getType(), AI->getPointerAddressSpace());
        } else if (auto *AI = dyn_cast<AtomicCmpXchgInst>(&I)) {
          assert(isNoopCast(AI->getPointerOperand()));
          PointerMap[AI->getPointerOperand()] = TypedPointerType::get(
              AI->getNewValOperand()->getType(), AI->getPointerAddressSpace());
        } else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
          assert(isNoopCast(GEP->getPointerOperand()));
          PointerMap[GEP->getPointerOperand()] = TypedPointerType::get(
              GEP->getSourceElementType(), GEP->getAddressSpace());
          assert(GEP->hasOneUse() && isNoopCast(GEP->user_back()));
          PointerMap[GEP] = TypedPointerType::get(GEP->getResultElementType(),
                                                  GEP->getAddressSpace());
        } else if (auto *AI = dyn_cast<AllocaInst>(&I)) {
          assert(AI->hasOneUse() && isNoopCast(AI->user_back()));
          PointerMap[AI] = TypedPointerType::get(AI->getAllocatedType(),
                                                 AI->getAddressSpace());
        }
      }
    }
  }

  // functions
  for (const Function &F : M) {
    auto *FTy = F.getFunctionType();
    auto *NewFTy = getTypedFunctionType(&F);
    PointerMap[&F] = TypedPointerType::get(NewFTy, F.getAddressSpace());
    if (FTy == NewFTy)
      continue;

    for (unsigned int i = 0; i < FTy->getNumParams(); i++) {
      auto OldTy = FTy->getParamType(i);
      auto NewTy = NewFTy->getParamType(i);
      if (OldTy == NewTy)
        continue;

      for (const User *U : F.users()) {
        if (auto *CI = dyn_cast<CallInst>(U)) {
          assert(isNoopCast(CI->getArgOperand(i)));
          PointerMap[CI->getArgOperand(i)] = cast<TypedPointerType>(NewTy);
        }
      }
    }
  }

  return PointerMap;
}

bool PointerRewriter::run() {
  // get rid of constant expressions so that we can more easily rewrite them
  bool Changed = demotePointerConstexprs(M);

  // insert no-op bitcasts surrounding pointer values
  Changed |= bitcastGlobals(M);
  Changed |= bitcastInstructionOperands(M);
  Changed |= bitcastFunctionOperands(M);

  // TODO: remove double bitcasts?

  return Changed;
}
