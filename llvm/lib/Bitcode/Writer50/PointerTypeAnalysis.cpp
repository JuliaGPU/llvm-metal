//===- Target/DirectX/PointerTypeAnalisis.cpp - PointerType analysis ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Analysis pass to assign types to opaque pointers.
//
//===----------------------------------------------------------------------===//

#include "PointerTypeAnalysis.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;

namespace {

static Type* classifyPointer(const Value *V, PointerTypeMap &Map) {
  assert(V->getType()->isOpaquePointerTy() && "Expected opaque pointer type");

  // If we don't know anything about this value, fall back to i8*
  auto It = Map.find(V);
  if (It == Map.end())
    return TypedPointerType::get(Type::getInt8Ty(V->getContext()),
                                 V->getType()->getPointerAddressSpace());

  return It->second;
}

// Propagate pointer type information from instructions like alloca's
// forwards to their users
static void classifyInstructionsForwards(const Value *V, PointerTypeMap &Map) {
  // Skip non-pointer instructions
  if (!V->getType()->isOpaquePointerTy())
    return;

  // Skip instructions we already processed
  auto It = Map.find(V);
  if (It != Map.end())
    return;

  // See if we can derive a pointer type from this instruction
  Type *PointeeTy = nullptr;
  if (auto *Inst = dyn_cast<GetElementPtrInst>(V)) {
    if (!Inst->getResultElementType()->isOpaquePointerTy())
      PointeeTy = Inst->getResultElementType();
  } else if (auto *Inst = dyn_cast<AllocaInst>(V))
    PointeeTy = Inst->getAllocatedType();
  else if (auto *GV = dyn_cast<GlobalVariable>(V))
    PointeeTy = GV->getValueType();
  if (!PointeeTy)
    return;
  auto TypedPtrTy =
      TypedPointerType::get(PointeeTy, V->getType()->getPointerAddressSpace());
  Map[V] = TypedPtrTy;

  // Propagate the pointer type forwards
  std::function<void(const Value*, TypedPointerType*)> PropagateType = [&](const Value *V, TypedPointerType *Ty) {
    for (const auto *User : V->users()) {
      Type *PointeeTy = nullptr;
      if (auto *Inst = dyn_cast<SelectInst>(User))
        PointeeTy = Ty->getElementType();
      else if (auto *Inst = dyn_cast<PHINode>(User))
        PointeeTy = Ty->getElementType();
      else if (auto *Inst = dyn_cast<AddrSpaceCastInst>(User))
        PointeeTy = Ty->getElementType();
      if (!PointeeTy)
        continue;
      auto TypedPtrTy =
          TypedPointerType::get(PointeeTy, V->getType()->getPointerAddressSpace());
      Map[V] = TypedPtrTy;

      PropagateType(User, TypedPtrTy);
    }
  };
  PropagateType(V, TypedPtrTy);
}

// Propagate element type information from instructions like loads
// backwards to their operands
void classifyInstructionsBackwards(const Value *V, PointerTypeMap &Map) {
  // See if we can derive an element type from this instruction
  Type *ElementType = nullptr;
  int PointerIndex;
  if (const auto *Inst = dyn_cast<LoadInst>(V)) {
    ElementType = Inst->getType();
    PointerIndex = Inst->getPointerOperandIndex();
  } else if (const auto *Inst = dyn_cast<StoreInst>(V)) {
    ElementType = Inst->getValueOperand()->getType();
    // When store value is ptr type, cannot get more type info.
    if (ElementType->isOpaquePointerTy())
      return;
    PointerIndex = Inst->getPointerOperandIndex();
  } else if (const auto *Inst = dyn_cast<GetElementPtrInst>(V)) {
    ElementType = Inst->getSourceElementType();
    PointerIndex = Inst->getPointerOperandIndex();
  }
  if (!ElementType)
    return;
  Value *PointerOperand = cast<Instruction>(V)->getOperand(PointerIndex);

  // Propagate the element type backwards
  std::function<void(const Value*, Type*)> PropagateType = [&](const Value *V, Type *Ty) {
    // Stop if we already processed this value
    // XXX: what with values that have multiple values flowing into it?
    auto It = Map.find(V);
    if (It != Map.end())
      return;
    auto TypedPtrTy = TypedPointerType::get(Ty, V->getType()->getPointerAddressSpace());
    Map[V] = TypedPtrTy;

    for (const auto *User : V->users()) {
      SmallVector<const Value *, 8> PointerOperands;
      if (const auto *Inst = dyn_cast<SelectInst>(User))
        for (const auto &Op : Inst->operands())
          PointerOperands.push_back(Op.getUser());
      else if (const auto *Inst = dyn_cast<PHINode>(User))
        for (const auto &Op : Inst->operands())
          PointerOperands.push_back(Op);
      else if (const auto *Inst = dyn_cast<AddrSpaceCastInst>(User))
        PointerOperands.push_back(Inst->getOperand(0));
      if (PointerOperands.empty())
        continue;
      for (const auto *Op : PointerOperands)
        PropagateType(Op, Ty);
    }
  };
  PropagateType(PointerOperand, ElementType);
}

// This function constructs a function type accepting typed pointers. It only
// handles function arguments and return types, and assigns the function type to
// the function's value in the type map.
Type *classifyFunctionType(const Function &F, PointerTypeMap &Map) {
  auto It = Map.find(&F);
  if (It != Map.end())
    return It->second;

  SmallVector<Type *, 8> NewArgs;
  Type *RetTy = F.getReturnType();
  LLVMContext &Ctx = F.getContext();
  if (RetTy->isOpaquePointerTy()) {
    RetTy = nullptr;
    for (const auto &B : F) {
      const auto *RetInst = dyn_cast_or_null<ReturnInst>(B.getTerminator());
      if (!RetInst)
        continue;

      Type *NewRetTy = classifyPointer(RetInst->getReturnValue(), Map);
      if (!RetTy)
        RetTy = NewRetTy;
      else if (RetTy != NewRetTy)
        RetTy = TypedPointerType::get(
            Type::getInt8Ty(Ctx), F.getReturnType()->getPointerAddressSpace());
    }
    // For function decl.
    if (!RetTy)
      RetTy = TypedPointerType::get(
          Type::getInt8Ty(Ctx), F.getReturnType()->getPointerAddressSpace());
  }
  for (auto &A : F.args()) {
    Type *ArgTy = A.getType();
    if (ArgTy->isOpaquePointerTy())
      ArgTy = classifyPointer(&A, Map);
    NewArgs.push_back(ArgTy);
  }
  auto *TypedPtrTy =
      TypedPointerType::get(FunctionType::get(RetTy, NewArgs, false), 0);
  Map[&F] = TypedPtrTy;
  return TypedPtrTy;
}
} // anonymous namespace

