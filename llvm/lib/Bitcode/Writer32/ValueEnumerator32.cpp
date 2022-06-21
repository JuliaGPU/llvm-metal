//===- ValueEnumerator32.cpp - Number values and types for bitcode writer -===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the ValueEnumerator32 class.
//
//===----------------------------------------------------------------------===//

#include "ValueEnumerator32.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/UseListOrder.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
using namespace llvm;

static bool isIntOrIntVectorValue(const std::pair<const Value*, unsigned> &V) {
  return V.first->getType()->isIntOrIntVectorTy();
}

ValueEnumerator32::ValueEnumerator32(const Module &M)
    : HasMDString(false), HasDILocation(false), HasGenericDINode(false) {
  // Enumerate the global variables.
  for (const GlobalVariable &GV : M.globals())
    EnumerateValue(&GV);

  // Enumerate the functions.
  for (const Function & F : M) {
    EnumerateValue(&F);
    EnumerateAttributes(F.getAttributes(), F.getContext());
  }

  // Enumerate the aliases.
  for (const GlobalAlias &GA : M.aliases())
    EnumerateValue(&GA);

  // Remember what is the cutoff between globalvalue's and other constants.
  unsigned FirstConstant = Values.size();

  // Enumerate the global variable initializers.
  for (const GlobalVariable &GV : M.globals())
    if (GV.hasInitializer())
      EnumerateValue(GV.getInitializer());

  // Enumerate the aliasees.
  for (const GlobalAlias &GA : M.aliases())
    EnumerateValue(GA.getAliasee());

  // Enumerate any optional Function data.
  for (const Function &F : M)
    for (const Use &U : F.operands())
      EnumerateValue(U.get());

  // Enumerate the metadata type.
  //
  // TODO: Move this to ValueEnumerator32::EnumerateOperandType() once bitcode
  // only encodes the metadata type when it's used as a value.
  EnumerateType(Type::getMetadataTy(M.getContext()));

  // Insert constants and metadata that are named at module level into the slot
  // pool so that the module symbol table can refer to them...
  EnumerateValueSymbolTable(M.getValueSymbolTable());
  EnumerateNamedMetadata(M);

  SmallVector<std::pair<unsigned, MDNode *>, 8> MDs;

  // Enumerate types used by function bodies and argument lists.
  for (const Function &F : M) {
    for (const Argument &A : F.args())
      EnumerateType(A.getType());

    for (const BasicBlock &BB : F)
      for (const Instruction &I : BB) {
        for (const Use &Op : I.operands()) {
          auto *MD = dyn_cast<MetadataAsValue>(&Op);
          if (!MD) {
            EnumerateOperandType(Op);
            continue;
          }

          // Local metadata is enumerated during function-incorporation.
          if (isa<LocalAsMetadata>(MD->getMetadata()))
            continue;

          EnumerateMetadata(MD->getMetadata());
        }
        EnumerateType(I.getType());
        if (const CallInst *CI = dyn_cast<CallInst>(&I))
          EnumerateAttributes(CI->getAttributes(), M.getContext());
        else if (const InvokeInst *II = dyn_cast<InvokeInst>(&I))
          EnumerateAttributes(II->getAttributes(), M.getContext());
        else if (const UnaryOperator *UnOp = dyn_cast<UnaryOperator>(&I);
                 UnOp && UnOp->getOpcode() == Instruction::FNeg) {
          // add -0.0 value that we'll use later
          EnumerateValue(ConstantFP::get(UnOp->getOperand(0)->getType(), -0.0));
        } else if (auto *SVI = dyn_cast<ShuffleVectorInst>(&I))
          EnumerateType(SVI->getShuffleMaskForBitcode()->getType());

        // Enumerate metadata attached with this instruction.
        MDs.clear();
        I.getAllMetadataOtherThanDebugLoc(MDs);
        for (unsigned i = 0, e = MDs.size(); i != e; ++i)
          EnumerateMetadata(MDs[i].second);

        // Don't enumerate the location directly -- it has a special record
        // type -- but enumerate its operands.
        if (DILocation *L = I.getDebugLoc())
          EnumerateMDNodeOperands(L);
      }
  }

  // Optimize constant ordering.
  OptimizeConstants(FirstConstant, Values.size());
}

