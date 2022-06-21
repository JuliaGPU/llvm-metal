//===- FMACombiner.cpp - --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass does fma combining/folding in fadd, fmul, fsub and fma instructions.
//
// NOTE:
// * blatently copied from DAGCombiner, with some additions and modifications
// * except for fpext this has all "aggressiveness" enabled
//
// TODO: canonicalize fadd/fsub/fmul/fneg
// TODO: add general fmul/fneg handling
// TODO: try combining in reverse, this might give more interesting results
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DerivedTypes.h"
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
#include "llvm/IR/PatternMatch.h"
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
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <array>
using namespace llvm;

#define DEBUG_TYPE "FMACombiner"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

// enable to add the line # to the name of new fma instructions
//#define DEBUG_FMA 1

namespace {
	// FMACombiner
	struct FMACombiner : public FunctionPass, InstVisitor<FMACombiner> {
		friend class InstVisitor<FMACombiner>;
		
		static char ID; // Pass identification, replacement for typeid
		
		static constexpr const char fma_prefix[] { "_Z3fma" };
		static constexpr const char fma_half[] { "_Z3fmaDhDhDh" };
		static constexpr const char fma_float[] { "_Z3fmafff" };
		static constexpr const char fma_double[] { "_Z3fmaddd" };
		
		std::shared_ptr<llvm::IRBuilder<>> builder;
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		bool is_kernel_func { false };
		bool is_vertex_func { false };
		bool is_fragment_func { false };
		bool is_tess_control_func { false };
		bool is_tess_eval_func { false };
		
		std::unordered_set<Instruction*> unreachable_kill_list;
		
		FMACombiner() : FunctionPass(ID) {
			initializeFMACombinerPass(*PassRegistry::getPassRegistry());
		}
		
		bool runOnFunction(Function &F) override {
			// exit if empty function
			if(F.empty()) return false;
			
			// determine this function type + exit if it isn't a kernel or shader function
			is_kernel_func = F.getCallingConv() == CallingConv::FLOOR_KERNEL;
			is_vertex_func = F.getCallingConv() == CallingConv::FLOOR_VERTEX;
			is_fragment_func = F.getCallingConv() == CallingConv::FLOOR_FRAGMENT;
			is_tess_control_func = F.getCallingConv() == CallingConv::FLOOR_TESS_CONTROL;
			is_tess_eval_func = F.getCallingConv() == CallingConv::FLOOR_TESS_EVAL;
			if (!is_kernel_func &&
				!is_vertex_func &&
				!is_fragment_func &&
				!is_tess_control_func &&
				!is_tess_eval_func) {
				return false;
			}
			
			DBG(errs() << "> FMA combiner: "; errs().write_escaped(F.getName()) << '\n';)
			
			//
			M = F.getParent();
			ctx = &M->getContext();
			func = &F;
			builder = std::make_shared<llvm::IRBuilder<>>(*ctx);
			
			// visit everything in this function
			DBG(errs() << "> FMA combiner pass #1\n";)
			visit(F);
			
			// for now: make a second pass over the function, there can still by folding opportunities
			// TODO: try to already fold things when emitting new instructions, then this might not be necessary
			DBG(errs() << "> FMA combiner pass #2\n";)
			visit(F);
			
			DBG(errs() << "< FMA combiner done\n";)
			
			// always modified
			return true; // TODO: only if actually modified
		}
		
		// InstVisitor overrides...
		using InstVisitor<FMACombiner>::visit;
		void visit(Instruction& I) {
			InstVisitor<FMACombiner>::visit(I);
		}
		
		// fma is a function call -> forward it
		void visitCallInst(CallInst &I) {
			if(I.getCalledFunction()->getName().startswith(fma_prefix)) {
				visitFMA(I);
				return;
			}
		}
		
