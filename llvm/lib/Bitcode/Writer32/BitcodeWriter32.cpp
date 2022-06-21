//===--- Bitcode/Writer32/BitcodeWriter32.cpp - Bitcode 3.2 Writer --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Bitcode 3.2 writer implementation.
//
//===----------------------------------------------------------------------===//

// enable errors when using > 3.2 bitcode enums from LLVMBitCodes.h
#define LLVM_BITCODE_32 1

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "ValueEnumerator32.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Bitstream/BitstreamWriter.h"
#include "llvm/Bitcode/LLVMBitCodes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/UseListOrder.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include <cctype>
#include <map>
using namespace llvm;

#define SPIR32_TRIPLE "spir-unknown-unknown"
#define SPIR64_TRIPLE "spir64-unknown-unknown"
#define SPIR32_DATALAYOUT                                         \
  "e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-"     \
  "f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-"  \
  "v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-" \
  "v512:512:512-v1024:1024:1024"
#define SPIR64_DATALAYOUT                                         \
  "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-"     \
  "f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-"  \
  "v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-" \
  "v512:512:512-v1024:1024:1024"

/// These are manifest constants used by the bitcode writer. They do not need to
/// be kept in sync with the reader, but need to be consistent within this file.
enum {
  // VALUE_SYMTAB_BLOCK abbrev id's.
  VST_ENTRY_8_ABBREV = bitc::FIRST_APPLICATION_ABBREV,
  VST_ENTRY_7_ABBREV,
  VST_ENTRY_6_ABBREV,
  VST_BBENTRY_6_ABBREV,

  // CONSTANTS_BLOCK abbrev id's.
  CONSTANTS_SETTYPE_ABBREV = bitc::FIRST_APPLICATION_ABBREV,
  CONSTANTS_INTEGER_ABBREV,
  CONSTANTS_CE_CAST_Abbrev,
  CONSTANTS_NULL_Abbrev,

  // FUNCTION_BLOCK abbrev id's.
  FUNCTION_INST_LOAD_ABBREV = bitc::FIRST_APPLICATION_ABBREV,
  FUNCTION_INST_BINOP_ABBREV,
  FUNCTION_INST_BINOP_FLAGS_ABBREV,
  FUNCTION_INST_CAST_ABBREV,
  FUNCTION_INST_RET_VOID_ABBREV,
  FUNCTION_INST_RET_VAL_ABBREV,
  FUNCTION_INST_UNREACHABLE_ABBREV,
  FUNCTION_INST_GEP_ABBREV BC38,

  // SwitchInst Magic
  SWITCH_INST_MAGIC = 0x4B5 // May 2012 => 1205 => Hex
};

static unsigned GetEncodedCastOpcode(unsigned Opcode) {
  switch (Opcode) {
  default: llvm_unreachable("Unknown cast instruction!");
  case Instruction::Trunc   : return bitc::CAST_TRUNC;
  case Instruction::ZExt    : return bitc::CAST_ZEXT;
  case Instruction::SExt    : return bitc::CAST_SEXT;
  case Instruction::FPToUI  : return bitc::CAST_FPTOUI;
  case Instruction::FPToSI  : return bitc::CAST_FPTOSI;
  case Instruction::UIToFP  : return bitc::CAST_UITOFP;
  case Instruction::SIToFP  : return bitc::CAST_SITOFP;
  case Instruction::FPTrunc : return bitc::CAST_FPTRUNC;
  case Instruction::FPExt   : return bitc::CAST_FPEXT;
  case Instruction::PtrToInt: return bitc::CAST_PTRTOINT;
  case Instruction::IntToPtr: return bitc::CAST_INTTOPTR;
  case Instruction::BitCast : return bitc::CAST_BITCAST;
  case Instruction::AddrSpaceCast:
    assert(false && "addrspacecast instructions must be filtered out before writing 3.2 bitcode");
    llvm_unreachable("addrspacecast instructions must be filtered out before writing 3.2 bitcode");
  }
}

static unsigned GetEncodedBinaryOpcode(unsigned Opcode) {
  switch (Opcode) {
  default: llvm_unreachable("Unknown binary instruction!");
  case Instruction::Add:
  case Instruction::FAdd: return bitc::BINOP_ADD;
  case Instruction::Sub:
  case Instruction::FSub: return bitc::BINOP_SUB;
  case Instruction::Mul:
  case Instruction::FMul: return bitc::BINOP_MUL;
  case Instruction::UDiv: return bitc::BINOP_UDIV;
  case Instruction::FDiv:
  case Instruction::SDiv: return bitc::BINOP_SDIV;
  case Instruction::URem: return bitc::BINOP_UREM;
  case Instruction::FRem:
  case Instruction::SRem: return bitc::BINOP_SREM;
  case Instruction::Shl:  return bitc::BINOP_SHL;
  case Instruction::LShr: return bitc::BINOP_LSHR;
  case Instruction::AShr: return bitc::BINOP_ASHR;
  case Instruction::And:  return bitc::BINOP_AND;
  case Instruction::Or:   return bitc::BINOP_OR;
  case Instruction::Xor:  return bitc::BINOP_XOR;
  }
}

static unsigned GetEncodedRMWOperation(AtomicRMWInst::BinOp Op) {
  switch (Op) {
  default: llvm_unreachable("Unknown RMW operation!");
  case AtomicRMWInst::Xchg: return bitc::RMW_XCHG;
  case AtomicRMWInst::Add: return bitc::RMW_ADD;
  case AtomicRMWInst::Sub: return bitc::RMW_SUB;
  case AtomicRMWInst::And: return bitc::RMW_AND;
  case AtomicRMWInst::Nand: return bitc::RMW_NAND;
  case AtomicRMWInst::Or: return bitc::RMW_OR;
  case AtomicRMWInst::Xor: return bitc::RMW_XOR;
  case AtomicRMWInst::Max: return bitc::RMW_MAX;
  case AtomicRMWInst::Min: return bitc::RMW_MIN;
  case AtomicRMWInst::UMax: return bitc::RMW_UMAX;
  case AtomicRMWInst::UMin: return bitc::RMW_UMIN;
  }
}

static unsigned GetEncodedOrdering(AtomicOrdering Ordering) {
  switch (Ordering) {
  case AtomicOrdering::NotAtomic: return bitc::ORDERING_NOTATOMIC;
  case AtomicOrdering::Unordered: return bitc::ORDERING_UNORDERED;
  case AtomicOrdering::Monotonic: return bitc::ORDERING_MONOTONIC;
  case AtomicOrdering::Acquire: return bitc::ORDERING_ACQUIRE;
  case AtomicOrdering::Release: return bitc::ORDERING_RELEASE;
  case AtomicOrdering::AcquireRelease: return bitc::ORDERING_ACQREL;
  case AtomicOrdering::SequentiallyConsistent: return bitc::ORDERING_SEQCST;
  }
  llvm_unreachable("Invalid ordering");
}

static unsigned GetEncodedSynchScope(SyncScope::ID SSID) {
  switch (SSID) {
    case SyncScope::SingleThread: return bitc::SYNCHSCOPE_SINGLETHREAD_OLD;
    case SyncScope::System: return bitc::SYNCHSCOPE_CROSSTHREAD_OLD;
  }
  llvm_unreachable("Invalid synch scope");
}

static void WriteStringRecord(unsigned Code, StringRef Str,
                              unsigned AbbrevToUse, BitstreamWriter &Stream) {
  SmallVector<unsigned, 64> Vals;

  // Code: [strchar x N]
  for (unsigned i = 0, e = Str.size(); i != e; ++i) {
    if (AbbrevToUse && !BitCodeAbbrevOp::isChar6(Str[i]))
      AbbrevToUse = 0;
    Vals.push_back(Str[i]);
  }

  // Emit the finished record.
  Stream.EmitRecord(Code, Vals, AbbrevToUse);
}

uint64_t getAttrKindEncodingBC32(Attribute::AttrKind Kind); // for use in ValueEnumerator32 as well
uint64_t getAttrKindEncodingBC32(Attribute::AttrKind Kind) {
  // NOTE: these are the only parameter attributes supported by SPIR 1.2 (see "3.9 Parameter Attributes")
  // NOTE: nocapture is technically supported as well, but leads to problems on certain implementations
  switch (Kind) {
  case Attribute::None:            return 0;
  case Attribute::ZExt:            return 1 << 0;
  case Attribute::SExt:            return 1 << 1;
  case Attribute::StructRet:       return 1 << 4;
  case Attribute::ByVal:           return 1 << 7;
  default: return 0; // just drop the attr
  }
}

static void WriteAttributeTable(const ValueEnumerator32 &VE,
                                BitstreamWriter &Stream) {
  const std::vector<AttributeList> &Attrs = VE.getAttributeLists();
  if (Attrs.empty()) return;

  Stream.EnterSubblock(bitc::PARAMATTR_BLOCK_ID, 3);

  SmallVector<uint64_t, 64> Record;
  for (unsigned i = 0, e = Attrs.size(); i != e; ++i) {
    AttributeList AL = Attrs[i];
    for (unsigned i : AL.indexes()) {
      // manual A.raw(i) so that we don't hit any asserts or unreachables (for attrs that have no raw representation)
      // also: ignore 3.5+ style alignment, 3.2 style alignment is handled after this
      uint64_t raw_attrs = 0;
      AttributeSet attrs = AL.getAttributes(i);
      for (auto Attr : attrs) {
        // can't handle non enum/int attrs
        if (!Attr.isEnumAttribute() && !Attr.isIntAttribute()) continue;

        Attribute::AttrKind Kind = Attr.getKindAsEnum();
        if (Kind == Attribute::Alignment) { /* drop it */ }
        else if (Kind == Attribute::StackAlignment) { /* drop it */ }
        else if (Kind == Attribute::Dereferenceable) { /* drop it */ }
        else raw_attrs |= getAttrKindEncodingBC32(Kind);
      }

      // Taken from LLVM 3.2 Attributes::encodeLLVMAttributesForBitcode
      uint64_t EncodedAttrs = raw_attrs & 0xffff;
      if (AL.hasAttributeAtIndex(i, Attribute::Alignment))
        EncodedAttrs |= AL.getAttributeAtIndex(i, Attribute::Alignment).getAlignment().getValue().value() << 16;
      EncodedAttrs |= (raw_attrs & (0xffffULL << 21)) << 11;

      Record.push_back(i);
      Record.push_back(EncodedAttrs);
    }

    Stream.EmitRecord(bitc::PARAMATTR_CODE_ENTRY_OLD, Record);
    Record.clear();
  }

  Stream.ExitBlock();
}

