//===- VulkanFinal.cpp - Vulkan final pass --------------------------------===//
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
// This file fixes certain post-codegen issues and transforms specific builtin
// functions with uses of new function arguments, as well as transforming
// input/output variables/arguments for later use in the SPIR-V backend.
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
#include "llvm/Analysis/ValueTracking.h"
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
#include "llvm/Transforms/LibFloor.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/LibFloor/AddressSpaceFix.h"
#include <algorithm>
#include <cstdarg>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <array>
using namespace llvm;

#define DEBUG_TYPE "VulkanFinal"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

namespace {
	// -> SPIRVInternal.h (can't include, b/c it's not in a proper path)
	static const uint32_t SPIRAS_Constant = 2;
	static const uint32_t SPIRAS_Uniform = 5;
	//static const uint32_t SPIRAS_Input = 6;
	static const uint32_t SPIRAS_Output = 7;
	static const uint32_t SPIRAS_StorageBuffer = 12;
	static const uint32_t SPIRAS_PhysicalStorageBuffer = 5349;

	// VulkanBuiltinParamHandling
	struct VulkanBuiltinParamHandling : public FunctionPass, InstVisitor<VulkanBuiltinParamHandling> {
		friend class InstVisitor<VulkanBuiltinParamHandling>;
		
		static char ID; // Pass identification, replacement for typeid
		
		std::shared_ptr<llvm::IRBuilder<>> builder;
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		bool is_kernel_func { false };
		bool is_vertex_func { false };
		bool is_fragment_func { false };
		bool is_tess_control_func { false };
		bool is_tess_eval_func { false };
		
		// added kernel function args
		Argument* global_id { nullptr };
		Argument* local_id { nullptr };
		Argument* group_id { nullptr };
		Argument* group_size { nullptr };
		GlobalVariable* workgroup_size { nullptr };
		
		// added general shader function args
		Argument* view_index { nullptr };
		
		// added vertex function args
		Argument* vertex_id { nullptr };
		Argument* instance_id { nullptr };
		
		// added fragment function args
		Argument* point_coord { nullptr };
		Argument* frag_coord { nullptr };
		Argument* primitive_id { nullptr };
		Argument* barycentric_coord { nullptr };
		
		// TODO: tessellation args!
		
		// any function args
		Argument* soft_printf { nullptr };
		
		VulkanBuiltinParamHandling() :
		FunctionPass(ID) {
			initializeVulkanBuiltinParamHandlingPass(*PassRegistry::getPassRegistry());
		}
		
		void getAnalysisUsage(AnalysisUsage &AU) const override {
			AU.addRequired<AAResultsWrapperPass>();
			AU.addRequired<GlobalsAAWrapperPass>();
			AU.addRequired<AssumptionCacheTracker>();
			AU.addRequired<TargetLibraryInfoWrapperPass>();
		}
		
		enum VULKAN_KERNEL_ARG_REV_IDX : int32_t {
			VULKAN_GLOBAL_ID = -4,
			VULKAN_LOCAL_ID = -3,
			VULKAN_GROUP_ID = -2,
			VULKAN_GROUP_SIZE = -1,
			
			VULKAN_KERNEL_ARG_COUNT = 4,
		};
		
		enum VULKAN_VERTEX_ARG_REV_IDX : int32_t {
			VULKAN_VERTEX_ID = -3,
			VULKAN_VERTEX_VIEW_INDEX = -2,
			VULKAN_INSTANCE_ID = -1,
			
			VULKAN_VERTEX_ARG_COUNT = 3,
		};
		
		enum VULKAN_FRAGMENT_ARG_REV_IDX : int32_t {
			VULKAN_POINT_COORD = -3,
			VULKAN_FRAG_COORD = -2,
			VULKAN_FRAGMENT_VIEW_INDEX = -1,
			
			VULKAN_FRAGMENT_ARG_COUNT = 3,
		};
		
		// TODO: tessellation arg enums/handling
		
		bool runOnFunction(Function &F) override {
			// exit if empty function
			if(F.empty()) return false;
			DBG(errs() << "in func: "; errs().write_escaped(F.getName()) << '\n';)
			
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
			
			//
			M = F.getParent();
			ctx = &M->getContext();
			func = &F;
			builder = std::make_shared<llvm::IRBuilder<>>(*ctx);
			
			const auto get_arg_by_idx = [&F](const int32_t& rev_idx) -> llvm::Argument* {
				auto arg_iter = F.arg_end();
				std::advance(arg_iter, rev_idx);
				return &*arg_iter;
			};
			
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
			
			DBG(errs() << "> adding built-in args ...\n";)
			// add args if this is a kernel function
			if(is_kernel_func) {
				if (F.arg_size() >= VULKAN_KERNEL_ARG_COUNT + (has_soft_printf ? 1 : 0)) {
					global_id = get_arg_by_idx(VULKAN_GLOBAL_ID);
					local_id = get_arg_by_idx(VULKAN_LOCAL_ID);
					group_id = get_arg_by_idx(VULKAN_GROUP_ID);
					group_size = get_arg_by_idx(VULKAN_GROUP_SIZE);
					if (has_soft_printf) {
						soft_printf = get_arg_by_idx(-(VULKAN_KERNEL_ARG_COUNT + 1));
					}
				} else {
					errs() << "invalid kernel function (" << F.getName() << ") argument count: " << F.arg_size() << "\n";
					global_id = nullptr;
					local_id = nullptr;
					group_id = nullptr;
					group_size = nullptr;
					soft_printf = nullptr;
				}
			}
			
			// add args if this is a vertex function
			if(is_vertex_func) {
				if (F.arg_size() >= VULKAN_VERTEX_ARG_COUNT + (has_soft_printf ? 1 : 0)) {
					// TODO: this should be optional / only happen on request
					vertex_id = get_arg_by_idx(VULKAN_VERTEX_ID);
					view_index = get_arg_by_idx(VULKAN_VERTEX_VIEW_INDEX);
					instance_id = get_arg_by_idx(VULKAN_INSTANCE_ID);
					if (has_soft_printf) {
						soft_printf = get_arg_by_idx(-(VULKAN_VERTEX_ARG_COUNT + 1));
					}
				} else {
					errs() << "invalid vertex function (" << F.getName() << ") argument count: " << F.arg_size() << "\n";
					vertex_id = nullptr;
					view_index = nullptr;
					instance_id = nullptr;
					soft_printf = nullptr;
				}
			}
			
			// add args if this is a fragment function
			if(is_fragment_func) {
				const uint32_t opt_arg_count = (has_soft_printf ? 1u : 0u) + (has_primitive_id ? 1u : 0u) + (has_barycentric_coord ? 1u : 0u);
				if (F.arg_size() >= VULKAN_FRAGMENT_ARG_COUNT + opt_arg_count) {
					point_coord = get_arg_by_idx(VULKAN_POINT_COORD);
					frag_coord = get_arg_by_idx(VULKAN_FRAG_COORD);
					view_index = get_arg_by_idx(VULKAN_FRAGMENT_VIEW_INDEX);
					
					// NOTE: reverse order!
					uint32_t opt_arg_counter = 1;
					if (has_barycentric_coord) {
						barycentric_coord = get_arg_by_idx(-(VULKAN_FRAGMENT_ARG_COUNT + opt_arg_counter++));
					} else {
						barycentric_coord = nullptr;
					}
					if (has_primitive_id) {
						primitive_id = get_arg_by_idx(-(VULKAN_FRAGMENT_ARG_COUNT + opt_arg_counter++));
					} else {
						primitive_id = nullptr;
					}
					if (has_soft_printf) {
						soft_printf = get_arg_by_idx(-(VULKAN_FRAGMENT_ARG_COUNT + opt_arg_counter++));
					} else {
						soft_printf = nullptr;
					}
				} else {
					errs() << "invalid fragment function (" << F.getName() << ") argument count: " << F.arg_size() << "\n";
					point_coord = nullptr;
					frag_coord = nullptr;
					view_index = nullptr;
					primitive_id = nullptr;
					barycentric_coord = nullptr;
					soft_printf = nullptr;
				}
			}
			
			// emit work-group size variable for this function (initialized externally at "run-time" / SPIR-V spec)
			if(is_kernel_func) {
				auto workgroup_size_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 3);
				workgroup_size = new GlobalVariable(*M,
													workgroup_size_type,
													true,
													GlobalVariable::ExternalLinkage,
													nullptr,
													F.getName().str() + ".vulkan_constant.workgroup_size",
													nullptr,
													GlobalValue::NotThreadLocal,
													0,
													true);
			}
			