		// properly kills 'inst' and replaces all its uses with 'repl' if 'repl' is not nullptr
		static void kill_instruction(Instruction* inst, Value* repl = nullptr) {
			DBG(errs() << "removing: " << *inst;)
			if(repl != nullptr) {
				DBG(errs() << " => repl: " << *repl << "\n";)
				inst->replaceAllUsesWith(repl);
			}
			DBG(else errs() << "\n";)
			inst->dropAllReferences();
			inst->removeFromParent();
		}
		// returns the opcode of 'V' if 'V' is an instruction, otherwise returns 0
		static uint32_t get_opcode(llvm::Value* V) {
			if(auto instr = dyn_cast<Instruction>(V)) {
				return instr->getOpcode();
			}
			return 0;
		};
		// creates a negate instruction of 'op', inserted before 'insert_before'
		Value* create_negate(Value* op, Instruction* insert_before) {
			// use builder so that we can constant fold
			builder->SetInsertPoint(insert_before);
			return builder->CreateFNeg(op, op->hasName() ? op->getName() + ".neg" : "neg");
		}
		// creates a negate instruction of either 'op_0' or 'op_1', prefering the one which is a constant value,
		// if neither is a constant, will use 'op_0', returns the instruction and 0 or 1, depending on which op was negated
		std::pair<Value*, uint32_t> create_negate(Value* op_0, Value* op_1, Instruction* insert_before) {
			builder->SetInsertPoint(insert_before);
			uint32_t op_num = 0;
			if(isa<ConstantFP>(op_0)) {
				op_num = 0;
			}
			else if(isa<ConstantFP>(op_1)) {
				op_num = 1;
			}
			auto op = (op_num == 0 ? op_0 : op_1);
			return { builder->CreateFNeg(op, op->hasName() ? op->getName() + ".neg" : "neg"), op_num };
		}
#if !defined(DEBUG_FMA)
		Instruction* emit_fma(
#else
#define emit_fma(...) emit_fma_(__LINE__, __VA_ARGS__)
		Instruction* emit_fma_(uint32_t line,
#endif
							  Value* a, Value* b, Value* c,
							  Instruction* repl,
							  Instruction* opt_repl = nullptr,
							  // if nullptr, insert before repl
							  Instruction* insert_before = nullptr,
							  bool is_remove = true) {
			// TODO: already do simple constant folding in here
			//      +fma(x, y, 0) -> fmul(x, y)
			//      +fma(0, y, z) || fma(x, 0, z) -> z
			//      +fma(c1, c2, z) -> fadd(z, c1*c2)
			//      +fma(x, 1, z) || fma(1, y, z) -> fadd(x|y, z)
			// -> add fma constant folder helper function?
			
			auto fp_type = a->getType();
			if(fp_type != b->getType() || fp_type != c->getType()) {
				// only replace with fma if all three operands have the same type
				return nullptr;
			}
			if(!fp_type->isHalfTy() &&
			   !fp_type->isFloatTy() &&
			   !fp_type->isDoubleTy()) {
				// must be 16-bit, 32-bit or 64-bit fp type
				return nullptr;
			}
			
			std::vector<Type*> param_types(3, fp_type);
			const auto func_type = llvm::FunctionType::get(fp_type, param_types, false);
			
			std::string func_name;
			if(fp_type->isHalfTy()) func_name = fma_half;
			else if(fp_type->isFloatTy()) func_name = fma_float;
			else if(fp_type->isDoubleTy()) func_name = fma_double;
			else llvm_unreachable("invalid fp type");
			
			// emit in canonical form if a is const: fma(non-const a, const b, any c)
			const auto is_a_const = isa<ConstantFP>(a);
			const auto is_b_const = isa<ConstantFP>(b);
			SmallVector<Value*, 3> args {
				is_a_const && !is_b_const ? b : a,
				is_a_const && !is_b_const ? a : b,
				c
			};
			auto CI = CallInst::Create(M->getOrInsertFunction(func_name, func_type), args,
									   (repl->hasName() ? repl->getName() + ".fma" : "fma")
#if defined(DEBUG_FMA)
									   + "_" + std::to_string(line) + "_"
#endif
									   , (insert_before == nullptr ? repl : insert_before));
			CI->setCallingConv(CallingConv::FLOOR_FUNC);
			CI->setDoesNotAccessMemory();
			CI->setNotConvergent();
			CI->setDebugLoc(repl->getDebugLoc()); // keep debug loc of first repl
			DBG(errs() << "emitting: " << *CI << "\n";)
			
			if(is_remove) {
				kill_instruction(repl, CI);
			
				if(opt_repl != nullptr &&
				   opt_repl->getNumUses() == 0) {
					kill_instruction(opt_repl);
				}
			}
			
			return CI;
		}
		void visitFAdd(BinaryOperator &I) {
			// skip if fast-math/reassoc is not set
			if(!I.hasAllowReassoc()) {
				return;
			}
			
			DBG(errs() << "@fadd: " << I << "\n";)
			
			auto N0 = I.getOperand(0);
			auto N1 = I.getOperand(1);
			auto N0I = dyn_cast<Instruction>(N0);
			auto N1I = dyn_cast<Instruction>(N1);
			
			// If we have two choices trying to fold (fadd (fmul u, v), (fmul x, y)),
			// prefer to fold the multiply with fewer uses.
			if(get_opcode(N0) == Instruction::FMul &&
			   get_opcode(N1) == Instruction::FMul) {
				if(N0->getNumUses() > N1->getNumUses()) {
					std::swap(N0, N1);
					std::swap(N0I, N1I);
				}
			}
			
			// fold (fadd (fmul x, y), z) -> (fma x, y, z)
			if(get_opcode(N0) == Instruction::FMul) {
				if(emit_fma(N0I->getOperand(0), N0I->getOperand(1), N1, &I, N0I)) {
					return;
				}
			}
			
			// fold (fadd x, (fmul y, z)) -> (fma y, z, x)
			// Note: Commutes FADD operands.
			if(get_opcode(N1) == Instruction::FMul) {
				if(emit_fma(N1I->getOperand(0), N1I->getOperand(1), N0, &I, N1I)) {
					return;
				}
			}
			
			// fold (fadd (fma x, y, (fmul u, v)), z) -> (fma x, y (fma u, v, z))
			if(auto FMACI = dyn_cast<CallInst>(N0)) {
				if(FMACI->getCalledFunction()->getName().startswith(fma_prefix) &&
				   get_opcode(FMACI->getOperand(2)) == Instruction::FMul) {
					auto mul_op = cast<Instruction>(FMACI->getOperand(2));
					
					DBG(errs() << "(fadd (fma x, y, (fmul u, v)), z) -> (fma x, y (fma u, v, z))\nfma:"
							   << *FMACI << "\nmul: " << mul_op << "\n";)
					
					auto inner_fma = emit_fma(mul_op->getOperand(0), mul_op->getOperand(1), N1,
											  mul_op, nullptr,
											  // must insert before current instruction, b/c we don't know where 'z' came from
											  &I,
											  // don't remove yet, if sth fails, these are still needed
											  // -> rely on DCE to handle this
											  false);
					
					if(inner_fma) {
						auto outer_fma = emit_fma(FMACI->getOperand(0), FMACI->getOperand(1), inner_fma, &I);
						if(outer_fma) {
							if(FMACI->getNumUses() == 0) {
								kill_instruction(FMACI);
							}
							if(mul_op->getNumUses() == 0) {
								kill_instruction(mul_op);
							}
							return;
						}
					}
					DBG(errs() << "!! did not fold\n";)
				}
			}
			
			// fold (fadd x, (fma y, z, (fmul u, v)) -> (fma y, z (fma u, v, x))
			if(auto FMACI = dyn_cast<CallInst>(N1)) {
				if(FMACI->getCalledFunction()->getName().startswith(fma_prefix) &&
				   get_opcode(FMACI->getOperand(2)) == Instruction::FMul) {
					auto mul_op = cast<Instruction>(FMACI->getOperand(2));
					
					DBG(errs() << "(fadd x, (fma y, z, (fmul u, v)) -> (fma y, z (fma u, v, x)):\nfma:"
							   << *FMACI << "\nmul: " << mul_op << "\n";)
					
					auto inner_fma = emit_fma(mul_op->getOperand(0), mul_op->getOperand(1), N0,
											  mul_op, nullptr,
											  // must insert before current instruction, b/c we don't know where 'x' came from
											  &I,
											  // don't remove yet, if sth fails, these are still needed
											  // -> rely on DCE to handle this
											  false);
					
					if(inner_fma) {
						auto outer_fma = emit_fma(FMACI->getOperand(0), FMACI->getOperand(1), inner_fma, &I);
						if(outer_fma) {
							if(FMACI->getNumUses() == 0) {
								kill_instruction(FMACI);
							}
							if(mul_op->getNumUses() == 0) {
								kill_instruction(mul_op);
							}
							return;
						}
					}
					DBG(errs() << "!! did not fold\n";)
				}
			}
			
			// fold (fadd x, (fma a, b, (fma u, v, (fmul s t)))), or swizzled fadd operands
			// -> (fma s t (fma u v (fma a b x)))
			{
				auto FMACI0 = dyn_cast<CallInst>(N0);
				auto FMACI1 = dyn_cast<CallInst>(N1);
				auto FMACI = (FMACI0 != nullptr && FMACI0->getCalledFunction()->getName().startswith(fma_prefix) ? FMACI0 :
							  FMACI1 != nullptr && FMACI1->getCalledFunction()->getName().startswith(fma_prefix) ? FMACI1 :
							  nullptr);
				if(FMACI) {
					auto FMA2CI = dyn_cast<CallInst>(FMACI->getOperand(2));
					if(FMA2CI != nullptr && FMA2CI->getCalledFunction()->getName().startswith(fma_prefix)) {
						auto mul_op = dyn_cast<BinaryOperator>(FMA2CI->getOperand(2));
						if(mul_op != nullptr && mul_op->getOpcode() == Instruction::FMul) {
							DBG(errs() << "fold (fadd x, (fma a, b, (fma u, v, (fmul s t)))) -> (fma s t (fma u v (fma a b x))):\n"
									   "fma1: " << *FMACI << "\nfma2: " << FMA2CI << "\nfmul: " << mul_op << "\n";)
							
							Value* x = (FMACI == FMACI0 ? N1 : N0);
							Value* a = FMACI->getOperand(0);
							Value* b = FMACI->getOperand(1);
							Value* u = FMA2CI->getOperand(0);
							Value* v = FMA2CI->getOperand(1);
							Value* s = mul_op->getOperand(0);
							Value* t = mul_op->getOperand(1);
							
							auto fma_abx = emit_fma(a, b, x, FMACI, nullptr, &I, false);
							if(fma_abx) {
								auto fma_uv_abx = emit_fma(u, v, fma_abx, FMA2CI, nullptr, &I, false);
								if(fma_uv_abx) {
									auto fma_st_uv_abx = emit_fma(s, t, fma_uv_abx, &I, nullptr);
									if(fma_st_uv_abx) {
										return;
									}
								}
							}
							DBG(errs() << "!! did not fold\n";)
						}
					}
				}
			}
		}
		void visitFSub(BinaryOperator &I) {
			// skip if fast-math/reassoc is not set
			if(!I.hasAllowReassoc()) {
				return;
			}
			
			DBG(errs() << "@fsub: " << I << "\n";)
			
			auto N0 = I.getOperand(0);
			auto N1 = I.getOperand(1);
			auto N0I = dyn_cast<Instruction>(N0);
			auto N1I = dyn_cast<Instruction>(N1);
			
			// fold (fsub (fmul x, y), z) -> (fma x, y, (fneg z))
			if(get_opcode(N0) == Instruction::FMul) {
				DBG(errs() << "fsub fold #1\n";)
				auto fneg_N1 = create_negate(N1, &I);
				if(emit_fma(N0I->getOperand(0), N0I->getOperand(1), fneg_N1, &I, N0I)) {
					return;
				}
				DBG(errs() << "!! did not fold\n";)
			}
			
			// fold (fsub x, (fmul y, z)) -> (fma (fneg y), z, x)
			// NOTE: Commutes FSUB operands.
			// NOTE: can negate either y or z
			if(get_opcode(N1) == Instruction::FMul) {
				DBG(errs() << "fsub fold #2\n";)
				auto N1op0 = N1I->getOperand(0);
				auto N1op1 = N1I->getOperand(1);
				auto neg = create_negate(N1op0, N1op1, &I);
				if(emit_fma(neg.first, neg.second == 0 ? N1op1 : N1op0, N0, &I, N1I)) {
					return;
				}
				DBG(errs() << "!! did not fold\n";)
			}
			
			// fold (fsub (fneg (fmul, x, y)), z) -> (fma (fneg x), y, (fneg z))
			// NOTE: can negate either x or y
			Value* fneg_val;
			if(PatternMatch::match(N0, PatternMatch::m_FNeg(PatternMatch::m_Value(fneg_val)))) { // TODO: is this correct?
				auto N0op0I = dyn_cast<Instruction>(N0I->getOperand(0));
				if(N0op0I != nullptr && N0op0I->getOpcode() == Instruction::FMul) {
					DBG(errs() << "fsub fold #3\n";)
					auto N00 = N0op0I->getOperand(0);
					auto N01 = N0op0I->getOperand(1);
					auto fneg_xy = create_negate(N00, N01, &I);
					auto fneg_N1 = create_negate(N1, &I);
					if(emit_fma(fneg_xy.first, fneg_xy.second == 0 ? N01 : N00, fneg_N1, &I, N0op0I)) {
						// TODO: also kill fneg
						return;
					}
					DBG(errs() << "!! did not fold\n";)
				}
			}
			
			// fold (fsub (fma x, y, (fmul u, v)), z)
			//   -> (fma x, y (fma u, v, (fneg z)))
			if(auto FMACI = dyn_cast<CallInst>(N0)) {
				if(FMACI->getCalledFunction()->getName().startswith(fma_prefix) &&
				   get_opcode(FMACI->getOperand(2)) == Instruction::FMul) {
					auto mul_op = cast<Instruction>(FMACI->getOperand(2));
					
					DBG(errs() << "fold (fsub (fma x, y, (fmul u, v)), z) -> (fma x, y (fma u, v, (fneg z))):\n"
							   << "fma: " << *FMACI << "\nfmul: " << mul_op << "\n";)
					
					auto fneg_N1 = create_negate(N1, &I);
					auto inner_fma = emit_fma(mul_op->getOperand(0), mul_op->getOperand(1), fneg_N1,
											  mul_op, nullptr,
											  // must insert before current instruction, b/c we don't know where 'x' came from
											  &I,
											  // don't remove yet, if sth fails, these are still needed
											  // -> rely on DCE to handle this
											  false);
					
					if(inner_fma) {
						auto outer_fma = emit_fma(FMACI->getOperand(0), FMACI->getOperand(1), inner_fma, &I);
						if(outer_fma) {
							if(FMACI->getNumUses() == 0) {
								kill_instruction(FMACI);
							}
							if(mul_op->getNumUses() == 0) {
								kill_instruction(mul_op);
							}
							return;
						}
					}
					DBG(errs() << "!! did not fold\n";)
				}
			}
	
			// fold (fsub x, (fma y, z, (fmul u, v)))
			//   -> (fma (fneg y), z, (fma (fneg u), v, x))
			// NOTE: can negate either y or z, and either u or v
			if(auto FMACI = dyn_cast<CallInst>(N1)) {
				if(FMACI->getCalledFunction()->getName().startswith(fma_prefix) &&
				   get_opcode(FMACI->getOperand(2)) == Instruction::FMul) {
					auto mul_op = cast<Instruction>(FMACI->getOperand(2));
					
					DBG(errs() << "fold (fsub x, (fma y, z, (fmul u, v))) -> (fma (fneg y), z, (fma (fneg u), v, x)):\n"
							   << "fma: " << *FMACI << "\nfmul: " << mul_op << "\n";)
					
					auto N20 = mul_op->getOperand(0);
					auto N21 = mul_op->getOperand(1);
					auto fneg_uv = create_negate(N20, N21, &I);
					auto inner_fma = emit_fma(fneg_uv.first, fneg_uv.second == 0 ? N21 : N20, N0,
											  mul_op, nullptr,
											  // must insert before current instruction, b/c we don't know where 'x' came from
											  &I,
											  // don't remove yet, if sth fails, these are still needed
											  // -> rely on DCE to handle this
											  false);
					
					if(inner_fma) {
						auto N1op0 = FMACI->getOperand(0);
						auto N1op1 = FMACI->getOperand(1);
						auto fneg_yz = create_negate(N1op0, N1op1, &I);
						auto outer_fma = emit_fma(fneg_yz.first, fneg_yz.second == 0 ? N1op1 : N1op0, inner_fma, &I);
						if(outer_fma) {
							if(FMACI->getNumUses() == 0) {
								kill_instruction(FMACI);
							}
							if(mul_op->getNumUses() == 0) {
								kill_instruction(mul_op);
							}
							return;
						}
					}
					DBG(errs() << "!! did not fold\n";)
				}
			}
		}
		void visitFMul(BinaryOperator &I) {
			// skip if fast-math/reassoc is not set
			if(!I.hasAllowReassoc()) {
				return;
			}
			
			DBG(errs() << "@fmul: " << I << "\n";)
			
			auto N0 = I.getOperand(0);
			auto N1 = I.getOperand(1);
			
			// fold (fmul (fadd x, +1.0), y) -> (fma x, y, y)
			// fold (fmul (fadd x, -1.0), y) -> (fma x, y, (fneg y))
			auto FuseFADD = [&](Value* X, Value* Y) -> Instruction* {
				if(get_opcode(X) == Instruction::FAdd) {
					auto XInst = cast<Instruction>(X);
					auto XC1 = dyn_cast<ConstantFP>(XInst->getOperand(1));
					if(XC1 && XC1->isExactlyValue(+1.0)) {
						return emit_fma(XInst->getOperand(0), Y, Y, &I, XInst);
					}
					if(XC1 && XC1->isExactlyValue(-1.0)) {
						auto fneg_y = create_negate(Y, &I);
						return emit_fma(XInst->getOperand(0), Y, fneg_y, &I, XInst);
					}
				}
				return nullptr;
			};
			
			if(FuseFADD(N0, N1) ||
			   FuseFADD(N1, N0)) {
				return;
			}
			
			// fold (fmul (fsub +1.0, x), y) -> (fma (fneg x), y, y)
			// NOTE: can fold either x or y
			// fold (fmul (fsub -1.0, x), y) -> (fma (fneg x), y, (fneg y))
			// NOTE: can folder either x or y, prefer y unless x is constant
			// fold (fmul (fsub x, +1.0), y) -> (fma x, y, (fneg y))
			// fold (fmul (fsub x, -1.0), y) -> (fma x, y, y)
			auto FuseFSUB = [&](Value* X, Value* Y) -> Instruction* {
				if(get_opcode(X) == Instruction::FSub) {
					auto XInst = cast<Instruction>(X);
					auto XC0 = dyn_cast<ConstantFP>(XInst->getOperand(0));
					if(XC0 && XC0->isExactlyValue(+1.0)) {
						auto xop1 = XInst->getOperand(1);
						auto fneg = create_negate(xop1, Y, &I);
						return emit_fma(fneg.first, fneg.second == 0 ? Y : xop1, Y, &I, XInst);
					}
					if(XC0 && XC0->isExactlyValue(-1.0)) {
						auto fneg_y = create_negate(Y, &I);
						auto xop1 = XInst->getOperand(1);
						auto fneg = (isa<ConstantFP>(xop1) ? create_negate(xop1, &I) : fneg_y);
						return emit_fma(fneg, (fneg == fneg_y ? xop1 : Y), fneg_y, &I, XInst);
					}
					
					auto XC1 = dyn_cast<ConstantFP>(XInst->getOperand(1));
					if(XC1 && XC1->isExactlyValue(+1.0)) {
						auto fneg_y = create_negate(Y, &I);
						return emit_fma(XInst->getOperand(0), Y, fneg_y, &I, XInst);
					}
					if(XC1 && XC1->isExactlyValue(-1.0)) {
						return emit_fma(XInst->getOperand(0), Y, Y, &I, XInst);
					}
				}
				return nullptr;
			};
			
			if(FuseFSUB(N0, N1) ||
			   FuseFSUB(N1, N0)) {
				return;
			}
		}
		void visitFMA(CallInst &I) {
			auto N0 = I.getOperand(0);
			auto N1 = I.getOperand(1);
			auto N2 = I.getOperand(2);
			
			auto N0CFP = dyn_cast<ConstantFP>(N0);
			auto N1CFP = dyn_cast<ConstantFP>(N1);
			auto N2CFP = dyn_cast<ConstantFP>(N2);
			
			auto N0I = dyn_cast<Instruction>(N0);
			auto N1I = dyn_cast<Instruction>(N1);
			auto N2I = dyn_cast<Instruction>(N2);
			
			DBG(errs() << "@fma: " << I << "\n";)
			
			// Constant fold FMA.
			if(N0CFP && N1CFP && N2CFP) {
				DBG(errs() << "fma constant fold\n";)
				APFloat a = N0CFP->getValueAPF();
				const auto& b = N1CFP->getValueAPF();
				const auto& c = N2CFP->getValueAPF();
				if(a.fusedMultiplyAdd(b, c, APFloat::rmNearestTiesToEven) != APFloat::opInvalidOp) {
					kill_instruction(&I, ConstantFP::get(*ctx, a));
					return;
				}
				DBG(errs() << "!! did not fold\n";)
				return;
			}
			
			// mul with 0 -> N2
			if((N0CFP && N0CFP->isZero()) ||
			   (N1CFP && N1CFP->isZero())) {
				DBG(errs() << "fma 0 mul\n";)
				kill_instruction(&I, N2);
				return;
			}
			
			// (fma c1 c2 y) -> (fadd y c1*c2)
			if(N0CFP && N1CFP) {
				DBG(errs() << "fma -> fadd fold\n";)
				APFloat a = N0CFP->getValueAPF();
				const auto& b = N1CFP->getValueAPF();
				if(a.multiply(b, APFloat::rmNearestTiesToEven) != APFloat::opInvalidOp) {
					auto fadd = BinaryOperator::CreateFAdd(N2, ConstantFP::get(*ctx, a),
														   I.hasName() ? I.getName() + ".fadd_c" : "fadd_c", &I);
					fadd->setDebugLoc(I.getDebugLoc());
					kill_instruction(&I, fadd);
					return;
				}
				DBG(errs() << "!! did not fold\n";)
				return;
			}
			
			// (fma x y 0) -> (fmul x y)
			if(N2CFP && N2CFP->isZero()) {
				DBG(errs() << "fma -> add 0 fold\n";)
				auto fmul = BinaryOperator::CreateFMul(N0, N1, I.hasName() ? I.getName() + ".fmul_a0" : "fmul_a0", &I);
				fmul->setDebugLoc(I.getDebugLoc());
				kill_instruction(&I, fmul);
				return;
			}
			
			// Canonicalize (fma c, x, y) -> (fma x, c, y)
			if(N0CFP && !N1CFP) {
				I.setOperand(0, N1);
				I.setOperand(1, N0);
				
				// -> continue with swizzled ops
				std::swap(N0, N1);
				std::swap(N0CFP, N1CFP);
				std::swap(N0I, N1I);
			}
			
			// (fma x, 1, y) -> (fadd x, y)
			if(N1CFP && N1CFP->isExactlyValue(1.0)) {
				// N0 + N2
				DBG(errs() << "fma -> fadd fold (0+2)\n";)
				auto fadd = BinaryOperator::CreateFAdd(N0, N2, I.hasName() ? I.getName() + ".fadd" : "fadd", &I);
				fadd->setDebugLoc(I.getDebugLoc());
				kill_instruction(&I, fadd);
				return;
			}
			
			// (fma x, c1, (fmul x, c2)) -> (fmul x, c1+c2)
			if(N2I && N2I->getOpcode() == Instruction::FMul &&
			   N0 == N2I->getOperand(0) &&
			   N1CFP &&
			   isa<ConstantFP>(N2I->getOperand(1))) {
				DBG(errs() << "fma -> x mul c1+c2 fold: " << *N2I << "\n";)
				
				APFloat c1 = N1CFP->getValueAPF();
				const auto& c2 = cast<ConstantFP>(N2I->getOperand(1))->getValueAPF();
				if(c1.add(c2, APFloat::rmNearestTiesToEven) != APFloat::opInvalidOp) {
					auto fmul = BinaryOperator::CreateFMul(N0, ConstantFP::get(*ctx, c1),
														   I.hasName() ? I.getName() + ".fmul_c" : "fmul_c", &I);
					fmul->setDebugLoc(I.getDebugLoc());
					kill_instruction(&I, fmul);
					return;
				}
				DBG(errs() << "!! did not fold\n";)
				return;
			}
			
			// (fma (fmul x, c1), c2, y) -> (fma x, c1*c2, y)
			if(N0I && N0I->getOpcode() == Instruction::FMul &&
			   N1CFP &&
			   isa<ConstantFP>(N0I->getOperand(1))) {
				DBG(errs() << "fma -> fma x c1*c2 y fold: " << *N0I << "\n";)
				
				APFloat c1 = cast<ConstantFP>(N0I->getOperand(1))->getValueAPF();
				const auto& c2 = N1CFP->getValueAPF();
				if(c1.multiply(c2, APFloat::rmNearestTiesToEven) != APFloat::opInvalidOp) {
					emit_fma(N0I->getOperand(0), ConstantFP::get(*ctx, c1), N2, &I, N0I);
					return;
				}
				DBG(errs() << "!! did not fold\n";)
				return;
			}
			
			// NOTE: don't fold: (fma x, -1, y) -> (fadd (fneg x), y)
			
			// creates a fp constant of "value" using the same type/semantics as "copy_sema"
			const auto ap_constant_from_type = [](const APFloat& copy_sema, const double& value) {
				if (&copy_sema.getSemantics() == &APFloat::IEEEdouble()) {
					return APFloat(value);
				} else {
					return APFloat(float(value));
				}
			};
			
			// (fma x, c, x) -> (fmul x, (c+1))
			if(N1CFP && N0 == N2) {
				DBG(errs() << "fma -> fmul x c+1 fold\n";)
				APFloat c = N1CFP->getValueAPF();
				if(c.add(ap_constant_from_type(c, 1.0), APFloat::rmNearestTiesToEven) != APFloat::opInvalidOp) {
					auto fmul = BinaryOperator::CreateFMul(N0, ConstantFP::get(*ctx, c),
														   I.hasName() ? I.getName() + ".fmul_1c" : "fmul_1c", &I);
					fmul->setDebugLoc(I.getDebugLoc());
					kill_instruction(&I, fmul);
				}
				DBG(errs() << "!! did not fold\n";)
				return;
			}
			
			// (fma x, c, (fneg x)) -> (fmul x, (c-1))
			Value* fneg_val;
			if(N1CFP &&
			   N2I && N2I->getOperand(0) == N0 &&
			   PatternMatch::match(N2I, PatternMatch::m_FNeg(PatternMatch::m_Value(fneg_val)))) { // TODO: is this correct?
				DBG(errs() << "fma -> fmul x c-1 fold\n";)
				APFloat c = N1CFP->getValueAPF();
				if(c.subtract(ap_constant_from_type(c, 1.0), APFloat::rmNearestTiesToEven) != APFloat::opInvalidOp) {
					auto fmul = BinaryOperator::CreateFMul(N0, ConstantFP::get(*ctx, c),
														   I.hasName() ? I.getName() + ".fmul_s1c" : "fmul_s1c", &I);
					fmul->setDebugLoc(I.getDebugLoc());
					kill_instruction(&I, fmul);
				}
				DBG(errs() << "!! did not fold\n";)
				return;
			}
			
			// TODO: fold (fma (fadd x (fadd y c1)) c2 c3) -> (fma (fadd x y) c2 (c1*c2 + c3))
			// TODO: same for sub and neg ops?
		}
	};
	
}

char FMACombiner::ID = 0;
constexpr const char FMACombiner::fma_prefix[];
constexpr const char FMACombiner::fma_half[];
constexpr const char FMACombiner::fma_float[];
constexpr const char FMACombiner::fma_double[];

FunctionPass *llvm::createFMACombinerPass() {
	return new FMACombiner();
}
INITIALIZE_PASS_BEGIN(FMACombiner, "FMACombiner", "FMACombiner Pass", false, false)
INITIALIZE_PASS_END(FMACombiner, "FMACombiner", "FMACombiner Pass", false, false)
