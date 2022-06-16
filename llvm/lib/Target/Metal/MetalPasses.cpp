//===- MetalPasses.cpp - Metal-related passes -----------------------------===//
//
//  Flo's Open libRary (floor)
//  Copyright (C) 2004 - 2021 Florian Ziesche
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
// This file fixes certain post-codegen issues.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
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

#include <algorithm>
#include <cstdarg>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <cxxabi.h>

#include "Metal.h"

using namespace llvm;

#define DEBUG_TYPE "MetalFinal"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

namespace {
	// MetalFinal
	struct MetalFinal : public FunctionPass, InstVisitor<MetalFinal> {
		friend class InstVisitor<MetalFinal>;

		static char ID; // Pass identification, replacement for typeid

		std::shared_ptr<llvm::IRBuilder<>> builder;

		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		bool was_modified { false };
		bool is_kernel_func { false };

		MetalFinal() : FunctionPass(ID) {
			initializeMetalFinalPass(*PassRegistry::getPassRegistry());
		}

		void getAnalysisUsage(AnalysisUsage &AU) const override {
			AU.addRequired<AAResultsWrapperPass>();
			AU.addRequired<GlobalsAAWrapperPass>();
			AU.addRequired<AssumptionCacheTracker>();
			AU.addRequired<TargetLibraryInfoWrapperPass>();
		}

		template <Instruction::CastOps cast_op, typename std::enable_if<(cast_op == llvm::Instruction::FPToSI ||
																		 cast_op == llvm::Instruction::FPToUI ||
																		 cast_op == llvm::Instruction::SIToFP ||
																		 cast_op == llvm::Instruction::UIToFP), int>::type = 0>
		llvm::Value* call_conversion_func(llvm::Value* from, llvm::Type* to_type) {
			// metal only supports conversion of a specific set of integer and float types
			// -> find and check them
			const auto from_type = from->getType();
			static const std::unordered_map<llvm::Type*, const char*> type_map {
				{ llvm::Type::getInt1Ty(*ctx), ".i1" }, // not sure about signed/unsigned conversion here
				{ llvm::Type::getInt8Ty(*ctx), ".i8" },
				{ llvm::Type::getInt16Ty(*ctx), ".i16" },
				{ llvm::Type::getInt32Ty(*ctx), ".i32" },
				{ llvm::Type::getInt64Ty(*ctx), ".i64" },
				{ llvm::Type::getHalfTy(*ctx), "f.f16" },
				{ llvm::Type::getFloatTy(*ctx), "f.f32" },
				{ llvm::Type::getDoubleTy(*ctx), "f.f64" },
			};
			const auto from_iter = type_map.find(from_type);
			if(from_iter == end(type_map)) {
				DBG(errs() << "failed to find conversion function for: " << *from_type << " -> " << *to_type << "\n";)
				return from;
			}
			const auto to_iter = type_map.find(to_type);
			if(to_iter == end(type_map)) {
				DBG(errs() << "failed to find conversion function for: " << *from_type << " -> " << *to_type << "\n";)
				return from;
			}

			// figure out if from/to type is signed/unsigned
			bool from_signed = false, to_signed = false;
			switch(cast_op) {
				case llvm::Instruction::FPToSI: from_signed = true; to_signed = true; break;
				case llvm::Instruction::FPToUI: from_signed = true; to_signed = false; break;
				case llvm::Instruction::SIToFP: from_signed = true; to_signed = true; break;
				case llvm::Instruction::UIToFP: from_signed = false; to_signed = true; break;
				default: __builtin_unreachable();
			}

			DBG(errs() << "converting: " << *from_type << " (" << (from_signed ? "signed" : "unsigned") << ") -> " << *to_type << "(" << (to_signed ? "signed" : "unsigned") << ")\n";)

			// air.convert.<to_type>.<from_type>
			std::string func_name = "air.convert.";

			if(to_iter->second[0] == '.') {
				func_name += (to_signed ? 's' : 'u');
			}
			func_name += to_iter->second;

			func_name += '.';
			if(from_iter->second[0] == '.') {
				func_name += (from_signed ? 's' : 'u');
			}
			func_name += from_iter->second;

			SmallVector<llvm::Type*, 1> params(1, from_type);
			const auto func_type = llvm::FunctionType::get(to_type, params, false);
			return builder->CreateCall(M->getOrInsertFunction(func_name, func_type), from);
		}