/// WriteTypeTable - Write out the type table for a module.
static void WriteTypeTable(const ValueEnumerator32 &VE, BitstreamWriter &Stream) {
  const ValueEnumerator32::TypeList &TypeList = VE.getTypes();

  Stream.EnterSubblock(bitc::TYPE_BLOCK_ID_NEW, 4 /*count from # abbrevs */);
  SmallVector<uint64_t, 64> TypeVals;

  uint64_t NumBits = VE.computeBitsRequiredForTypeIndicies();

  // Abbrev for TYPE_CODE_POINTER.
  auto Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_POINTER));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, NumBits));
  Abbv->Add(BitCodeAbbrevOp(0));  // Addrspace = 0
  unsigned PtrAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Abbrev for TYPE_CODE_FUNCTION.
  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_FUNCTION));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 1));  // isvararg
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, NumBits));

  unsigned FunctionAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Abbrev for TYPE_CODE_STRUCT_ANON.
  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_STRUCT_ANON));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 1));  // ispacked
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, NumBits));

  unsigned StructAnonAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Abbrev for TYPE_CODE_STRUCT_NAME.
  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_STRUCT_NAME));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Char6));
  unsigned StructNameAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Abbrev for TYPE_CODE_STRUCT_NAMED.
  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_STRUCT_NAMED));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 1));  // ispacked
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, NumBits));

  unsigned StructNamedAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // Abbrev for TYPE_CODE_ARRAY.
  Abbv = std::make_shared<BitCodeAbbrev>();
  Abbv->Add(BitCodeAbbrevOp(bitc::TYPE_CODE_ARRAY));
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));   // size
  Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, NumBits));

  unsigned ArrayAbbrev = Stream.EmitAbbrev(std::move(Abbv));

  // don't emit token types for 3.2
  unsigned int ignored_types = 0;
  for (const auto& type : TypeList) {
    if (type->getTypeID() == Type::TokenTyID) {
      ++ignored_types;
    }
  }

  // Emit an entry count so the reader can reserve space.
  TypeVals.push_back(TypeList.size() - ignored_types);
  Stream.EmitRecord(bitc::TYPE_CODE_NUMENTRY, TypeVals);
  TypeVals.clear();

  // Loop over all of the types, emitting each in turn.
  for (unsigned i = 0, e = TypeList.size(); i != e; ++i) {
    Type *T = TypeList[i];
    int AbbrevToUse = 0;
    unsigned Code = 0;

    // not in 3.2
    if (T->getTypeID() == Type::TokenTyID) continue;

    switch (T->getTypeID()) {
    case Type::VoidTyID:      Code = bitc::TYPE_CODE_VOID;      break;
    case Type::HalfTyID:      Code = bitc::TYPE_CODE_HALF;      break;
    case Type::FloatTyID:     Code = bitc::TYPE_CODE_FLOAT;     break;
    case Type::DoubleTyID:    Code = bitc::TYPE_CODE_DOUBLE;    break;
    case Type::X86_FP80TyID:  Code = bitc::TYPE_CODE_X86_FP80;  break;
    case Type::FP128TyID:     Code = bitc::TYPE_CODE_FP128;     break;
    case Type::PPC_FP128TyID: Code = bitc::TYPE_CODE_PPC_FP128; break;
    case Type::LabelTyID:     Code = bitc::TYPE_CODE_LABEL;     break;
    case Type::MetadataTyID:  Code = bitc::TYPE_CODE_METADATA;  break;
    case Type::X86_MMXTyID:   Code = bitc::TYPE_CODE_X86_MMX;   break;
    case Type::TokenTyID: break; // already handled
    case Type::IntegerTyID:
      // INTEGER: [width]
      Code = bitc::TYPE_CODE_INTEGER;
      TypeVals.push_back(cast<IntegerType>(T)->getBitWidth());
      break;
    case Type::PointerTyID: {
      PointerType *PTy = cast<PointerType>(T);
      // POINTER: [pointee type, address space]
      Code = bitc::TYPE_CODE_POINTER;
      TypeVals.push_back(VE.getTypeID(PTy->getElementType()));
      unsigned AddressSpace = PTy->getAddressSpace();
      TypeVals.push_back(AddressSpace);
      if (AddressSpace == 0) AbbrevToUse = PtrAbbrev;
      break;
    }
    case Type::FunctionTyID: {
      FunctionType *FT = cast<FunctionType>(T);
      // FUNCTION: [isvararg, retty, paramty x N]
      Code = bitc::TYPE_CODE_FUNCTION;
      TypeVals.push_back(FT->isVarArg());
      TypeVals.push_back(VE.getTypeID(FT->getReturnType()));
      for (unsigned i = 0, e = FT->getNumParams(); i != e; ++i)
        TypeVals.push_back(VE.getTypeID(FT->getParamType(i)));
      AbbrevToUse = FunctionAbbrev;
      break;
    }
    case Type::StructTyID: {
      StructType *ST = cast<StructType>(T);
      // STRUCT: [ispacked, eltty x N]
      TypeVals.push_back(ST->isPacked());
      // Output all of the element types.
      for (StructType::element_iterator I = ST->element_begin(),
           E = ST->element_end(); I != E; ++I)
        TypeVals.push_back(VE.getTypeID(*I));

      if (ST->isLiteral()) {
        Code = bitc::TYPE_CODE_STRUCT_ANON;
        AbbrevToUse = StructAnonAbbrev;
      } else {
        if (ST->isOpaque()) {
          Code = bitc::TYPE_CODE_OPAQUE;
        } else {
          Code = bitc::TYPE_CODE_STRUCT_NAMED;
          AbbrevToUse = StructNamedAbbrev;
        }

        // Emit the name if it is present.
        if (!ST->getName().empty())
          WriteStringRecord(bitc::TYPE_CODE_STRUCT_NAME, ST->getName(),
                            StructNameAbbrev, Stream);
      }
      break;
    }
    case Type::ArrayTyID: {
      ArrayType *AT = cast<ArrayType>(T);
      // ARRAY: [numelts, eltty]
      Code = bitc::TYPE_CODE_ARRAY;
      TypeVals.push_back(AT->getNumElements());
      TypeVals.push_back(VE.getTypeID(AT->getElementType()));
      AbbrevToUse = ArrayAbbrev;
      break;
    }
    case Type::FixedVectorTyID: {
      FixedVectorType *VT = cast<FixedVectorType>(T);
      // VECTOR [numelts, eltty]
      Code = bitc::TYPE_CODE_VECTOR;
      TypeVals.push_back(VT->getNumElements());
      TypeVals.push_back(VE.getTypeID(VT->getElementType()));
      break;
    }
    case Type::ScalableVectorTyID:
      report_fatal_error("scalar vector types are not supported with LLVM 3.2");
      break;
    case Type::BFloatTyID:
      report_fatal_error("bfloat16 type is not supported with LLVM 3.2");
      break;
    case Type::X86_AMXTyID:
      report_fatal_error("AMX types are not supported with LLVM 3.2");
      break;
    }

    // Emit the finished record.
    Stream.EmitRecord(Code, TypeVals, AbbrevToUse);
    TypeVals.clear();
  }

  Stream.ExitBlock();
}

static unsigned getEncodedLinkage(const GlobalValue &GV) {
  switch (GV.getLinkage()) {
  case GlobalValue::ExternalLinkage:                 return 0;
  case GlobalValue::WeakAnyLinkage:                  return 1;
  case GlobalValue::AppendingLinkage:                return 2;
  case GlobalValue::InternalLinkage:                 return 3;
  case GlobalValue::LinkOnceAnyLinkage:              return 4;
  case GlobalValue::ExternalWeakLinkage:             return 7;
  case GlobalValue::CommonLinkage:                   return 8;
  case GlobalValue::PrivateLinkage:                  return 9;
  case GlobalValue::WeakODRLinkage:                  return 10;
  case GlobalValue::LinkOnceODRLinkage:              return 11;
  case GlobalValue::AvailableExternallyLinkage:      return 12;
  }
  llvm_unreachable("Invalid linkage");
}

static unsigned getEncodedVisibility(const GlobalValue &GV) {
  switch (GV.getVisibility()) {
  case GlobalValue::DefaultVisibility:   return 0;
  case GlobalValue::HiddenVisibility:    return 1;
  case GlobalValue::ProtectedVisibility: return 2;
  }
  llvm_unreachable("Invalid visibility");
}

static unsigned getEncodedThreadLocalMode(const GlobalValue &GV) {
  switch (GV.getThreadLocalMode()) {
    case GlobalVariable::NotThreadLocal:         return 0;
    case GlobalVariable::GeneralDynamicTLSModel: return 1;
    case GlobalVariable::LocalDynamicTLSModel:   return 2;
    case GlobalVariable::InitialExecTLSModel:    return 3;
    case GlobalVariable::LocalExecTLSModel:      return 4;
  }
  llvm_unreachable("Invalid TLS model");
}

/// Emit top-level description of module, including target triple, inline asm,
/// descriptors for global variables, and function prototype info.
/// Returns the bit offset to backpatch with the location of the real VST.
static uint64_t WriteModuleInfo(const Module *M, const ValueEnumerator32 &VE,
                                BitstreamWriter &Stream) {
  // always override target triple and data layout
  const bool is_64_bit = (M->getTargetTriple().compare(0, 6, "spir64") == 0);
  WriteStringRecord(bitc::MODULE_CODE_TRIPLE,
                    is_64_bit ? SPIR64_TRIPLE : SPIR32_TRIPLE,
                    0, Stream);
  WriteStringRecord(bitc::MODULE_CODE_DATALAYOUT,
                    is_64_bit ? SPIR64_DATALAYOUT : SPIR32_DATALAYOUT,
                    0, Stream);

  if (!M->getModuleInlineAsm().empty())
    WriteStringRecord(bitc::MODULE_CODE_ASM, M->getModuleInlineAsm(),
                      0/*TODO*/, Stream);

  // Emit information about sections and GC, computing how many there are. Also
  // compute the maximum alignment value.
  std::map<std::string, unsigned> SectionMap;
  std::map<std::string, unsigned> GCMap;
  MaybeAlign MaxAlignment;
  unsigned MaxGlobalType = 0;
  const auto UpdateMaxAlignment = [&MaxAlignment](const MaybeAlign A) {
    if (A)
      MaxAlignment = !MaxAlignment ? *A : std::max(*MaxAlignment, *A);
  };
  for (const GlobalVariable &GV : M->globals()) {
    UpdateMaxAlignment(GV.getAlign());
    MaxGlobalType = std::max(MaxGlobalType, VE.getTypeID(GV.getType()));
    if (GV.hasSection()) {
      // Give section names unique ID's.
      unsigned &Entry = SectionMap[std::string(GV.getSection())];
      if (!Entry) {
        WriteStringRecord(bitc::MODULE_CODE_SECTIONNAME, GV.getSection(),
                          0/*TODO*/, Stream);
        Entry = SectionMap.size();
      }
    }
  }
  for (const Function &F : *M) {
    UpdateMaxAlignment(F.getAlign());
    if (F.hasSection()) {
      // Give section names unique ID's.
      unsigned &Entry = SectionMap[std::string(F.getSection())];
      if (!Entry) {
        WriteStringRecord(bitc::MODULE_CODE_SECTIONNAME, F.getSection(),
                          0/*TODO*/, Stream);
        Entry = SectionMap.size();
      }
    }
    if (F.hasGC()) {
      // Same for GC names.
      unsigned &Entry = GCMap[F.getGC()];
      if (!Entry) {
        WriteStringRecord(bitc::MODULE_CODE_GCNAME, F.getGC(),
                          0/*TODO*/, Stream);
        Entry = GCMap.size();
      }
    }
  }

  // Emit abbrev for globals, now that we know # sections and max alignment.
  unsigned SimpleGVarAbbrev = 0;
  if (!M->global_empty()) {
    // Add an abbrev for common globals with no visibility or thread localness.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::MODULE_CODE_GLOBALVAR));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,
                              Log2_32_Ceil(MaxGlobalType+1)));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 1));      // Constant.
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));        // Initializer.
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 4));      // Linkage.
    if (!MaxAlignment)                                         // Alignment.
      Abbv->Add(BitCodeAbbrevOp(0));
    else {
      unsigned MaxEncAlignment = Log2_32(MaxAlignment.getValue().value())+1;
      Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,
                               Log2_32_Ceil(MaxEncAlignment+1)));
    }
    if (SectionMap.empty())                                    // Section.
      Abbv->Add(BitCodeAbbrevOp(0));
    else
      Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,
                               Log2_32_Ceil(SectionMap.size()+1)));
    // Don't bother emitting vis + thread local.
    SimpleGVarAbbrev = Stream.EmitAbbrev(std::move(Abbv));
  }

  // Emit the global variable information.
  SmallVector<unsigned, 64> Vals;
  for (const GlobalVariable &GV : M->globals()) {
    unsigned AbbrevToUse = 0;

    // GLOBALVAR: [type, isconst, initid,
    //             linkage, alignment, section, visibility, threadlocal,
    //             unnamed_addr]
    Vals.push_back(VE.getTypeID(GV.getType()));
    Vals.push_back(GV.isConstant());
    Vals.push_back(GV.isDeclaration() ? 0 :
                   (VE.getValueID(GV.getInitializer()) + 1));
    Vals.push_back(getEncodedLinkage(GV));
    Vals.push_back(Log2_32(GV.getAlignment())+1);
    Vals.push_back(GV.hasSection() ? SectionMap[std::string(GV.getSection())] : 0);
    if (GV.isThreadLocal() ||
        GV.getVisibility() != GlobalValue::DefaultVisibility ||
        GV.hasGlobalUnnamedAddr()) {
      Vals.push_back(getEncodedVisibility(GV));
      Vals.push_back(getEncodedThreadLocalMode(GV));
      Vals.push_back(GV.hasGlobalUnnamedAddr());
    } else {
      AbbrevToUse = SimpleGVarAbbrev;
    }

    Stream.EmitRecord(bitc::MODULE_CODE_GLOBALVAR, Vals, AbbrevToUse);
    Vals.clear();
  }

  // Emit the function proto information.
  for (const Function &F : *M) {
    // FUNCTION:  [type, callingconv, isproto, linkage, paramattrs, alignment,
    //             section, visibility, gc, unnamed_addr]
    Vals.push_back(VE.getTypeID(F.getType()));
    Vals.push_back(F.getCallingConv());
    Vals.push_back(F.isDeclaration());
    Vals.push_back(getEncodedLinkage(F));
    Vals.push_back(VE.getAttributeListID(F.getAttributes()));
    Vals.push_back(Log2_32(F.getAlignment())+1);
    Vals.push_back(F.hasSection() ? SectionMap[std::string(F.getSection())] : 0);
    Vals.push_back(getEncodedVisibility(F));
    Vals.push_back(F.hasGC() ? GCMap[F.getGC()] : 0);
    Vals.push_back(F.hasGlobalUnnamedAddr());

    unsigned AbbrevToUse = 0;
    Stream.EmitRecord(bitc::MODULE_CODE_FUNCTION, Vals, AbbrevToUse);
    Vals.clear();
  }

  // Emit the alias information.
  for (const GlobalAlias &A : M->aliases()) {
    // ALIAS: [alias type, aliasee val#, linkage, visibility]
    Vals.push_back(VE.getTypeID(A.getType()));
    Vals.push_back(VE.getValueID(A.getAliasee()));
    Vals.push_back(getEncodedLinkage(A));
    Vals.push_back(getEncodedVisibility(A));
    unsigned AbbrevToUse = 0;
    Stream.EmitRecord(bitc::MODULE_CODE_ALIAS_OLD, Vals, AbbrevToUse);
    Vals.clear();
  }

  return 0;
}