unsigned ValueEnumerator32::getInstructionID(const Instruction *Inst) const {
  InstructionMapType::const_iterator I = InstructionMap.find(Inst);
  assert(I != InstructionMap.end() && "Instruction is not mapped!");
  return I->second;
}

unsigned ValueEnumerator32::getComdatID(const Comdat *C) const {
  unsigned ComdatID = Comdats.idFor(C);
  assert(ComdatID && "Comdat not found!");
  return ComdatID;
}

void ValueEnumerator32::setInstructionID(const Instruction *I) {
  InstructionMap[I] = InstructionCount++;
}

unsigned ValueEnumerator32::getMetadataID(const Metadata *MD) const {
  auto ID = getMetadataOrNullID(MD);
#if 0
  if(ID == 0) {
    errs() << "invalid MD: ";
    if(MD) errs() << MD;
    else errs() << "nullptr";
    errs() << "\n";
  }
#endif
  assert(ID != 0 && "Metadata not in slotcalculator!");
  return ID - 1;
}

unsigned ValueEnumerator32::getValueID(const Value *V) const {
  if (auto *MD = dyn_cast<MetadataAsValue>(V))
    return getMetadataID(MD->getMetadata());

  ValueMapType::const_iterator I = ValueMap.find(V);
#if 0
  if(I == ValueMap.end()) {
    errs() << "invalid value: " << *V << "\n";
  }
#endif
  assert(I != ValueMap.end() && "Value not in slotcalculator!");
  return I->second-1;
}

void ValueEnumerator32::dump() const {
  print(dbgs(), ValueMap, "Default");
  dbgs() << '\n';
  print(dbgs(), MetadataMap, "MetaData");
  dbgs() << '\n';
}

void ValueEnumerator32::print(raw_ostream &OS, const ValueMapType &Map,
                              const char *Name) const {

  OS << "Map Name: " << Name << "\n";
  OS << "Size: " << Map.size() << "\n";
  for (ValueMapType::const_iterator I = Map.begin(),
         E = Map.end(); I != E; ++I) {

    const Value *V = I->first;
    if (V->hasName())
      OS << "Value: " << V->getName();
    else
      OS << "Value: [null]\n";
    V->print(errs());
    errs() << '\n';

    OS << " Uses(" << std::distance(V->use_begin(),V->use_end()) << "):";
    for (const Use &U : V->uses()) {
      if (&U != &*V->use_begin())
        OS << ",";
      if(U->hasName())
        OS << " " << U->getName();
      else
        OS << " [null]";

    }
    OS <<  "\n\n";
  }
}

void ValueEnumerator32::print(raw_ostream &OS, const MetadataMapType &Map,
                              const char *Name) const {

  OS << "Map Name: " << Name << "\n";
  OS << "Size: " << Map.size() << "\n";
  for (auto I = Map.begin(), E = Map.end(); I != E; ++I) {
    const Metadata *MD = I->first;
    OS << "Metadata: slot = " << I->second << "\n";
    MD->print(OS);
  }
}

/// OptimizeConstants - Reorder constant pool for denser encoding.
void ValueEnumerator32::OptimizeConstants(unsigned CstStart, unsigned CstEnd) {
  if (CstStart == CstEnd || CstStart+1 == CstEnd) return;

  std::stable_sort(Values.begin() + CstStart, Values.begin() + CstEnd,
                   [this](const std::pair<const Value *, unsigned> &LHS,
                          const std::pair<const Value *, unsigned> &RHS) {
    // Sort by plane.
    if (LHS.first->getType() != RHS.first->getType())
      return getTypeID(LHS.first->getType()) < getTypeID(RHS.first->getType());
    // Then by frequency.
    return LHS.second > RHS.second;
  });

  // Ensure that integer and vector of integer constants are at the start of the
  // constant pool.  This is important so that GEP structure indices come before
  // gep constant exprs.
  std::partition(Values.begin()+CstStart, Values.begin()+CstEnd,
                 isIntOrIntVectorValue);

  // Rebuild the modified portion of ValueMap.
  for (; CstStart != CstEnd; ++CstStart)
    ValueMap[Values[CstStart].first] = CstStart+1;
}