		// dummy
		template <Instruction::CastOps cast_op, typename std::enable_if<!(cast_op == llvm::Instruction::FPToSI ||
																		  cast_op == llvm::Instruction::FPToUI ||
																		  cast_op == llvm::Instruction::SIToFP ||
																		  cast_op == llvm::Instruction::UIToFP), int>::type = 0>
		llvm::Value* call_conversion_func(llvm::Value* from, llvm::Type*) {
			return from;
		}

		bool runOnFunction(Function &F) override {
			func = &F;
			M = F.getParent();
			ctx = &M->getContext();
			builder = std::make_shared<llvm::IRBuilder<>>(*ctx);

			// exit if empty function
			if(F.empty()) return false;

			// visit everything in this function
			was_modified = false; // reset every time
			DBG(errs() << "in func: "; errs().write_escaped(F.getName()) << '\n';)
			visit(F);

			// always modified
			return was_modified || is_kernel_func;
		}

		// InstVisitor overrides...
		using InstVisitor<MetalFinal>::visit;
		void visit(Instruction& I) {
			// remove fpmath metadata from all instructions
			if (MDNode* MD = I.getMetadata(LLVMContext::MD_fpmath)) {
				I.setMetadata(LLVMContext::MD_fpmath, nullptr);
				was_modified = true;
			}

			InstVisitor<MetalFinal>::visit(I);
		}

		static Optional<std::string> get_suffix_for_type(llvm::Type* type, const bool is_signed) {
			std::string ret = ".";
			switch (type->getTypeID()) {
				case llvm::Type::IntegerTyID:
					ret += (is_signed ? "s." : "u.");
					ret += "i" + std::to_string(cast<IntegerType>(type)->getBitWidth());
					break;
				// NOTE: we generally omit the ".f" here, because it's usually not wanted
				case llvm::Type::HalfTyID:
					ret += "f16";
					break;
				case llvm::Type::FloatTyID:
					ret += "f32";
					break;
				case llvm::Type::DoubleTyID:
					ret += "f64";
					break;
				default:
					return {};
			}
			return ret;
		}