			// visit everything in this function
			DBG(errs() << "> handling instructions ...\n";)
			visit(F);
			
			// always modified
			DBG(errs() << "> " << F.getName() << " done\n";)
			return true;
		}
		
		// InstVisitor overrides...
		using InstVisitor<VulkanBuiltinParamHandling>::visit;
		void visit(Instruction& I) {
			InstVisitor<VulkanBuiltinParamHandling>::visit(I);
		}
		
		//
		void visitCallInst(CallInst &I) {
			const auto func_name = I.getCalledFunction()->getName();
			if(!func_name.startswith("floor.builtin.")) return;
			
			builder->SetInsertPoint(&I);
			
			// figure out which one we need
			Argument* id;
			if(func_name == "floor.builtin.global_id.i32") {
				id = global_id;
			}
			else if(func_name == "floor.builtin.local_id.i32") {
				id = local_id;
			}
			else if(func_name == "floor.builtin.group_id.i32") {
				id = group_id;
			}
			else if(func_name == "floor.builtin.group_size.i32") {
				id = group_size;
			}
			else if(func_name == "floor.builtin.local_size.i32") {
				// this doesn't have a direct built-in equivalent, but must be loaded from the WorkgroupSize constant
				I.replaceAllUsesWith(builder->CreateExtractElement(builder->CreateLoad(workgroup_size->getType()->getPointerElementType(), workgroup_size), I.getOperand(0)));
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.global_size.i32") {
				// this doesn't have a direct built-in equivalent, but must be computed from the WorkgroupSize constant
				// TODO/NOTE: this might need some more work on the spir-v side, right now this is always constant folded
				auto dim_idx = I.getOperand(0);
				auto wg_size_dim = builder->CreateExtractElement(builder->CreateLoad(workgroup_size->getType()->getPointerElementType(), workgroup_size), dim_idx);
				auto grp_count_dim = builder->CreateExtractElement(builder->CreateLoad(group_size->getType()->getPointerElementType(), group_size), dim_idx);
				auto global_size_dim = builder->CreateMul(wg_size_dim, grp_count_dim);
				I.replaceAllUsesWith(global_size_dim);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.work_dim.i32") {
				if(group_size == nullptr) {
					DBG(printf("failed to get group_size arg, probably not in a kernel function?\n"); fflush(stdout);)
					return;
				}
				
				// special case
				// => group_size.z == 1 ? (group_size.y == 1 ? 1 : 2) : 3
				const auto loaded_group_size = builder->CreateLoad(group_size->getType()->getPointerElementType(), group_size);
				const auto size_z = builder->CreateExtractElement(loaded_group_size, builder->getInt32(2));
				const auto size_y = builder->CreateExtractElement(loaded_group_size, builder->getInt32(1));
				const auto cmp_z = builder->CreateICmp(ICmpInst::ICMP_EQ, size_z, builder->getInt32(1));
				const auto cmp_y = builder->CreateICmp(ICmpInst::ICMP_EQ, size_y, builder->getInt32(1));
				const auto sel_x_or_y = builder->CreateSelect(cmp_y, builder->getInt32(1), builder->getInt32(2));
				const auto sel_xy_or_z = builder->CreateSelect(cmp_z, sel_x_or_y, builder->getInt32(3));
				I.replaceAllUsesWith(sel_xy_or_z);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.vertex_id.i32") {
				if(vertex_id == nullptr) {
					DBG(printf("failed to get vertex_id arg, probably not in a vertex function?\n"); fflush(stdout);)
					return;
				}
				
				I.replaceAllUsesWith(builder->CreateLoad(vertex_id->getType()->getPointerElementType(), vertex_id, "vertex_index"));
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.instance_id.i32") {
				if(instance_id == nullptr) {
					DBG(printf("failed to get instance_id arg, probably not in a vertex function?\n"); fflush(stdout);)
					return;
				}
				
				I.replaceAllUsesWith(builder->CreateLoad(instance_id->getType()->getPointerElementType(), instance_id, "instance_index"));
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.point_coord.float2") {
				if(point_coord == nullptr) {
					DBG(printf("failed to get point_coord arg, probably not in a fragment function?\n"); fflush(stdout);)
					return;
				}
				
				I.replaceAllUsesWith(builder->CreateLoad(point_coord->getType()->getPointerElementType(), point_coord, "point_coord"));
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.frag_coord.float4") {
				if(frag_coord == nullptr) {
					DBG(printf("failed to get frag_coord arg, probably not in a fragment function?\n"); fflush(stdout);)
					return;
				}
				
				I.replaceAllUsesWith(builder->CreateLoad(frag_coord->getType()->getPointerElementType(), frag_coord, "frag_coord"));
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.get_printf_buffer") {
				if(soft_printf == nullptr) {
					DBG(printf("failed to get printf_buffer arg, probably not in a kernel/vertex/fragment function?\n"); fflush(stdout);)
					return;
				}
				
				// special case
				I.replaceAllUsesWith(soft_printf);
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.view_index.i32") {
				if(view_index == nullptr) {
					DBG(printf("failed to get view_index arg, probably not in a shader function?\n"); fflush(stdout);)
					return;
				}
				
				I.replaceAllUsesWith(builder->CreateLoad(view_index->getType()->getPointerElementType(), view_index, "view_index"));
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.primitive_id.i32") {
				if (primitive_id == nullptr) {
					llvm::errs() << "failed to get primitive_id arg, not in a fragment function or feature is not enabled\n";
					llvm::errs().flush();
					return;
				}
				
				I.replaceAllUsesWith(builder->CreateLoad(primitive_id->getType()->getPointerElementType(), primitive_id, "primitive_id"));
				I.eraseFromParent();
				return;
			}
			else if(func_name == "floor.builtin.barycentric_coord.float3") {
				if (barycentric_coord == nullptr) {
					llvm::errs() << "failed to get barycentric_coord arg, not in a fragment function or feature is not enabled\n";
					llvm::errs().flush();
					return;
				}
				
				I.replaceAllUsesWith(builder->CreateLoad(barycentric_coord->getType()->getPointerElementType(), barycentric_coord, "barycentric_coord"));
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
			I.replaceAllUsesWith(builder->CreateExtractElement(builder->CreateLoad(id->getType()->getPointerElementType(), id), I.getOperand(0)));
			I.eraseFromParent();
		}
	};
	
	// VulkanFinal
	struct VulkanFinal : public FunctionPass, InstVisitor<VulkanFinal> {
		friend class InstVisitor<VulkanFinal>;
		
		static char ID; // Pass identification, replacement for typeid
		
		std::shared_ptr<llvm::IRBuilder<>> builder;
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		bool is_kernel_func { false };
		bool is_vertex_func { false };
		bool is_fragment_func { false };
		bool is_tess_control_func { false };
		bool is_tess_eval_func { false };
		
		VulkanFinal() :
		FunctionPass(ID) {
			initializeVulkanFinalPass(*PassRegistry::getPassRegistry());
		}
		
		void getAnalysisUsage(AnalysisUsage &AU) const override {
			AU.addRequired<AAResultsWrapperPass>();
			AU.addRequired<GlobalsAAWrapperPass>();
			AU.addRequired<AssumptionCacheTracker>();
			AU.addRequired<TargetLibraryInfoWrapperPass>();
		}
		
		// retrieves the target Vulkan version from metadata
		static std::pair<uint32_t, uint32_t> get_vulkan_version(Module& M) {
			const llvm::NamedMDNode *VulkanVersion =
			M.getNamedMetadata("vulkan.version");
			if (VulkanVersion == nullptr || VulkanVersion->getNumOperands() != 1) {
				return {};
			}
			
			const MDNode *vulkan_version_md = VulkanVersion->getOperand(0);
			if (vulkan_version_md->getNumOperands() < 2) {
				return {};
			}
			
			uint64_t version_major = 0, version_minor = 0;
			
			const MDOperand &version_major_op = vulkan_version_md->getOperand(0);
			if (const ConstantAsMetadata *version_major_md =
				dyn_cast_or_null<ConstantAsMetadata>(version_major_op.get())) {
				if (const ConstantInt *version_major_int =
					dyn_cast_or_null<ConstantInt>(version_major_md->getValue())) {
					version_major = version_major_int->getZExtValue();
				} else {
					return {};
				}
			} else {
				return {};
			}
			
			const MDOperand &version_minor_op = vulkan_version_md->getOperand(1);
			if (const ConstantAsMetadata *version_minor_md =
				dyn_cast_or_null<ConstantAsMetadata>(version_minor_op.get())) {
				if (const ConstantInt *version_minor_int =
					dyn_cast_or_null<ConstantInt>(version_minor_md->getValue())) {
					version_minor = version_minor_int->getZExtValue();
				} else {
					return {};
				}
			} else {
				return {};
			}
			
			return { version_major, version_minor };
		}