/// EnumerateValueSymbolTable - Insert all of the values in the specified symbol
/// table into the values table.
void ValueEnumerator32::EnumerateValueSymbolTable(const ValueSymbolTable &VST) {
  for (ValueSymbolTable::const_iterator VI = VST.begin(), VE = VST.end();
       VI != VE; ++VI)
    EnumerateValue(VI->getValue());
}

/// Insert all of the values referenced by named metadata in the specified
/// module.
void ValueEnumerator32::EnumerateNamedMetadata(const Module &M) {
  for (const auto &I : M.named_metadata())
    EnumerateNamedMDNode(&I);
}

void ValueEnumerator32::EnumerateNamedMDNode(const NamedMDNode *MD) {
  for (unsigned i = 0, e = MD->getNumOperands(); i != e; ++i)
    EnumerateMetadata(MD->getOperand(i));
}

/// EnumerateMDNodeOperands - Enumerate all non-function-local values
/// and types referenced by the given MDNode.
void ValueEnumerator32::EnumerateMDNodeOperands(const MDNode *N) {
  for (unsigned i = 0, e = N->getNumOperands(); i != e; ++i) {
    const Metadata* MD = N->getOperand(i);
    if (!MD) {
      EnumerateType(Type::getVoidTy(N->getContext()));
      continue;
    }
    assert(!isa<LocalAsMetadata>(MD) && "MDNodes cannot be function-local");
    if(isa<MDNode>(MD) || isa<MDString>(MD)) {
      EnumerateMetadata(MD);
    } else if(auto* V = dyn_cast<ValueAsMetadata>(MD)) {
      EnumerateValue(V->getValue());
    }
  }
}

#define EnumerateI1(DI_obj, val) EnumerateValue(ConstantInt::get(Type::getInt1Ty(DI_obj->getContext()), val))
#define EnumerateI32(DI_obj, val) EnumerateValue(ConstantInt::get(Type::getInt32Ty(DI_obj->getContext()), val))
#define EnumerateI64(DI_obj, val) EnumerateValue(ConstantInt::get(Type::getInt64Ty(DI_obj->getContext()), val))
#define DW_TAG(tag) (tag | (12 << 16))