		void visitIntrinsicInst(IntrinsicInst &I) {
			const auto print_instr = [](const Instruction& instr) {
				std::string instr_str;
				llvm::raw_string_ostream instr_stream(instr_str);
				instr.print(instr_stream);
				return instr_str;
			};

			// kill or replace certain llvm.* instrinsic calls
			switch (I.getIntrinsicID()) {
				case Intrinsic::experimental_noalias_scope_decl:
				case Intrinsic::lifetime_start:
				case Intrinsic::lifetime_end:
				case Intrinsic::assume:
					I.eraseFromParent();
					was_modified = true;
					break;

				case Intrinsic::copysign: {
					auto op_arg0 = I.getOperand(0);
					auto op_arg1 = I.getOperand(1);
					builder->SetInsertPoint(&I);

					assert(op_arg0->getType() == op_arg1->getType());
					auto float_typ = op_arg0->getType();

					// get bits
					auto width = float_typ->getPrimitiveSizeInBits();
					auto int_typ = IntegerType::get(*ctx, width);
					auto int_arg0 = builder->CreateBitCast(op_arg0, int_typ);
					auto int_arg1 = builder->CreateBitCast(op_arg1, int_typ);

					// twiddle bits
					auto sign =
						builder->CreateAnd(int_arg1,
					                       int_typ->getMask() & int_typ->getSignBit());
					auto mantissa =
						builder->CreateAnd(int_arg0,
					                       int_typ->getMask() & ~int_typ->getSignBit());
					auto new_value = builder->CreateOr(sign, mantissa);

					I.replaceAllUsesWith(builder->CreateBitCast(new_value, float_typ));
					I.eraseFromParent();
					was_modified = true;
					break;
				}

				// single arguments cases
				case Intrinsic::abs: {
					auto op_val = I.getOperand(0);

					// handled signedness and AIR function name
					bool is_signed = true;
					std::string func_name = "air.";
					switch (I.getIntrinsicID()) {
						case Intrinsic::abs:
							func_name += "abs";
							break;
						default:
							ctx->emitError(&I, "unexpected intrinsic:\n" + print_instr(I));
							return;
					}

					auto suffix = get_suffix_for_type(op_val->getType(), is_signed);
					if (!suffix) {
						ctx->emitError(&I, "unexpected type in intrinsic:\n" + print_instr(I));
						return;
					}
					func_name += *suffix;

					// create the new call
					SmallVector<llvm::Type*, 1> param_types { op_val->getType() };
					const auto func_type = llvm::FunctionType::get(I.getType(), param_types, false);
					builder->SetInsertPoint(&I);

					auto call = builder->CreateCall(M->getOrInsertFunction(func_name, func_type), { op_val });
					call->setDebugLoc(I.getDebugLoc());

					I.replaceAllUsesWith(call);
					I.eraseFromParent();
					was_modified = true;
					break;
				}

				// two arguments cases
				case Intrinsic::umin:
				case Intrinsic::smin:
				case Intrinsic::umax:
				case Intrinsic::smax:
				case Intrinsic::minnum:
				case Intrinsic::maxnum: {
					auto op_lhs = I.getOperand(0);
					auto op_rhs = I.getOperand(1);

					// handled signedness and AIR function name
					bool is_signed = true;
					std::string func_name = "air.";
					switch (I.getIntrinsicID()) {
						case Intrinsic::umin:
							is_signed = false;
							func_name += "min";
							break;
						case Intrinsic::smin:
							func_name += "min";
							break;
						case Intrinsic::umax:
							is_signed = false;
							func_name += "max";
							break;
						case Intrinsic::smax:
							func_name += "max";
							break;
						case Intrinsic::minnum:
							func_name += (op_lhs->getType()->isFloatTy() ? "fast_fmin" : "fmin");
							break;
						case Intrinsic::maxnum:
							func_name += (op_lhs->getType()->isFloatTy() ? "fast_fmax" : "fmax");
							break;
						default:
							ctx->emitError(&I, "unexpected intrinsic:\n" + print_instr(I));
							return;
					}

					auto suffix = get_suffix_for_type(op_lhs->getType(), is_signed);
					if (!suffix) {
						ctx->emitError(&I, "unexpected type in intrinsic:\n" + print_instr(I));
						return;
					}
					func_name += *suffix;

					// create the new call
					SmallVector<llvm::Type*, 2> param_types { op_lhs->getType(), op_rhs->getType() };
					const auto func_type = llvm::FunctionType::get(I.getType(), param_types, false);
					builder->SetInsertPoint(&I);

					auto call = builder->CreateCall(M->getOrInsertFunction(func_name, func_type), { op_lhs, op_rhs });
					call->setDebugLoc(I.getDebugLoc());

					I.replaceAllUsesWith(call);
					I.eraseFromParent();
					was_modified = true;
					break;
				}

#if 0 // TODO: implement these
				case Intrinsic::vector_reduce_fadd: {
					auto init = I.getOperand(0);
					auto vec = I.getOperand(1);
					const auto vec_type = dyn_cast_or_null<FixedVectorType>(vec->getType());
					if (!vec_type) {
						ctx->emitError(&I, "expected vector type in operand #1:\n" + print_instr(I));
						return;
					}
					const auto elem_type = vec_type->getElementType();
					if (!elem_type->isFloatTy()) {
						ctx->emitError(&I, "expected element type of vector to be f32:\n" + print_instr(I));
					}

					const auto width = vec_type->getNumElements();
					if (width != 1 && width != 2 && width != 3 && width != 4 && width != 8 && width != 16) {
						ctx->emitError(&I, "unexpected vector width " + std::to_string(width) + ":\n" + print_instr(I));
						return;
					}

					SmallVector<llvm::Type*, 2> func_arg_types;
					SmallVector<llvm::Value*, 2> func_args;
					func_arg_types.push_back(vec_type);
					func_arg_types.push_back(vec_type);
					func_args.push_back(vec);
					func_args.push_back(ConstantVector::getSplat(ElementCount::getFixed(width), ConstantFP::get(elem_type, 1.0)));

					// -> build get func name
					const std::string get_func_name = "air.dot.v" + std::to_string(width) + "f32";

					AttrBuilder attr_builder;
					attr_builder.addAttribute(llvm::Attribute::NoUnwind);
					attr_builder.addAttribute(llvm::Attribute::ReadOnly);
					auto func_attrs = AttributeList::get(*ctx, ~0, attr_builder);

					// create the air call
					const auto func_type = llvm::FunctionType::get(elem_type, func_arg_types, false);
					builder->SetInsertPoint(&I);
					llvm::CallInst* get_call = builder->CreateCall(M->getOrInsertFunction(get_func_name, func_type, func_attrs), func_args);
					get_call->setDoesNotThrow();
					get_call->setOnlyReadsMemory();
					get_call->setDebugLoc(I.getDebugLoc()); // keep debug loc

					// TODO: handle "init" if not 0

					I.replaceAllUsesWith(get_call);
					I.eraseFromParent();
					was_modified = true;
					break;
				}

				case Intrinsic::vector_reduce_add:
				case Intrinsic::vector_reduce_and:
				case Intrinsic::vector_reduce_fadd:
				case Intrinsic::vector_reduce_fmax:
				case Intrinsic::vector_reduce_fmin:
				case Intrinsic::vector_reduce_fmul:
				case Intrinsic::vector_reduce_mul:
				case Intrinsic::vector_reduce_or:
				case Intrinsic::vector_reduce_smax:
				case Intrinsic::vector_reduce_smin:
				case Intrinsic::vector_reduce_umax:
				case Intrinsic::vector_reduce_umin:
				case Intrinsic::vector_reduce_xor:
					ctx->emitError(&I, "unsupported vector intrinsic:\n" + print_instr(I));
#endif

				// by default, assume that the intrinsics will be supported
				default:
					break;
			}
		}

