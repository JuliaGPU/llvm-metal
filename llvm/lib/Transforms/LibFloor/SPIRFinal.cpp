//===- SPIRFinal.cpp - OpenCL/SPIR fixes ----------------------------------===//
//
//  Flo's Open libRary (floor)
//  Copyright (C) 2004 - 2022 Florian Ziesche
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; version 2 of the License only.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License along
//  with this program; if not, write to the Free Software Foundation, Inc.,
//  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
//===----------------------------------------------------------------------===//
//
// This file tries to fix the LLVM IR so that it is SPIR-conformant.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/LibFloor.h"
#include <algorithm>
#include <cstdarg>
#include <memory>
#include <cxxabi.h>
using namespace llvm;

#define DEBUG_TYPE "SPIRFinal"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

namespace {
	// SPIRFinal
	struct SPIRFinal : public FunctionPass, InstVisitor<SPIRFinal> {
		friend class InstVisitor<SPIRFinal>;
		
		static char ID; // Pass identification, replacement for typeid
		
		std::shared_ptr<llvm::IRBuilder<>> builder;
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		Instruction* alloca_insert { nullptr };
		bool was_modified { false };
		
		SPIRFinal() : FunctionPass(ID) {
			initializeSPIRFinalPass(*PassRegistry::getPassRegistry());
		}
		
		bool runOnFunction(Function &F) override {
			// exit if empty function
			if (F.empty()) return false;
			
			//
			M = F.getParent();
			ctx = &M->getContext();
			func = &F;
			builder = std::make_shared<llvm::IRBuilder<>>(*ctx);
			
			// visit everything in this function
			was_modified = false; // reset every time
			DBG(errs() << "in func: "; errs().write_escaped(F.getName()) << '\n';)
			
			// update function signature / param list
			if(F.getCallingConv() == CallingConv::FLOOR_KERNEL) {
				std::vector<Type*> param_types;
				for(auto& arg : F.args()) {
					// add "byval" attribute for all kernel struct parameters
					if(arg.getType()->isPointerTy() &&
					   arg.getType()->getPointerElementType()->isAggregateType() &&
					   arg.getType()->getPointerAddressSpace() == 0 &&
					   !F.getAttributes().hasAttributeAtIndex(arg.getArgNo() + 1, Attribute::ByVal)) {
						arg.addAttr(Attribute::getWithByValType(*ctx, arg.getType()->getPointerElementType()));
						was_modified = true;
					}
					
					param_types.push_back(arg.getType());
				}
				if(was_modified) {
					auto new_func_type = PointerType::get(FunctionType::get(F.getReturnType(), param_types, false), 0);
					F.mutateType(new_func_type);
				}
			}
			
			//
			visit(F);
			if(was_modified) {
				DBG(errs() << "!! modified function: ";)
				DBG(errs().write_escaped(F.getName()) << '\n';)
			}
			return was_modified;
		}
		
		// InstVisitor overrides...
		using InstVisitor<SPIRFinal>::visit;
		void visit(Instruction& I) {
			InstVisitor<SPIRFinal>::visit(I);
		}
		
		// SPIR only supports scalar conversion ops ->
		// * scalarize source vector
		// * call conversion op for each scalar
		// * reassemble a vector from the converted scalars
		// * replace all uses of the original vector
		template <Instruction::CastOps cast_op>
		__attribute__((always_inline))
		void vec_to_scalar_ops(CastInst& I) {
			if (!I.getType()->isVectorTy()) {
				return;
			}
			
			// setup
			auto* src_vec = I.getOperand(0);
			auto src_vec_type = dyn_cast_or_null<FixedVectorType>(src_vec->getType());
			if (!src_vec_type) {
				return;
			}
			const auto dim = src_vec_type->getNumElements();
			const auto si_type = I.getDestTy();
			const auto si_scalar_type = si_type->getScalarType();
			llvm::Value* dst_vec = UndefValue::get(si_type);
			
			// start insertion before instruction
			builder->SetInsertPoint(&I);
			
			// iterate over all vector components, emit a scalar instruction and insert into a new vector
			auto DL = I.getDebugLoc();
			for(uint32_t i = 0; i < dim; ++i) {
				auto scalar = builder->CreateExtractElement(src_vec, builder->getInt32(i));
				dst_vec = builder->CreateInsertElement(dst_vec,
													   builder->CreateCast(cast_op, scalar, si_scalar_type),
													   builder->getInt32(i));
				((ExtractElementInst*)scalar)->setDebugLoc(DL);
				((InsertElementInst*)dst_vec)->setDebugLoc(DL);
			}
			
			// finally, replace all uses with the new vector and remove the old vec instruction
			I.replaceAllUsesWith(dst_vec);
			I.eraseFromParent();
			was_modified = true;
		}
		