static uint64_t GetOptimizationFlags(const Value *V) {
  uint64_t Flags = 0;

  if (const auto *OBO = dyn_cast<OverflowingBinaryOperator>(V)) {
    if (OBO->hasNoSignedWrap())
      Flags |= 1 << bitc::OBO_NO_SIGNED_WRAP;
    if (OBO->hasNoUnsignedWrap())
      Flags |= 1 << bitc::OBO_NO_UNSIGNED_WRAP;
  } else if (const auto *PEO = dyn_cast<PossiblyExactOperator>(V)) {
    if (PEO->isExact())
      Flags |= 1 << bitc::PEO_EXACT;
  }
  // no fast math flags in 3.2

  return Flags;
}

static void WriteValueAsMetadata(const ValueAsMetadata *MD,
                                 const ValueEnumerator32 &VE,
                                 BitstreamWriter &Stream,
                                 SmallVectorImpl<uint64_t> &Record,
                                 const bool func_local = false) {
  // Mimic an MDNode with a value as one operand.
  Value *V = MD->getValue();
  Record.push_back(VE.getTypeID(V->getType()));
  Record.push_back(VE.getValueID(V));
  Stream.EmitRecord(!func_local ? bitc::METADATA_OLD_NODE : bitc::METADATA_OLD_FN_NODE, Record, 0);
  Record.clear();
}

// NOTE: similar to WriteMDNode from 3.2 (however, no function-local in here!)
static void WriteMDTuple(const MDTuple *N, const ValueEnumerator32 &VE,
                         BitstreamWriter &Stream,
                         SmallVectorImpl<uint64_t> &Record, unsigned Abbrev) {
  for (unsigned i = 0, e = N->getNumOperands(); i != e; ++i) {
    const Metadata* op = N->getOperand(i);
    if (op == nullptr) {
      Record.push_back(VE.getTypeID(Type::getVoidTy(N->getContext())));
      Record.push_back(0);
      continue;
    }
    
    switch (op->getMetadataID()) {
      case Metadata::LocalAsMetadataKind:
        assert(false && "Unexpected function-local metadata");
        break;
      case Metadata::ConstantAsMetadataKind: {
        auto V = dyn_cast<ConstantAsMetadata>(op)->getValue();
        Record.push_back(VE.getTypeID(V->getType()));
        Record.push_back(VE.getValueID(V));
        break;
      }
      default:
        Record.push_back(VE.getTypeID(Type::getMetadataTy(N->getContext())));
        Record.push_back(VE.getMetadataID(op));
        break;
    }
  }
  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
}

// DI* helper functions/macros
static void WriteDI_UNIMPLEMENTED(BitstreamWriter &Stream) {
  SmallVector<uint64_t, 1> empty_record;
  Stream.EmitRecord(bitc::METADATA_OLD_NODE, empty_record, 0);
}

#define DI_TYPE_I1() Record.push_back(VE.getTypeID(Type::getInt1Ty(N->getContext())))
#define DI_I1(val) { DI_TYPE_I1(); Record.push_back(VE.getValueID(ConstantInt::get(llvm::Type::getInt1Ty(N->getContext()), val))); }

#define DI_TYPE_I32() Record.push_back(VE.getTypeID(Type::getInt32Ty(N->getContext())))
#define DI_I32(val) { DI_TYPE_I32(); Record.push_back(VE.getValueID(ConstantInt::get(llvm::Type::getInt32Ty(N->getContext()), val))); }

#define DI_TYPE_I64() Record.push_back(VE.getTypeID(Type::getInt64Ty(N->getContext())))
#define DI_I64(val) { DI_TYPE_I64(); Record.push_back(VE.getValueID(ConstantInt::get(llvm::Type::getInt64Ty(N->getContext()), val))); }

#define DI_TYPE_META() Record.push_back(VE.getTypeID(Type::getMetadataTy(N->getContext())))
#define DI_META(val) { DI_TYPE_META(); Record.push_back(VE.getMetadataID(val)); }

#define DI_TYPE_VOID() Record.push_back(VE.getTypeID(Type::getVoidTy(N->getContext())))
#define DI_NULL() { DI_TYPE_VOID(); Record.push_back(0); }

#define DI_FUNC(func) { Record.push_back(VE.getTypeID(func->getType())); Record.push_back(VE.getValueID(func)); }

#define DI_TAG(tag) { DI_TYPE_I32(); Record.push_back(VE.getValueID(GetTagConstant(N->getContext(), tag))); }

#define DI_META_OR_NULL(val) if(val) DI_META(val) else DI_NULL()

// from 3.2 DIBuilder.cpp
static Constant *GetTagConstant(LLVMContext &VMContext, unsigned Tag) {
  assert((Tag & 0xffff0000 /* LLVMDebugVersionMask */) == 0 &&
         "Tag too large for debug encoding!");
  return ConstantInt::get(Type::getInt32Ty(VMContext), Tag | (12 << 16) /* LLVMDebugVersion */);
}

static void WriteDILocation(const DILocation *N, const ValueEnumerator32 &VE,
                            BitstreamWriter &Stream,
                            SmallVectorImpl<uint64_t> &Record,
                            unsigned Abbrev) {
  DI_I32(N->getLine());
  DI_I32(N->getColumn());
  DI_META(N->getScope());
  DI_META_OR_NULL(N->getInlinedAt());

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
}