void ValueEnumerator32::EnumerateMetadata(const Metadata *MD) {
  assert(
      (isa<MDNode>(MD) || isa<MDString>(MD) || isa<ConstantAsMetadata>(MD)) &&
      "Invalid metadata kind");

  // Insert a dummy ID to block the co-recursive call to
  // EnumerateMDNodeOperands() from re-visiting MD in a cyclic graph.
  //
  // Return early if there's already an ID.
  if (!MetadataMap.insert(std::make_pair(MD, 0)).second)
    return;

  // Visit operands first to minimize RAUW.
  // NOTE: debug info must be handled manually (this is different to 3.8 handling)
  if (auto *DILoc = dyn_cast<DILocation>(MD)) {
    EnumerateI32(DILoc, DILoc->getLine());
    EnumerateI32(DILoc, DILoc->getColumn());
    EnumerateMDNodeOperands(DILoc);
  }
  else if (auto *DIF = dyn_cast<DIFile>(MD)) {
    EnumerateI32(DIF, DW_TAG(dwarf::DW_TAG_file_type));
    EnumerateMDNodeOperands(DIF);
    SmallVector<Metadata*, 2> file_node {{ DIF->getRawFilename(), DIF->getRawDirectory() }};
    const_cast<DIFile*>(DIF)->contained_node = MDTuple::get(DIF->getContext(), file_node);
    EnumerateMetadata(DIF->contained_node);
  }
  else if (auto *DICU = dyn_cast<DICompileUnit>(MD)) {
    EnumerateI32(DICU, DW_TAG(dwarf::DW_TAG_compile_unit));
    if(DICU->getFile()) {
      // doesn't point to an actual DIFile node, but directly to { file, dir }
      auto DIF = DICU->getFile();
      EnumerateMDNodeOperands(DIF);
      SmallVector<Metadata*, 2> file_node {{ DIF->getRawFilename(), DIF->getRawDirectory() }};
      DIF->contained_node = MDTuple::get(DICU->getContext(), file_node);
      EnumerateMetadata(DIF->contained_node);
    }
    EnumerateI32(DICU, DICU->getSourceLanguage());
    if(DICU->getRawProducer()) EnumerateMetadata(DICU->getRawProducer());
    EnumerateI1(DICU, DICU->isOptimized());
    if(DICU->getRawFlags()) EnumerateMetadata(DICU->getRawFlags());
    EnumerateI32(DICU, DICU->getRuntimeVersion());
    if(DICU->getRawEnumTypes()) EnumerateMetadata(DICU->getRawEnumTypes());
    if(DICU->getRawRetainedTypes()) EnumerateMetadata(DICU->getRawRetainedTypes());
    //if(DICU->getRawSubprograms()) EnumerateMetadata(DICU->getRawSubprograms()); // TODO: fix subprograms
    if(DICU->getRawGlobalVariables()) EnumerateMetadata(DICU->getRawGlobalVariables());
    if(DICU->getRawImportedEntities()) EnumerateMetadata(DICU->getRawImportedEntities());
    if(DICU->getRawSplitDebugFilename()) EnumerateMetadata(DICU->getRawSplitDebugFilename());
    EnumerateI32(DICU, DICU->getEmissionKind());
  }
  else if (auto *DISP = dyn_cast<DISubprogram>(MD)) {
    EnumerateI32(DISP, DW_TAG(dwarf::DW_TAG_subprogram));
    if(DISP->getFile()) EnumerateMetadata(DISP->getFile());
    if(DISP->getScope()) EnumerateMetadata(DISP->getScope());
    if(DISP->getRawName()) EnumerateMetadata(DISP->getRawName());
    if(DISP->getRawLinkageName()) EnumerateMetadata(DISP->getRawLinkageName());
    EnumerateI32(DISP, DISP->getLine());
    if(DISP->getType()) EnumerateMetadata(DISP->getType());
    EnumerateI1(DISP, DISP->isLocalToUnit());
    EnumerateI1(DISP, DISP->isDefinition());
    EnumerateI32(DISP, DISP->getVirtuality());
    EnumerateI32(DISP, DISP->getVirtualIndex());
    if(DISP->getContainingType()) EnumerateMetadata(DISP->getContainingType());
    EnumerateI32(DISP, DISP->getFlags());
    EnumerateI1(DISP, DISP->isOptimized());
    if(DISP->associated_function) {
      EnumerateValue(DISP->associated_function);
    }
    if(DISP->getTemplateParams()) EnumerateMetadata(DISP->getTemplateParams().get());
    if(DISP->getDeclaration()) EnumerateMetadata(DISP->getDeclaration());
    
    if(DISP->getRetainedNodes()) EnumerateMetadata(DISP->getRetainedNodes().get());
    else {
      auto empty_node = MDTuple::getTemporary(DISP->getContext(), {});
      EnumerateMetadata(empty_node.get());
    }
    
    EnumerateI32(DISP, DISP->getScopeLine());
  }
  else if(auto *DILB = dyn_cast<DILexicalBlock>(MD)) {
    EnumerateI32(DILB, DW_TAG(dwarf::DW_TAG_lexical_block));
    
    static unsigned int unique_id = 0;
    if(DILB->getFile()) EnumerateMetadata(DILB->getFile());
    if(DILB->getScope()) EnumerateMetadata(DILB->getScope());
    EnumerateI32(DILB, DILB->getLine());
    EnumerateI32(DILB, DILB->getColumn());
    EnumerateI32(DILB, 0);
    EnumerateI32(DILB, unique_id++);
  }
  else if(auto *DIST = dyn_cast<DISubroutineType>(MD)) {
    EnumerateI32(DIST, DW_TAG(dwarf::DW_TAG_subroutine_type));
    
    EnumerateI32(DIST, 0);
    auto empty_str_node = MDString::get(DIST->getContext(), "");
    EnumerateMetadata(empty_str_node);
    EnumerateI64(DIST, 0);
    EnumerateI32(DIST, DIST->getFlags());
    if(DIST->getTypeArray()) EnumerateMetadata(DIST->getTypeArray().get());
  }
  else if (auto *N = dyn_cast<MDNode>(MD))
    EnumerateMDNodeOperands(N);
  else if (auto *C = dyn_cast<ConstantAsMetadata>(MD))
    EnumerateValue(C->getValue());

  HasMDString |= isa<MDString>(MD);
  HasDILocation |= isa<DILocation>(MD);
  HasGenericDINode |= isa<GenericDINode>(MD);

  // Replace the dummy ID inserted above with the correct one.  MetadataMap may
  // have changed by inserting operands, so we need a fresh lookup here.
  MDs.push_back(MD);
  MetadataMap[MD] = MDs.size();
}