		void visitTruncInst(TruncInst &I) {
			vec_to_scalar_ops<Instruction::Trunc>(I);
		}
		void visitZExtInst(ZExtInst &I) {
			vec_to_scalar_ops<Instruction::ZExt>(I);
		}
		void visitSExtInst(SExtInst &I) {
			vec_to_scalar_ops<Instruction::SExt>(I);
		}
		void visitFPTruncInst(FPTruncInst &I) {
			vec_to_scalar_ops<Instruction::FPTrunc>(I);
		}
		void visitFPExtInst(FPExtInst &I) {
			vec_to_scalar_ops<Instruction::FPExt>(I);
		}
		void visitFPToUIInst(FPToUIInst &I) {
			vec_to_scalar_ops<Instruction::FPToUI>(I);
		}
		void visitFPToSIInst(FPToSIInst &I) {
			vec_to_scalar_ops<Instruction::FPToSI>(I);
		}
		void visitUIToFPInst(UIToFPInst &I) {
			vec_to_scalar_ops<Instruction::UIToFP>(I);
		}
		void visitSIToFPInst(SIToFPInst &I) {
			vec_to_scalar_ops<Instruction::SIToFP>(I);
		}
		
		// SPIR doesn't support LLVM lifetime and assume intrinsics
		// -> simply remove them
		// TODO: should probably kill the global decl as well
		void visitIntrinsicInst(IntrinsicInst &I) {
			if (I.getIntrinsicID() == Intrinsic::lifetime_start ||
				I.getIntrinsicID() == Intrinsic::lifetime_end ||
				I.getIntrinsicID() == Intrinsic::assume) {
				I.eraseFromParent();
				was_modified = true;
			}
		}
		
		// "ashr" instructions may not be "exact"
		void visitAShr(BinaryOperator& O) {
			auto* ashr = dyn_cast_or_null<PossiblyExactOperator>(&O);
			if(ashr && ashr->isExact()) {
				// -> replace with a non-exact version
				builder->SetInsertPoint(&O);
				auto new_ashr = builder->CreateAShr(O.getOperand(0), O.getOperand(1));
				((Instruction*)new_ashr)->setDebugLoc(O.getDebugLoc());
				O.replaceAllUsesWith(new_ashr);
				O.eraseFromParent();
				was_modified = true;
			}
		}
		
		// unsupported LLVM IR instructions - fail on these
		void visitIndirectBrInst(IndirectBrInst &I) {
			ctx->emitError(&I, "indirect-br instruction is not supported by SPIR");
		}
		void visitInvokeInst(InvokeInst &I) {
			ctx->emitError(&I, "invoke instruction is not supported by SPIR");
		}
		// NOTE: unwind no longer exists
		void visitResumeInst(ResumeInst &I) {
			ctx->emitError(&I, "resume instruction is not supported by SPIR");
		}
		void visitFenceInst(FenceInst &I) {
			ctx->emitError(&I, "fence instruction is not supported by SPIR");
		}
		void visitAtomicCmpXchgInst(AtomicCmpXchgInst &I) {
			ctx->emitError(&I, "atomic-cmp-xchg instruction is not supported by SPIR - use atomic_* function calls instead!");
		}
		void visitAtomicRMWInst(AtomicRMWInst &I) {
			ctx->emitError(&I, "atomic-rmv instruction is not supported by SPIR - use atomic_* function calls instead!");
		}
		void visitVAArgInst(VAArgInst &I) {
			ctx->emitError(&I, "va-arg instruction is not supported by SPIR");
		}
		void visitLandingPadInst(LandingPadInst &I) {
			ctx->emitError(&I, "landing-pad instruction is not supported by SPIR");
		}
		
