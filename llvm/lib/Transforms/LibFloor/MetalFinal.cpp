//===- MetalFinal.cpp - Metal final pass ----------------------------------===//
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
#include "../../IR/LLVMContextImpl.h"
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
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <optional>
#include <cxxabi.h>
using namespace llvm;

#if defined(DEBUG_TYPE)
#undef DEBUG_TYPE
#endif
#define DEBUG_TYPE "MetalFinal"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

//////////////////////////////////////////
// blatantly copied/transplanted from SROA
namespace {
/// \brief A custom IRBuilder inserter which prefixes all names, but only in
/// Assert builds.
class IRBuilderPrefixedInserter : public IRBuilderDefaultInserter {
  std::string Prefix;
  const Twine getNameWithPrefix(const Twine &Name) const {
    return Name.isTriviallyEmpty() ? Name : Prefix + Name;
  }

public:
  void SetNamePrefix(const Twine &P) { Prefix = P.str(); }

protected:
  void InsertHelper(Instruction *I, const Twine &Name, BasicBlock *BB,
                    BasicBlock::iterator InsertPt) const override {
    IRBuilderDefaultInserter::InsertHelper(I, getNameWithPrefix(Name), BB,
                                           InsertPt);
  }
};

/// \brief Provide a typedef for IRBuilder that drops names in release builds.
using IRBuilderTy = llvm::IRBuilder<ConstantFolder, IRBuilderPrefixedInserter>;
}

namespace {
  /// \brief Generic recursive split emission class.
  template <typename Derived>
  class OpSplitter {
  protected:
    /// The builder used to form new instructions.
    IRBuilderTy IRB;
    /// The indices which to be used with insert- or extractvalue to select the
    /// appropriate value within the aggregate.
    SmallVector<unsigned, 4> Indices;
    /// The indices to a GEP instruction which will move Ptr to the correct slot
    /// within the aggregate.
    SmallVector<Value *, 4> GEPIndices;
    /// The base pointer of the original op, used as a base for GEPing the
    /// split operations.
    Value *Ptr;

    /// Initialize the splitter with an insertion point, Ptr and start with a
    /// single zero GEP index.
    OpSplitter(Instruction *InsertionPoint, Value *Ptr)
      : IRB(InsertionPoint), GEPIndices(1, IRB.getInt32(0)), Ptr(Ptr) {}

  public:
    /// \brief Generic recursive split emission routine.
    ///
    /// This method recursively splits an aggregate op (load or store) into
    /// scalar or vector ops. It splits recursively until it hits a single value
    /// and emits that single value operation via the template argument.
    ///
    /// The logic of this routine relies on GEPs and insertvalue and
    /// extractvalue all operating with the same fundamental index list, merely
    /// formatted differently (GEPs need actual values).
    ///
    /// \param Ty  The type being split recursively into smaller ops.
    /// \param Agg The aggregate value being built up or stored, depending on
    /// whether this is splitting a load or a store respectively.
    void emitSplitOps(Type *Ty, Value *&Agg, const Twine &Name) {
      if (Ty->isSingleValueType())
        return static_cast<Derived *>(this)->emitFunc(Ty, Agg, Name);

      if (ArrayType *ATy = dyn_cast<ArrayType>(Ty)) {
        unsigned OldSize = Indices.size();
        (void)OldSize;
        for (unsigned Idx = 0, Size = ATy->getNumElements(); Idx != Size;
             ++Idx) {
          assert(Indices.size() == OldSize && "Did not return to the old size");
          Indices.push_back(Idx);
          GEPIndices.push_back(IRB.getInt32(Idx));
          emitSplitOps(ATy->getElementType(), Agg, Name + "." + Twine(Idx));
          GEPIndices.pop_back();
          Indices.pop_back();
        }
        return;
      }

      if (StructType *STy = dyn_cast<StructType>(Ty)) {
        unsigned OldSize = Indices.size();
        (void)OldSize;
        for (unsigned Idx = 0, Size = STy->getNumElements(); Idx != Size;
             ++Idx) {
          assert(Indices.size() == OldSize && "Did not return to the old size");
          Indices.push_back(Idx);
          GEPIndices.push_back(IRB.getInt32(Idx));
          emitSplitOps(STy->getElementType(Idx), Agg, Name + "." + Twine(Idx));
          GEPIndices.pop_back();
          Indices.pop_back();
        }
        return;
      }

      llvm_unreachable("Only arrays and structs are aggregate loadable types");
    }
  };

  struct LoadOpSplitter : public OpSplitter<LoadOpSplitter> {
    LoadOpSplitter(Instruction *InsertionPoint, Value *Ptr)
      : OpSplitter<LoadOpSplitter>(InsertionPoint, Ptr) {}