static Type *classifyConstantWithOpaquePtr(const Constant *C,
                                           PointerTypeMap &Map) {
  // FIXME: support ConstantPointerNull which could map to more than one
  // TypedPointerType.
  // See https://github.com/llvm/llvm-project/issues/57942.
  if (isa<ConstantPointerNull>(C))
    return TypedPointerType::get(Type::getInt8Ty(C->getContext()),
                                 C->getType()->getPointerAddressSpace());

  // Skip ConstantData which cannot have opaque ptr.
  if (isa<ConstantData>(C))
    return C->getType();

  auto It = Map.find(C);
  if (It != Map.end())
    return It->second;

  if (const auto *F = dyn_cast<Function>(C))
    return classifyFunctionType(*F, Map);

  Type *Ty = C->getType();
  Type *TargetTy = nullptr;
  if (auto *CS = dyn_cast<ConstantStruct>(C)) {
    SmallVector<Type *> EltTys;
    for (unsigned int I = 0; I < CS->getNumOperands(); ++I) {
      const Constant *Elt = C->getAggregateElement(I);
      Type *EltTy = classifyConstantWithOpaquePtr(Elt, Map);
      EltTys.emplace_back(EltTy);
    }
    TargetTy = StructType::get(C->getContext(), EltTys);
  } else if (auto *CA = dyn_cast<ConstantAggregate>(C)) {

    Type *TargetEltTy = nullptr;
    for (auto &Elt : CA->operands()) {
      Type *EltTy = classifyConstantWithOpaquePtr(cast<Constant>(&Elt), Map);
      assert(TargetEltTy == EltTy || TargetEltTy == nullptr);
      TargetEltTy = EltTy;
    }

    if (auto *AT = dyn_cast<ArrayType>(Ty)) {
      TargetTy = ArrayType::get(TargetEltTy, AT->getNumElements());
    } else {
      // Not struct, not array, must be vector here.
      auto *VT = cast<VectorType>(Ty);
      TargetTy = VectorType::get(TargetEltTy, VT);
    }
  }
  // Must have a target ty when map.
  assert(TargetTy && "PointerTypeAnalyisis failed to identify target type");

  // Same type, no need to map.
  if (TargetTy == Ty)
    return Ty;

  Map[C] = TargetTy;
  return TargetTy;
}

static void classifyGlobalCtorPointerType(const GlobalVariable &GV,
                                          PointerTypeMap &Map) {
  const auto *CA = cast<ConstantArray>(GV.getInitializer());
  // Type for global ctor should be array of { i32, void ()*, i8* }.
  Type *CtorArrayTy = classifyConstantWithOpaquePtr(CA, Map);

  // Map the global type.
  Map[&GV] = TypedPointerType::get(CtorArrayTy,
                                   GV.getType()->getPointerAddressSpace());
}

PointerTypeMap PointerTypeAnalysis::run(const Module &M) {
  PointerTypeMap Map;
  for (auto &G : M.globals()) {
    classifyInstructionsForwards(&G, Map);
    if (G.getName() == "llvm.global_ctors")
      classifyGlobalCtorPointerType(G, Map);
  }

  for (auto &F : M) {
    for (auto &A : F.args())
      classifyInstructionsForwards(&A, Map);
    for (const auto &B : F)
      for (const auto &I : B)
        classifyInstructionsForwards(&I, Map);
    for (const auto &B : F)
      for (const auto &I : B)
        classifyInstructionsBackwards(&I, Map);

    classifyFunctionType(F, Map);
  }

  return Map;
}