		// like SPIR, Metal only supports scalar conversion ops ->
		// * scalarize source vector
		// * call conversion op for each scalar
		// * reassemble a vector from the converted scalars
		// * replace all uses of the original vector
		template <Instruction::CastOps cast_op>
		__attribute__((always_inline))
		bool vec_to_scalar_ops(CastInst& I) {
			if(!I.getType()->isVectorTy()) return false;

			// start insertion before instruction
			builder->SetInsertPoint(&I);

			// setup
			auto src_vec = I.getOperand(0);
			const auto src_vec_type = dyn_cast<FixedVectorType>(src_vec->getType());
			if (!src_vec_type) {
				return false;
			}
			const auto dim = src_vec_type->getNumElements();

			const auto si_type = I.getDestTy();
			const auto dst_scalar_type = si_type->getScalarType();
			llvm::Value* dst_vec = UndefValue::get(si_type);

			// iterate over all vector components, emit a scalar instruction and insert into a new vector
			for(uint32_t i = 0; i < dim; ++i) {
				auto scalar = builder->CreateExtractElement(src_vec, builder->getInt32(i));
				llvm::Value* cast;
				switch(cast_op) {
					case llvm::Instruction::FPToSI:
					case llvm::Instruction::FPToUI:
					case llvm::Instruction::SIToFP:
					case llvm::Instruction::UIToFP:
						cast = call_conversion_func<cast_op>(scalar, dst_scalar_type);
						break;
					default:
						cast = builder->CreateCast(cast_op, scalar, dst_scalar_type);
						break;
				}
				dst_vec = builder->CreateInsertElement(dst_vec, cast, builder->getInt32(i));
			}

			// finally, replace all uses with the new vector and remove the old vec instruction
			I.replaceAllUsesWith(dst_vec);
			I.eraseFromParent();
			was_modified = true;
			return true;
		}