		// calls to function pointers are not allowed
		void visitCallInst(CallInst &I) {
			if(I.getCalledFunction() == nullptr) {
				ctx->emitError(&I, "indirect function call / call to function pointer is not supported by SPIR");
			}
		}
		
		// atomic load/store instructions are not allowed
		void visitLoadInst(LoadInst &I) {
			if(I.isAtomic()) {
				ctx->emitError(&I, "atomic-load instruction is not supported by SPIR - use atomic_* function calls instead!");
			}
		}
		void visitStoreInst(StoreInst &I) {
			if(I.isAtomic()) {
				ctx->emitError(&I, "atomic-store instruction is not supported by SPIR - use atomic_* function calls instead!");
			}
		}
		
		// convert illegal integer types in select and switch instructions to legal ones
		// TODO/NOTE: ideally, this should be done for all instructions in the module, but right now this
		//            is the only place where illegal int types get emitted
		void visitSelectInst(SelectInst& SI) {
			if(!SI.getType()->isIntegerTy()) return;
			
			const auto bit_width = SI.getType()->getIntegerBitWidth();
			if(M->getDataLayout().isLegalInteger(bit_width) || bit_width == 1 /* always allow bool */) {
				return;
			}
			
			const bool mutate_true_val = (SI.getTrueValue()->getType()->getIntegerBitWidth() == bit_width);
			const bool mutate_false_val = (SI.getFalseValue()->getType()->getIntegerBitWidth() == bit_width);
			if((mutate_true_val && !isa<ConstantInt>(SI.getTrueValue())) ||
			   (mutate_false_val && !isa<ConstantInt>(SI.getFalseValue()))) {
				ctx->emitError(&SI, "select uses an illegal integer bit width (" + std::to_string(bit_width) + ") " +
							   "and true/false values can not be in-place converted to a legal integer width, " +
							   "because they are not constant values!");
				SI.print(errs());
				errs() << '\n';
				return;
			}
			
			const auto smallest_legal_type = M->getDataLayout().getSmallestLegalIntType(*ctx, bit_width);
			if(mutate_true_val) SI.getTrueValue()->mutateType(smallest_legal_type);
			if(mutate_false_val) SI.getFalseValue()->mutateType(smallest_legal_type);
			SI.mutateType(smallest_legal_type);
		}
		void visitSwitchInst(SwitchInst& SI) {
			if(!SI.getCondition()->getType()->isIntegerTy()) return;
			
			const auto bit_width = SI.getCondition()->getType()->getIntegerBitWidth();
			if(M->getDataLayout().isLegalInteger(bit_width)) {
				return;
			}
			
			const auto smallest_legal_type = M->getDataLayout().getSmallestLegalIntType(*ctx, bit_width);
			SI.getCondition()->mutateType(smallest_legal_type);
		}
		
		void visitGetElementPtrInst(GetElementPtrInst& GEP) {
			// fix internal GEP address space mismatch
			if (auto *PTy = dyn_cast<PointerType>(GEP.getType())) {
				if (GEP.getAddressSpace() != PTy->getAddressSpace()) {
					auto new_ptr_type = PointerType::get(GEP.getType()->getPointerElementType(), GEP.getAddressSpace());
					DBG(errs() << ">> fix GEP/PTy mismatch: " << GEP.getAddressSpace() << " != " << PTy->getAddressSpace() << ": " << *GEP.getType();)
					GEP.mutateType(new_ptr_type);
					DBG(errs() << " -> " << *GEP.getType() << "\n";)
					was_modified = true;
				}
			}
		}
	};
}

char SPIRFinal::ID = 0;
INITIALIZE_PASS_BEGIN(SPIRFinal, "SPIRFinal", "SPIRFinal Pass", false, false)
INITIALIZE_PASS_END(SPIRFinal, "SPIRFinal", "SPIRFinal Pass", false, false)

FunctionPass *llvm::createSPIRFinalPass() {
	return new SPIRFinal();
}
