//===----- CGOpenCLRuntime.cpp - Interface to OpenCL Runtimes -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This provides an abstract class for OpenCL code generation.  Concrete
// subclasses of this implement code generation for specific OpenCL
// runtime libraries.
//
//===----------------------------------------------------------------------===//

#include "CGOpenCLRuntime.h"
#include "CodeGenFunction.h"
#include "TargetInfo.h"
#include "clang/CodeGen/ConstantInitBuilder.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalValue.h"
#include <assert.h>

using namespace clang;
using namespace CodeGen;

CGOpenCLRuntime::~CGOpenCLRuntime() {}

void CGOpenCLRuntime::EmitWorkGroupLocalVarDecl(CodeGenFunction &CGF,
                                                const VarDecl &D) {
  return CGF.EmitStaticVarDecl(D, llvm::GlobalValue::InternalLinkage);
}

llvm::Type *CGOpenCLRuntime::convertOpenCLSpecificType(const Type *T) {
  assert(T->isOpenCLSpecificType() &&
         "Not an OpenCL specific type!");

  llvm::LLVMContext& Ctx = CGM.getLLVMContext();
  uint32_t AddrSpc = CGM.getContext().getTargetAddressSpace(
      CGM.getContext().getOpenCLTypeAddrSpace(T));
  if (!CGM.getLangOpts().Metal) { // OpenCL/SPIR and SPIR-V/Vulkan
    switch (cast<BuiltinType>(T)->getKind()) {
    default:
      llvm_unreachable("Unexpected opencl builtin type!");
      return nullptr;
#define IMAGE_TYPE(ImgType, Id, SingletonId, Access, Suffix) \
    case BuiltinType::Id: \
      return llvm::PointerType::get( \
          llvm::StructType::create(Ctx, "opencl." #ImgType #Suffix "_t"), \
          AddrSpc);
#include "clang/Basic/OpenCLImageTypes.def"
    case BuiltinType::OCLSampler:
      return CGM.getLangOpts().CLSamplerOpaque ? (llvm::Type*)getSamplerType(T) : (llvm::Type*)llvm::IntegerType::get(Ctx, 32);
    case BuiltinType::OCLEvent:
      return llvm::PointerType::get(
          llvm::StructType::create(Ctx, "opencl.event_t"), AddrSpc);
    case BuiltinType::OCLClkEvent:
      return llvm::PointerType::get(
          llvm::StructType::create(Ctx, "opencl.clk_event_t"), AddrSpc);
    case BuiltinType::OCLQueue:
      return llvm::PointerType::get(
          llvm::StructType::create(Ctx, "opencl.queue_t"), AddrSpc);
    case BuiltinType::OCLReserveID:
      return llvm::PointerType::get(
          llvm::StructType::create(Ctx, "opencl.reserve_id_t"), AddrSpc);
    case BuiltinType::OCLPatchControlPoint:
      return llvm::PointerType::get(
          llvm::StructType::create(Ctx, "struct._patch_control_point_t"), AddrSpc);
#define EXT_OPAQUE_TYPE(ExtType, Id, Ext) \
    case BuiltinType::Id: \
      return llvm::PointerType::get( \
          llvm::StructType::create(Ctx, "opencl." #ExtType), AddrSpc);
#include "clang/Basic/OpenCLExtensionTypes.def"
    }
  } else if (CGM.getLangOpts().Metal) { // Metal/AIR
    const auto get_or_create_opaque_ptr_type = [&Ctx](const char* name, uint32_t address_space) {
      auto opaque_type = llvm::StructType::getTypeByName(Ctx, name);
      if (!opaque_type) {
        opaque_type = llvm::StructType::create(Ctx, name);
      }
      return llvm::PointerType::get(opaque_type, address_space);
    };
    switch (cast<BuiltinType>(T)->getKind()) {
      default:
        llvm_unreachable("Unexpected metal builtin type!");
        return nullptr;
      case BuiltinType::OCLImage1d:
        return get_or_create_opaque_ptr_type("struct._texture_1d_t", AddrSpc);
      case BuiltinType::OCLImage1dArray:
                return get_or_create_opaque_ptr_type("struct._texture_1d_array_t", AddrSpc);
      case BuiltinType::OCLImage1dBuffer:
        llvm_unreachable("Unsupported image type (1D-buffer is not supported by metal)!");
        return nullptr;
      case BuiltinType::OCLImage2d:
        return get_or_create_opaque_ptr_type("struct._texture_2d_t", AddrSpc);
      case BuiltinType::OCLImage2dArray:
        return get_or_create_opaque_ptr_type("struct._texture_2d_array_t", AddrSpc);
      case BuiltinType::OCLImage2dDepth:
        return get_or_create_opaque_ptr_type("struct._depth_2d_t", AddrSpc);
      case BuiltinType::OCLImage2dArrayDepth:
        return get_or_create_opaque_ptr_type("struct._depth_2d_array_t", AddrSpc);
      case BuiltinType::OCLImage2dMSAA:
        return get_or_create_opaque_ptr_type("struct._texture_2d_ms_t", AddrSpc);
      case BuiltinType::OCLImage2dArrayMSAA:
        return get_or_create_opaque_ptr_type("struct._texture_2d_ms_array_t", AddrSpc);
      case BuiltinType::OCLImage2dMSAADepth:
        return get_or_create_opaque_ptr_type("struct._depth_2d_ms_t", AddrSpc);
      case BuiltinType::OCLImage2dArrayMSAADepth:
        return get_or_create_opaque_ptr_type("struct._depth_2d_ms_array_t", AddrSpc);
      case BuiltinType::OCLImageCube:
        return get_or_create_opaque_ptr_type("struct._texture_cube_t", AddrSpc);
      case BuiltinType::OCLImageCubeArray:
        return get_or_create_opaque_ptr_type("struct._texture_cube_array_t", AddrSpc);
      case BuiltinType::OCLImageCubeDepth:
        return get_or_create_opaque_ptr_type("struct._depth_cube_t", AddrSpc);
      case BuiltinType::OCLImageCubeArrayDepth:
        return get_or_create_opaque_ptr_type("struct._depth_cube_array_t", AddrSpc);
      case BuiltinType::OCLImage3d:
        return get_or_create_opaque_ptr_type("struct._texture_3d_t", AddrSpc);
      case BuiltinType::OCLSampler:
        return get_or_create_opaque_ptr_type("struct._sampler_t", CGM.getContext().getTargetAddressSpace(LangAS::opencl_constant));
      case BuiltinType::OCLEvent:
        return get_or_create_opaque_ptr_type("struct._event_t", 0);
      case BuiltinType::OCLPatchControlPoint:
        return get_or_create_opaque_ptr_type("struct._patch_control_point_t", AddrSpc);
    }
  }
  llvm_unreachable("Unexpected builtin type!");
  return nullptr;
}

llvm::Type *CGOpenCLRuntime::getPipeType(const PipeType *T) {
  if (T->isReadOnly())
    return getPipeType(T, "opencl.pipe_ro_t", PipeROTy);
  else
    return getPipeType(T, "opencl.pipe_wo_t", PipeWOTy);
}

llvm::Type *CGOpenCLRuntime::getPipeType(const PipeType *T, StringRef Name,
                                         llvm::Type *&PipeTy) {
  if (!PipeTy)
    PipeTy = llvm::PointerType::get(llvm::StructType::create(
      CGM.getLLVMContext(), Name),
      CGM.getContext().getTargetAddressSpace(
          CGM.getContext().getOpenCLTypeAddrSpace(T)));
  return PipeTy;
}

llvm::PointerType *CGOpenCLRuntime::getSamplerType(const Type *T) {
  if (!SamplerTy)
    SamplerTy = llvm::PointerType::get(llvm::StructType::create(
      CGM.getLLVMContext(), "opencl.sampler_t"),
      CGM.getContext().getTargetAddressSpace(
          CGM.getContext().getOpenCLTypeAddrSpace(T)));
  return SamplerTy;
}

llvm::Value *CGOpenCLRuntime::getPipeElemSize(const Expr *PipeArg) {
  const PipeType *PipeTy = PipeArg->getType()->castAs<PipeType>();
  // The type of the last (implicit) argument to be passed.
  llvm::Type *Int32Ty = llvm::IntegerType::getInt32Ty(CGM.getLLVMContext());
  unsigned TypeSize = CGM.getContext()
                          .getTypeSizeInChars(PipeTy->getElementType())
                          .getQuantity();
  return llvm::ConstantInt::get(Int32Ty, TypeSize, false);
}

llvm::Value *CGOpenCLRuntime::getPipeElemAlign(const Expr *PipeArg) {
  const PipeType *PipeTy = PipeArg->getType()->castAs<PipeType>();
  // The type of the last (implicit) argument to be passed.
  llvm::Type *Int32Ty = llvm::IntegerType::getInt32Ty(CGM.getLLVMContext());
  unsigned TypeSize = CGM.getContext()
                          .getTypeAlignInChars(PipeTy->getElementType())
                          .getQuantity();
  return llvm::ConstantInt::get(Int32Ty, TypeSize, false);
}

llvm::PointerType *CGOpenCLRuntime::getGenericVoidPointerType() {
  assert(CGM.getLangOpts().OpenCL);
  return llvm::IntegerType::getInt8PtrTy(
      CGM.getLLVMContext(),
      CGM.getContext().getTargetAddressSpace(LangAS::opencl_generic));
}

// Get the block literal from an expression derived from the block expression.
// OpenCL v2.0 s6.12.5:
// Block variable declarations are implicitly qualified with const. Therefore
// all block variables must be initialized at declaration time and may not be
// reassigned.
static const BlockExpr *getBlockExpr(const Expr *E) {
  const Expr *Prev = nullptr; // to make sure we do not stuck in infinite loop.
  while(!isa<BlockExpr>(E) && E != Prev) {
    Prev = E;
    E = E->IgnoreCasts();
    if (auto DR = dyn_cast<DeclRefExpr>(E)) {
      E = cast<VarDecl>(DR->getDecl())->getInit();
    }
  }
  return cast<BlockExpr>(E);
}

/// Record emitted llvm invoke function and llvm block literal for the
/// corresponding block expression.
void CGOpenCLRuntime::recordBlockInfo(const BlockExpr *E,
                                      llvm::Function *InvokeF,
                                      llvm::Value *Block) {
  assert(EnqueuedBlockMap.find(E) == EnqueuedBlockMap.end() &&
         "Block expression emitted twice");
  assert(isa<llvm::Function>(InvokeF) && "Invalid invoke function");
  assert(Block->getType()->isPointerTy() && "Invalid block literal type");
  EnqueuedBlockMap[E].InvokeFunc = InvokeF;
  EnqueuedBlockMap[E].BlockArg = Block;
  EnqueuedBlockMap[E].Kernel = nullptr;
}

llvm::Function *CGOpenCLRuntime::getInvokeFunction(const Expr *E) {
  return EnqueuedBlockMap[getBlockExpr(E)].InvokeFunc;
}

CGOpenCLRuntime::EnqueuedBlockInfo
CGOpenCLRuntime::emitOpenCLEnqueuedBlock(CodeGenFunction &CGF, const Expr *E) {
  CGF.EmitScalarExpr(E);

  // The block literal may be assigned to a const variable. Chasing down
  // to get the block literal.
  const BlockExpr *Block = getBlockExpr(E);

  assert(EnqueuedBlockMap.find(Block) != EnqueuedBlockMap.end() &&
         "Block expression not emitted");

  // Do not emit the block wrapper again if it has been emitted.
  if (EnqueuedBlockMap[Block].Kernel) {
    return EnqueuedBlockMap[Block];
  }

  auto *F = CGF.getTargetHooks().createEnqueuedBlockKernel(
      CGF, EnqueuedBlockMap[Block].InvokeFunc,
      EnqueuedBlockMap[Block].BlockArg->stripPointerCasts());

  // The common part of the post-processing of the kernel goes here.
  F->addFnAttr(llvm::Attribute::NoUnwind);
  F->setCallingConv(
      CGF.getTypes().ClangCallConvToLLVMCallConv(CallingConv::CC_FloorKernel));
  EnqueuedBlockMap[Block].Kernel = F;
  return EnqueuedBlockMap[Block];
}

//
// Ocl20Mangler
//
Ocl20Mangler::Ocl20Mangler(llvm::SmallVectorImpl<char>& SS): MangledString(&SS) {}

Ocl20Mangler& Ocl20Mangler::appendReservedId() {
  this->appendString("13ocl_reserveid");
  return *this;
}

Ocl20Mangler& Ocl20Mangler::appendPipe() {
  this->appendString("8ocl_pipe");
  return *this;
}

Ocl20Mangler& Ocl20Mangler::appendInt() {
  MangledString->push_back('i');
  return *this;
}

Ocl20Mangler& Ocl20Mangler::appendUint() {
  MangledString->push_back('j');
  return *this;
}

Ocl20Mangler& Ocl20Mangler::appendVoid() {
  MangledString->push_back('v');
  return *this;
}

Ocl20Mangler& Ocl20Mangler::appendPointer() {
  this->appendString("P");
  return *this;
}

Ocl20Mangler& Ocl20Mangler::appendPointer(int addressSpace) {
  assert(addressSpace >=0 && addressSpace <= 4 &&
         "Illegal address space for OpenCL");
  if (!addressSpace)
    return appendPointer();

  this->appendString("PU3AS");
  MangledString->push_back('0' + addressSpace);
  return *this;
}

Ocl20Mangler& Ocl20Mangler::appendString(llvm::StringRef S) {
  MangledString->append(S.begin(), S.end());
  return *this;
}