		bool runOnFunction(Function &F) override {
			// exit if empty function
			if(F.empty()) return false;
			DBG(errs() << "in func: "; errs().write_escaped(F.getName()) << '\n';)
			
			// determine this function type + exit if it isn't a kernel or shader function
			is_kernel_func = F.getCallingConv() == CallingConv::FLOOR_KERNEL;
			is_vertex_func = F.getCallingConv() == CallingConv::FLOOR_VERTEX;
			is_fragment_func = F.getCallingConv() == CallingConv::FLOOR_FRAGMENT;
			is_tess_control_func = F.getCallingConv() == CallingConv::FLOOR_TESS_CONTROL;
			is_tess_eval_func = F.getCallingConv() == CallingConv::FLOOR_TESS_EVAL;
			if(!is_kernel_func && !is_vertex_func && !is_fragment_func) return false;
			
			//
			M = F.getParent();
			ctx = &M->getContext();
			func = &F;
			builder = std::make_shared<llvm::IRBuilder<>>(*ctx);
			
			//const auto vulkan_version = get_vulkan_version(*M);
			
			// handle return value / output
			DBG(errs() << "> handling return values ...\n";)
			if(is_vertex_func || is_fragment_func || is_tess_eval_func) {
				const auto emit_output_var = [this](const std::string& var_name,
													llvm::Type* global_type,
													uint32_t address_space,
													bool is_constant,
													Constant* initializer) {
					auto GV = new GlobalVariable(*M,
												 global_type,
												 is_constant,
												 GlobalVariable::InternalLinkage,
												 initializer,
												 var_name,
												 nullptr,
												 GlobalValue::NotThreadLocal,
												 address_space);
					return GV;
				};
				
				const auto ret_type = F.getReturnType();
				const auto func_name = F.getName().str();
				uint32_t return_idx = 0;
				if(ret_type->isStructTy()) {
					const auto st_type = cast<llvm::StructType>(ret_type);
					for(const auto& elem : st_type->elements()) {
						emit_output_var(func_name + ".vulkan_output." + std::to_string(return_idx++),
										elem, SPIRAS_Output, false, nullptr);
					}
				}
				else if(ret_type->isArrayTy()) {
					emit_output_var(func_name + ".vulkan_output.0", ret_type, SPIRAS_Output, false, nullptr);
				}
				else if(!ret_type->isVoidTy()) {
					emit_output_var(func_name + ".vulkan_output.0", ret_type, SPIRAS_Output, false, nullptr);
				}
				// else: nothing/passthrough
			}
			
			// visit everything in this function
			DBG(errs() << "> handling instructions ...\n";)
			visit(F);
			
			DBG(errs() << "> updating function signature / parameters ...\n";)
			{
				// update function signature / parameters:
				//  * change the return type to void (vs/fs returns have already been modified)
				//  * transform constant AS pointers to either Uniform or StorageBuffer AS
				//  * transform StorageBuffer AS image pointers to Uniform AS
				//  * enclose non-struct Uniform parameters in a struct (note that enclosing SSBOs happens later)
				//  * transform pointers inside argument buffers to PhysicalStorageBuffer AS
				// NOTE: must be called after visiting rets and other ret type/val users
				std::vector<Type*> param_types;
				DBG(errs() << "func: " << F.getName().str() << "\n";)
				for (auto& arg : F.args()) {
					auto arg_type = arg.getType();
					DBG(errs() << "\targ: " << arg.getName().str() << ": " << *arg_type;)
					
					// arg type must be a pointer
					if (const auto ptr_type = dyn_cast<llvm::PointerType>(arg_type);
						ptr_type && (ptr_type->getAddressSpace() == SPIRAS_Constant || ptr_type->getAddressSpace() == SPIRAS_StorageBuffer)) {
						const auto ptr_as = ptr_type->getAddressSpace();
						auto elem_type = ptr_type->getElementType();
						const auto arg_idx = param_types.size();
						
						// handle IUBs
						const auto iub_attr = F.getAttributeAtIndex(llvm::AttributeList::FirstArgIndex + arg_idx, "vulkan_iub");
						const auto is_iub = (iub_attr.getRawPointer() != nullptr);
						DBG(
							if (is_iub) {
								errs() << " (IUB)";
							}
						)
						
						// handle arg buffers
						const auto ab_attr = F.getAttributeAtIndex(llvm::AttributeList::FirstArgIndex + arg_idx, "vulkan_arg_buffer");
						const auto is_arg_buffer = (ab_attr.getRawPointer() != nullptr);
						DBG(
							if (is_arg_buffer) {
								errs() << " (arg-buffer)";
							}
						)
						assert(!(is_arg_buffer && is_iub) && "IUB and AB are mutually exclusive");
						assert(!is_arg_buffer || (is_arg_buffer && elem_type->isStructTy()) && "AB element type must be a struct");
						
						// since there is a limit on how many IUBs we can have and how large they can be, some arguments might fall back to using SSBOs
						const auto is_ssbo_uniform = (!is_iub && !is_arg_buffer && arg.onlyReadsMemory() &&
													  (arg.hasAttribute(Attribute::Dereferenceable) ||
													   arg.hasAttribute(Attribute::DereferenceableOrNull)));
						
						// any image/opaque type is unsized
						const auto is_sized = elem_type->isSized();
						
						// decide storage class
						const auto storage_class = (is_iub || !is_sized ? SPIRAS_Uniform : SPIRAS_StorageBuffer);
						
						// transform storage class if necessary +
						// for IUBs/SSBO-Uniform: enclose in struct if the element type is not a struct
						if ((is_iub || is_ssbo_uniform) && !elem_type->isStructTy()) {
							llvm::Type* st_elems[] { elem_type };
							elem_type = llvm::StructType::create(*ctx, st_elems, "enclose." + arg.getName().str());
							arg_type = elem_type->getPointerTo(storage_class);
							arg.mutateType(arg_type);
							
							// replace users, should usually only have one load
							llvm::Value* idx_list[] {
								llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx), 0),
								llvm::ConstantInt::get(llvm::Type::getInt32Ty(*ctx), 0),
							};
							std::vector<User*> users;
							for(auto* user : arg.users()) {
								users.emplace_back(user);
							}
							for (auto& user : users) {
								if (auto instr = dyn_cast<Instruction>(user)) {
									if (isa<LoadInst>(instr)) {
										auto elem_gep = llvm::GetElementPtrInst::CreateInBounds(arg.getType()->getScalarType()->getPointerElementType(),
																								&arg, idx_list, "", instr);
										auto repl_instr = new LoadInst(elem_gep->getType()->getPointerElementType(), elem_gep, instr->getName(), false, instr);
										repl_instr->setDebugLoc(instr->getDebugLoc());
										instr->replaceAllUsesWith(repl_instr);
										instr->eraseFromParent();
									} else {
										DBG(errs() << "\nunhandled arg user: " << *instr << "\n";)
										DBG(errs().flush();)
										assert(false && "unhandled arg user");
									}
								} else {
									DBG(errs() << "\narg user is not an instruction\n";)
									DBG(errs().flush();)
									assert(false && "arg user is not an instruction");
								}
							}
						} else if (storage_class != ptr_as) {
							arg_type = elem_type->getPointerTo(storage_class);
							arg.mutateType(arg_type);
							
							// update users
							std::vector<User*> users;
							for (auto* user : arg.users()) {
								users.emplace_back(user);
							}
							std::vector<ReturnInst*> returns; // returns to fix -> there shouldn't be any here
							for (auto& user : users) {
								if (auto instr = dyn_cast<Instruction>(user)) {
									fix_instruction_users(*ctx, *instr, arg, storage_class, returns);
								}
							}
							assert(returns.empty() && "unexpected return type change");
						}

						// transform argument buffer struct and uses
						if (is_arg_buffer) {
							// note that we only need to cover the pointers at the top-level of the struct (i.e. either direct pointers or array of pointers)
							auto elem_struct_type = dyn_cast<llvm::StructType>(elem_type);
							std::vector<Type*> adj_struct_member_types;
							bool struct_needs_adjustment = false;
							for (auto& member_type : elem_struct_type->elements()) {
								if (member_type->isPointerTy()) {
									auto member_ptr_type = dyn_cast<llvm::PointerType>(member_type);
									if (member_ptr_type->getAddressSpace() != SPIRAS_StorageBuffer) {
										ctx->emitError("unsupported pointer address space in argument buffer of function " + func->getName().str());
										return false;
									}
									
									member_ptr_type = member_ptr_type->getElementType()->getPointerTo(SPIRAS_PhysicalStorageBuffer);
									adj_struct_member_types.emplace_back(member_ptr_type);
									struct_needs_adjustment = true;
								} else if (member_type->isArrayTy()) {
									auto arr_elem_type = member_type->getArrayElementType();
									if (!arr_elem_type->isPointerTy()) {
										adj_struct_member_types.emplace_back(member_type);
										continue;
									}
									
									auto arr_elem_ptr_type = dyn_cast<llvm::PointerType>(arr_elem_type);
									if (arr_elem_ptr_type->getAddressSpace() != SPIRAS_StorageBuffer) {
										ctx->emitError("unsupported pointer address space in buffer array in argument buffer of function " + func->getName().str());
										return false;
									}
									
									arr_elem_ptr_type = arr_elem_ptr_type->getElementType()->getPointerTo(SPIRAS_PhysicalStorageBuffer);
									arr_elem_type = llvm::ArrayType::get(arr_elem_ptr_type, member_type->getArrayNumElements());
									adj_struct_member_types.emplace_back(arr_elem_type);
									struct_needs_adjustment = true;
								} else {
									adj_struct_member_types.emplace_back(member_type);
								}
							}
							if (struct_needs_adjustment) {
								auto adj_struct_type = llvm::StructType::create(*ctx, adj_struct_member_types, elem_struct_type->getName().str() + ".adj");
								arg_type = adj_struct_type->getPointerTo(storage_class);
								arg.mutateType(arg_type);
								
								// update users
								std::vector<User*> users;
								for (auto* user : arg.users()) {
									users.emplace_back(user);
								}
								for (auto& user : users) {
									// all users should just be GEPs
									if (auto GEP = dyn_cast<GetElementPtrInst>(user)) {
										const auto result_type = GEP->getResultElementType();
										if (!result_type->isPointerTy() || result_type->getPointerAddressSpace() != SPIRAS_StorageBuffer) {
											continue;
										}
										
										// modify GEP types to use the correct address spaces
										auto new_result_type = result_type->getPointerElementType()->getPointerTo(SPIRAS_PhysicalStorageBuffer);
										auto new_gep_type = PointerType::get(new_result_type, SPIRAS_StorageBuffer);
										GEP->mutateType(new_gep_type);
										GEP->setResultElementType(new_result_type);
										
										// fix GEP users
										std::vector<User*> gep_users;
										for (auto* gep_user : GEP->users()) {
											gep_users.emplace_back(gep_user);
										}
										for (auto& gep_user : gep_users) {
											if (auto gep_user_instr = dyn_cast<Instruction>(gep_user)) {
												std::vector<ReturnInst*> returns; // returns to fix -> there shouldn't be any here
												fix_instruction_users(*ctx, *gep_user_instr, *GEP, SPIRAS_PhysicalStorageBuffer, returns);
												assert(returns.empty() && "unexpected return type change");
											}
										}
									} else {
										DBG(errs() << "\nunhandled AB user: " << *user << "\n";)
										DBG(errs().flush();)
										assert(false && "unhandled AB user");
									}
								}
							}
						}

						DBG(errs() << " -> " << *arg_type;)
					}
					DBG(errs() << "\n";)
					
					param_types.push_back(arg_type);
				}