/// EnumerateFunctionLocalMetadataa - Incorporate function-local metadata
/// information reachable from the metadata.
void ValueEnumerator32::EnumerateFunctionLocalMetadata(
    const LocalAsMetadata *Local) {
  // Check to see if it's already in!
  unsigned &MetadataID = MetadataMap[Local];
  if (MetadataID)
    return;

  MDs.push_back(Local);
  MetadataID = MDs.size();

  EnumerateValue(Local->getValue());

  // Also, collect all function-local metadata for easy access.
  FunctionLocalMDs.push_back(Local);
}

void ValueEnumerator32::EnumerateValue(const Value *V) {
  assert(!V->getType()->isVoidTy() && "Can't insert void values!");
  assert(!isa<MetadataAsValue>(V) && "EnumerateValue doesn't handle Metadata!");

  // Check to see if it's already in!
  unsigned &ValueID = ValueMap[V];
  if (ValueID) {
    // Increment use count.
    Values[ValueID-1].second++;
    return;
  }

  if (auto *GO = dyn_cast<GlobalObject>(V))
    if (const Comdat *C = GO->getComdat())
      Comdats.insert(C);

  // Enumerate the type of this value.
  EnumerateType(V->getType());

  if (const Constant *C = dyn_cast<Constant>(V)) {
    if (isa<GlobalValue>(C)) {
      // Initializers for globals are handled explicitly elsewhere.
    } else if (C->getNumOperands()) {
      // If a constant has operands, enumerate them.  This makes sure that if a
      // constant has uses (for example an array of const ints), that they are
      // inserted also.

      // We prefer to enumerate them with values before we enumerate the user
      // itself.  This makes it more likely that we can avoid forward references
      // in the reader.  We know that there can be no cycles in the constants
      // graph that don't go through a global variable.
      for (User::const_op_iterator I = C->op_begin(), E = C->op_end();
           I != E; ++I)
        if (!isa<BasicBlock>(*I)) // Don't enumerate BB operand to BlockAddress.
          EnumerateValue(*I);
      if (auto *CE = dyn_cast<ConstantExpr>(C))
        if (CE->getOpcode() == Instruction::ShuffleVector)
          EnumerateValue(CE->getShuffleMaskForBitcode());

      // Finally, add the value.  Doing this could make the ValueID reference be
      // dangling, don't reuse it.
      Values.push_back(std::make_pair(V, 1U));
      ValueMap[V] = Values.size();
      return;
    }
  }

  // Add the value.
  Values.push_back(std::make_pair(V, 1U));
  ValueID = Values.size();
}


