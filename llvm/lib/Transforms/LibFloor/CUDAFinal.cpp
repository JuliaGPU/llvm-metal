//===- CUDAFinal.cpp - CUDA final pass ------------------------------------===//
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

#define DEBUG_TYPE "CUDAFinal"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

namespace {
	// CUDAFinal
	struct CUDAFinal : public FunctionPass, InstVisitor<CUDAFinal> {
		friend class InstVisitor<CUDAFinal>;
		
		static char ID; // Pass identification, replacement for typeid
		
		std::shared_ptr<llvm::IRBuilder<>> builder;
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		Instruction* alloca_insert { nullptr };
		bool was_modified { false };
		
		CUDAFinal() : FunctionPass(ID) {
			initializeCUDAFinalPass(*PassRegistry::getPassRegistry());
		}
		
		bool runOnFunction(Function &F) override {
			// exit if empty function
			if(F.empty()) return false;
			
			// if not a kernel function, return (for now)
			if(F.getCallingConv() != CallingConv::FLOOR_KERNEL) return false;
			
			// reset
			M = F.getParent();
			ctx = &M->getContext();
			func = &F;
			builder = std::make_shared<llvm::IRBuilder<>>(*ctx);
			was_modified = false;
			
			// visit everything in this function
			DBG(errs() << "in func: "; errs().write_escaped(F.getName()) << '\n';)
			visit(F);
			return was_modified;
		}
		
		// InstVisitor overrides...
		using InstVisitor<CUDAFinal>::visit;
		void visit(Instruction& I) {
			InstVisitor<CUDAFinal>::visit(I);
		}
		
	};
}

char CUDAFinal::ID = 0;
INITIALIZE_PASS_BEGIN(CUDAFinal, "CUDAFinal", "CUDAFinal Pass", false, false)
INITIALIZE_PASS_END(CUDAFinal, "CUDAFinal", "CUDAFinal Pass", false, false)

FunctionPass *llvm::createCUDAFinalPass() {
	return new CUDAFinal();
}