		// si/ui/fp -> si/ui/fp conversions require a call to an intrinsic air function (air.convert.*)
		template <Instruction::CastOps cast_op>
		__attribute__((always_inline))
		void scalar_conversion(CastInst& I) {
			builder->SetInsertPoint(&I);

			// replace original conversion
			I.replaceAllUsesWith(call_conversion_func<cast_op>(I.getOperand(0), I.getDestTy()));
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
			if(!vec_to_scalar_ops<Instruction::FPToUI>(I)) {
				scalar_conversion<Instruction::FPToUI>(I);
			}
		}
		void visitFPToSIInst(FPToSIInst &I) {
			if(!vec_to_scalar_ops<Instruction::FPToSI>(I)) {
				scalar_conversion<Instruction::FPToSI>(I);
			}
		}
		void visitUIToFPInst(UIToFPInst &I) {
			if(!vec_to_scalar_ops<Instruction::UIToFP>(I)) {
				scalar_conversion<Instruction::UIToFP>(I);
			}
		}
		void visitSIToFPInst(SIToFPInst &I) {
			if(!vec_to_scalar_ops<Instruction::SIToFP>(I)) {
				scalar_conversion<Instruction::SIToFP>(I);
			}
		}

		// metal can only handle i32 indices
		void visitExtractElement(ExtractElementInst& EEI) {
			const auto idx_op = EEI.getIndexOperand();
			const auto idx_type = idx_op->getType();
			if(!idx_type->isIntegerTy(32)) {
				if(const auto const_idx_op = dyn_cast_or_null<ConstantInt>(idx_op)) {
					EEI.setOperand(1 /* idx op */, builder->getInt32((int32_t)const_idx_op->getValue().getZExtValue()));
				}
				else {
					builder->SetInsertPoint(&EEI);
					const auto i32_index = builder->CreateIntCast(idx_op, builder->getInt32Ty(), false);
					EEI.setOperand(1 /* idx op */, i32_index);
				}
				was_modified = true;
			}
		}

		// metal can only handle i32 indices
		void visitInsertElement(InsertElementInst& IEI) {
			const auto idx_op = IEI.llvm::User::getOperand(2);
			const auto idx_type = idx_op->getType();
			if(!idx_type->isIntegerTy(32)) {
				if(const auto const_idx_op = dyn_cast_or_null<ConstantInt>(idx_op)) {
					IEI.setOperand(2 /* idx op */, builder->getInt32((int32_t)const_idx_op->getValue().getZExtValue()));
				}
				else {
					builder->SetInsertPoint(&IEI);
					const auto i32_index = builder->CreateIntCast(idx_op, builder->getInt32Ty(), false);
					IEI.setOperand(2 /* idx op */, i32_index);
				}
				was_modified = true;
			}
		}
	};

	// MetalFinalModuleCleanup:
	// * calling convention cleanup
	// * strip unused functions/prototypes/externs
	struct MetalFinalModuleCleanup : public ModulePass {
		static char ID; // Pass identification, replacement for typeid

		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		bool was_modified { false };

		MetalFinalModuleCleanup() : ModulePass(ID) {
			initializeMetalFinalModuleCleanupPass(*PassRegistry::getPassRegistry());
		}

		bool runOnModule(Module& Mod) override {
			M = &Mod;
			ctx = &M->getContext();

			// * strip metal calling convention from all functions and their users (replace it with C CC)
			bool module_modified = false;
			for(auto func_iter = Mod.begin(); func_iter != Mod.end();) {
				auto& func = *func_iter;
				if(func.getCallingConv() != CallingConv::C) {
					func.setCallingConv(CallingConv::C);
					for(auto user : func.users()) {
						if(auto CB = dyn_cast<CallBase>(user)) {
							CB->setCallingConv(CallingConv::C);
						}
					}
					module_modified = true;
				}
				++func_iter;
			}
			return module_modified;
		}

	};

}

char MetalFinal::ID = 0;
FunctionPass *llvm::createMetalFinalPass() {
	return new MetalFinal();
}
INITIALIZE_PASS_BEGIN(MetalFinal, "MetalFinal", "MetalFinal Pass", false, false)
INITIALIZE_PASS_END(MetalFinal, "MetalFinal", "MetalFinal Pass", false, false)

char MetalFinalModuleCleanup::ID = 0;
ModulePass *llvm::createMetalFinalModuleCleanupPass() {
	return new MetalFinalModuleCleanup();
}
INITIALIZE_PASS_BEGIN(MetalFinalModuleCleanup, "MetalFinal module cleanup", "MetalFinal module cleanup Pass", false, false)
INITIALIZE_PASS_END(MetalFinalModuleCleanup, "MetalFinal module cleanup", "MetalFinal module cleanup Pass", false, false)