// NOTE: not in 3.2
static void WriteGenericDINode(const GenericDINode *,
                               const ValueEnumerator32 &,
                               BitstreamWriter &Stream,
                               SmallVectorImpl<uint64_t> &,
                               unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

#if 0
static uint64_t rotateSign(int64_t I) {
  uint64_t U = I;
  return I < 0 ? ~(U << 1) : U << 1;
}
#endif

static void WriteDISubrange(const DISubrange *N, const ValueEnumerator32 &,
                            BitstreamWriter &Stream,
                            SmallVectorImpl<uint64_t> &Record,
                            unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(N->getCount());
  Record.push_back(rotateSign(N->getLowerBound()));

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

static void WriteDIEnumerator(const DIEnumerator *N, const ValueEnumerator32 &VE,
                              BitstreamWriter &Stream,
                              SmallVectorImpl<uint64_t> &Record,
                              unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(rotateSign(N->getValue()));
  Record.push_back(VE.getMetadataOrNullID(N->getRawName()));

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

static void WriteDIBasicType(const DIBasicType *N, const ValueEnumerator32 &VE,
                             BitstreamWriter &Stream,
                             SmallVectorImpl<uint64_t> &Record,
                             unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(N->getTag());
  Record.push_back(VE.getMetadataOrNullID(N->getRawName()));
  Record.push_back(N->getSizeInBits());
  Record.push_back(N->getAlignInBits());
  Record.push_back(N->getEncoding());

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

static void WriteDIDerivedType(const DIDerivedType *N,
                               const ValueEnumerator32 &VE,
                               BitstreamWriter &Stream,
                               SmallVectorImpl<uint64_t> &Record,
                               unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(N->getTag());
  Record.push_back(VE.getMetadataOrNullID(N->getRawName()));
  Record.push_back(VE.getMetadataOrNullID(N->getFile()));
  Record.push_back(N->getLine());
  Record.push_back(VE.getMetadataOrNullID(N->getScope()));
  Record.push_back(VE.getMetadataOrNullID(N->getBaseType()));
  Record.push_back(N->getSizeInBits());
  Record.push_back(N->getAlignInBits());
  Record.push_back(N->getOffsetInBits());
  Record.push_back(N->getFlags());
  Record.push_back(VE.getMetadataOrNullID(N->getExtraData()));

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

static void WriteDICompositeType(const DICompositeType *N,
                                 const ValueEnumerator32 &VE,
                                 BitstreamWriter &Stream,
                                 SmallVectorImpl<uint64_t> &Record,
                                 unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(N->getTag());
  Record.push_back(VE.getMetadataOrNullID(N->getRawName()));
  Record.push_back(VE.getMetadataOrNullID(N->getFile()));
  Record.push_back(N->getLine());
  Record.push_back(VE.getMetadataOrNullID(N->getScope()));
  Record.push_back(VE.getMetadataOrNullID(N->getBaseType()));
  Record.push_back(N->getSizeInBits());
  Record.push_back(N->getAlignInBits());
  Record.push_back(N->getOffsetInBits());
  Record.push_back(N->getFlags());
  Record.push_back(VE.getMetadataOrNullID(N->getElements().get()));
  Record.push_back(N->getRuntimeLang());
  Record.push_back(VE.getMetadataOrNullID(N->getVTableHolder()));
  Record.push_back(VE.getMetadataOrNullID(N->getTemplateParams().get()));
  Record.push_back(VE.getMetadataOrNullID(N->getRawIdentifier()));

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

static void WriteDISubroutineType(const DISubroutineType *N,
                                  const ValueEnumerator32 &VE,
                                  BitstreamWriter &Stream,
                                  SmallVectorImpl<uint64_t> &Record,
                                  unsigned Abbrev) {
  DI_TAG(dwarf::DW_TAG_subroutine_type);

  DI_I32(0);
  DI_NULL();
  auto empty_str_node = MDString::get(N->getContext(), "");
  DI_META(empty_str_node);
  DI_I32(0);
  DI_I64(0);
  DI_I64(0);
  DI_I64(0);
  DI_I32(N->getFlags());
  DI_NULL();
  DI_META_OR_NULL(N->getTypeArray().get());
  DI_I32(0);
  DI_NULL();
  DI_NULL();
  DI_NULL();

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
}

static void WriteDIFile(const DIFile *N, const ValueEnumerator32 &VE,
                        BitstreamWriter &Stream,
                        SmallVectorImpl<uint64_t> &Record, unsigned Abbrev) {
  // NOTE: { file, dir } node will already have been written
  DI_TAG(dwarf::DW_TAG_file_type);

  DI_META(N->contained_node);

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
}

static void WriteDICompileUnit(const DICompileUnit *N,
                               const ValueEnumerator32 &VE,
                               BitstreamWriter &Stream,
                               SmallVectorImpl<uint64_t> &Record,
                               unsigned Abbrev) {
  assert(N->isDistinct() && "Expected distinct compile units");

  DI_TAG(dwarf::DW_TAG_compile_unit);

  if (N->getFile()) {
    // not an actual DIFile node, but directly points to a { file, dir } node
    DI_META(N->getFile()->contained_node);
  } else DI_NULL()

  DI_I32(N->getSourceLanguage());
  DI_META_OR_NULL(N->getRawProducer());
  DI_I1(N->isOptimized());
  DI_META_OR_NULL(N->getRawFlags());
  DI_I32(N->getRuntimeVersion());
  DI_META_OR_NULL(N->getEnumTypes().get());
  DI_META_OR_NULL(N->getRetainedTypes().get());
  DI_META_OR_NULL(/*N->getSubprograms().get()*/ nullptr); // TODO: fix this, subprogram <-> cu ownership switched in 3.9
  DI_META_OR_NULL(N->getGlobalVariables().get());
  DI_META_OR_NULL(N->getImportedEntities().get());
  DI_META_OR_NULL(N->getRawSplitDebugFilename());
  DI_I32(N->getEmissionKind());
  // NOTE: no macros or dwarf id

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
}

static void WriteDISubprogram(const DISubprogram *N, const ValueEnumerator32 &VE,
                              BitstreamWriter &Stream,
                              SmallVectorImpl<uint64_t> &Record,
                              unsigned Abbrev) {
  DI_TAG(dwarf::DW_TAG_subprogram);

  DI_META_OR_NULL(N->getFile());
  DI_META_OR_NULL(N->getScope());
  DI_META_OR_NULL(N->getRawName());
  DI_META_OR_NULL(N->getRawName());
  DI_META_OR_NULL(N->getRawLinkageName());
  DI_I32(N->getLine());
  DI_META_OR_NULL(N->getType());
  DI_I1(N->isLocalToUnit());
  DI_I1(N->isDefinition());
  DI_I32(N->getVirtuality());
  DI_I32(N->getVirtualIndex());
  DI_META_OR_NULL(N->getContainingType());
  DI_I32(N->getFlags());
  DI_I1(N->isOptimized());
  if (N->associated_function) {
    DI_FUNC(N->associated_function);
  }
  else DI_NULL();
  DI_META_OR_NULL(N->getTemplateParams().get());
  DI_META_OR_NULL(N->getDeclaration());
  // TODO: always pointing to an empty node if non-existent?
  if (N->getRetainedNodes()) {
    DI_META(N->getRetainedNodes().get());
  }
  else {
    auto empty_node = MDTuple::getTemporary(N->getContext(), {});
    DI_META(empty_node.get());
  }
  DI_I32(N->getScopeLine());

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
}

static void WriteDILexicalBlock(const DILexicalBlock *N,
                                const ValueEnumerator32 &VE,
                                BitstreamWriter &Stream,
                                SmallVectorImpl<uint64_t> &Record,
                                unsigned Abbrev) {
  DI_TAG(dwarf::DW_TAG_lexical_block);

  static unsigned int unique_id = 0;
  DI_META_OR_NULL(N->getFile());
  DI_META_OR_NULL(N->getScope());
  DI_I32(N->getLine());
  DI_I32(N->getColumn());
  DI_I32(0); // NOTE: no discriminator (also 0 in 3.2)
  DI_I32(unique_id++);

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
}

static void WriteDILexicalBlockFile(const DILexicalBlockFile *N,
                                    const ValueEnumerator32 &VE,
                                    BitstreamWriter &Stream,
                                    SmallVectorImpl<uint64_t> &Record,
                                    unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(VE.getMetadataOrNullID(N->getScope()));
  Record.push_back(VE.getMetadataOrNullID(N->getFile()));
  Record.push_back(N->getDiscriminator());

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

static void WriteDINamespace(const DINamespace *N, const ValueEnumerator32 &VE,
                             BitstreamWriter &Stream,
                             SmallVectorImpl<uint64_t> &Record,
                             unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(VE.getMetadataOrNullID(N->getScope()));
  Record.push_back(VE.getMetadataOrNullID(N->getFile()));
  Record.push_back(VE.getMetadataOrNullID(N->getRawName()));
  Record.push_back(N->getLine());

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

// NOTE: not in 3.2
static void WriteDIMacro(const DIMacro *, const ValueEnumerator32 &,
                         BitstreamWriter &Stream,
                         SmallVectorImpl<uint64_t> &, unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

// NOTE: not in 3.2
static void WriteDIMacroFile(const DIMacroFile *, const ValueEnumerator32 &,
                             BitstreamWriter &Stream,
                             SmallVectorImpl<uint64_t> &,
                             unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

// NOTE: not in 3.2
static void WriteDIModule(const DIModule *, const ValueEnumerator32 &,
                          BitstreamWriter &Stream,
                          SmallVectorImpl<uint64_t> &, unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

// NOTE: not in 3.2
static void WriteDICommonBlock(const DICommonBlock *, const ValueEnumerator32 &,
                               BitstreamWriter &Stream,
                               SmallVectorImpl<uint64_t> &,
                               unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

// NOTE: not in 3.2
static void WriteDIArgList(const DIArgList *, const ValueEnumerator32 &,
                           BitstreamWriter &Stream, SmallVectorImpl<uint64_t> &,
                           unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

// NOTE: not in 3.2
static void WriteDIStringType(const DIStringType *, const ValueEnumerator32 &,
                              BitstreamWriter &Stream,
                              SmallVectorImpl<uint64_t> &, unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

// NOTE: not in 3.2
static void WriteDIGenericSubrange(const DIGenericSubrange *, const ValueEnumerator32 &,
                                   BitstreamWriter &Stream,
                                   SmallVectorImpl<uint64_t> &,
                                   unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

static void WriteDITemplateTypeParameter(const DITemplateTypeParameter *N,
                                         const ValueEnumerator32 &VE,
                                         BitstreamWriter &Stream,
                                         SmallVectorImpl<uint64_t> &Record,
                                         unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(VE.getMetadataOrNullID(N->getRawName()));
  Record.push_back(VE.getMetadataOrNullID(N->getType()));

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

static void WriteDITemplateValueParameter(const DITemplateValueParameter *N,
                                          const ValueEnumerator32 &VE,
                                          BitstreamWriter &Stream,
                                          SmallVectorImpl<uint64_t> &Record,
                                          unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(N->getTag());
  Record.push_back(VE.getMetadataOrNullID(N->getRawName()));
  Record.push_back(VE.getMetadataOrNullID(N->getType()));
  Record.push_back(VE.getMetadataOrNullID(N->getValue()));

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

static void WriteDIGlobalVariable(const DIGlobalVariable *N,
                                  const ValueEnumerator32 &VE,
                                  BitstreamWriter &Stream,
                                  SmallVectorImpl<uint64_t> &Record,
                                  unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(VE.getMetadataOrNullID(N->getScope()));
  Record.push_back(VE.getMetadataOrNullID(N->getRawName()));
  Record.push_back(VE.getMetadataOrNullID(N->getRawLinkageName()));
  Record.push_back(VE.getMetadataOrNullID(N->getFile()));
  Record.push_back(N->getLine());
  Record.push_back(VE.getMetadataOrNullID(N->getType()));
  Record.push_back(N->isLocalToUnit());
  Record.push_back(N->isDefinition());
  Record.push_back(VE.getMetadataOrNullID(N->getRawVariable()));
  Record.push_back(VE.getMetadataOrNullID(N->getStaticDataMemberDeclaration()));

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

// NOTE: not in 3.2
static void WriteDIGlobalVariableExpression(const DIGlobalVariableExpression *,
                                            const ValueEnumerator32 &,
                                            BitstreamWriter &Stream,
                                            SmallVectorImpl<uint64_t> &,
                                            unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

// NOTE: not in 3.2
static void WriteDILabel(const DILabel *N,
                         const ValueEnumerator32 &,
                         BitstreamWriter &Stream,
                         SmallVectorImpl<uint64_t> &Record,
                         unsigned Abbrev) {
  WriteDI_UNIMPLEMENTED(Stream);
}

// NOTE: not in 3.2
static void WriteDILocalVariable(const DILocalVariable *,
                                 const ValueEnumerator32 &,
                                 BitstreamWriter &Stream,
                                 SmallVectorImpl<uint64_t> &,
                                 unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

// NOTE: not in 3.2
static void WriteDIExpression(const DIExpression *, const ValueEnumerator32 &,
                              BitstreamWriter &Stream,
                              SmallVectorImpl<uint64_t> &,
                              unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

// NOTE: not supported, since there is no objective-c
static void WriteDIObjCProperty(const DIObjCProperty *,
                                const ValueEnumerator32 &,
                                BitstreamWriter &Stream,
                                SmallVectorImpl<uint64_t> &,
                                unsigned) {
  WriteDI_UNIMPLEMENTED(Stream);
}

static void WriteDIImportedEntity(const DIImportedEntity *N,
                                  const ValueEnumerator32 &VE,
                                  BitstreamWriter &Stream,
                                  SmallVectorImpl<uint64_t> &Record,
                                  unsigned Abbrev) {
#if 1
  WriteDI_UNIMPLEMENTED(Stream);
#else
  Record.push_back(N->getTag());
  Record.push_back(VE.getMetadataOrNullID(N->getScope()));
  Record.push_back(VE.getMetadataOrNullID(N->getEntity()));
  Record.push_back(N->getLine());
  Record.push_back(VE.getMetadataOrNullID(N->getRawName()));

  Stream.EmitRecord(bitc::METADATA_OLD_NODE, Record, Abbrev);
  Record.clear();
#endif
}

static void WriteModuleMetadata(const Module *M,
                                const ValueEnumerator32 &VE,
                                BitstreamWriter &Stream) {
  const auto &MDs = VE.getMDs();
  if (MDs.empty() && M->named_metadata_empty())
    return;

  // NOTE: always present with AIR/SPIR (no need for StartedMetadataBlock)
  Stream.EnterSubblock(bitc::METADATA_BLOCK_ID, 3);

  unsigned MDSAbbrev = 0;
  if (VE.hasMDString()) {
    // Abbrev for METADATA_STRING_OLD.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::METADATA_STRING_OLD));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));
    MDSAbbrev = Stream.EmitAbbrev(std::move(Abbv));
  }

  SmallVector<uint64_t, 64> Record;
  for (const Metadata *MD : MDs) {
    if (const MDNode *N = dyn_cast<MDNode>(MD)) {
      assert(N->isResolved() && "Expected forward references to be resolved");

      // NOTE: no function-local in here
      // NOTE: no abbreviations in here
      switch (N->getMetadataID()) {
      default:
        llvm_unreachable("Invalid MDNode subclass");
#define HANDLE_MDNODE_LEAF(CLASS)                                              \
  case Metadata::CLASS##Kind:                                                  \
    Write##CLASS(cast<CLASS>(N), VE, Stream, Record, 0);                       \
    continue;
#include "llvm/IR/Metadata.def"
      }
    } else if (const auto *MDC = dyn_cast<ConstantAsMetadata>(MD)) {
      WriteValueAsMetadata(MDC, VE, Stream, Record);
    } else if (const MDString *MDS = dyn_cast<MDString>(MD)) {
      // Code: [strchar x N]
      Record.append(MDS->bytes_begin(), MDS->bytes_end());

      // Emit the finished record.
      Stream.EmitRecord(bitc::METADATA_STRING_OLD, Record, MDSAbbrev);
      Record.clear();
    } else {
      assert(false && "unhandled MD type");
    }
  }

  // Write named metadata.
  for (const NamedMDNode &NMD : M->named_metadata()) {
    // Write name.
    StringRef Str = NMD.getName();
    Record.append(Str.bytes_begin(), Str.bytes_end());
    Stream.EmitRecord(bitc::METADATA_NAME, Record, 0);
    Record.clear();

    // Write named metadata operands.
    for (const MDNode *N : NMD.operands())
      Record.push_back(VE.getMetadataID(N));
    Stream.EmitRecord(bitc::METADATA_NAMED_NODE, Record, 0);
    Record.clear();
  }

  Stream.ExitBlock();
}

static void WriteFunctionLocalMetadata(const Function &F,
                                       const ValueEnumerator32 &VE,
                                       BitstreamWriter &Stream) {
  bool StartedMetadataBlock = false;
  SmallVector<uint64_t, 64> Record;
  const SmallVectorImpl<const LocalAsMetadata *> &MDs =
      VE.getFunctionLocalMDs();
  for (unsigned i = 0, e = MDs.size(); i != e; ++i) {
    assert(MDs[i] && "Expected valid function-local metadata");
    if (!StartedMetadataBlock) {
      Stream.EnterSubblock(bitc::METADATA_BLOCK_ID, 3);
      StartedMetadataBlock = true;
    }
    WriteValueAsMetadata(MDs[i], VE, Stream, Record, true);
  }

  if (StartedMetadataBlock)
    Stream.ExitBlock();
}

static void WriteMetadataAttachment(const Function &F,
                                    const ValueEnumerator32 &VE,
                                    BitstreamWriter &Stream) {
  Stream.EnterSubblock(bitc::METADATA_ATTACHMENT_ID, 3);

  SmallVector<uint64_t, 64> Record;

  // Write metadata attachments
  // METADATA_ATTACHMENT - [m x [value, [n x [id, mdnode]]]
  SmallVector<std::pair<unsigned, MDNode *>, 4> MDs;

  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB) {
      MDs.clear();
      I.getAllMetadataOtherThanDebugLoc(MDs);

      // If no metadata, ignore instruction.
      if (MDs.empty()) continue;

      Record.push_back(VE.getInstructionID(&I));

      for (unsigned i = 0, e = MDs.size(); i != e; ++i) {
        Record.push_back(MDs[i].first);
        Record.push_back(VE.getMetadataID(MDs[i].second));
      }
      Stream.EmitRecord(bitc::METADATA_ATTACHMENT, Record, 0);
      Record.clear();
    }

  Stream.ExitBlock();
}

static void WriteModuleMetadataStore(const Module *M, BitstreamWriter &Stream) {
  SmallVector<uint64_t, 64> Record;

  // Write metadata kinds
  // METADATA_KIND - [n x [id, name]]
  SmallVector<StringRef, 8> Names;
  M->getMDKindNames(Names);

  if (Names.empty()) return;

  Stream.EnterSubblock(bitc::METADATA_BLOCK_ID, 3);

  for (unsigned MDKindID = 0, e = Names.size(); MDKindID != e; ++MDKindID) {
    Record.push_back(MDKindID);
    StringRef KName = Names[MDKindID];
    Record.append(KName.begin(), KName.end());

    Stream.EmitRecord(bitc::METADATA_KIND, Record, 0);
    Record.clear();
  }

  Stream.ExitBlock();
}

static void emitSignedInt64(SmallVectorImpl<uint64_t> &Vals, uint64_t V) {
  if ((int64_t)V >= 0)
    Vals.push_back(V << 1);
  else
    Vals.push_back((-V << 1) | 1);
}

static void EmitAPInt(SmallVectorImpl<uint64_t> &Vals,
                      unsigned &Code, unsigned &AbbrevToUse, const APInt &Val,
                      bool EmitSizeForWideNumbers = false) {
  if (Val.getBitWidth() <= 64) {
    uint64_t V = Val.getSExtValue();
    emitSignedInt64(Vals, V);
    Code = bitc::CST_CODE_INTEGER;
    AbbrevToUse = CONSTANTS_INTEGER_ABBREV;
  } else {
    // Wide integers, > 64 bits in size.
    // We have an arbitrary precision integer value to write whose
    // bit width is > 64. However, in canonical unsigned integer
    // format it is likely that the high bits are going to be zero.
    // So, we only write the number of active words.
    unsigned NWords = Val.getActiveWords();

    if (EmitSizeForWideNumbers)
      Vals.push_back(NWords);

    const uint64_t *RawWords = Val.getRawData();
    for (unsigned i = 0; i != NWords; ++i) {
      emitSignedInt64(Vals, RawWords[i]);
    }
    Code = bitc::CST_CODE_WIDE_INTEGER;
  }
}

static void WriteConstants(unsigned FirstVal, unsigned LastVal,
                           const ValueEnumerator32 &VE,
                           BitstreamWriter &Stream, bool isGlobal) {
  if (FirstVal == LastVal) return;

  Stream.EnterSubblock(bitc::CONSTANTS_BLOCK_ID, 4);

  unsigned AggregateAbbrev = 0;
  unsigned String8Abbrev = 0;
  unsigned CString7Abbrev = 0;
  unsigned CString6Abbrev = 0;
  // If this is a constant pool for the module, emit module-specific abbrevs.
  if (isGlobal) {
    // Abbrev for CST_CODE_AGGREGATE.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_AGGREGATE));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, Log2_32_Ceil(LastVal+1)));
    AggregateAbbrev = Stream.EmitAbbrev(std::move(Abbv));

    // Abbrev for CST_CODE_STRING.
    Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_STRING));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));
    String8Abbrev = Stream.EmitAbbrev(std::move(Abbv));
    // Abbrev for CST_CODE_CSTRING.
    Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_CSTRING));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 7));
    CString7Abbrev = Stream.EmitAbbrev(std::move(Abbv));
    // Abbrev for CST_CODE_CSTRING.
    Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_CSTRING));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Char6));
    CString6Abbrev = Stream.EmitAbbrev(std::move(Abbv));
  }

  SmallVector<uint64_t, 64> Record;

  const ValueEnumerator32::ValueList &Vals = VE.getValues();
  Type *LastTy = nullptr;
  for (unsigned i = FirstVal; i != LastVal; ++i) {
    const Value *V = Vals[i].first;
    // If we need to switch types, do so now.
    if (V->getType() != LastTy) {
      LastTy = V->getType();
      Record.push_back(VE.getTypeID(LastTy));
      Stream.EmitRecord(bitc::CST_CODE_SETTYPE, Record,
                        CONSTANTS_SETTYPE_ABBREV);
      Record.clear();
    }

    if (const InlineAsm *IA = dyn_cast<InlineAsm>(V)) {
      Record.push_back(unsigned(IA->hasSideEffects()) |
                       unsigned(IA->isAlignStack()) << 1 |
                       unsigned(IA->getDialect()&1) << 2);

      // Add the asm string.
      const std::string &AsmStr = IA->getAsmString();
      Record.push_back(AsmStr.size());
      Record.append(AsmStr.begin(), AsmStr.end());

      // Add the constraint string.
      const std::string &ConstraintStr = IA->getConstraintString();
      Record.push_back(ConstraintStr.size());
      Record.append(ConstraintStr.begin(), ConstraintStr.end());
      Stream.EmitRecord(bitc::CST_CODE_INLINEASM_OLD2, Record);
      Record.clear();
      continue;
    }
    const Constant *C = cast<Constant>(V);
    unsigned Code = -1U;
    unsigned AbbrevToUse = 0;
    if (C->isNullValue()) {
      Code = bitc::CST_CODE_NULL;
    } else if (isa<UndefValue>(C)) {
      Code = bitc::CST_CODE_UNDEF;
    } else if (const ConstantInt *IV = dyn_cast<ConstantInt>(C)) {
      EmitAPInt(Record, Code, AbbrevToUse, IV->getValue());
    } else if (const ConstantFP *CFP = dyn_cast<ConstantFP>(C)) {
      Code = bitc::CST_CODE_FLOAT;
      Type *Ty = CFP->getType();
      if (Ty->isHalfTy() || Ty->isFloatTy() || Ty->isDoubleTy()) {
        Record.push_back(CFP->getValueAPF().bitcastToAPInt().getZExtValue());
      } else if (Ty->isX86_FP80Ty()) {
        // api needed to prevent premature destruction
        // bits are not in the same order as a normal i80 APInt, compensate.
        APInt api = CFP->getValueAPF().bitcastToAPInt();
        const uint64_t *p = api.getRawData();
        Record.push_back((p[1] << 48) | (p[0] >> 16));
        Record.push_back(p[0] & 0xffffLL);
      } else if (Ty->isFP128Ty() || Ty->isPPC_FP128Ty()) {
        APInt api = CFP->getValueAPF().bitcastToAPInt();
        const uint64_t *p = api.getRawData();
        Record.push_back(p[0]);
        Record.push_back(p[1]);
      } else {
        assert (0 && "Unknown FP type!");
      }
    } else if (isa<ConstantDataSequential>(C) &&
               cast<ConstantDataSequential>(C)->isString()) {
      const ConstantDataSequential *Str = cast<ConstantDataSequential>(C);
      // Emit constant strings specially.
      unsigned NumElts = Str->getNumElements();
      // If this is a null-terminated string, use the denser CSTRING encoding.
      if (Str->isCString()) {
        Code = bitc::CST_CODE_CSTRING;
        --NumElts;  // Don't encode the null, which isn't allowed by char6.
      } else {
        Code = bitc::CST_CODE_STRING;
        AbbrevToUse = String8Abbrev;
      }
      bool isCStr7 = Code == bitc::CST_CODE_CSTRING;
      bool isCStrChar6 = Code == bitc::CST_CODE_CSTRING;
      for (unsigned i = 0; i != NumElts; ++i) {
        unsigned char V = Str->getElementAsInteger(i);
        Record.push_back(V);
        isCStr7 &= (V & 128) == 0;
        if (isCStrChar6)
          isCStrChar6 = BitCodeAbbrevOp::isChar6(V);
      }

      if (isCStrChar6)
        AbbrevToUse = CString6Abbrev;
      else if (isCStr7)
        AbbrevToUse = CString7Abbrev;
    } else if (const ConstantDataSequential *CDS =
                  dyn_cast<ConstantDataSequential>(C)) {
      Code = bitc::CST_CODE_DATA;
      Type *EltTy = CDS->getElementType();
      if (isa<IntegerType>(EltTy)) {
        for (unsigned i = 0, e = CDS->getNumElements(); i != e; ++i)
          Record.push_back(CDS->getElementAsInteger(i));
      } else if (EltTy->isFloatTy()) {
        for (unsigned i = 0, e = CDS->getNumElements(); i != e; ++i) {
          union { float F; uint32_t I; };
          F = CDS->getElementAsFloat(i);
          Record.push_back(I);
        }
      } else {
        assert(EltTy->isDoubleTy() && "Unknown ConstantData element type");
        for (unsigned i = 0, e = CDS->getNumElements(); i != e; ++i) {
          union { double F; uint64_t I; };
          F = CDS->getElementAsDouble(i);
          Record.push_back(I);
        }
      }
    } else if (isa<ConstantArray>(C) || isa<ConstantStruct>(C) ||
               isa<ConstantVector>(C)) {
      Code = bitc::CST_CODE_AGGREGATE;
      for (const Value *Op : C->operands())
        Record.push_back(VE.getValueID(Op));
      AbbrevToUse = AggregateAbbrev;
    } else if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
      switch (CE->getOpcode()) {
      default:
        if (Instruction::isCast(CE->getOpcode())) {
          Code = bitc::CST_CODE_CE_CAST;
          Record.push_back(GetEncodedCastOpcode(CE->getOpcode()));
          Record.push_back(VE.getTypeID(C->getOperand(0)->getType()));
          Record.push_back(VE.getValueID(C->getOperand(0)));
          AbbrevToUse = CONSTANTS_CE_CAST_Abbrev;
        } else {
          assert(CE->getNumOperands() == 2 && "Unknown constant expr!");
          Code = bitc::CST_CODE_CE_BINOP;
          Record.push_back(GetEncodedBinaryOpcode(CE->getOpcode()));
          Record.push_back(VE.getValueID(C->getOperand(0)));
          Record.push_back(VE.getValueID(C->getOperand(1)));
          uint64_t Flags = GetOptimizationFlags(CE);
          if (Flags != 0)
            Record.push_back(Flags);
        }
        break;
      case Instruction::GetElementPtr: {
        Code = bitc::CST_CODE_CE_GEP;
        const auto *GO = cast<GEPOperator>(C);
        if (GO->isInBounds())
          Code = bitc::CST_CODE_CE_INBOUNDS_GEP;
        for (unsigned i = 0, e = CE->getNumOperands(); i != e; ++i) {
          Record.push_back(VE.getTypeID(C->getOperand(i)->getType()));
          Record.push_back(VE.getValueID(C->getOperand(i)));
        }
        break;
      }
      case Instruction::Select:
        Code = bitc::CST_CODE_CE_SELECT;
        Record.push_back(VE.getValueID(C->getOperand(0)));
        Record.push_back(VE.getValueID(C->getOperand(1)));
        Record.push_back(VE.getValueID(C->getOperand(2)));
        break;
      case Instruction::ExtractElement:
        Code = bitc::CST_CODE_CE_EXTRACTELT;
        Record.push_back(VE.getTypeID(C->getOperand(0)->getType()));
        Record.push_back(VE.getValueID(C->getOperand(0)));
        Record.push_back(VE.getValueID(C->getOperand(1)));
        break;
      case Instruction::InsertElement:
        Code = bitc::CST_CODE_CE_INSERTELT;
        Record.push_back(VE.getValueID(C->getOperand(0)));
        Record.push_back(VE.getValueID(C->getOperand(1)));
        Record.push_back(VE.getValueID(C->getOperand(2)));
        break;
      case Instruction::ShuffleVector:
        // If the return type and argument types are the same, this is a
        // standard shufflevector instruction.  If the types are different,
        // then the shuffle is widening or truncating the input vectors, and
        // the argument type must also be encoded.
        if (C->getType() == C->getOperand(0)->getType()) {
          Code = bitc::CST_CODE_CE_SHUFFLEVEC;
        } else {
          Code = bitc::CST_CODE_CE_SHUFVEC_EX;
          Record.push_back(VE.getTypeID(C->getOperand(0)->getType()));
        }
        Record.push_back(VE.getValueID(C->getOperand(0)));
        Record.push_back(VE.getValueID(C->getOperand(1)));
        Record.push_back(VE.getValueID(C->getOperand(2)));
        break;
      case Instruction::ICmp:
      case Instruction::FCmp:
        Code = bitc::CST_CODE_CE_CMP;
        Record.push_back(VE.getTypeID(C->getOperand(0)->getType()));
        Record.push_back(VE.getValueID(C->getOperand(0)));
        Record.push_back(VE.getValueID(C->getOperand(1)));
        Record.push_back(CE->getPredicate());
        break;
      }
    } else if (const BlockAddress *BA = dyn_cast<BlockAddress>(C)) {
      Code = bitc::CST_CODE_BLOCKADDRESS;
      Record.push_back(VE.getTypeID(BA->getFunction()->getType()));
      Record.push_back(VE.getValueID(BA->getFunction()));
      Record.push_back(VE.getGlobalBasicBlockID(BA->getBasicBlock()));
    } else {
#ifndef NDEBUG
      C->dump();
#endif
      llvm_unreachable("Unknown constant!");
    }
    Stream.EmitRecord(Code, Record, AbbrevToUse);
    Record.clear();
  }

  Stream.ExitBlock();
}

static void WriteModuleConstants(const ValueEnumerator32 &VE,
                                 BitstreamWriter &Stream) {
  const ValueEnumerator32::ValueList &Vals = VE.getValues();

  // Find the first constant to emit, which is the first non-globalvalue value.
  // We know globalvalues have been emitted by WriteModuleInfo.
  for (unsigned i = 0, e = Vals.size(); i != e; ++i) {
    if (!isa<GlobalValue>(Vals[i].first)) {
      WriteConstants(i, Vals.size(), VE, Stream, true);
      return;
    }
  }
}

/// PushValueAndType - The file has to encode both the value and type id for
/// many values, because we need to know what type to create for forward
/// references.  However, most operands are not forward references, so this type
/// field is not needed.
///
/// This function adds V's value ID to Vals.  If the value ID is higher than the
/// instruction ID, then it is a forward reference, and it also includes the
/// type ID.  The value ID that is written is encoded relative to the InstID.
static bool PushValueAndType(const Value *V, unsigned InstID,
                             SmallVectorImpl<unsigned> &Vals,
                             ValueEnumerator32 &VE) {
  unsigned ValID = VE.getValueID(V);
  // Make encoding relative to the InstID.
  Vals.push_back(InstID - ValID);
  if (ValID >= InstID) {
    Vals.push_back(VE.getTypeID(V->getType()));
    return true;
  }
  return false;
}

/// pushValue - Like PushValueAndType, but where the type of the value is
/// omitted (perhaps it was already encoded in an earlier operand).
static void pushValue(const Value *V, unsigned InstID,
                      SmallVectorImpl<unsigned> &Vals,
                      ValueEnumerator32 &VE) {
  unsigned ValID = VE.getValueID(V);
  Vals.push_back(InstID - ValID);
}

static void pushValue64(const Value *V, unsigned InstID,
                        SmallVector<uint64_t, 128> &Vals,
                        ValueEnumerator32 &VE) {
  uint64_t ValID = VE.getValueID(V);
  Vals.push_back(InstID - ValID);
}

static void pushValueSigned(const Value *V, unsigned InstID,
                            SmallVectorImpl<uint64_t> &Vals,
                            ValueEnumerator32 &VE) {
  unsigned ValID = VE.getValueID(V);
  int64_t diff = ((int32_t)InstID - (int32_t)ValID);
  emitSignedInt64(Vals, diff);
}

/// WriteInstruction - Emit an instruction to the specified stream.
static void WriteInstruction(const Instruction &I, unsigned InstID,
                             ValueEnumerator32 &VE, BitstreamWriter &Stream,
                             SmallVectorImpl<unsigned> &Vals) {
  unsigned Code = 0;
  unsigned AbbrevToUse = 0;
  VE.setInstructionID(&I);
  switch (I.getOpcode()) {
  default:
    if (Instruction::isCast(I.getOpcode())) {
      Code = bitc::FUNC_CODE_INST_CAST;
      if (!PushValueAndType(I.getOperand(0), InstID, Vals, VE))
        AbbrevToUse = FUNCTION_INST_CAST_ABBREV;
      Vals.push_back(VE.getTypeID(I.getType()));
      Vals.push_back(GetEncodedCastOpcode(I.getOpcode()));
    } else {
      assert(isa<BinaryOperator>(I) && "Unknown instruction!");
      Code = bitc::FUNC_CODE_INST_BINOP;
      if (!PushValueAndType(I.getOperand(0), InstID, Vals, VE))
        AbbrevToUse = FUNCTION_INST_BINOP_ABBREV;
      pushValue(I.getOperand(1), InstID, Vals, VE);
      Vals.push_back(GetEncodedBinaryOpcode(I.getOpcode()));
      uint64_t Flags = GetOptimizationFlags(&I);
      if (Flags != 0) {
        if (AbbrevToUse == FUNCTION_INST_BINOP_ABBREV)
          AbbrevToUse = FUNCTION_INST_BINOP_FLAGS_ABBREV;
        Vals.push_back(Flags);
      }
    }
    break;

  case Instruction::FNeg: {
    // emit as "fsub -0, value"
    Code = bitc::FUNC_CODE_INST_BINOP;
    pushValue(ConstantFP::get(I.getOperand(0)->getType(), -0.0), InstID, Vals, VE);
    if (!PushValueAndType(I.getOperand(0), InstID, Vals, VE))
      AbbrevToUse = FUNCTION_INST_BINOP_ABBREV;
    Vals.push_back(GetEncodedBinaryOpcode(Instruction::FSub));
    uint64_t Flags = GetOptimizationFlags(&I);
    if (Flags != 0) {
      if (AbbrevToUse == FUNCTION_INST_BINOP_ABBREV)
        AbbrevToUse = FUNCTION_INST_BINOP_FLAGS_ABBREV;
      Vals.push_back(Flags);
    }
    break;
  }
  case Instruction::GetElementPtr: {
    Code = bitc::FUNC_CODE_INST_GEP_OLD;
    //AbbrevToUse = FUNCTION_INST_GEP_ABBREV;
    auto &GEPInst = cast<GetElementPtrInst>(I);
    if (GEPInst.isInBounds())
      Code = bitc::FUNC_CODE_INST_INBOUNDS_GEP_OLD;
    for (unsigned i = 0, e = I.getNumOperands(); i != e; ++i)
      PushValueAndType(I.getOperand(i), InstID, Vals, VE);
    break;
  }
  case Instruction::ExtractValue: {
    Code = bitc::FUNC_CODE_INST_EXTRACTVAL;
    PushValueAndType(I.getOperand(0), InstID, Vals, VE);
    const ExtractValueInst *EVI = cast<ExtractValueInst>(&I);
    Vals.append(EVI->idx_begin(), EVI->idx_end());
    break;
  }
  case Instruction::InsertValue: {
    Code = bitc::FUNC_CODE_INST_INSERTVAL;
    PushValueAndType(I.getOperand(0), InstID, Vals, VE);
    PushValueAndType(I.getOperand(1), InstID, Vals, VE);
    const InsertValueInst *IVI = cast<InsertValueInst>(&I);
    Vals.append(IVI->idx_begin(), IVI->idx_end());
    break;
  }
  case Instruction::Select:
    Code = bitc::FUNC_CODE_INST_VSELECT;
    PushValueAndType(I.getOperand(1), InstID, Vals, VE);
    pushValue(I.getOperand(2), InstID, Vals, VE);
    PushValueAndType(I.getOperand(0), InstID, Vals, VE);
    break;
  case Instruction::ExtractElement:
    Code = bitc::FUNC_CODE_INST_EXTRACTELT;
    PushValueAndType(I.getOperand(0), InstID, Vals, VE);
    pushValue(I.getOperand(1), InstID, Vals, VE);
    break;
  case Instruction::InsertElement:
    Code = bitc::FUNC_CODE_INST_INSERTELT;
    PushValueAndType(I.getOperand(0), InstID, Vals, VE);
    pushValue(I.getOperand(1), InstID, Vals, VE);
    pushValue(I.getOperand(2), InstID, Vals, VE);
    break;
  case Instruction::ShuffleVector:
    Code = bitc::FUNC_CODE_INST_SHUFFLEVEC;
    PushValueAndType(I.getOperand(0), InstID, Vals, VE);
    pushValue(I.getOperand(1), InstID, Vals, VE);
    //pushValue(I.getOperand(2), InstID, Vals, VE);
    pushValue(cast<ShuffleVectorInst>(I).getShuffleMaskForBitcode(), InstID,
              Vals, VE);
    break;
  case Instruction::ICmp:
  case Instruction::FCmp: {
    // compare returning Int1Ty or vector of Int1Ty
    Code = bitc::FUNC_CODE_INST_CMP2;
    PushValueAndType(I.getOperand(0), InstID, Vals, VE);
    pushValue(I.getOperand(1), InstID, Vals, VE);
    Vals.push_back(cast<CmpInst>(I).getPredicate());
    break;
  }

  case Instruction::Ret:
    {
      Code = bitc::FUNC_CODE_INST_RET;
      unsigned NumOperands = I.getNumOperands();
      if (NumOperands == 0)
        AbbrevToUse = FUNCTION_INST_RET_VOID_ABBREV;
      else if (NumOperands == 1) {
        if (!PushValueAndType(I.getOperand(0), InstID, Vals, VE))
          AbbrevToUse = FUNCTION_INST_RET_VAL_ABBREV;
      } else {
        for (unsigned i = 0, e = NumOperands; i != e; ++i)
          PushValueAndType(I.getOperand(i), InstID, Vals, VE);
      }
    }
    break;
  case Instruction::Br:
    {
      Code = bitc::FUNC_CODE_INST_BR;
      const BranchInst &II = cast<BranchInst>(I);
      Vals.push_back(VE.getValueID(II.getSuccessor(0)));
      if (II.isConditional()) {
        Vals.push_back(VE.getValueID(II.getSuccessor(1)));
        pushValue(II.getCondition(), InstID, Vals, VE);
      }
    }
    break;
  case Instruction::Switch:
    {
      // Redefine Vals, since here we need to use 64 bit values
      // explicitly to store large APInt numbers.
      SmallVector<uint64_t, 128> Vals64;

      Code = bitc::FUNC_CODE_INST_SWITCH;
      const SwitchInst &SI = cast<SwitchInst>(I);

      // Compute hash (LLVM 3.2 SwitchInst::Hash)
      uint32_t NumberOfCases = (uint32_t)SI.getNumCases();
      uint16_t Hash = (0xFFFF & NumberOfCases) ^ (NumberOfCases >> 16);
      for (SwitchInst::ConstCaseIt i = SI.case_begin(), e = SI.case_end();
           i != e; ++i) {
        uint32_t NumItems = 1;
        Hash = (Hash << 1) ^ (0xFFFF & NumItems) ^ (NumItems >> 16);
      }

      uint32_t SwitchRecordHeader = Hash | (SWITCH_INST_MAGIC << 16);
      Vals64.push_back(SwitchRecordHeader);

      Vals64.push_back(VE.getTypeID(SI.getCondition()->getType()));
      pushValue64(SI.getCondition(), InstID, Vals64, VE);
      Vals64.push_back(VE.getValueID(SI.getDefaultDest()));
      Vals64.push_back(SI.getNumCases());
      for (auto Case : SI.cases()) {
        unsigned Code, Abbrev; // will unused.
        Vals64.push_back(1/*NumItems = 1*/);
        Vals64.push_back(true/*IsSingleNumber = true*/);
        EmitAPInt(Vals64, Code, Abbrev, Case.getCaseValue()->getValue(), true);
        Vals64.push_back(VE.getValueID(Case.getCaseSuccessor()));
      }

      Stream.EmitRecord(Code, Vals64, AbbrevToUse);

      // Also do expected action - clear external Vals collection:
      Vals.clear();
      return;
    }
    break;
  case Instruction::IndirectBr:
    Code = bitc::FUNC_CODE_INST_INDIRECTBR;
    Vals.push_back(VE.getTypeID(I.getOperand(0)->getType()));
    // Encode the address operand as relative, but not the basic blocks.
    pushValue(I.getOperand(0), InstID, Vals, VE);
    for (unsigned i = 1, e = I.getNumOperands(); i != e; ++i)
      Vals.push_back(VE.getValueID(I.getOperand(i)));
    break;

  case Instruction::Invoke: {
    const InvokeInst *II = cast<InvokeInst>(&I);
    const Value *Callee = II->getCalledOperand();
    FunctionType *FTy = II->getFunctionType();

    Code = bitc::FUNC_CODE_INST_INVOKE;

    Vals.push_back(VE.getAttributeListID(II->getAttributes()));
    Vals.push_back(II->getCallingConv());
    Vals.push_back(VE.getValueID(II->getNormalDest()));
    Vals.push_back(VE.getValueID(II->getUnwindDest()));
    PushValueAndType(Callee, InstID, Vals, VE);

    // Emit value #'s for the fixed parameters.
    for (unsigned i = 0, e = FTy->getNumParams(); i != e; ++i)
      pushValue(I.getOperand(i), InstID, Vals, VE);  // fixed param.

    // Emit type/value pairs for varargs params.
    if (FTy->isVarArg()) {
      for (unsigned i = FTy->getNumParams(), e = I.getNumOperands()-3;
           i != e; ++i)
        PushValueAndType(I.getOperand(i), InstID, Vals, VE); // vararg
    }
    break;
  }
  case Instruction::Resume:
    Code = bitc::FUNC_CODE_INST_RESUME;
    PushValueAndType(I.getOperand(0), InstID, Vals, VE);
    break;
  case Instruction::CleanupRet:
  case Instruction::CatchRet:
  case Instruction::CleanupPad:
  case Instruction::CatchPad:
  case Instruction::CatchSwitch:
    // all unsupported in 3.2
    assert(false && "encountered unsupported instruction (these need to be filtered out before writing 3.2 bitcode)");
    return;
  case Instruction::Unreachable:
    Code = bitc::FUNC_CODE_INST_UNREACHABLE;
    AbbrevToUse = FUNCTION_INST_UNREACHABLE_ABBREV;
    break;

  case Instruction::PHI: {
    const PHINode &PN = cast<PHINode>(I);
    Code = bitc::FUNC_CODE_INST_PHI;
    // With the newer instruction encoding, forward references could give
    // negative valued IDs.  This is most common for PHIs, so we use
    // signed VBRs.
    SmallVector<uint64_t, 128> Vals64;
    Vals64.push_back(VE.getTypeID(PN.getType()));
    for (unsigned i = 0, e = PN.getNumIncomingValues(); i != e; ++i) {
      pushValueSigned(PN.getIncomingValue(i), InstID, Vals64, VE);
      Vals64.push_back(VE.getValueID(PN.getIncomingBlock(i)));
    }
    // Emit a Vals64 vector and exit.
    Stream.EmitRecord(Code, Vals64, AbbrevToUse);
    Vals64.clear();
    return;
  }

  case Instruction::LandingPad: {
    const LandingPadInst &LP = cast<LandingPadInst>(I);
    Code = bitc::FUNC_CODE_INST_LANDINGPAD_OLD;
    Vals.push_back(VE.getTypeID(LP.getType()));
    PushValueAndType(LP.getFunction()->getPersonalityFn(), InstID, Vals, VE);
    Vals.push_back(LP.isCleanup());
    Vals.push_back(LP.getNumClauses());
    for (unsigned I = 0, E = LP.getNumClauses(); I != E; ++I) {
      if (LP.isCatch(I))
        Vals.push_back(LandingPadInst::Catch);
      else
        Vals.push_back(LandingPadInst::Filter);
      PushValueAndType(LP.getClause(I), InstID, Vals, VE);
    }
    break;
  }

  case Instruction::Alloca: {
    Code = bitc::FUNC_CODE_INST_ALLOCA;
    const AllocaInst &AI = cast<AllocaInst>(I);
    Vals.push_back(VE.getTypeID(I.getType()));
    Vals.push_back(VE.getTypeID(I.getOperand(0)->getType()));
    Vals.push_back(VE.getValueID(I.getOperand(0))); // size.
    unsigned AlignRecord = Log2_32(AI.getAlignment()) + 1;
    assert(Log2_32(AI.getAlignment()) + 1 < 1 << 5 &&
           "not enough bits for maximum alignment");
    assert(AlignRecord < 1 << 5 && "alignment greater than 1 << 64");
    Vals.push_back(AlignRecord);
    break;
  }

  case Instruction::Load:
    if (cast<LoadInst>(I).isAtomic()) {
      Code = bitc::FUNC_CODE_INST_LOADATOMIC;
      PushValueAndType(I.getOperand(0), InstID, Vals, VE);
    } else {
      Code = bitc::FUNC_CODE_INST_LOAD;
      if (!PushValueAndType(I.getOperand(0), InstID, Vals, VE))  // ptr
        AbbrevToUse = FUNCTION_INST_LOAD_ABBREV;
    }
    Vals.push_back(Log2_32(cast<LoadInst>(I).getAlignment())+1);
    Vals.push_back(cast<LoadInst>(I).isVolatile());
    if (cast<LoadInst>(I).isAtomic()) {
      Vals.push_back(GetEncodedOrdering(cast<LoadInst>(I).getOrdering()));
      Vals.push_back(GetEncodedSynchScope(cast<LoadInst>(I).getSyncScopeID()));
    }
    break;
  case Instruction::Store:
    if (cast<StoreInst>(I).isAtomic())
      Code = bitc::FUNC_CODE_INST_STOREATOMIC_OLD;
    else
      Code = bitc::FUNC_CODE_INST_STORE_OLD;
    PushValueAndType(I.getOperand(1), InstID, Vals, VE);  // ptrty + ptr
    pushValue(I.getOperand(0), InstID, Vals, VE);         // val.
    Vals.push_back(Log2_32(cast<StoreInst>(I).getAlignment())+1);
    Vals.push_back(cast<StoreInst>(I).isVolatile());
    if (cast<StoreInst>(I).isAtomic()) {
      Vals.push_back(GetEncodedOrdering(cast<StoreInst>(I).getOrdering()));
      Vals.push_back(GetEncodedSynchScope(cast<StoreInst>(I).getSyncScopeID()));
    }
    break;
  case Instruction::AtomicCmpXchg:
    Code = bitc::FUNC_CODE_INST_CMPXCHG_OLD;
    PushValueAndType(I.getOperand(0), InstID, Vals, VE);  // ptrty + ptr
    pushValue(I.getOperand(1), InstID, Vals, VE);         // cmp.
    pushValue(I.getOperand(2), InstID, Vals, VE);         // newval.
    Vals.push_back(cast<AtomicCmpXchgInst>(I).isVolatile());
    Vals.push_back(GetEncodedOrdering(
                     cast<AtomicCmpXchgInst>(I).getSuccessOrdering()));
    Vals.push_back(GetEncodedSynchScope(
                     cast<AtomicCmpXchgInst>(I).getSyncScopeID()));
    break;
  case Instruction::AtomicRMW:
    Code = bitc::FUNC_CODE_INST_ATOMICRMW_OLD;
    PushValueAndType(I.getOperand(0), InstID, Vals, VE);  // ptrty + ptr
    pushValue(I.getOperand(1), InstID, Vals, VE);         // val.
    Vals.push_back(GetEncodedRMWOperation(
                     cast<AtomicRMWInst>(I).getOperation()));
    Vals.push_back(cast<AtomicRMWInst>(I).isVolatile());
    Vals.push_back(GetEncodedOrdering(cast<AtomicRMWInst>(I).getOrdering()));
    Vals.push_back(GetEncodedSynchScope(
                     cast<AtomicRMWInst>(I).getSyncScopeID()));
    break;
  case Instruction::Fence:
    Code = bitc::FUNC_CODE_INST_FENCE;
    Vals.push_back(GetEncodedOrdering(cast<FenceInst>(I).getOrdering()));
    Vals.push_back(GetEncodedSynchScope(cast<FenceInst>(I).getSyncScopeID()));
    break;
  case Instruction::Call: {
    const CallInst &CI = cast<CallInst>(I);
    FunctionType *FTy = CI.getFunctionType();

    Code = bitc::FUNC_CODE_INST_CALL;

    Vals.push_back(VE.getAttributeListID(CI.getAttributes()));
    Vals.push_back(CI.getCallingConv() << bitc::CALL_CCONV |
                   unsigned(CI.isTailCall()) << bitc::CALL_TAIL);

    PushValueAndType(CI.getCalledOperand(), InstID, Vals, VE);  // Callee

    // Emit value #'s for the fixed parameters.
    for (unsigned i = 0, e = FTy->getNumParams(); i != e; ++i) {
      // Check for labels (can happen with asm labels).
      if (FTy->getParamType(i)->isLabelTy())
        Vals.push_back(VE.getValueID(CI.getArgOperand(i)));
      else
        pushValue(CI.getArgOperand(i), InstID, Vals, VE);  // fixed param.
    }

    // Emit type/value pairs for varargs params.
    if (FTy->isVarArg()) {
      for (unsigned i = FTy->getNumParams(), e = CI.arg_size(); i != e; ++i)
        PushValueAndType(CI.getArgOperand(i), InstID, Vals, VE);  // varargs
    }
    break;
  }
  case Instruction::VAArg:
    Code = bitc::FUNC_CODE_INST_VAARG;
    Vals.push_back(VE.getTypeID(I.getOperand(0)->getType()));   // valistty
    pushValue(I.getOperand(0), InstID, Vals, VE); // valist.
    Vals.push_back(VE.getTypeID(I.getType())); // restype.
    break;
  case Instruction::Freeze: {
    // freeze instruction is not supported by LLVM 3.2,
    // but we can more or less emulate it as an identity function
    // -> encode as a bitcast to the same type
    auto Operand = I.getOperand(0);
    Code = bitc::FUNC_CODE_INST_CAST;
    if (!PushValueAndType(Operand, InstID, Vals, VE))
      AbbrevToUse = FUNCTION_INST_CAST_ABBREV;
    Vals.push_back(VE.getTypeID(Operand->getType()));
    Vals.push_back(GetEncodedCastOpcode(Instruction::BitCast));
    break;
  }
  case Instruction::CallBr:
    report_fatal_error("can not encode CallBr instruction for LLVM 3.2");
    break;
  }

  Stream.EmitRecord(Code, Vals, AbbrevToUse);
  Vals.clear();
}

enum StringEncoding { SE_Char6, SE_Fixed7, SE_Fixed8 };

/// Determine the encoding to use for the given string name and length.
static StringEncoding getStringEncoding(const char *Str, unsigned StrLen) {
  bool isChar6 = true;
  for (const char *C = Str, *E = C + StrLen; C != E; ++C) {
    if (isChar6)
      isChar6 = BitCodeAbbrevOp::isChar6(*C);
    if ((unsigned char)*C & 128)
      // don't bother scanning the rest.
      return SE_Fixed8;
  }
  if (isChar6)
    return SE_Char6;
  else
    return SE_Fixed7;
}

/// Emit names for globals/functions etc. The VSTOffsetPlaceholder,
/// BitcodeStartBit and FunctionIndex are only passed for the module-level
/// VST, where we are including a function bitcode index and need to
/// backpatch the VST forward declaration record.
static void WriteValueSymbolTable(
    const ValueSymbolTable &VST, const ValueEnumerator32 &VE,
    BitstreamWriter &Stream, uint64_t VSTOffsetPlaceholder = 0,
    uint64_t BitcodeStartBit = 0
// TODO: is this necessary? handle it?
// -> new one: DenseMap<const Function *, uint64_t> *FunctionToBitcodeIndex
    /*DenseMap<const Function *, std::unique_ptr<FunctionInfo>> *FunctionIndex =
        nullptr*/) {
  if (VST.empty()) {
    // WriteValueSymbolTableForwardDecl should have returned early as
    // well. Ensure this handling remains in sync by asserting that
    // the placeholder offset is not set.
    assert(VSTOffsetPlaceholder == 0);
    return;
  }

  Stream.EnterSubblock(bitc::VALUE_SYMTAB_BLOCK_ID, 4);

  // FIXME: Set up the abbrev, we know how many values there are!
  // FIXME: We know if the type names can use 7-bit ascii.
  SmallVector<unsigned, 64> NameVals;

  for (const ValueName &Name : VST) {
    // Figure out the encoding to use for the name.
    StringEncoding Bits =
        getStringEncoding(Name.getKeyData(), Name.getKeyLength());

    unsigned AbbrevToUse = VST_ENTRY_8_ABBREV;
    NameVals.push_back(VE.getValueID(Name.getValue()));

    // VST_ENTRY:   [valueid, namechar x N]
    // VST_BBENTRY: [bbid, namechar x N]
    unsigned Code;
    if (isa<BasicBlock>(Name.getValue())) {
      Code = bitc::VST_CODE_BBENTRY;
      if (Bits == SE_Char6)
        AbbrevToUse = VST_BBENTRY_6_ABBREV;
    } else {
      Code = bitc::VST_CODE_ENTRY;
      if (Bits == SE_Char6)
        AbbrevToUse = VST_ENTRY_6_ABBREV;
      else if (Bits == SE_Fixed7)
        AbbrevToUse = VST_ENTRY_7_ABBREV;
    }

    for (const auto P : Name.getKey())
      NameVals.push_back((unsigned char)P);

    // Emit the finished record.
    Stream.EmitRecord(Code, NameVals, AbbrevToUse);
    NameVals.clear();
  }
  Stream.ExitBlock();
}

/// Emit a function body to the module stream.
static void WriteFunction(
    const Function &F, ValueEnumerator32 &VE, BitstreamWriter &Stream) {
  Stream.EnterSubblock(bitc::FUNCTION_BLOCK_ID, 4);
  VE.incorporateFunction(F);

  SmallVector<unsigned, 64> Vals;

  // Emit the number of basic blocks, so the reader can create them ahead of
  // time.
  Vals.push_back(VE.getBasicBlocks().size());
  Stream.EmitRecord(bitc::FUNC_CODE_DECLAREBLOCKS, Vals);
  Vals.clear();

  // If there are function-local constants, emit them now.
  unsigned CstStart, CstEnd;
  VE.getFunctionConstantRange(CstStart, CstEnd);
  WriteConstants(CstStart, CstEnd, VE, Stream, false);

  // If there is function-local metadata, emit it now.
  WriteFunctionLocalMetadata(F, VE, Stream);

  // Keep a running idea of what the instruction ID is.
  unsigned InstID = CstEnd;

  bool NeedsMetadataAttachment = false;

  DILocation *LastDL = nullptr;

  // Finally, emit all the instructions, in order.
  for (Function::const_iterator BB = F.begin(), E = F.end(); BB != E; ++BB)
    for (BasicBlock::const_iterator I = BB->begin(), E = BB->end();
         I != E; ++I) {
      WriteInstruction(*I, InstID, VE, Stream, Vals);

      if (!I->getType()->isVoidTy())
        ++InstID;

      // If the instruction has metadata, write a metadata attachment later.
      NeedsMetadataAttachment |= I->hasMetadataOtherThanDebugLoc();

      // If the instruction has a debug location, emit it.
      DILocation *DL = I->getDebugLoc();
      if (!DL)
        continue;

      if (DL == LastDL) {
        // Just repeat the same debug loc as last time.
        Stream.EmitRecord(bitc::FUNC_CODE_DEBUG_LOC_AGAIN, Vals);
        continue;
      }

      Vals.push_back(DL->getLine());
      Vals.push_back(DL->getColumn());
      Vals.push_back(VE.getMetadataOrNullID(DL->getScope()));
      Vals.push_back(VE.getMetadataOrNullID(DL->getInlinedAt()));
      Stream.EmitRecord(bitc::FUNC_CODE_DEBUG_LOC, Vals);
      Vals.clear();

      LastDL = DL;
    }

  // Emit names for all the instructions etc.
  if (auto *Symtab = F.getValueSymbolTable())
    WriteValueSymbolTable(*Symtab, VE, Stream);

  if (NeedsMetadataAttachment)
    WriteMetadataAttachment(F, VE, Stream);
  VE.purgeFunction();
  Stream.ExitBlock();
}

// Emit blockinfo, which defines the standard abbreviations etc.
static void WriteBlockInfo(const ValueEnumerator32 &VE, BitstreamWriter &Stream) {
  // We only want to emit block info records for blocks that have multiple
  // instances: CONSTANTS_BLOCK, FUNCTION_BLOCK and VALUE_SYMTAB_BLOCK.
  // Other blocks can define their abbrevs inline.
  Stream.EnterBlockInfoBlock();

  { // 8-bit fixed-width VST_ENTRY/VST_BBENTRY strings.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 3));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 8));
    if (Stream.EmitBlockInfoAbbrev(bitc::VALUE_SYMTAB_BLOCK_ID,
                                   Abbv) != VST_ENTRY_8_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }

  { // 7-bit fixed width VST_ENTRY strings.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::VST_CODE_ENTRY));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 7));
    if (Stream.EmitBlockInfoAbbrev(bitc::VALUE_SYMTAB_BLOCK_ID,
                                   Abbv) != VST_ENTRY_7_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }
  { // 6-bit char6 VST_ENTRY strings.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::VST_CODE_ENTRY));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Char6));
    if (Stream.EmitBlockInfoAbbrev(bitc::VALUE_SYMTAB_BLOCK_ID,
                                   Abbv) != VST_ENTRY_6_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }
  { // 6-bit char6 VST_BBENTRY strings.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::VST_CODE_BBENTRY));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Array));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Char6));
    if (Stream.EmitBlockInfoAbbrev(bitc::VALUE_SYMTAB_BLOCK_ID,
                                   Abbv) != VST_BBENTRY_6_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }



  { // SETTYPE abbrev for CONSTANTS_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_SETTYPE));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,
                              VE.computeBitsRequiredForTypeIndicies()));
    if (Stream.EmitBlockInfoAbbrev(bitc::CONSTANTS_BLOCK_ID,
                                   Abbv) != CONSTANTS_SETTYPE_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }

  { // INTEGER abbrev for CONSTANTS_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_INTEGER));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));
    if (Stream.EmitBlockInfoAbbrev(bitc::CONSTANTS_BLOCK_ID,
                                   Abbv) != CONSTANTS_INTEGER_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }

  { // CE_CAST abbrev for CONSTANTS_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_CE_CAST));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 4));  // cast opc
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,       // typeid
                              VE.computeBitsRequiredForTypeIndicies()));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 8));    // value id

    if (Stream.EmitBlockInfoAbbrev(bitc::CONSTANTS_BLOCK_ID,
                                   Abbv) != CONSTANTS_CE_CAST_Abbrev)
      llvm_unreachable("Unexpected abbrev ordering!");
  }
  { // NULL abbrev for CONSTANTS_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::CST_CODE_NULL));
    if (Stream.EmitBlockInfoAbbrev(bitc::CONSTANTS_BLOCK_ID,
                                   Abbv) != CONSTANTS_NULL_Abbrev)
      llvm_unreachable("Unexpected abbrev ordering!");
  }

  // FIXME: This should only use space for first class types!

  { // INST_LOAD abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_LOAD));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // Ptr
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 4)); // Align
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 1)); // volatile
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID,
                                   Abbv) != FUNCTION_INST_LOAD_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }
  { // INST_BINOP abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_BINOP));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // LHS
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // RHS
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 4)); // opc
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID,
                                   Abbv) != FUNCTION_INST_BINOP_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }
  { // INST_BINOP_FLAGS abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_BINOP));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // LHS
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // RHS
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 4)); // opc
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 7)); // flags
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID,
                                   Abbv) != FUNCTION_INST_BINOP_FLAGS_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }
  { // INST_CAST abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_CAST));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));    // OpVal
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed,       // dest ty
                              VE.computeBitsRequiredForTypeIndicies()));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 4));  // opc
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID,
                                   Abbv) != FUNCTION_INST_CAST_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }

  { // INST_RET abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_RET));
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID,
                                   Abbv) != FUNCTION_INST_RET_VOID_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }
  { // INST_RET abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_RET));
    Abbv->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6)); // ValID
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID,
                                   Abbv) != FUNCTION_INST_RET_VAL_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }
  { // INST_UNREACHABLE abbrev for FUNCTION_BLOCK.
    auto Abbv = std::make_shared<BitCodeAbbrev>();
    Abbv->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_UNREACHABLE));
    if (Stream.EmitBlockInfoAbbrev(bitc::FUNCTION_BLOCK_ID,
                                   Abbv) != FUNCTION_INST_UNREACHABLE_ABBREV)
      llvm_unreachable("Unexpected abbrev ordering!");
  }

  Stream.ExitBlock();
}