void ValueEnumerator32::EnumerateType(Type *Ty) {
  unsigned *TypeID = &TypeMap[Ty];

  // We've already seen this type.
  if (*TypeID)
    return;

  // If it is a non-anonymous struct, mark the type as being visited so that we
  // don't recursively visit it.  This is safe because we allow forward
  // references of these in the bitcode reader.
  if (StructType *STy = dyn_cast<StructType>(Ty))
    if (!STy->isLiteral())
      *TypeID = ~0U;

  // Enumerate all of the subtypes before we enumerate this type.  This ensures
  // that the type will be enumerated in an order that can be directly built.
  for (Type *SubTy : Ty->subtypes())
    EnumerateType(SubTy);

  // Refresh the TypeID pointer in case the table rehashed.
  TypeID = &TypeMap[Ty];

  // Check to see if we got the pointer another way.  This can happen when
  // enumerating recursive types that hit the base case deeper than they start.
  //
  // If this is actually a struct that we are treating as forward ref'able,
  // then emit the definition now that all of its contents are available.
  if (*TypeID && *TypeID != ~0U)
    return;

  // Add this type now that its contents are all happily enumerated.
  Types.push_back(Ty);

  *TypeID = Types.size();
}

// Enumerate the types for the specified value.  If the value is a constant,
// walk through it, enumerating the types of the constant.
void ValueEnumerator32::EnumerateOperandType(const Value *V) {
  EnumerateType(V->getType());

  if (auto *MD = dyn_cast<MetadataAsValue>(V)) {
    assert(!isa<LocalAsMetadata>(MD->getMetadata()) &&
           "Function-local metadata should be left for later");

    EnumerateMetadata(MD->getMetadata());
    return;
  }

  const Constant *C = dyn_cast<Constant>(V);
  if (!C)
    return;

  // If this constant is already enumerated, ignore it, we know its type must
  // be enumerated.
  if (ValueMap.count(C))
    return;

  // This constant may have operands, make sure to enumerate the types in
  // them.
  for (const Value *Op : C->operands()) {
    // Don't enumerate basic blocks here, this happens as operands to
    // blockaddress.
    if (isa<BasicBlock>(Op))
      continue;

    EnumerateOperandType(Op);
  }
  if (auto *CE = dyn_cast<ConstantExpr>(C))
    if (CE->getOpcode() == Instruction::ShuffleVector)
      EnumerateOperandType(CE->getShuffleMaskForBitcode());
}

extern uint64_t getAttrKindEncodingBC32(Attribute::AttrKind Kind);
void ValueEnumerator32::EnumerateAttributes(AttributeList PAL, LLVMContext& Context) {
  if (PAL.isEmpty()) return;  // null is always 0.

  // Do a lookup.
  unsigned &Entry = AttributeListMap[PAL];
  if (Entry == 0) {
    // Never saw this before, add it.
    AttributeLists.push_back(PAL);
    Entry = AttributeLists.size();
  }

  // Do lookups for all attribute groups.
  for (unsigned i : PAL.indexes()) {
    AttributeSet AS = PAL.getAttributes(i);
    if (!AS.hasAttributes())
      continue;
    // we need to skip attribute sets that don't have any valid LLVM BC 3.2 attribute
    bool has_any_valid_attr = false;
    for (Attribute Attr : AS) {
      if (Attr.isEnumAttribute() || Attr.isIntAttribute()) {
        if (getAttrKindEncodingBC32(Attr.getKindAsEnum()) > 0) {
          has_any_valid_attr = true;
          break;
        }
      } else if (Attr.isStringAttribute()) {
        has_any_valid_attr = true;
        break;
      }
      // else: ignore type attributes
    }
    auto AS_index = i;
    if (!has_any_valid_attr) {
      AS_index = ~0u;
    }

    IndexAndAttrSet Pair = {AS_index, AS};
    unsigned &Entry = AttributeGroupMap[Pair];
    if (Entry == 0) {
      AttributeGroups.push_back(Pair);
      Entry = AttributeGroups.size();
    }
  }
}