				auto new_func_type = FunctionType::get(llvm::Type::getVoidTy(*ctx), param_types, false);
				F.mutateType(PointerType::get(new_func_type, 0));
				F.mutateFunctionType(new_func_type);
			}
			
			// always modified
			DBG(errs() << "> " << F.getName() << " done\n";)
			return true;
		}
		
		// InstVisitor overrides...
		using InstVisitor<VulkanFinal>::visit;
		void visit(Instruction& I) {
			InstVisitor<VulkanFinal>::visit(I);
		}
		
		// prefer i32 indices so that we don't need the Int64 capability
		void visitExtractElement(ExtractElementInst& EEI) {
			const auto idx_op = EEI.getIndexOperand();
			const auto idx_type = idx_op->getType();
			if(!idx_type->isIntegerTy(32)) {
				if(const auto const_idx_op = dyn_cast_or_null<ConstantInt>(idx_op)) {
					EEI.setOperand(1 /* idx op */, builder->getInt32((int32_t)const_idx_op->getValue().getZExtValue()));
				}
				// else: can't do anything, b/c other int type would still be used when casting
			}
		}
		
		// prefer i32 indices so that we don't need the Int64 capability
		void visitInsertElement(InsertElementInst& IEI) {
			const auto idx_op = IEI.llvm::User::getOperand(2);
			const auto idx_type = idx_op->getType();
			if(!idx_type->isIntegerTy(32)) {
				if(const auto const_idx_op = dyn_cast_or_null<ConstantInt>(idx_op)) {
					IEI.setOperand(2 /* idx op */, builder->getInt32((int32_t)const_idx_op->getValue().getZExtValue()));
				}
				// else: can't do anything, b/c other int type would still be used when casting
			}
		}
		
		void visitAllocaInst(AllocaInst &AI) {
			// TODO: no pointers to pointers in vulkan
		}
		
		void visitReturnInst(ReturnInst &RI) {
			if(!is_vertex_func && !is_fragment_func && !is_tess_eval_func) return;
			
			auto ret_val = RI.getReturnValue();
			const auto ret_type = ret_val->getType();
			if(ret_val == nullptr) return;
			
			DebugLoc DL;
			if(auto ret_instr = dyn_cast<Instruction>(ret_val)) {
				DL = ret_instr->getDebugLoc();
			}
			
			const auto get_output_var = [this](uint32_t& idx) -> GlobalValue* {
				const auto name = func->getName().str() + ".vulkan_output." + std::to_string(idx++);
				auto output_var = M->getNamedValue(name);
				if(output_var == nullptr) {
					errs() << "output variable \"" << name << "\" doesn't exist\n";
					return nullptr;
				}
				return output_var;
			};
			
			uint32_t ret_idx = 0;
			if(ret_type->isStructTy()) {
				// struct -> split up to individual field stores
				const auto st_type = cast<llvm::StructType>(ret_type);
				for(uint32_t i = 0; i < st_type->getNumElements(); ++i) {
					auto output_var = get_output_var(ret_idx);
					if(output_var == nullptr) return;
					
					SmallVector<uint32_t, 1> idx_list { ret_idx - 1 };
					auto st_extract = ExtractValueInst::Create(ret_val, idx_list, "ret.extract", &RI);
					auto st = new StoreInst(st_extract, output_var, false, &RI);
					if(DL) st->setDebugLoc(DL);
				}
			}
			else {
				// else: array, scalars, vectors -> direct store
				auto output_var = get_output_var(ret_idx);
				if(output_var == nullptr) return;
				auto st = new StoreInst(ret_val, output_var, false, &RI);
				if(DL) st->setDebugLoc(DL);
			}
			
			// clear return
			ReturnInst::Create(*ctx, RI.getParent());
			RI.eraseFromParent();
		}
	};
	
	// VulkanPreFinal
	struct VulkanPreFinal : public FunctionPass, InstVisitor<VulkanPreFinal> {
		friend class InstVisitor<VulkanPreFinal>;
		
		static char ID; // Pass identification, replacement for typeid
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		bool is_kernel_func { false };
		bool is_vertex_func { false };
		bool is_fragment_func { false };
		bool is_tess_control_func { false };
		bool is_tess_eval_func { false };
		bool was_modified { false };
		ConstantFolder folder;
		
		// function input/originating pointers (i.e. parameters + allocas)
		std::unordered_set<Value*> input_ptrs;
		std::vector<GetElementPtrInst*> gep_ptrs;
		std::vector<SelectInst*> select_ptrs;
		std::vector<PHINode*> phi_ptrs;
		
		VulkanPreFinal() :
		FunctionPass(ID) {
			initializeVulkanPreFinalPass(*PassRegistry::getPassRegistry());
		}
		
		void getAnalysisUsage(AnalysisUsage &AU) const override {
			AU.addRequired<AAResultsWrapperPass>();
			AU.addRequired<GlobalsAAWrapperPass>();
			AU.addRequired<AssumptionCacheTracker>();
			AU.addRequired<TargetLibraryInfoWrapperPass>();
			AU.addRequired<AssumptionCacheTracker>();
			AU.addRequired<DominatorTreeWrapperPass>();
		}
		
		bool runOnFunction(Function &F) override {
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
			
			//
			M = F.getParent();
			ctx = &M->getContext();
			func = &F;
			input_ptrs.clear();
			gep_ptrs.clear();
			select_ptrs.clear();
			phi_ptrs.clear();
			
			// store parameter pointers + globals used in this function
			for(auto& arg : F.args()) {
				if(arg.getType()->isPointerTy()) {
					input_ptrs.emplace(&arg);
				}
			}
			for (auto& GV : M->globals()) {
				if(!GV.getType()->isPointerTy()) continue;
				for(const auto& user : GV.users()) {
					if(Instruction* instr = dyn_cast_or_null<Instruction>(user)) {
						if(instr->getParent()->getParent() == &F) {
							input_ptrs.emplace(&GV);
							break;
						}
					}
				}
			}
			
			// gather all stuff
			was_modified = false;
			visit(F);
			
			// NOTE: disabled for now as we don't need this any more (probably ...)
			// handle everything
			//handle_pointers();
			
			return was_modified;
		}
		
		// InstVisitor overrides...
		using InstVisitor<VulkanPreFinal>::visit;
		void visit(Instruction& I) {
			InstVisitor<VulkanPreFinal>::visit(I);
		}
		
		void visitAllocaInst(AllocaInst& AI) {
			// store allocas
			input_ptrs.emplace(&AI);
		}
		
		// fuses "base_gep" and "gep" to a single GEP, possibly replacing "gep" with the new GEP
		// NOTE: for simple cases, the returned GEP can be the input "gep" (with indices replaced)
		std::pair<GetElementPtrInst*, bool /* abort */>
		fuse_geps(GetElementPtrInst* base_gep, GetElementPtrInst* gep, const bool replace_old_gep) {
			const auto fuse_index = [this](Value* lhs, Value* rhs, Instruction* insert_pos) -> Value* {
				ConstantInt* lhs_const = dyn_cast_or_null<ConstantInt>(lhs);
				ConstantInt* rhs_const = dyn_cast_or_null<ConstantInt>(rhs);
				if(lhs_const != nullptr && rhs_const != nullptr) {
					// both are constant -> simply fold them
					return folder.CreateAdd(lhs_const, rhs_const);
				}
				else if(lhs_const != nullptr && lhs_const->getZExtValue() == 0) {
					// lhs idx is 0
					return rhs;
				}
				else if(rhs_const != nullptr && rhs_const->getZExtValue() == 0) {
					// rhs idx is 0
					return lhs;
				}
				else {
					// need to create a proper addition
					std::string name = ".gep.idx.add";
					if(rhs->hasName()) name = rhs->getName().str() + name;
					if(lhs->hasName()) name = lhs->getName().str() + name;
					return BinaryOperator::CreateAdd(lhs, rhs, name, insert_pos);
				}
			};
			
			const auto base_idx_count = base_gep->getNumIndices();
			const auto idx_count = gep->getNumIndices();
			
			DBG(errs() << ">>> fusing: " << *base_gep << " + " << *gep << "\n";)
			
			if(base_idx_count == 1 && replace_old_gep) {
				// -> base is just an offset pointer, can simply fuse both GEPs
				DBG(errs() << ">> fused (simple): " << *gep;)
				
				// replace pointer base
				gep->setOperand(0, base_gep->getPointerOperand());
				
				// fuse first index
				gep->setOperand(1, fuse_index(base_gep->getOperand(1), gep->getOperand(1), gep));
				
				DBG(errs() << ", with " << *gep << "\n";)
				return { gep, false };
			}
			else {
				// -> need to create a new GEP so that it can contain all indices from both GEPs
				// NOTE: last idx in base + first in current always matches -> -1
				const auto fused_idx_count = base_idx_count + idx_count - 1;
				
				// create fused idx list
				SmallVector<Value*, 4> idx_list;
				
				// add base indices
				for(uint32_t i = 1, count = base_gep->getNumIndices() + 1; i < count; ++i) {
					idx_list.push_back(base_gep->getOperand(i));
				}
				
				// fust last (base) + first (current) idx
				idx_list[idx_list.size() - 1] = fuse_index(idx_list.back(), gep->getOperand(1), gep);
				
				// add current indices
				for(uint32_t i = 2, count = gep->getNumIndices() + 1; i < count; ++i) {
					idx_list.push_back(gep->getOperand(i));
				}
				assert(idx_list.size() == fused_idx_count && "invalid idx list size");
				
				// create the new GEP
				auto gep_ptr = base_gep->getPointerOperand();
				auto gep_ptr_type = cast<PointerType>(gep_ptr->getType()->getScalarType())->getElementType();
				if (const auto idx_type = GetElementPtrInst::getIndexedType(gep_ptr_type, idx_list); !idx_type) {
					// can't handle it -> ignore it
					// NOTE: since variable pointers are now used by default, PtrAccessChain is possible -> we don't necessarily
					// need fused GEPs (but it's still a good thing that enables other transformations)
					return { gep, true };
				}
				auto fused_gep = GetElementPtrInst::CreateInBounds(gep_ptr_type, gep_ptr, idx_list,
																   gep->getName() + ".gep.fused", gep);
				fused_gep->setDebugLoc(gep->getDebugLoc());
				
				// replace this with new fused one
				DBG(errs() << ">> fused: replacing " << *gep << ", with " << *fused_gep << "\n";)
				if(replace_old_gep) {
					gep->replaceAllUsesWith(fused_gep);
					gep->eraseFromParent();
				}
				return { fused_gep, false };
			}
		}
		
		void visitGetElementPtrInst(GetElementPtrInst &I) {
			// prefer i32 indices so that we don't need the Int64 capability
			for(auto& op : I.operands()) {
				if(op->getType()->isIntegerTy() &&
				   !op->getType()->isIntegerTy(32)) {
					if(const auto const_idx_op = dyn_cast_or_null<ConstantInt>(op)) {
						op.set(ConstantInt::get(Type::getInt32Ty(*ctx),
												(int32_t)const_idx_op->getValue().getZExtValue()));
					}
					else {
						// TODO: does this make sense? would need Int64 cap either way
						op.set(CastInst::CreateIntegerCast(op, Type::getInt32Ty(*ctx), false, "gep.idx.i32.cast", &I));
					}
				}
			}
			
			// GEP fusion: we can't have GEPs into GEPs, so fuse them
			// NOTE: this has to be done recursively of course
			GetElementPtrInst* output_gep = &I;
			bool did_fuse = false;
			do {
				did_fuse = false;
				if(GetElementPtrInst* GEP = dyn_cast_or_null<GetElementPtrInst>(output_gep->getPointerOperand())) {
					bool abort = false;
					std::tie(output_gep, abort) = fuse_geps(GEP, output_gep, true /* replace_old_gep */);
					if (abort) {
						break;
					}
					did_fuse = true;
				}
			} while(did_fuse);
			
			gep_ptrs.push_back(output_gep);
		}
		
		void visitSelectInst(SelectInst& SI) {
			// we're only interested in pointer selects, as these aren't supported in vulkan
			if(!SI.getType()->isPointerTy()) {
				return;
			}
			select_ptrs.emplace_back(&SI);
		}
		
		void visitPHINode(PHINode& PHI) {
			// we're only interested in pointer selects, as these aren't supported in vulkan
			if(!PHI.getType()->isPointerTy()) {
				return;
			}
			phi_ptrs.emplace_back(&PHI);
		}
		
		CallInst* insert_keep_block_marker(BasicBlock* keep_block) {
			Function* keep_block_func = M->getFunction("floor.keep_block");
			if(keep_block_func == nullptr) {
				FunctionType* keep_block_type = FunctionType::get(llvm::Type::getVoidTy(*ctx), false);
				keep_block_func = (Function*)M->getOrInsertFunction("floor.keep_block", keep_block_type).getCallee();
				keep_block_func->setCallingConv(CallingConv::FLOOR_FUNC);
				keep_block_func->setCannotDuplicate();
				keep_block_func->setDoesNotThrow();
				keep_block_func->setNotConvergent();
				keep_block_func->setDoesNotRecurse();
			}
			CallInst* keep_block_call = CallInst::Create(keep_block_func, "", keep_block->getTerminator());
			keep_block_call->setCallingConv(CallingConv::FLOOR_FUNC);
			return keep_block_call;
		}
		
		void handle_pointers() {
			DBG(errs() << "####################\n## in " << func->getName() << "\n";
				for(const auto& iptr : input_ptrs) {
					errs() << "input: " << *iptr << "\n";
				}
				for(const auto& gep : gep_ptrs) {
					errs() << "GEP: " << *gep << "\n";
				}
				for(const auto& sel : select_ptrs) {
					errs() << "select: " << *sel << "\n";
				}
				for(const auto& phi : phi_ptrs) {
					errs() << "phi: " << *phi << "\n";
				}
			)
			was_modified = true; // TODO: do this properly
			
			//
			std::vector<GetElementPtrInst*> problem_geps;
			for(const auto& gep : gep_ptrs) {
				if(input_ptrs.count(gep->getPointerOperand()) == 0) {
					problem_geps.emplace_back(gep);
					continue;
				}
			}
			DBG(errs() << "\n";
				for(const auto& gep : problem_geps) {
					errs() << "PROBLEM GEP: " << *gep << "\n";
				}
			)
			
			// needed for finding the origin pointers
			auto DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
			DT->recalculate(*func);
			LoopInfo LI(*DT);
			
			// find set of problem instructions and their pointer origin(s)
			std::deque<Instruction*> problem_instrs;
			problem_instrs.insert(begin(problem_instrs), begin(select_ptrs), end(select_ptrs));
			problem_instrs.insert(begin(problem_instrs), begin(phi_ptrs), end(phi_ptrs));
			problem_instrs.insert(begin(problem_instrs), begin(problem_geps), end(problem_geps));
			
			struct ptr_set {
				// pointer origin(s)
				std::unordered_set<const Value*> src;
				// pointer producers (PHIs, selects, GEPs)
				// TODO: pointer bitcasts should be handled previously
				std::unordered_set<Instruction*> producers;
				// pointer consumers (loads, stores)
				std::unordered_set<Instruction*> consumers;
			};
			std::vector<std::shared_ptr<ptr_set>> ptr_sets;
			std::unordered_map<Instruction*, ptr_set*> ptr_set_map;
			std::unordered_multimap<const Value*, ptr_set*> origin_map;
			
			// create initial ptr_sets from our "problem" instructions
			for(; !problem_instrs.empty(); problem_instrs.pop_front()) {
				const auto& instr = problem_instrs[0];
				
				// already handled elsewhere?
				if(ptr_set_map.count(instr) > 0) continue;
				
				// figure out which origin pointer(s) this comes from
				SmallVector<const Value*, 3> origins;
				DBG(errs() << "\n-> " << *instr << "\n";)
				getUnderlyingObjects(instr, origins, &LI, 0);
				DBG(for(const auto& orig : origins) {
					errs() << "     origin: " << *orig << "\n";
				})
				if(origins.empty()) {
					ctx->emitError(instr, "instruction has no origin");
					return;
				}
				
				// check if we have a ptr_set for this origin combination yet
				ptr_set* pset = nullptr;
				auto orig_range = origin_map.equal_range(origins[0]);
				for(auto oiter = orig_range.first; oiter != orig_range.second; ++oiter) {
					// same size?
					if(oiter->second->src.size() != origins.size()) {
						continue;
					}
					
					// same content?
					bool equal_set = true;
					for(const auto& origin : origins) {
						if(oiter->second->src.count(origin) == 0) {
							equal_set = false;
							break;
						}
					}
					if(!equal_set) continue;
					
					// found a ptr_set that matches
					pset = oiter->second;
					break;
				}
				
				// create a ptr_set if none exists yet
				if(pset == nullptr) {
					auto new_ptr_set = std::make_shared<ptr_set>();
					ptr_sets.push_back(new_ptr_set);
					pset = new_ptr_set.get();
					
					pset->src.insert(origins.begin(), origins.end());
					for(const auto& origin : origins) {
						origin_map.emplace(origin, pset);
					}
				}
				
				// add this instruction
				pset->producers.emplace(instr);
				ptr_set_map.emplace(instr, pset);
				
				// we must also handle the pointers contained within instructions
				// -> this makes sure that we recursively get the complete net/graph of pointers
				if(PHINode* phi = dyn_cast_or_null<PHINode>(instr)) {
					for(const auto& phi_inc_val : phi->incoming_values()) {
						// only add the value/instruction if we haven't handled it yet + if it isn't a input ptr
						const auto phi_inc_instr = dyn_cast_or_null<Instruction>(&phi_inc_val);
						if(phi_inc_instr != nullptr &&
						   input_ptrs.count(phi_inc_instr) == 0 &&
						   ptr_set_map.count(phi_inc_instr) == 0) {
							problem_instrs.emplace_back(phi_inc_instr);
						}
					}
				}
				else if(SelectInst* sel = dyn_cast_or_null<SelectInst>(instr)) {
					const auto true_instr = dyn_cast_or_null<Instruction>(sel->getTrueValue());
					const auto false_instr = dyn_cast_or_null<Instruction>(sel->getFalseValue());
					if(true_instr != nullptr &&
					   input_ptrs.count(true_instr) == 0 &&
					   ptr_set_map.count(true_instr) == 0) {
						problem_instrs.emplace_back(true_instr);
					}
					if(false_instr != nullptr &&
					   input_ptrs.count(false_instr) == 0 &&
					   ptr_set_map.count(false_instr) == 0) {
						problem_instrs.emplace_back(false_instr);
					}
				}
				else if(GetElementPtrInst* GEP = dyn_cast_or_null<GetElementPtrInst>(instr)) {
					const auto gep_instr = dyn_cast_or_null<Instruction>(GEP->getPointerOperand());
					if(gep_instr != nullptr &&
					   input_ptrs.count(gep_instr) == 0 &&
					   ptr_set_map.count(gep_instr) == 0) {
						problem_instrs.emplace_back(gep_instr);
					}
				}
			}
			
			// find all consumers of our producers
			for(const auto& instr_pset : ptr_set_map) {
				const auto& instr = instr_pset.first;
				auto& pset = instr_pset.second;
				
				for(const auto& user : instr->users()) {
					// only need to consider direct users this time
					if(Instruction* user_instr = dyn_cast_or_null<Instruction>(user)) {
						if(isa<GetElementPtrInst>(user_instr) ||
						   isa<SelectInst>(user_instr) ||
						   isa<PHINode>(user_instr)) {
							// not interested in these, should already be handled elsewhere
							const auto pset_iter = ptr_set_map.find(user_instr);
							assert(pset_iter != ptr_set_map.end() && "unknown producer");
							assert(pset_iter->second->producers.count(user_instr) > 0 && "unhandled producer");
							continue;
						}
						// NOTE: we don't care about what kind of instructions consumers are, because
						// we can simply iterate over all operands of the instruction and replace the
						// producer(s) accordingly
						pset->consumers.emplace(user_instr);
					}
				}
			}
			
			// debug output
			DBG(errs() << "\n\n## ptr sets:\n";
				for(const auto& pset : ptr_sets) {
					errs() << "# set\n";
					errs() << "origins:\n";
					for(const auto& origin : pset->src) {
						errs() << "\t" << *origin << "\n";
					}
					errs() << "producers:\n";
					for(const auto& prod : pset->producers) {
						errs() << "\t" << *prod << "\n";
					}
					errs() << "consumers:\n";
					for(const auto& cons : pset->consumers) {
						errs() << "\t" << *cons << "\n";
					}
					
					errs() << "\n";
				}
			)
			
			//
			const auto idx_type = Type::getInt32Ty(*ctx);
			const auto idx_zero = ConstantInt::get(idx_type, 0);
			for(const auto& pset : ptr_sets) {
				// ignore set if there are no consumers (that we need to handle)
				if(pset->consumers.empty()) continue;
				
				if(pset->src.size() == 1) {
					// simple case with a single origin ptr
					// -> can replace everything with indices
					
					// ptr -> index map
					std::unordered_map<Instruction*, Instruction*> index_map;
					
					// TODO: handle necessary index count (> 1)
					// * check which op#s are constant (throughout?) -> no need to handle them
					// * for non-constant -> use array? n-vector? multiple scalar indices?
					
					// create indices / replace pointers with integers - due to chain dependencies this is a bit tricky
					const auto replace_with_idx = [&pset, &index_map, &idx_zero](Value* val) -> Value* {
						// is this an origin pointer - if so, index is 0
						if(pset->src.count(val) > 0) {
							return idx_zero;
						}
						
						// otherwise it must be an instruction
						const auto instr = dyn_cast_or_null<Instruction>(val);
						assert(instr != nullptr && "must be an instruction");
						
						const auto idx_iter = index_map.find(instr);
						assert(idx_iter != index_map.end() && "no index for this instruction");
						
						return idx_iter->second;
					};
					
					bool all_indices_created = false;
					while(!all_indices_created) {
						DBG(errs() << "... index round ...\n";)
						all_indices_created = true;
						for(const auto& prod : pset->producers) {
							if(index_map.count(prod) > 0) continue;
							
							Instruction* idx = nullptr;
							if(GetElementPtrInst* GEP = dyn_cast_or_null<GetElementPtrInst>(prod)) {
								const auto base_ptr = GEP->getPointerOperand();
								const auto base_ptr_instr = dyn_cast_or_null<Instruction>(base_ptr);
								if(pset->src.count(base_ptr) > 0 ||
								   (base_ptr_instr != nullptr && index_map.count(base_ptr_instr) > 0)) {
									idx = BinaryOperator::CreateAdd(replace_with_idx(base_ptr), GEP->getOperand(1),
																	GEP->getName() + ".idx", GEP);
								}
								else {
									// can't handle this yet - do another round
									DBG(errs() << " -- can't handle yet: " << *GEP << "\n";)
									all_indices_created = false;
									continue;
								}
							}
							else if(PHINode* phi = dyn_cast_or_null<PHINode>(prod)) {
								// NOTE: we will make sure this contains the proper incoming values later
								idx = PHINode::Create(idx_type, phi->getNumIncomingValues(),
													  phi->getName() + ".idx", phi);
							}
							else if(SelectInst* sel = dyn_cast_or_null<SelectInst>(prod)) {
								// NOTE: we will make sure this contains the proper true/false values later
								idx = SelectInst::Create(sel->getCondition(), idx_zero, idx_zero);
							}
							
							if (auto dbg_loc = prod->getDebugLoc(); dbg_loc) {
								idx->setDebugLoc(dbg_loc);
							}
							index_map.emplace(prod, idx);
							DBG(errs() << "idx: " << *prod << " -> " << *idx << "\n";)
						}
					}
					
					// phi/select index fixup now that we have created all indices
					for(const auto& prod : pset->producers) {
						if(PHINode* phi = dyn_cast_or_null<PHINode>(prod)) {
							auto idx_phi = dyn_cast<PHINode>(index_map[prod]);
							for(uint32_t i = 0, count = phi->getNumIncomingValues(); i < count; ++i) {
								idx_phi->addIncoming(replace_with_idx(phi->getIncomingValue(i)),
													 phi->getIncomingBlock(i));
							}
							DBG(errs() << "updated phi: " << *idx_phi << "\n";)
						}
						else if(SelectInst* sel = dyn_cast_or_null<SelectInst>(prod)) {
							auto idx_sel = dyn_cast<SelectInst>(index_map[prod]);
							idx_sel->setTrueValue(replace_with_idx(sel->getTrueValue()));
							idx_sel->setFalseValue(replace_with_idx(sel->getFalseValue()));
							DBG(errs() << "updated sel: " << *idx_sel << "\n";)
						}
					}
					
					// materialize pointers/GEPs for users (from indices)
					auto base_ptr = *pset->src.begin();
					for(Instruction* cons : pset->consumers) {
						DBG(errs() << ">> cons before: " << *cons << "\n";)
						for(uint32_t i = 0, count = cons->getNumOperands(); i < count; ++i) {
							const auto op = cons->getOperand(i);
							const auto idx_iter = index_map.find((Instruction*)op);
							if(idx_iter != index_map.end()) {
								// replace with new materialized GEP
								SmallVector<Value*, 3> idx_list;
								idx_list.push_back(idx_zero);
								idx_list.push_back(idx_iter->second);
								auto val_ptr = const_cast<Value*>(base_ptr);
								auto mat_gep = GetElementPtrInst::CreateInBounds(val_ptr->getType()->getScalarType()->getPointerElementType(),
																				 val_ptr, idx_list,
																				 op->getName() + ".idx.mat.gep", cons);
								if(Instruction* op_instr = dyn_cast_or_null<Instruction>(op)) {
									mat_gep->setDebugLoc(op_instr->getDebugLoc());
								}
								DBG(errs() << "> consumer op replace: " << *op << " -> " << *mat_gep << "\n";)
								cons->setOperand(i, mat_gep);
							}
						}
						DBG(errs() << ">> cons after: " << *cons << "\n";)
					}
				}
				else if(pset->src.size() > 1) {
					// complex case with 2 or more origin ptrs
					
					// go down the condition tree until we hit a leaf / origin pointer, in between:
					//  * for GEPs: store them in a chain (in the same order as they were encountered) and pass/copy
					//              them through to each tree branch (so that each branch has its individual chain)
					//  * for SELs: create if/else branches for each SEL (using the same condition as the SEL),
					//              then recursively go down each branch, meeting up again after the SEL instruction,
					//              also insert a PHI if the consumer produces a value (replace consumer with that at the end)
					//  * for PHIs: not handled yet - need to create a new integer condition, then use that for branching?
					//  * for origin ptrs: fuse the GEP chain up to that point, replace pointer with the origin pointer (now
					//                     unambiguous), replace use of producer pointer in the consumer with the new GEP
					
					// (upside-down) condition tree
					struct condition_tree {
						enum class TYPE : uint32_t {
							COMPARE_BOOL,
							COMPARE_PHI,
							ORIGIN_PTR,
							GEP,
						};
						TYPE type;
						
						// if COMPARE_BOOL: i1 compare value
						// if COMPARE_PHI: i32 compare value
						// if ORIGIN_PTR: actual origin ptr (+this is a leaf node)
						// if GEP: intermediate GEP
						Value* val;
						
						// pointers to the next condition node(s) or actual origin ptrs (if they are leaf nodes)
						std::vector<condition_tree> nodes;
					};
					uint32_t leaf_count = 0;
					const std::function<void(Value*, condition_tree&)> create_condition_tree =
					[&create_condition_tree, &pset, &leaf_count, this](Value* prod, condition_tree& node) {
						// origin ptr: attach as leaf node
						if(pset->src.count(prod) > 0) {
							node.type = condition_tree::TYPE::ORIGIN_PTR;
							node.val = prod;
							++leaf_count;
							return;
						}
						
						// intermediate GEP, continue with pointer op
						if(GetElementPtrInst* GEP = dyn_cast_or_null<GetElementPtrInst>(prod)) {
							node.type = condition_tree::TYPE::GEP;
							node.val = GEP;
							
							condition_tree child;
							create_condition_tree(GEP->getPointerOperand(), child);
							node.nodes.push_back(child);
						}
						// select, store condition + continue traversal with true and false value
						else if(SelectInst* sel = dyn_cast_or_null<SelectInst>(prod)) {
							node.type = condition_tree::TYPE::COMPARE_BOOL;
							node.val = sel->getCondition();
							
							// NOTE: must be in this order
							condition_tree true_child, false_child;
							create_condition_tree(sel->getTrueValue(), true_child);
							create_condition_tree(sel->getFalseValue(), false_child);
							node.nodes.push_back(true_child);
							node.nodes.push_back(false_child);
						}
						else if(PHINode* phi = dyn_cast_or_null<PHINode>(prod)) {
							// TODO: handle PHIs!
							node.type = condition_tree::TYPE::COMPARE_PHI;
							ctx->emitError(phi, "PHI nodes are not handled yet");
							return;
						}
						else {
							if(Instruction* instr = dyn_cast_or_null<Instruction>(prod)) {
								ctx->emitError(instr, "unhandled producer while creating condition tree");
							}
							else {
								ctx->emitError("unhandled non-instruction producer value while creating condition tree");
							}
						}
					};
					
					// for debugging purposes
					const std::function<void(const condition_tree&, const uint32_t)> dump_condition_tree =
					[&dump_condition_tree](const condition_tree& node, const uint32_t level) {
						errs() << std::string(level, '-') << "> ";
						switch(node.type) {
							case condition_tree::TYPE::COMPARE_BOOL:
								errs() << "SELECT: " << *node.val << "\n";
								dump_condition_tree(node.nodes[0], level + 1);
								dump_condition_tree(node.nodes[1], level + 1);
								break;
							case condition_tree::TYPE::COMPARE_PHI:
								errs() << "PHI: " << *node.val << "\n";
								for(const auto& child : node.nodes) {
									dump_condition_tree(child, level + 1);
								}
								break;
							case condition_tree::TYPE::GEP:
								errs() << "GEP: " << *node.val << "\n";
								dump_condition_tree(node.nodes[0], level + 1);
								break;
							case condition_tree::TYPE::ORIGIN_PTR:
								errs() << "LEAF: " << *node.val << "\n";
								break;
						}
					};
					
					for(Instruction* cons : pset->consumers) {
						// find producer
						Instruction* producer = nullptr;
						for(uint32_t i = 0, count = cons->getNumOperands(); i < count; ++i) {
							const auto op = cons->getOperand(i);
							const auto piter = pset->producers.find((Instruction*)op);
							if(piter != pset->producers.end()) {
								producer = *piter;
								break;
							}
						}
						assert(producer != nullptr && "consumer has no producer");
						DBG(errs() << "-> cons/prod: " << *cons << " -> " << *producer << "\n";)
						
						// find the condition(s) on which we need to branch
						condition_tree cond_tree;
						create_condition_tree(producer, cond_tree);
						DBG(dump_condition_tree(cond_tree, 0); errs() << "\n";)
						
						// TODO: once switches are supported, combine all conditions to a single large switch,
						// instead of creating a condition tree
						// TODO: try to merge multiple users to a single block per origin instead of per GEP/user
						
						BasicBlock* continue_block = nullptr; // code/block _after_ the consumer instruction
						PHINode* continue_phi = nullptr;
						const std::function<void(const condition_tree&, std::vector<GetElementPtrInst*>, Instruction*)>
						handle_condition_tree = [this, &handle_condition_tree, &continue_block, &continue_phi, &leaf_count,
												 &cons, &producer](const condition_tree& node,
																   std::vector<GetElementPtrInst*> gep_chain,
																   Instruction* branch_term) {
							switch(node.type) {
								case condition_tree::TYPE::COMPARE_BOOL: {
									DBG(errs() << "@SEL: " << *node.val << "\n";)
									// select: create an if-else branch
									Value* cond = node.val;
									
									Instruction* true_term = nullptr;
									Instruction* false_term = nullptr;
									if(continue_block == nullptr) {
										// create the first split and final continue block
										SplitBlockAndInsertIfThenElse(cond, cons, &true_term, &false_term);
										assert(true_term != nullptr && "failed to create true branch/term");
										assert(false_term != nullptr && "failed to create false branch/term");
										continue_block = cons->getParent();
										continue_block->setName("continue.block");
										
										// insert continue phi if the consumer produces a value
										if(!cons->getType()->isVoidTy()) {
											continue_phi = PHINode::Create(cons->getType(), leaf_count, "continue.phi",
																		   cons->getNextNode());
										}
									}
									else {
										// TODO: implement this
										assert(false && "not implemented yet");
										ctx->emitError("multi-select not implemented yet");
										return;
									}
									
									true_term->getParent()->setName("sel.true");
									false_term->getParent()->setName("sel.false");
									
									// copy GEP chain into both branches
									handle_condition_tree(node.nodes[0], gep_chain, true_term);
									handle_condition_tree(node.nodes[1], gep_chain, false_term);
									break;
								}
								case condition_tree::TYPE::COMPARE_PHI: {
									DBG(errs() << "@PHI: " << *node.val << "\n";)
									ctx->emitError("PHI nodes are not handled yet");
									break;
								}
								case condition_tree::TYPE::GEP: {
									DBG(errs() << "@GEP: " << *node.val << "\n";)
									// add GEP to chain and continue
									gep_chain.emplace_back(dyn_cast<GetElementPtrInst>(node.val));
									handle_condition_tree(node.nodes[0], std::move(gep_chain), branch_term);
									break;
								}
								case condition_tree::TYPE::ORIGIN_PTR: {
									DBG(errs() << "@PTR: " << *node.val << "\n";)
									// leaf: finally reached an origin pointer
									assert(branch_term != nullptr && "must have a branch terminator");
									
									// fuse GEPs with origin ptr
									GetElementPtrInst* mat_gep = (GetElementPtrInst*)gep_chain[0]->clone();
									mat_gep->setOperand(0, node.val);
									mat_gep->setName(gep_chain.back()->getName());
									mat_gep->setDebugLoc(gep_chain.back()->getDebugLoc());
									for(size_t i = 1, count = gep_chain.size(); i < count; ++i) {
										bool abort = false;
										std::tie(mat_gep, abort) = fuse_geps(mat_gep, gep_chain[i], false); // TODO
										if (abort) {
											break;
										}
									}
									mat_gep->insertBefore(branch_term);
									DBG(errs() << ">> mat_gep: " << *mat_gep << "\n";)
									
									// copy consumer
									auto cons_clone = cons->clone();
									if(cons->hasName()) {
										cons_clone->setName(cons->getName());
									}
									cons_clone->setDebugLoc(cons->getDebugLoc());
									cons_clone->insertBefore(branch_term);
									
									// replace consumer ptr
									DBG(errs() << ">> cons before: " << *cons_clone << "\n";)
									for(uint32_t i = 0, count = cons_clone->getNumOperands(); i < count; ++i) {
										const auto op = cons_clone->getOperand(i);
										if(op == producer) {
											cons_clone->setOperand(i, mat_gep);
										}
									}
									DBG(errs() << ">> cons after: " << *cons_clone << "\n";)
									
									// branch to continue block
									auto br_inst = dyn_cast_or_null<BranchInst>(branch_term);
									assert(br_inst != nullptr && "invalid terminator");
									assert(br_inst->isUnconditional() && "branch must be unconditional");
									br_inst->setSuccessor(0, continue_block);
									
									// update continue phi (if there is one)
									if(continue_phi != nullptr) {
										continue_phi->addIncoming(cons_clone, cons_clone->getParent());
										DBG(errs() << ">> updated phi: " << *continue_phi << "\n";)
									}
									break;
								}
							}
						};
						handle_condition_tree(cond_tree, {}, nullptr);
						
						if(continue_phi != nullptr) {
							cons->replaceAllUsesWith(continue_phi);
						}
						cons->eraseFromParent();
					}
				}
			}
			
			// TODO: cleanup / DCE (will be done at the end anyways, but might be better to do here already)
		}
	};
	
	// VulkanFinalModuleCleanup:
	// * strip unused functions/prototypes/externs
	struct VulkanFinalModuleCleanup : public ModulePass {
		static char ID; // Pass identification, replacement for typeid
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		
		VulkanFinalModuleCleanup() : ModulePass(ID) {
			initializeVulkanFinalModuleCleanupPass(*PassRegistry::getPassRegistry());
		}
		
		bool runOnModule(Module& Mod) override {
			M = &Mod;
			ctx = &M->getContext();
			
			// kill all functions named floor.builtin.* (we still need other floor.* functions)
			bool module_modified = false;
			for(auto func_iter = Mod.begin(); func_iter != Mod.end();) {
				auto& func = *func_iter;
				if(func.getName().startswith("floor.builtin.")) {
					if(func.getNumUses() != 0) {
						errs() << func.getName() << " should not have any uses at this point!\n";
					}
					++func_iter; // inc before erase
					func.eraseFromParent();
					module_modified = true;
					continue;
				}
				++func_iter;
			}
			return module_modified;
		}
		
	};
	
}