/// WriteModule - Emit the specified module to the bitstream.
static void WriteModule(const Module *M, BitstreamWriter &Stream) {
  Stream.EnterSubblock(bitc::MODULE_BLOCK_ID, 3);

  SmallVector<unsigned, 1> Vals;
  unsigned CurVersion = 1;
  Vals.push_back(CurVersion);
  Stream.EmitRecord(bitc::MODULE_CODE_VERSION, Vals);

  // Analyze the module, enumerating globals, functions, etc.
  ValueEnumerator32 VE(*M);

  // Emit blockinfo, which defines the standard abbreviations etc.
  WriteBlockInfo(VE, Stream);

  // Emit information about parameter attributes.
  WriteAttributeTable(VE, Stream);

  // Emit information describing all of the types in the module.
  WriteTypeTable(VE, Stream);

  // Emit top-level description of module, including target triple, inline asm,
  // descriptors for global variables, and function prototype info.
  WriteModuleInfo(M, VE, Stream);

  // Emit constants.
  WriteModuleConstants(VE, Stream);

  // Emit metadata.
  WriteModuleMetadata(M, VE, Stream);

  // Emit metadata.
  WriteModuleMetadataStore(M, Stream);

  // Emit names for globals/functions etc.
  WriteValueSymbolTable(M->getValueSymbolTable(), VE, Stream);

  // Emit function bodies.
  for (Module::const_iterator F = M->begin(), E = M->end(); F != E; ++F)
    if (!F->isDeclaration())
      WriteFunction(*F, VE, Stream);

  Stream.ExitBlock();
}