void ValueEnumerator32::incorporateFunction(const Function &F) {
  InstructionCount = 0;
  NumModuleValues = Values.size();
  NumModuleMDs = MDs.size();

  // Adding function arguments to the value table.
  for (const auto &I : F.args())
    EnumerateValue(&I);

  FirstFuncConstantID = Values.size();

  // Add all function-level constants to the value table.
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      for (const Use &OI : I.operands()) {
        if ((isa<Constant>(OI) && !isa<GlobalValue>(OI)) || isa<InlineAsm>(OI))
          EnumerateValue(OI);
      }
      if (auto *SVI = dyn_cast<ShuffleVectorInst>(&I))
        EnumerateValue(SVI->getShuffleMaskForBitcode());
    }
    BasicBlocks.push_back(&BB);
    ValueMap[&BB] = BasicBlocks.size();
  }

  // Optimize the constant layout.
  OptimizeConstants(FirstFuncConstantID, Values.size());

  // Add the function's parameter attributes so they are available for use in
  // the function's instruction.
  EnumerateAttributes(F.getAttributes(), F.getContext());

  FirstInstID = Values.size();

  SmallVector<LocalAsMetadata *, 8> FnLocalMDVector;
  // Add all of the instructions.
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      for (const Use &OI : I.operands()) {
        if (auto *MD = dyn_cast<MetadataAsValue>(&OI))
          if (auto *Local = dyn_cast<LocalAsMetadata>(MD->getMetadata()))
            // Enumerate metadata after the instructions they might refer to.
            FnLocalMDVector.push_back(Local);
      }

      if (!I.getType()->isVoidTy())
        EnumerateValue(&I);
    }
  }

  // Add all of the function-local metadata.
  for (unsigned i = 0, e = FnLocalMDVector.size(); i != e; ++i)
    EnumerateFunctionLocalMetadata(FnLocalMDVector[i]);
}

void ValueEnumerator32::purgeFunction() {
  /// Remove purged values from the ValueMap.
  for (unsigned i = NumModuleValues, e = Values.size(); i != e; ++i)
    ValueMap.erase(Values[i].first);
  for (unsigned i = NumModuleMDs, e = MDs.size(); i != e; ++i)
    MetadataMap.erase(MDs[i]);
  for (unsigned i = 0, e = BasicBlocks.size(); i != e; ++i)
    ValueMap.erase(BasicBlocks[i]);

  Values.resize(NumModuleValues);
  MDs.resize(NumModuleMDs);
  BasicBlocks.clear();
  FunctionLocalMDs.clear();
}

static void IncorporateFunctionInfoGlobalBBIDs(const Function *F,
                                 DenseMap<const BasicBlock*, unsigned> &IDMap) {
  unsigned Counter = 0;
  for (const BasicBlock &BB : *F)
    IDMap[&BB] = ++Counter;
}

/// getGlobalBasicBlockID - This returns the function-specific ID for the
/// specified basic block.  This is relatively expensive information, so it
/// should only be used by rare constructs such as address-of-label.
unsigned ValueEnumerator32::getGlobalBasicBlockID(const BasicBlock *BB) const {
  unsigned &Idx = GlobalBasicBlockIDs[BB];
  if (Idx != 0)
    return Idx-1;

  IncorporateFunctionInfoGlobalBBIDs(BB->getParent(), GlobalBasicBlockIDs);
  return getGlobalBasicBlockID(BB);
}

uint64_t ValueEnumerator32::computeBitsRequiredForTypeIndicies() const {
  return Log2_32_Ceil(getTypes().size() + 1);
}