    /// Emit a leaf load of a single value. This is called at the leaves of the
    /// recursive emission to actually load values.
    void emitFunc(Type *Ty, Value *&Agg, const Twine &Name) {
      assert(Ty->isSingleValueType());
      // Load the single value and insert it using the indices.
      auto elem_type = Ptr->getType()->getScalarType()->getPointerElementType();
      Value *GEP = IRB.CreateInBoundsGEP(elem_type, Ptr, GEPIndices, Name + ".gep");
      Value *Load = IRB.CreateLoad(elem_type, GEP, Name + ".load");
      Agg = IRB.CreateInsertValue(Agg, Load, Indices, Name + ".insert");
      DBG(dbgs() << "          to: " << *Load << "\n");
    }
  };

  struct StoreOpSplitter : public OpSplitter<StoreOpSplitter> {
    StoreOpSplitter(Instruction *InsertionPoint, Value *Ptr)
      : OpSplitter<StoreOpSplitter>(InsertionPoint, Ptr) {}

    /// Emit a leaf store of a single value. This is called at the leaves of the
    /// recursive emission to actually produce stores.
    void emitFunc(Type *Ty, Value *&Agg, const Twine &Name) {
      assert(Ty->isSingleValueType());
      // Extract the single value and store it using the indices.
      //
      // The gep and extractvalue values are factored out of the CreateStore
      // call to make the output independent of the argument evaluation order.
      Value *ExtractValue =
          IRB.CreateExtractValue(Agg, Indices, Name + ".extract");
      Value *InBoundsGEP =
          IRB.CreateInBoundsGEP(nullptr, Ptr, GEPIndices, Name + ".gep");
      Value *Store = IRB.CreateStore(ExtractValue, InBoundsGEP);
      (void)Store;
      DBG(dbgs() << "          to: " << *Store << "\n");
    }
  };

}
//////////////////////////////////////////

namespace {
	// MetalFirst
	struct MetalFirst : public FunctionPass, InstVisitor<MetalFirst> {
		friend class InstVisitor<MetalFirst>;
		
		static char ID; // Pass identification, replacement for typeid
		const bool enable_intel_workarounds, enable_nvidia_workarounds;
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		
		bool was_modified { false };
		bool is_vertex_func { false };
		bool is_fragment_func { false };
		bool is_kernel_func { false };
		bool is_tess_control_func { false };
		bool is_tess_eval_func { false };
		
		MetalFirst(const bool enable_intel_workarounds_ = false,
				   const bool enable_nvidia_workarounds_ = false) :
		FunctionPass(ID),
		enable_intel_workarounds(enable_intel_workarounds_),
		enable_nvidia_workarounds(enable_nvidia_workarounds_) {
			initializeMetalFirstPass(*PassRegistry::getPassRegistry());
		}
		
		bool runOnFunction(Function &F) override {
			// exit if empty function
			if(F.empty()) return false;
			
			//
			M = F.getParent();
			ctx = &M->getContext();
			
			is_vertex_func = F.getCallingConv() == CallingConv::FLOOR_VERTEX;
			is_fragment_func = F.getCallingConv() == CallingConv::FLOOR_FRAGMENT;
			is_kernel_func = F.getCallingConv() == CallingConv::FLOOR_KERNEL;
			is_tess_control_func = F.getCallingConv() == CallingConv::FLOOR_TESS_CONTROL;
			is_tess_eval_func = F.getCallingConv() == CallingConv::FLOOR_TESS_EVAL;
			
			// NOTE: for now, this is no longer needed
			was_modified = false;
			//visit(F);
			
			return was_modified;
		}
		
		// InstVisitor overrides...
		using InstVisitor<MetalFirst>::visit;
		void visit(Instruction& /* I */) {
			//InstVisitor<MetalFirst>::visit(I);
		}
	};
	
	// MetalFinal
	struct MetalFinal : public FunctionPass, InstVisitor<MetalFinal> {
		friend class InstVisitor<MetalFinal>;
		
		static char ID; // Pass identification, replacement for typeid
		const bool enable_intel_workarounds, enable_nvidia_workarounds;
		
		std::shared_ptr<llvm::IRBuilder<>> builder;
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		Instruction* alloca_insert { nullptr };
		bool was_modified { false };
		bool is_kernel_func { false };
		bool is_vertex_func { false };
		bool is_fragment_func { false };
		bool is_tess_control_func { false };
		bool is_tess_eval_func { false };
		
		// added kernel function args
		Argument* global_id { nullptr };
		Argument* global_size { nullptr };
		Argument* local_id { nullptr };
		Argument* local_size { nullptr };
		Argument* group_id { nullptr };
		Argument* group_size { nullptr };
		Argument* sub_group_id { nullptr };
		Argument* sub_group_local_id { nullptr };
		Argument* sub_group_size { nullptr };
		Argument* num_sub_groups { nullptr };
		
		// added vertex function args
		Argument* vertex_id { nullptr };
		Argument* instance_id { nullptr };
		
		// added fragment function args
		Argument* point_coord { nullptr };
		Argument* primitive_id { nullptr };
		Argument* barycentric_coord { nullptr };
		
		// added tessellation evaluation function args
		Argument* patch_id { nullptr };
		Argument* position_in_patch { nullptr };
		
		// any function args
		Argument* soft_printf { nullptr };
		
