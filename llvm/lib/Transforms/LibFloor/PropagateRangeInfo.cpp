//===- PropagateRangeInfo.cpp - Vulkan final pass -------------------------===//
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
// TODO
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/LoopInfo.h"
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
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>
#include <cstdarg>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <array>
using namespace llvm;

#define DEBUG_TYPE "PropagateRangeInfo"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

namespace {
	// PropagateRangeInfo
	struct PropagateRangeInfo : public FunctionPass, InstVisitor<PropagateRangeInfo> {
		friend class InstVisitor<PropagateRangeInfo>;
		
		static char ID; // Pass identification, replacement for typeid
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		
		bool was_modified { false };
		
		PropagateRangeInfo() :
		FunctionPass(ID) {
			initializePropagateRangeInfoPass(*PassRegistry::getPassRegistry());
		}
		
		StringRef getPassName() const override {
			return "Propagate Range Info";
		}
		
		void getAnalysisUsage(AnalysisUsage &AU) const override {
			AU.setPreservesCFG();
			FunctionPass::getAnalysisUsage(AU);
		}
		
		bool runOnFunction(Function &F) override {
			// exit if empty function
			if(F.empty()) return false;
			
			//
			M = F.getParent();
			ctx = &M->getContext();
			func = &F;
			was_modified = false;
			
			visit(F);
			return was_modified;
		}
		
		// InstVisitor overrides...
		using InstVisitor<PropagateRangeInfo>::visit;
		void visit(Instruction& I) {
			InstVisitor<PropagateRangeInfo>::visit(I);
		}
		
		// we always start at call instructions, because this is where range info originates from
		void visitCallInst(CallInst &CI) {
			MDNode* range = CI.getMetadata(LLVMContext::MD_range);
			if(range == nullptr) return;
			
			for(User* user : CI.users()) {
				if(Instruction* I = dyn_cast<Instruction>(user)) {
					DBG(errs() << "adding range info to user: " << *I << "\n";)
					addRangeInfo(*I, range);
				}
			}
		}
		
		void addRangeInfo(Instruction& I, MDNode* range) {
			// can't handle non-integer types or int bit-width > 64 yet
			IntegerType* int_type = dyn_cast<IntegerType>(I.getType());
			if(int_type == nullptr ||
			   int_type->getBitWidth() > 64) {
				return;
			}
			
			// can only propagate to certain instruction
			switch(I.getOpcode()) {
				// trunc/extend are obvious
				case llvm::Instruction::Trunc:
				case llvm::Instruction::SExt:
				case llvm::Instruction::ZExt:
					break;
				
				// TODO: integer/other ops when the operand is a ConstantInt
				// add/sub/mul/div/rem/shl/shr -> obvious
				// and -> [0, currrent max]?
				// or/xor -> remove range info, b/c [0, int max]?
				// select -> merge
				// phi -> merge if all have range info
				
				// else: can't propagate
				default: return;
			}
			
			// will modify now
			was_modified = true;
			
			// check if we need to convert the integer type
			// TODO: how to handle signed values? can't retrieve that info from APInt or ConstantInt
			const auto range_type = mdconst::extract<ConstantInt>(range->getOperand(0))->getType();
			MDNode* apply_range = range;
			if(range_type != int_type) {
				const uint64_t max_value = int_type->getBitMask();
				
				SmallVector<Metadata*, 2> conv_range_ints;
				conv_range_ints.reserve(range->getNumOperands());
				for(const auto& op : range->operands()) {
					const auto val = mdconst::extract<ConstantInt>(op)->getValue();
					const auto clamped_val = std::min(val.getZExtValue(), max_value);
					conv_range_ints.push_back(llvm::ConstantAsMetadata::get(ConstantInt::get(int_type, clamped_val, false)));
				}
				apply_range = MDNode::get(*ctx, conv_range_ints);
			}
			
			// if the instruction already contains range info, merge it
			MDNode* instr_range = I.getMetadata(LLVMContext::MD_range);
			if(instr_range == nullptr) {
				I.setMetadata(LLVMContext::MD_range, apply_range);
			}
			else {
				I.setMetadata(LLVMContext::MD_range, MDNode::getMostGenericRange(apply_range, instr_range));
			}
			
			// continue the recursion
			for(User* user : I.users()) {
				if(Instruction* UI = dyn_cast<Instruction>(user)) {
					DBG(errs() << "> adding range info to user: " << *UI << "\n";)
					addRangeInfo(*UI, apply_range);
				}
			}
		}
		
	};
	
}

char PropagateRangeInfo::ID = 0;
FunctionPass *llvm::createPropagateRangeInfoPass() {
	return new PropagateRangeInfo();
}
INITIALIZE_PASS_BEGIN(PropagateRangeInfo, "PropagateRangeInfo", "PropagateRangeInfo Pass", false, false)
INITIALIZE_PASS_END(PropagateRangeInfo, "PropagateRangeInfo", "PropagateRangeInfo Pass", false, false)