char VulkanFinal::ID = 0;
FunctionPass *llvm::createVulkanFinalPass() {
	return new VulkanFinal();
}
INITIALIZE_PASS_BEGIN(VulkanFinal, "VulkanFinal", "VulkanFinal Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(VulkanFinal, "VulkanFinal", "VulkanFinal Pass", false, false)

char VulkanBuiltinParamHandling::ID = 0;
FunctionPass *llvm::createVulkanBuiltinParamHandlingPass() {
	return new VulkanBuiltinParamHandling();
}
INITIALIZE_PASS_BEGIN(VulkanBuiltinParamHandling, "VulkanBuiltinParamHandling", "VulkanBuiltinParamHandling Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_END(VulkanBuiltinParamHandling, "VulkanBuiltinParamHandling", "VulkanBuiltinParamHandling Pass", false, false)

char VulkanPreFinal::ID = 0;
FunctionPass *llvm::createVulkanPreFinalPass() {
	return new VulkanPreFinal();
}
INITIALIZE_PASS_BEGIN(VulkanPreFinal, "VulkanPreFinal", "VulkanPreFinal Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(VulkanPreFinal, "VulkanPreFinal", "VulkanPreFinal Pass", false, false)

char VulkanFinalModuleCleanup::ID = 0;
ModulePass *llvm::createVulkanFinalModuleCleanupPass() {
	return new VulkanFinalModuleCleanup();
}
INITIALIZE_PASS_BEGIN(VulkanFinalModuleCleanup, "VulkanFinal module cleanup", "VulkanFinal module cleanup Pass", false, false)
INITIALIZE_PASS_END(VulkanFinalModuleCleanup, "VulkanFinal module cleanup", "VulkanFinal module cleanup Pass", false, false)