		MetalFinal(const bool enable_intel_workarounds_ = false,
				   const bool enable_nvidia_workarounds_ = false) :
		FunctionPass(ID),
		enable_intel_workarounds(enable_intel_workarounds_),
		enable_nvidia_workarounds(enable_nvidia_workarounds_) {
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
			
			// for intel gpus any conversion from/to float from/to i8 or i16 needs to go through a i32 first
			if(enable_intel_workarounds && from_iter->second[0] == 'f') {
				if(to_iter->first == llvm::Type::getInt8Ty(*ctx) ||
				   to_iter->first == llvm::Type::getInt16Ty(*ctx)) {
					// convert to i32 first, then trunc from i32 to i8/i16
					const auto to_i32_cast = call_conversion_func<cast_op>(from, llvm::Type::getInt32Ty(*ctx));
					return builder->CreateTrunc(to_i32_cast, to_iter->first);
				}
			}
			
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
		
		enum METAL_KERNEL_ARG_REV_IDX : int32_t {
			METAL_KERNEL_ARG_COUNT = 6,
			METAL_KERNEL_SUB_GROUPS_ARG_COUNT = 10,
		};
		
		enum METAL_VERTEX_ARG_REV_IDX : int32_t {
			METAL_VERTEX_ID = -2,
			METAL_VS_INSTANCE_ID = -1,
			
			METAL_VERTEX_ARG_COUNT = 2,
		};
		
		enum METAL_FRAGMENT_ARG_REV_IDX : int32_t {
			METAL_POINT_COORD = -1,
			
			METAL_FRAGMENT_ARG_COUNT = 1,
		};
		
		enum METAL_TESS_EVAL_ARG_REV_IDX : int32_t {
			METAL_PATCH_ID = -3,
			METAL_TES_INSTANCE_ID = -2,
			METAL_POSITION_IN_PATCH = -1,
			
			METAL_TESS_EVAL_ARG_COUNT = 3,
		};
		
		bool runOnFunction(Function &F) override {
			// exit if empty function
			if(F.empty()) return false;
			
			//
			M = F.getParent();
			ctx = &M->getContext();
			func = &F;
			builder = std::make_shared<llvm::IRBuilder<>>(*ctx);
			
			for(auto& instr : F.getEntryBlock().getInstList()) {
				if(!isa<AllocaInst>(instr)) {
					alloca_insert = &instr;
					break;
				}
			}
			
			const auto get_arg_by_idx = [&F](const int32_t rev_idx) -> llvm::Argument* {
				auto arg_iter = F.arg_end();
				std::advance(arg_iter, rev_idx);
				return &*arg_iter;
			};
			
			// check for sub-group support
			const auto triple = llvm::Triple(M->getTargetTriple());
			bool has_sub_group_support = false;
			if (triple.getArch() == Triple::ArchType::air64) {
				if (triple.getOS() == Triple::OSType::MacOSX) {
					has_sub_group_support = true;
				}
			}
			
			// check for optional features: soft-printf, primitive id, barycentric coord
			bool has_soft_printf = false, has_primitive_id = false, has_barycentric_coord = false;
			if (auto soft_printf_meta = M->getNamedMetadata("floor.soft_printf")) {
				has_soft_printf = true;
			}
			if (auto primitive_id_meta = M->getNamedMetadata("floor.primitive_id")) {
				has_primitive_id = true;
			}
			if (auto barycentric_coord_meta = M->getNamedMetadata("floor.barycentric_coord")) {
				has_barycentric_coord = true;
			}
			
			// get args if this is a kernel function
			is_kernel_func = F.getCallingConv() == CallingConv::FLOOR_KERNEL;
			is_tess_control_func = F.getCallingConv() == CallingConv::FLOOR_TESS_CONTROL;
			if(is_kernel_func || is_tess_control_func) {
				if (F.arg_size() >= (has_sub_group_support ? METAL_KERNEL_SUB_GROUPS_ARG_COUNT : METAL_KERNEL_ARG_COUNT) + (has_soft_printf ? 1 : 0)) {
					int32_t rev_idx = -1;
					if (has_sub_group_support) {
						num_sub_groups = get_arg_by_idx(rev_idx--);
						sub_group_size = get_arg_by_idx(rev_idx--);
						sub_group_local_id = get_arg_by_idx(rev_idx--);
						sub_group_id = get_arg_by_idx(rev_idx--);
					}
					group_size = get_arg_by_idx(rev_idx--);
					group_id = get_arg_by_idx(rev_idx--);
					local_size = get_arg_by_idx(rev_idx--);
					local_id = get_arg_by_idx(rev_idx--);
					global_size = get_arg_by_idx(rev_idx--);
					global_id = get_arg_by_idx(rev_idx--);
					if (has_soft_printf) {
						soft_printf = get_arg_by_idx(rev_idx--);
					}
				} else {
					errs() << "invalid " << (is_kernel_func ? "kernel" : "tessellation-control");
					errs() << " function (" << F.getName() << ") argument count: " << F.arg_size() << "\n";
					global_id = nullptr;
					global_size = nullptr;
					local_id = nullptr;
					local_size = nullptr;
					group_id = nullptr;
					group_size = nullptr;
					sub_group_id = nullptr;
					sub_group_local_id = nullptr;
					sub_group_size = nullptr;
					num_sub_groups = nullptr;
					soft_printf = nullptr;
				}
			}
			
			// get args if this is a vertex function
			is_vertex_func = F.getCallingConv() == CallingConv::FLOOR_VERTEX;
			if(is_vertex_func) {
				if (F.arg_size() >= METAL_VERTEX_ARG_COUNT + (has_soft_printf ? 1 : 0)) {
					// TODO: this should be optional / only happen on request
					vertex_id = get_arg_by_idx(METAL_VERTEX_ID);
					instance_id = get_arg_by_idx(METAL_VS_INSTANCE_ID);
					if (has_soft_printf) {
						soft_printf = get_arg_by_idx(-(METAL_VERTEX_ARG_COUNT + 1));
					}
				} else {
					errs() << "invalid vertex function (" << F.getName() << ") argument count: " << F.arg_size() << "\n";
					vertex_id = nullptr;
					instance_id = nullptr;
					soft_printf = nullptr;
				}
			}
			
			// get args if this is a tessellation evaluation function
			is_tess_eval_func = F.getCallingConv() == CallingConv::FLOOR_TESS_EVAL;
			if(is_tess_eval_func) {
				if (F.arg_size() >= METAL_TESS_EVAL_ARG_COUNT + (has_soft_printf ? 1 : 0)) {
					// TODO: this should be optional / only happen on request
					patch_id = get_arg_by_idx(METAL_PATCH_ID);
					instance_id = get_arg_by_idx(METAL_TES_INSTANCE_ID);
					position_in_patch = get_arg_by_idx(METAL_POSITION_IN_PATCH);
					if (has_soft_printf) {
						soft_printf = get_arg_by_idx(-(METAL_TESS_EVAL_ARG_COUNT + 1));
					}
				} else {
					errs() << "invalid tessellation-evaluation function (" << F.getName() << ") argument count: " << F.arg_size() << "\n";
					patch_id = nullptr;
					instance_id = nullptr;
					position_in_patch = nullptr;
					soft_printf = nullptr;
				}
			}
			
			// get args if this is a fragment function
			is_fragment_func = F.getCallingConv() == CallingConv::FLOOR_FRAGMENT;
			if(is_fragment_func) {
				const uint32_t opt_arg_count = (has_soft_printf ? 1u : 0u) + (has_primitive_id ? 1u : 0u) + (has_barycentric_coord ? 1u : 0u);
				if (F.arg_size() >= METAL_FRAGMENT_ARG_COUNT + opt_arg_count) {
					point_coord = get_arg_by_idx(METAL_POINT_COORD);
					
					// NOTE: reverse order!
					uint32_t opt_arg_counter = 1;
					if (has_barycentric_coord) {
						barycentric_coord = get_arg_by_idx(-(METAL_FRAGMENT_ARG_COUNT + opt_arg_counter++));
					} else {
						barycentric_coord = nullptr;
					}
					if (has_primitive_id) {
						primitive_id = get_arg_by_idx(-(METAL_FRAGMENT_ARG_COUNT + opt_arg_counter++));
					} else {
						primitive_id = nullptr;
					}
					if (has_soft_printf) {
						soft_printf = get_arg_by_idx(-(METAL_FRAGMENT_ARG_COUNT + opt_arg_counter++));
					} else {
						soft_printf = nullptr;
					}
				} else {
					errs() << "invalid fragment function (" << F.getName() << ") argument count: " << F.arg_size() << "\n";
					point_coord = nullptr;
					primitive_id = nullptr;
					barycentric_coord = nullptr;
					soft_printf = nullptr;
				}
			}
			
			// update function signature / param list
			if(is_kernel_func || is_vertex_func || is_fragment_func || is_tess_control_func || is_tess_eval_func) {
				std::vector<Type*> param_types;
				for(auto& arg : F.args()) {
					param_types.push_back(arg.getType());
				}
				auto new_func_type = FunctionType::get(F.getReturnType(), param_types, false);
				F.mutateType(PointerType::get(new_func_type, 0));
				F.mutateFunctionType(new_func_type);
				
				// always remove "norecurse" and "min-legal-vector-width"
				F.removeFnAttr(Attribute::NoRecurse);
				F.removeFnAttr("min-legal-vector-width");
			}
			
			// visit everything in this function
			was_modified = false; // reset every time
			DBG(errs() << "in func: "; errs().write_escaped(F.getName()) << '\n';)
			visit(F);
			
			// always modified
			return was_modified || is_kernel_func || is_vertex_func || is_fragment_func || is_tess_control_func || is_tess_eval_func;
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
		
		static std::optional<std::string> get_suffix_for_type(llvm::Type* type, const bool is_signed) {
			std::string ret = ".";
			auto elem_type = type;
			if (auto vec_type = dyn_cast_or_null<FixedVectorType>(type); vec_type) {
				elem_type = vec_type->getElementType();
				ret += "v" + std::to_string(vec_type->getNumElements());
			}
			switch (elem_type->getTypeID()) {
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
				case Intrinsic::memcpy:
				case Intrinsic::memset:
				case Intrinsic::memmove:
				case Intrinsic::dbg_addr:
				case Intrinsic::dbg_label:
				case Intrinsic::dbg_value:
				case Intrinsic::dbg_declare:
					// pass
					break;
					
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
					const bool is_fast = (op_lhs->getType()->isFloatTy() ||
										  (op_lhs->getType()->isVectorTy() &&
										   cast<FixedVectorType>(op_lhs->getType())->getElementType()->isFloatTy()));
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
							func_name += (is_fast ? "fast_fmin" : "fmin");
							break;
						case Intrinsic::maxnum:
							func_name += (is_fast ? "fast_fmax" : "fmax");
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
#endif
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
				default: {
					ctx->emitError(&I, "unknown/unhandled intrinsic:\n" + print_instr(I));
					break;
				}
			}
		}
		
		//
		void visitCallInst(CallInst &I) {
			// if this isn't a kernel/shader function we don't need to do anything here (yet)
			if (!is_kernel_func &&
				!is_vertex_func &&
				!is_fragment_func &&
				!is_tess_control_func &&
				!is_tess_eval_func) {
				return;
			}
			
			const auto func = I.getCalledFunction();
			if (!func) return;
			const auto func_name = func->getName();
			if (func_name.startswith("air.")) {
				check_air_call(I);
				return;
			}
			if (!func_name.startswith("floor.")) return;
			
			builder->SetInsertPoint(&I);
			
			// figure out which one we need
			Argument* id;
			bool get_from_vector = false;
			if(func_name == "floor.get_global_id.i32") {
				id = global_id;
				get_from_vector = true;
			}
			else if(func_name == "floor.get_global_size.i32") {
				id = global_size;
				get_from_vector = true;
			}
			else if(func_name == "floor.get_local_id.i32") {
				id = local_id;
				get_from_vector = true;
			}
			else if(func_name == "floor.get_local_size.i32") {
				id = local_size;
				get_from_vector = true;
			}
			else if(func_name == "floor.get_group_id.i32") {
				id = group_id;
				get_from_vector = true;
			}
			else if(func_name == "floor.get_group_size.i32") {
				id = group_size;
				get_from_vector = true;
			}
			else if(func_name == "floor.get_sub_group_id.i32") {
				id = sub_group_id;
			}
			else if(func_name == "floor.get_sub_group_local_id.i32") {
				id = sub_group_local_id;
			}
			else if(func_name == "floor.get_sub_group_size.i32") {
				id = sub_group_size;
			}
			else if(func_name == "floor.get_num_sub_groups.i32") {
				id = num_sub_groups;
			}
			else if(func_name == "floor.get_work_dim.i32") {
				if(group_size == nullptr) {
					DBG(printf("failed to get group_size arg, probably not in a kernel or tessellation-control function?\n"); fflush(stdout);)
					return;
				}
				
				// special case
				// => group_size.z == 1 ? (group_size.y == 1 ? 1 : 2) : 3
				const auto size_z = builder->CreateExtractElement(group_size, builder->getInt32(2));
				const auto size_y = builder->CreateExtractElement(group_size, builder->getInt32(1));
				const auto cmp_z = builder->CreateICmp(ICmpInst::ICMP_EQ, size_z, builder->getInt32(1));
				const auto cmp_y = builder->CreateICmp(ICmpInst::ICMP_EQ, size_y, builder->getInt32(1));
				const auto sel_x_or_y = builder->CreateSelect(cmp_y, builder->getInt32(1), builder->getInt32(2));
				const auto sel_xy_or_z = builder->CreateSelect(cmp_z, sel_x_or_y, builder->getInt32(3));
				I.replaceAllUsesWith(sel_xy_or_z);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.get_vertex_id.i32") {
				if(vertex_id == nullptr) {
					DBG(printf("failed to get vertex_id arg, probably not in a vertex function?\n"); fflush(stdout);)
					return;
				}
				
				I.replaceAllUsesWith(vertex_id);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.get_patch_id.i32") {
				if(patch_id == nullptr) {
					DBG(printf("failed to get patch_id arg, probably not in a tessellation-evaluation function?\n"); fflush(stdout);)
					return;
				}
				
				I.replaceAllUsesWith(patch_id);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.get_instance_id.i32") {
				if(instance_id == nullptr) {
					DBG(printf("failed to get instance_id arg, probably not in a vertex or tessellation-evaluation function?\n"); fflush(stdout);)
					return;
				}
				
				I.replaceAllUsesWith(instance_id);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.get_position_in_patch.float3") {
				if(position_in_patch == nullptr) {
					DBG(printf("failed to get position_in_patch arg, probably not in a tessellation-evaluation function?\n"); fflush(stdout);)
					return;
				}
			
				I.replaceAllUsesWith(position_in_patch);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.get_point_coord.float2") {
				if(point_coord == nullptr) {
					DBG(printf("failed to get point_coord arg, probably not in a fragment function?\n"); fflush(stdout);)
					return;
				}
			
				I.replaceAllUsesWith(point_coord);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.get_printf_buffer") {
				if(soft_printf == nullptr) {
					DBG(printf("failed to get printf_buffer arg, probably not in a kernel/vertex/fragment/tessellation function?\n"); fflush(stdout);)
					return;
				}
				
				// special case
				I.replaceAllUsesWith(soft_printf);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.get_primitive_id.i32") {
				if (primitive_id == nullptr) {
					llvm::errs() << "failed to get primitive_id arg, not in a fragment function or feature is not enabled\n";
					llvm::errs().flush();
					return;
				}
				
				I.replaceAllUsesWith(primitive_id);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.get_barycentric_coord.float3") {
				if (barycentric_coord == nullptr) {
					llvm::errs() << "failed to get barycentric_coord arg, not in a fragment function or feature is not enabled\n";
					llvm::errs().flush();
					return;
				}
			
				I.replaceAllUsesWith(barycentric_coord);
				I.eraseFromParent();
				return;
			}
			// unknown -> ignore for now
			else return;
			
			if(id == nullptr) {
				DBG(printf("failed to get id arg, probably not in a kernel function?\n"); fflush(stdout);)
				return;
			}
			
			// replace call with vector load / elem extraction from the appropriate vector
			I.replaceAllUsesWith(get_from_vector ? builder->CreateExtractElement(id, I.getOperand(0)) : id);
			I.eraseFromParent();
		}
		
		// performs some simple air.* call checks (e.g. if the call is valid in the current function type)
		void check_air_call(CallInst& CI) {
			const auto air_func = CI.getCalledFunction();
			const auto air_func_name = air_func->getName();
			if (air_func_name == "air.dfdx.f32" || air_func_name == "air.dfdy.f32" || air_func_name == "air.fwidth.f32" ||
				air_func_name == "air.discard_fragment") {
				if (!is_fragment_func) {
					llvm::errs() << "in " << func->getName() << ": calling '" << air_func_name << "' is only allowed inside a fragment shader\n";
					llvm::errs() << CI << "\n";
					llvm::errs().flush();
				}
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
		
		void visitAllocaInst(AllocaInst &AI) {
			if(!enable_intel_workarounds) return;
			DBG(errs() << "alloca: " << AI << ", " << *AI.getType() << "\n";)
			
			BasicAAResult BAR(createLegacyPMBasicAAResult(*this, *func));
			AAResults AA(createLegacyPMAAResults(*this, *func, BAR));
			
			// recursively find all users of this alloca + store all select and phi instructions that select/choose based on the alloca pointer
			std::vector<Instruction*> users;
			std::unordered_set<Instruction*> visited;
			const std::function<void(Instruction&)> collect_users = [&users, &collect_users, &visited, &AI, &AA](Instruction& I) {
				for(auto user : I.users()) {
					auto instr = cast<Instruction>(user);
					const auto has_visited = visited.insert(instr);
					if(!has_visited.second) continue;
					
					// TODO: ideally, we want to track all GEPs and bitcasts to/of the alloca and only add select/phi instructions that
					//       either use these or directly use the alloca (and not all pointers) - for now, AA will do
					if(SelectInst* SI = dyn_cast<SelectInst>(user)) {
						DBG(errs() << ">> select: " << *SI << "\n";)
						DBG(errs() << "cond: " << *SI->getCondition() << "\n";)
						DBG(errs() << "ops: " << *SI->getTrueValue() << ", " << *SI->getFalseValue() << "\n";)
						
						// skip immediately if not a pointer type
						if(SI->getTrueValue()->getType()->isPointerTy() /* false val has the same type */) {
							// check if either true or false alias with our alloca
							const auto aa_res_true = AA.alias(SI->getTrueValue(), &AI);
							const auto aa_res_false = AA.alias(SI->getFalseValue(), &AI);
							DBG(errs() << "aa: " << aa_res_true << ", " << aa_res_false << "\n";)
							if(aa_res_true != AliasResult::NoAlias ||
							   aa_res_false != AliasResult::NoAlias) {
								// if so, add this select
								users.push_back(SI);
							}
						}
					}
					else if(PHINode* PHI = dyn_cast<PHINode>(user)) {
						DBG(errs() << ">> phi: " << *PHI << "\n";)
						DBG(errs() << "type: " << *PHI->getType() << "\n";)
						
						// skip immediately if not a pointer type
						if(PHI->getType()->isPointerTy()) {
							// check if it aliases with our alloca
							const auto aa_res = AA.alias(PHI, &AI);
							DBG(errs() << "aa: " << aa_res << "\n";)
							if(aa_res != AliasResult::NoAlias) {
								// if so, add this phi node
								users.push_back(PHI);
							}
						}
					}
					collect_users(*instr);
				}
			};
			collect_users(AI);
			
			DBG({
				errs() << "####### users ##\n";
				for(const auto& user : users) {
					errs() << "user: " << *user << "\n";
				}
				errs() << "\n";
			})
			
			// select replacement strategy:
			// * create a tmp alloca that will later hold the selected data
			// * replace the select with two branches (true/false)
			// * depending on the select condition, branch to either true/false branch
			// * inside these branches, store the corresponding true/false value into our tmp alloca, then branch back to after the select
			// * remove the select
			const auto select_replace = [&](SelectInst* SI) {
				builder->SetInsertPoint(alloca_insert);
				auto tmp_alloca = builder->CreateAlloca(AI.getType()->getPointerElementType(), nullptr, "sel_tmp");
				tmp_alloca->setAlignment(AI.getAlign());
				
				// create our branch condition and true/false blocks that will replace the select
				auto bb_true = BasicBlock::Create(*ctx, "sel.true", func);
				auto bb_false = BasicBlock::Create(*ctx, "sel.false", func);
				builder->SetInsertPoint(SI);
				builder->CreateCondBr(SI->getCondition(), bb_true, bb_false);
				
				// split block before the select instruction so that we can branch back to it later
				auto bb_start = SI->getParent();
				auto bb_end = SI->getParent()->splitBasicBlock(SI);
				// remove automatically inserted branch instruction from parent, since we already have a branch instruction
				bb_start->getTerminator()->eraseFromParent();
				
				// create true/false branches that will copy the true/false data to our tmp alloca accordingly
				// -> true branch
				builder->SetInsertPoint(bb_true);
				builder->CreateStore(builder->CreateLoad(SI->getTrueValue()->getType()->getPointerElementType(), SI->getTrueValue()), tmp_alloca);
				builder->CreateBr(bb_end);
				
				// -> false branch
				builder->SetInsertPoint(bb_false);
				builder->CreateStore(builder->CreateLoad(SI->getFalseValue()->getType()->getPointerElementType(), SI->getFalseValue()), tmp_alloca);
				builder->CreateBr(bb_end);
				
				// cleanup, replace select instruction with our new alloca
				SI->replaceAllUsesWith(tmp_alloca);
				SI->eraseFromParent();
			};
			
			// phi replacement strategy:
			// * create a tmp alloca (pointer), this will be used to store all phi pointers
			// * iterate over all incoming values/pointers, then create a store of their pointer to the tmp pointer in their originating block
			// * create a load from the tmp alloca and replace all uses of the phi node with it
			// NOTE: loads and stores are volatile, so that no optimization can do any re-phi-ification(tm) later on
			const auto phi_replace = [&](PHINode* PHI) {
				auto phi_tmp_alloca = new AllocaInst(PHI->getType(), 0, nullptr, PHI->getName() + ".tmp", alloca_insert);
				
				for(uint32_t i = 0; i < PHI->getNumIncomingValues(); ++i) {
					auto origin = PHI->getIncomingBlock(i);
					new StoreInst(PHI->getIncomingValue(i), phi_tmp_alloca, true, origin->getTerminator());
				}
				
				auto load_repl = new LoadInst(PHI->getType(), phi_tmp_alloca, PHI->getName() + ".repl", true,
											  PHI->getParent()->getFirstNonPHI());
				PHI->replaceAllUsesWith(load_repl);
				PHI->eraseFromParent();
			};
			
			for(const auto& user : users) {
				if(SelectInst* SI = dyn_cast<SelectInst>(user)) {
					select_replace(SI);
				}
				else if(PHINode* PHI = dyn_cast<PHINode>(user)) {
					phi_replace(PHI);
				}
			}
			was_modified = !users.empty();
		}
		
	};
	
	// MetalFinalModuleCleanup:
	// * image storage class name replacement
	// * calling convention cleanup
	// * strip unused functions/prototypes/externs
	// * debug info cleanup
	struct MetalFinalModuleCleanup : public ModulePass {
		static char ID; // Pass identification, replacement for typeid
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		bool was_modified { false };
		
		MetalFinalModuleCleanup() : ModulePass(ID) {
			initializeMetalFinalModuleCleanupPass(*PassRegistry::getPassRegistry());
		}
		
		// this finds all libfloor image storage class structs and other structs, and replaces their names with the appropriate Apple Metal struct type name
		// NOTE: we need to do this, since Apple decided to handle these specially based on their name alone (e.g. no allocating additional registers)
		bool run_array_of_images_name_replacement() {
			std::vector<llvm::StructType*> image_storage_types;
			for (auto& st_type : ctx->pImpl->NamedStructTypes) {
				if (st_type.first().startswith("class.floor_image::image")) {
					image_storage_types.emplace_back(st_type.second);
				} else {
					// simple libfloor/std name -> Metal name replacement
					// NOTE: since we need to match the start of the name, we can't simply use a map here
					static const std::vector<std::pair<std::string, std::string>> simple_repl_lut {
						{ "struct.std::__1::array", "struct.metal::array" },
						{ "struct.triangle_tessellation_levels_t", "struct.metal::MTLTriangleTessellationFactorsHalf" },
						{ "struct.quad_tessellation_levels_t", "struct.metal::MTLQuadTessellationFactorsHalf" },
					};
					for (const auto& repl : simple_repl_lut) {
						if (st_type.first().startswith(repl.first)) {
							st_type.second->setName(repl.second);
							break;
						}
					}
				}
			}
			for (auto& st_type : image_storage_types) {
				if (st_type->getNumElements() != 1) {
					// we only expect a single element
					continue;
				}
				auto img_ptr_type = st_type->getElementType(0);
				if (!img_ptr_type->isPointerTy()) {
					// expected a pointer type
					continue;
				}
				auto img_type = dyn_cast_or_null<llvm::StructType>(img_ptr_type->getPointerElementType());
				if (!img_type || !img_type->isOpaque()) {
					// expected an opaque struct type
					continue;
				}
				
				// we already emit the correct texture opaque texture type name -> find the corresponding Metal struct name
				static const std::unordered_map<std::string, std::string> metal_name_lut {
					{ "struct._texture_1d_t", "struct.metal::texture1d" },
					{ "struct._texture_1d_array_t", "struct.metal::texture1d_array" },
					{ "struct._texture_2d_t", "struct.metal::texture2d" },
					{ "struct._texture_2d_array_t", "struct.metal::texture2d_array" },
					{ "struct._depth_2d_t", "struct.metal::depth2d" },
					{ "struct._depth_2d_array_t", "struct.metal::depth2d_array" },
					{ "struct._texture_2d_ms_t", "struct.metal::texture2d_ms" },
					{ "struct._texture_2d_ms_array_t", "struct.metal::texture2d_ms_array" },
					{ "struct._depth_2d_ms_t", "struct.metal::depth2d_ms" },
					{ "struct._depth_2d_ms_array_t", "struct.metal::depth2d_ms_array" },
					{ "struct._texture_cube_t", "struct.metal::texturecube" },
					{ "struct._texture_cube_array_t", "struct.metal::texturecube_array" },
					{ "struct._depth_cube_t", "struct.metal::depthcube" },
					{ "struct._depth_cube_array_t", "struct.metal::depthcube_array" },
					{ "struct._texture_3d_t", "struct.metal::texture3d" },
				};
				auto repl_iter = metal_name_lut.find(img_type->getName().str());
				if (repl_iter == metal_name_lut.end()) {
					continue;
				}
				
				//llvm::dbgs() << "replace " << st_type->getName().str() << " (" << img_type->getName().str() << ") -> " << repl_iter->second << "\n";
				st_type->setName(repl_iter->second);
			}
			
			return false;
		}
		
		bool runOnModule(Module& Mod) override {
			M = &Mod;
			ctx = &M->getContext();
			
			bool module_modified = run_array_of_images_name_replacement();
			
			// * strip floor_* calling convention from all functions and their users (replace it with C CC)
			// * kill all functions named floor.*
			// * strip debug info from declarations
			for (auto func_iter = Mod.begin(); func_iter != Mod.end();) {
				auto& func = *func_iter;
				if (func.getName().startswith("floor.")) {
					if (func.getNumUses() != 0) {
						errs() << func.getName() << " should not have any uses at this point!\n";
					}
					++func_iter; // inc before erase
					func.eraseFromParent();
					module_modified = true;
					continue;
				}
				
				if (func.getCallingConv() != CallingConv::C) {
					func.setCallingConv(CallingConv::C);
					for (auto user : func.users()) {
						if (auto CB = dyn_cast<CallBase>(user)) {
							CB->setCallingConv(CallingConv::C);
						}
					}
					module_modified = true;
				}
				
				if (func.isDeclaration()) {
					if (DISubprogram* sub_prog_dbg = func.getSubprogram(); sub_prog_dbg) {
						func.setSubprogram(nullptr);
						module_modified = true;
					}
				}
				
				++func_iter;
			}
			return module_modified;
		}
		
	};
	
}

char MetalFirst::ID = 0;
FunctionPass *llvm::createMetalFirstPass(const bool enable_intel_workarounds,
										 const bool enable_nvidia_workarounds) {
	return new MetalFirst(enable_intel_workarounds, enable_nvidia_workarounds);
}
INITIALIZE_PASS_BEGIN(MetalFirst, "MetalFirst", "MetalFirst Pass", false, false)
INITIALIZE_PASS_END(MetalFirst, "MetalFirst", "MetalFirst Pass", false, false)

char MetalFinal::ID = 0;
FunctionPass *llvm::createMetalFinalPass(const bool enable_intel_workarounds,
										 const bool enable_nvidia_workarounds) {
	return new MetalFinal(enable_intel_workarounds, enable_nvidia_workarounds);
}
INITIALIZE_PASS_BEGIN(MetalFinal, "MetalFinal", "MetalFinal Pass", false, false)
INITIALIZE_PASS_END(MetalFinal, "MetalFinal", "MetalFinal Pass", false, false)

char MetalFinalModuleCleanup::ID = 0;
ModulePass *llvm::createMetalFinalModuleCleanupPass() {
	return new MetalFinalModuleCleanup();
}
INITIALIZE_PASS_BEGIN(MetalFinalModuleCleanup, "MetalFinal module cleanup", "MetalFinal module cleanup Pass", false, false)
INITIALIZE_PASS_END(MetalFinalModuleCleanup, "MetalFinal module cleanup", "MetalFinal module cleanup Pass", false, false)