/// EmitDarwinBCHeader - If generating a bc file on darwin, we have to emit a
/// header and trailer to make it compatible with the system archiver.  To do
/// this we emit the following header, and then emit a trailer that pads the
/// file out to be a multiple of 16 bytes.
///
/// struct bc_header {
///   uint32_t Magic;         // 0x0B17C0DE
///   uint32_t Version;       // Version, currently always 0.
///   uint32_t BitcodeOffset; // Offset to traditional bitcode file.
///   uint32_t BitcodeSize;   // Size of traditional bitcode file.
///   uint32_t CPUType;       // CPU specifier.
///   ... potentially more later ...
/// };
enum {
  DarwinBCSizeFieldOffset = 3*4, // Offset to bitcode_size.
  DarwinBCHeaderSize = 5*4
};

static void WriteInt32ToBuffer(uint32_t Value, SmallVectorImpl<char> &Buffer,
                               uint32_t &Position) {
  support::endian::write32le(&Buffer[Position], Value);
  Position += 4;
}

static void EmitDarwinBCHeaderAndTrailer(SmallVectorImpl<char> &Buffer,
                                         const Triple &TT) {
  unsigned CPUType = ~0U;

  // Match x86_64-*, i[3-9]86-*, powerpc-*, powerpc64-*, arm-*, thumb-*,
  // armv[0-9]-*, thumbv[0-9]-*, armv5te-*, or armv6t2-*. The CPUType is a magic
  // number from /usr/include/mach/machine.h.  It is ok to reproduce the
  // specific constants here because they are implicitly part of the Darwin ABI.
  enum {
    DARWIN_CPU_ARCH_ABI64      = 0x01000000,
    DARWIN_CPU_TYPE_X86        = 7,
    DARWIN_CPU_TYPE_ARM        = 12,
    DARWIN_CPU_TYPE_POWERPC    = 18
  };

  Triple::ArchType Arch = TT.getArch();
  if (Arch == Triple::x86_64)
    CPUType = DARWIN_CPU_TYPE_X86 | DARWIN_CPU_ARCH_ABI64;
  else if (Arch == Triple::x86)
    CPUType = DARWIN_CPU_TYPE_X86;
  else if (Arch == Triple::ppc)
    CPUType = DARWIN_CPU_TYPE_POWERPC;
  else if (Arch == Triple::ppc64)
    CPUType = DARWIN_CPU_TYPE_POWERPC | DARWIN_CPU_ARCH_ABI64;
  else if (Arch == Triple::arm || Arch == Triple::thumb)
    CPUType = DARWIN_CPU_TYPE_ARM;

  // Traditional Bitcode starts after header.
  assert(Buffer.size() >= DarwinBCHeaderSize &&
         "Expected header size to be reserved");
  unsigned BCOffset = DarwinBCHeaderSize;
  unsigned BCSize = Buffer.size()-DarwinBCHeaderSize;

  // Write the magic and version.
  unsigned Position = 0;
  WriteInt32ToBuffer(0x0B17C0DE , Buffer, Position);
  WriteInt32ToBuffer(0          , Buffer, Position); // Version.
  WriteInt32ToBuffer(BCOffset   , Buffer, Position);
  WriteInt32ToBuffer(BCSize     , Buffer, Position);
  WriteInt32ToBuffer(CPUType    , Buffer, Position);

  // If the file is not a multiple of 16 bytes, insert dummy padding.
  while (Buffer.size() & 15)
    Buffer.push_back(0);
}

/// Helper to write the header common to all bitcode files.
static void WriteBitcodeHeader(BitstreamWriter &Stream) {
  // Emit the file header.
  Stream.Emit((unsigned)'B', 8);
  Stream.Emit((unsigned)'C', 8);
  Stream.Emit(0x0, 4);
  Stream.Emit(0xC, 4);
  Stream.Emit(0xE, 4);
  Stream.Emit(0xD, 4);
}

/// WriteBitcodeToFile - Write the specified module to the specified output
/// stream.
void llvm::WriteBitcode32ToFile(const Module *M, raw_ostream &Out) {
  SmallVector<char, 0> Buffer;
  Buffer.reserve(256*1024);

  // swap out DEBUG_METADATA_VERSION if it is present (yes, this is evil)
  // NOTE: doesn't affect IOS_METAL_DEBUG_METADATA_VERSION, this is already correct
  StringRef debug_info_version_str = "Debug Info Version";
  if (auto debug_info_node = M->getModuleFlag(debug_info_version_str)) {
    if (auto debug_info_version = dyn_cast<ConstantAsMetadata>(debug_info_node)) {
      if(auto version_int = dyn_cast<ConstantInt>(debug_info_version->getValue())) {
        if(version_int->getZExtValue() == DEBUG_METADATA_VERSION) {
          // slightly awkward, the agony is real
          NamedMDNode* module_flags = const_cast<Module*>(M)->getOrInsertModuleFlagsMetadata();
          for(unsigned int i = 0, count = module_flags->getNumOperands(); i < count; ++i) {
            auto Flag = module_flags->getOperand(i);
            if (Flag->getNumOperands() >= 3 &&
                dyn_cast_or_null<MDString>(Flag->getOperand(1)) &&
                cast<MDString>(Flag->getOperand(1))->getString() == debug_info_version_str) {
              Metadata *Ops[3] = {
                ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(M->getContext()), llvm::Module::Warning)),
                MDString::get(M->getContext(), debug_info_version_str),
                ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(M->getContext()), DEBUG_METADATA_VERSION_32))
              };
              module_flags->setOperand(i, MDNode::get(M->getContext(), Ops));
              break;
            }
          }
        }
      }
    }
  }

  // If this is darwin or another generic macho target, reserve space for the
  // header.
  Triple TT(M->getTargetTriple());
  if (TT.isOSDarwin())
    Buffer.insert(Buffer.begin(), DarwinBCHeaderSize, 0);

  // Emit the module into the buffer.
  {
    BitstreamWriter Stream(Buffer);

    // Emit the file header.
    WriteBitcodeHeader(Stream);

    // Emit the module.
    WriteModule(M, Stream);
  }

  if (TT.isOSDarwin())
    EmitDarwinBCHeaderAndTrailer(Buffer, TT);

  // Write the generated bitstream to "Out".
  Out.write((char*)&Buffer.front(), Buffer.size());
}
