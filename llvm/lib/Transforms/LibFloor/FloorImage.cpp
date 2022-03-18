//===- FloorImage.cpp - base class for image transformations --------------===//
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
// This class defines and implements the base class for all
// image transformations (CUDA and opaque, as used for Metal, OpenCL and Vulkan).
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/AliasAnalysis.h"
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
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/LibFloor.h"
#include "llvm/Transforms/LibFloor/FloorImage.h"
#include <cxxabi.h>
using namespace llvm;

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

FloorImageBasePass::FloorImageBasePass(char &ID,
									   const IMAGE_TYPE_ID& image_type_id_,
									   const uint32_t& image_capabilities_)
: FunctionPass(ID), image_type_id(image_type_id_),
image_read_prefix(image_type_id == IMAGE_TYPE_ID::CUDA ? "floor.cuda.read_image." : "floor.opaque.read_image."),
image_write_prefix(image_type_id == IMAGE_TYPE_ID::CUDA ? "floor.cuda.write_image." : "floor.opaque.write_image."),
image_get_dim_prefix(image_type_id == IMAGE_TYPE_ID::CUDA ? "floor.cuda.get_image_dim" : "floor.opaque.get_image_dim"),
image_capabilities((IMAGE_CAPABILITY)image_capabilities_) {
}

bool FloorImageBasePass::runOnFunction(Function &F) {
	// exit if empty function
	if(F.empty()) return false;
	
	// reset
	M = F.getParent();
	ctx = &M->getContext();
	func = &F;
	builder = std::make_shared<llvm::IRBuilder<>>(*ctx);
	was_modified = false;
	
	// check triple
	const auto triple = llvm::Triple(M->getTargetTriple());
	if (triple.getArch() == Triple::ArchType::air64) {
		is_metal = true;
		uint32_t major_version = 0, minor_version = 0, micro_version = 0;
		if (triple.getOS() == Triple::OSType::IOS) {
			triple.getiOSVersion(major_version, minor_version, micro_version);
			if (major_version >= 14) {
				is_metal_2_3 = true;
			}
			if (major_version >= 15) {
				is_metal_2_4 = true;
			}
		} else if (triple.getOS() == Triple::OSType::MacOSX) {
			if (triple.getMacOSXVersion(major_version, minor_version, micro_version)) {
				if (major_version >= 11) {
					is_metal_2_3 = true;
				}
				if (major_version >= 12) {
					is_metal_2_4 = true;
				}
			}
		}
	}
	
	{
		AttrBuilder attr_builder;
		attr_builder.addAttribute(llvm::Attribute::NoUnwind);
		attr_builder.addAttribute(llvm::Attribute::ReadNone);
		nounwind_readnone_attr = AttributeList::get(*ctx, ~0, attr_builder);
	}
	{
		AttrBuilder attr_builder;
		attr_builder.addAttribute(llvm::Attribute::NoUnwind);
		nounwind_attr = AttributeList::get(*ctx, ~0, attr_builder);
	}
	
	// visit everything in this function
	DBG(errs() << "in func: "; errs().write_escaped(F.getName()) << '\n';)
	is_fragment_shader = (F.getCallingConv() == CallingConv::FLOOR_FRAGMENT);
	
	if (!triple.isNVPTX()) {
		visit(F);
		return was_modified;
	} else {
		// for CUDA we may make drastic changes to the BBs and instructions in them
		// -> can no longer use instruction vistor
		// -> do this on our own with hard restart if we modified anything
		bool was_modified_any = false, restart = true;
		while (restart) {
			restart = false;
			for (auto bb = func->begin(); bb != func->end(); ++bb) {
				for (auto instr = bb->begin(); instr != bb->end(); ++instr) {
					if (auto CB = dyn_cast_or_null<CallBase>(&*instr); CB) {
						visitCallBase(*CB);
						
						// restart!
						if (was_modified) {
							was_modified_any = true;
							was_modified = false;
							restart = true;
							break;
						}
					}
				}
				if (restart) {
					break;
				}
			}
		}
		return was_modified_any;
	}
}

void FloorImageBasePass::visit(Instruction& I) {
	InstVisitor<FloorImageBasePass>::visit(I);
}

void FloorImageBasePass::visitCallBase(CallBase& CB) {
	const auto func = CB.getCalledFunction();
	if(!func) return;
	
	const auto full_func_name = func->getName();
	if(full_func_name.startswith(image_read_prefix) ||
	   full_func_name.startswith(image_write_prefix)) {
		// strip off .(i|f)(1|2|3) from the end of the func name
		const auto func_name = full_func_name.rsplit('.').first;
		handle_image(CB, func_name);
		was_modified = true;
	} else if (full_func_name.startswith(image_get_dim_prefix)) {
		handle_image_query(CB, full_func_name);
		was_modified = true;
	}
}

void FloorImageBasePass::handle_image(CallBase& CB, const StringRef& func_name) {
	builder->SetInsertPoint(&CB);
	const bool is_image_read = func_name.startswith(image_read_prefix);
	
	/* args for cuda and opaque read/write functions:
	 
	 CUDA read:
	 uint64_t tex, COMPUTE_IMAGE_TYPE type,
	 coord_vec_type coord, uint32_t layer, uint32_t sample, offset_vec_type offset,
	 int32_t lod_i, float lod_or_bias_f, bool is_lod, bool is_lod_float, bool is_bias,
	 gradient_vec_type dpdx, gradient_vec_type dpdy, bool is_gradient,
	 COMPARE_FUNCTION compare_function, float compare_value, bool is_compare
	 
	 CUDA write:
	 uint64_t surf, COMPUTE_IMAGE_TYPE type, coord_vec_type coord, uint32_t layer, uint32_t lod, bool is_lod, data_vec_type data, COMPUTE_IMAGE_TYPE rt_type
	 
	 opaque read:
	 image_t img, sampler_type smplr, COMPUTE_IMAGE_TYPE type,
	 coord_vec_type coord, uint32_t layer, uint32_t sample, offset_vec_type offset,
	 int32_t lod_i, float lod_or_bias_f, bool is_lod, bool is_lod_float, bool is_bias,
	 gradient_vec_type dpdx, gradient_vec_type dpdy, bool is_gradient,
	 COMPARE_FUNCTION compare_function, float compare_value, bool is_compare
	 
	 opaque write:
	 image_t img, COMPUTE_IMAGE_TYPE type, coord_vec_type coord, uint32_t layer, uint32_t lod, bool is_lod, data_vec_type data
	 
	 */
	
	const uint32_t read_arg_count = (image_type_id == IMAGE_TYPE_ID::CUDA ? 17 : 18);
	const uint32_t write_arg_count = (image_type_id == IMAGE_TYPE_ID::CUDA ? 8 : 7);
	
	// as args are largely the same for cuda and opaque, just offset the arg num by 1 for opaque,
	// instead of doing this individually for each arg.
	// also: only offset for image reads, as image writes are identical.
	const uint32_t read_args_offset = (image_type_id == IMAGE_TYPE_ID::CUDA ? 0 : 1);
	const uint32_t write_args_offset = 0;
	const uint32_t args_offset = (is_image_read ? read_args_offset : write_args_offset);
	
	// check + get arguments
	if(is_image_read && CB.arg_size() != read_arg_count) {
		ctx->emitError(&CB, func_name + ": invalid argument count (expected " + std::to_string(read_arg_count) + ")");
		return;
	}
	if(!is_image_read && CB.arg_size() != write_arg_count) {
		ctx->emitError(&CB, func_name + ": invalid argument count (expected " + std::to_string(write_arg_count) + ")");
		return;
	}
	
	// -> tex/surf/img handle
	const auto img_handle_arg = CB.getOperand(0);
	if(image_type_id == IMAGE_TYPE_ID::CUDA &&
	   !img_handle_arg->getType()->isIntegerTy()) {
		ctx->emitError(&CB, "invalid image handle type (must be integer)");
		return;
	}
	else if(image_type_id == IMAGE_TYPE_ID::OPAQUE) {
		if(!img_handle_arg->getType()->isPointerTy()) {
			ctx->emitError(&CB, "invalid image handle type (must be an image pointer)");
			return;
		}
		// TODO: check opaque? check opencl image type?
	}
	
	// -> type enum
	const auto image_type_arg = dyn_cast_or_null<ConstantInt>(CB.getOperand(1 + args_offset));
	if(!image_type_arg) {
		ctx->emitError(&CB, "image type argument must be a constant value");
		return;
	}
	if(!image_type_arg->getType()->isIntegerTy()) {
		ctx->emitError(&CB, "invalid image-type type (must be enum/integer)");
		return;
	}
	const uint32_t image_channel_count = ((uint32_t(image_type_arg->getZExtValue()) &
										   uint32_t(COMPUTE_IMAGE_TYPE::__CHANNELS_MASK)) >>
										  uint32_t(COMPUTE_IMAGE_TYPE::__CHANNELS_SHIFT)) + 1u;
	const auto full_image_type = COMPUTE_IMAGE_TYPE(image_type_arg->getZExtValue());
	const COMPUTE_IMAGE_TYPE image_type = full_image_type & COMPUTE_IMAGE_TYPE::BASE_TYPE_MASK;
	const COMPUTE_IMAGE_TYPE format_type = full_image_type & COMPUTE_IMAGE_TYPE::__FORMAT_MASK;
	const COMPUTE_IMAGE_TYPE image_data_type = full_image_type & COMPUTE_IMAGE_TYPE::__DATA_TYPE_MASK;
	const bool is_normalized = has_flag<COMPUTE_IMAGE_TYPE::FLAG_NORMALIZED>(full_image_type);
	const bool is_depth = has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(full_image_type);
	
	// -> coord
	const auto coord_arg = CB.getOperand(2 + args_offset);
	const auto coord_arg_type = dyn_cast<FixedVectorType>(coord_arg->getType());
	if (!coord_arg_type) {
		ctx->emitError(&CB, "invalid image coordinate type");
		return;
	}
	if (!coord_arg_type->getElementType()->isFloatTy() && !coord_arg_type->getElementType()->isIntegerTy()) {
		ctx->emitError(&CB, "invalid image coordinate type");
		return;
	}
	const auto coord_dim = coord_arg_type->getNumElements();
	
	// -> layer
	const auto layer_arg = CB.getOperand(3 + args_offset);
	if(!layer_arg->getType()->isIntegerTy()) {
		ctx->emitError(&CB, "invalid image layer index type (must be integer)");
		return;
	}
	
	if(is_image_read) {
		// -> sampler
		// prefer const sampler / either const or dyn will be nullptr
		llvm::ConstantInt* const_sampler_arg = nullptr;
		llvm::Value* dyn_sampler_arg = nullptr;
		if(image_type_id != IMAGE_TYPE_ID::CUDA) {
			const_sampler_arg = dyn_cast_or_null<ConstantInt>(CB.getOperand(1));
			if(const_sampler_arg == nullptr) {
				dyn_sampler_arg = CB.getOperand(1);
			}
		}
		
		// -> sample
		const auto sample_arg = CB.getOperand(4 + args_offset);
		if(!sample_arg->getType()->isIntegerTy()) {
			ctx->emitError(&CB, "invalid image sample index type (must be integer)");
			return;
		}
		
		// -> offset
		const auto offset_arg = CB.getOperand(5 + args_offset);
		const auto offset_arg_type = dyn_cast<FixedVectorType>(offset_arg->getType());
		if(!offset_arg_type) {
			ctx->emitError(&CB, "invalid offset type (must be an int vector)");
			return;
		}
		if(!offset_arg_type->getElementType()->isIntegerTy()) {
			ctx->emitError(&CB, "invalid offset type (must be an int vector)");
			return;
		}
		if(coord_dim != offset_arg_type->getNumElements()) {
			ctx->emitError(&CB, "invalid offset vector dimension: should be " + std::to_string(coord_dim));
			return;
		}
		
		SmallVector<llvm::Value*, 3> offset_elems;
		bool is_offset = true;
		if(const auto const_offset_arg = dyn_cast_or_null<Constant>(offset_arg)) {
			// const 0 or undef -> no offset
			if(const_offset_arg->isZeroValue() ||
			   dyn_cast_or_null<UndefValue>(const_offset_arg)) {
				is_offset = false;
			}
		}
		
		if(is_offset) {
			// nobody supports this
			if(has_flag<COMPUTE_IMAGE_TYPE::FLAG_CUBE>(image_type)) {
				ctx->emitError(&CB, "image offset is not supported with cube maps");
				return;
			}
			
			// extract offset elems and check constant offset values
			for(uint32_t i = 0; i < coord_dim; ++i) {
				auto offset_elem = builder->CreateExtractElement(offset_arg, builder->getInt32(i));
				offset_elems.push_back(offset_elem);
				
				if(const auto const_offset_elem = dyn_cast_or_null<ConstantInt>(offset_elem)) {
					// can check if within required [-8, 7]
					const auto val = const_offset_elem->getSExtValue();
					if(val < -8 || val > 7) {
						ctx->emitError(&CB, "offset out of range (must be in [-8, 7]): " + std::to_string(val));
					}
				}
			}
		}
		
		// -> misc flags
		const auto is_lod_arg = dyn_cast_or_null<ConstantInt>(CB.getOperand(8 + args_offset));
		const auto is_lod_float_arg = dyn_cast_or_null<ConstantInt>(CB.getOperand(9 + args_offset));
		const auto is_bias_arg = dyn_cast_or_null<ConstantInt>(CB.getOperand(10 + args_offset));
		const auto is_gradient_arg = dyn_cast_or_null<ConstantInt>(CB.getOperand(13 + args_offset));
		const auto is_compare_arg = dyn_cast_or_null<ConstantInt>(CB.getOperand(16 + args_offset));
		if(!is_lod_arg) {
			ctx->emitError(&CB, "is_lod is not constant");
			return;
		}
		if(!is_lod_float_arg) {
			ctx->emitError(&CB, "is_lod_float is not constant");
			return;
		}
		if(!is_bias_arg) {
			ctx->emitError(&CB, "is_bias is not constant");
			return;
		}
		if(!is_gradient_arg) {
			ctx->emitError(&CB, "is_gradient is not constant");
			return;
		}
		if(!is_compare_arg) {
			ctx->emitError(&CB, "is_compare_arg is not constant");
			return;
		}
		
		const bool is_lod = is_lod_arg->isOne();
		const bool is_lod_float = is_lod_float_arg->isOne();
		const bool is_bias = is_bias_arg->isOne();
		const bool is_gradient = is_gradient_arg->isOne();
		const bool is_compare = is_compare_arg->isOne();
		
		if(is_lod && is_gradient) {
			ctx->emitError(&CB, "lod and gradient are mutually exclusive");
			return;
		}
		
		// -> lod and bias
		const auto lod_or_bias_arg = CB.getOperand(args_offset + (!is_bias && !is_lod_float ? 6 : 7));
		
		if(!lod_or_bias_arg->getType()->isIntegerTy() &&
		   !lod_or_bias_arg->getType()->isFloatTy()) {
			ctx->emitError(&CB, "lod must either be an integer or a float");
			return;
		}
		
		// -> gradient
		const auto dpdx_arg = CB.getOperand(11 + args_offset);
		const auto dpdy_arg = CB.getOperand(12 + args_offset);
		
		const auto dpdx_vec_type = dyn_cast<FixedVectorType>(dpdx_arg->getType());
		const auto dpdy_vec_type = dyn_cast<FixedVectorType>(dpdy_arg->getType());
		if(!dpdx_vec_type || !dpdy_vec_type) {
			ctx->emitError(&CB, "dpdx and dpdy must be vector types");
			return;
		}
		if(!dpdx_vec_type->getElementType()->isFloatTy() ||
		   !dpdy_vec_type->getElementType()->isFloatTy()) {
			ctx->emitError(&CB, "dpdx and dpdy element type must be float");
			return;
		}
		
		const auto dpdx_dim = dpdx_vec_type->getNumElements();
		const auto dpdy_dim = dpdy_vec_type->getNumElements();
		
		if(dpdx_dim != coord_dim || dpdy_dim != coord_dim) {
			ctx->emitError(&CB, "dpdx and dpdy vector dim must correspond to the coordinate dim");
			return;
		}
		
		// -> compare
		const auto compare_function_arg = dyn_cast_or_null<ConstantInt>(CB.getOperand(14 + args_offset));
		if(!compare_function_arg) {
			ctx->emitError(&CB, "compare function arg is not constant");
			return;
		}
		const COMPARE_FUNCTION compare_function = (COMPARE_FUNCTION)compare_function_arg->getZExtValue();
		if(compare_function >= COMPARE_FUNCTION::__MAX_COMPARE_FUNCTION) {
			ctx->emitError(&CB, "invalid compare function");
			return;
		}
		
		const auto compare_value_arg = CB.getOperand(15 + args_offset);
		
		handle_read_image(CB, func_name,
						  img_handle_arg, image_type,
						  const_sampler_arg, dyn_sampler_arg,
						  coord_arg, layer_arg, sample_arg,
						  offset_arg, offset_elems, is_offset,
						  lod_or_bias_arg, is_lod /* if false, then it's always bias */,
						  dpdx_arg, dpdy_arg, is_gradient,
						  compare_function, compare_value_arg, is_compare);
	}
	else {
		// -> data
		const auto data_arg = CB.getOperand(6 + args_offset);
		if (!is_depth || !is_metal) {
			const auto data_vec_type = dyn_cast<FixedVectorType>(data_arg->getType());
			if(!data_vec_type || data_vec_type->getNumElements() != 4) {
				ctx->emitError(&CB, "invalid image data type (must be 4-component vector)");
				return;
			}
			if(!data_vec_type->getElementType()->isFloatTy() &&
			   !data_vec_type->getElementType()->isHalfTy() &&
			   !data_vec_type->getElementType()->isIntegerTy()) {
				ctx->emitError(&CB, "invalid image data type (must be a float, half or integer vector)");
				return;
			}
		} else {
			const auto data_type = data_arg->getType();
			if (!data_type->isFloatTy() && !data_type->isHalfTy()) {
				ctx->emitError(&CB, "invalid image data type (must be a single float or half when writing depth)");
				return;
			}
		}
		
		// only writes with integer coordinates are allowed
		if(!coord_arg_type->getElementType()->isIntegerTy()) {
			ctx->emitError(&CB, "invalid coordinate type (must be integer)");
			return;
		}
		
		// -> lod
		const auto is_lod_arg = dyn_cast_or_null<ConstantInt>(CB.getOperand(5 + args_offset));
		const bool is_lod = is_lod_arg->isOne();
		const auto lod_arg = CB.getOperand(4 + args_offset);
		
		if(!lod_arg->getType()->isIntegerTy()) {
			ctx->emitError(&CB, "invalid lod type (must be integer)");
			return;
		}
		
		Value* image_rt_type_arg = nullptr;
		if (image_type_id == IMAGE_TYPE_ID::CUDA) {
			image_rt_type_arg = CB.getOperand(7 + args_offset);
			if (!image_rt_type_arg->getType()->isIntegerTy()) {
				ctx->emitError(&CB, "invalid run-time image-type type (must be enum/integer)");
				return;
			}
		}
		
		handle_write_image(CB, func_name,
						   img_handle_arg,
						   full_image_type, image_type, format_type, image_data_type, image_rt_type_arg,
						   is_normalized, image_channel_count,
						   coord_arg, layer_arg,
						   lod_arg, is_lod,
						   data_arg);
	}
}

void FloorImageBasePass::handle_image_query(CallBase& CB, const StringRef& func_name) {
	builder->SetInsertPoint(&CB);
	
	/* args for CUDA and opaque get_image_dim functions:
	 
	 CUDA get_image_dim:
	 uint64_t tex, COMPUTE_IMAGE_TYPE type, uint32_t lod
	 
	 opaque get_image_dim:
	 image_t img, COMPUTE_IMAGE_TYPE type, uint32_t lod
	 
	 */
	
	// check + get arguments
	constexpr const uint32_t arg_count = 3;
	if(CB.arg_size() != arg_count) {
		ctx->emitError(&CB, func_name + ": invalid argument count (expected " + std::to_string(arg_count) + ")");
		return;
	}
	
	// -> tex/surf/img handle
	const auto img_handle_arg = CB.getOperand(0);
	if(image_type_id == IMAGE_TYPE_ID::CUDA &&
	   !img_handle_arg->getType()->isIntegerTy()) {
		ctx->emitError(&CB, "invalid image handle type (must be integer)");
		return;
	}
	else if(image_type_id == IMAGE_TYPE_ID::OPAQUE) {
		if(!img_handle_arg->getType()->isPointerTy()) {
			ctx->emitError(&CB, "invalid image handle type (must be an image pointer)");
			return;
		}
	}
	
	// -> type enum
	const auto image_type_arg = dyn_cast_or_null<ConstantInt>(CB.getOperand(1));
	if(!image_type_arg) {
		ctx->emitError(&CB, "image type argument must be a constant value");
		return;
	}
	if(!image_type_arg->getType()->isIntegerTy()) {
		ctx->emitError(&CB, "invalid image-type type (must be enum/integer)");
		return;
	}
	const auto full_image_type = COMPUTE_IMAGE_TYPE(image_type_arg->getZExtValue());
	const COMPUTE_IMAGE_TYPE image_type = full_image_type & COMPUTE_IMAGE_TYPE::BASE_TYPE_MASK;
	
	const auto lod_arg = CB.getOperand(2);
	
	handle_get_image_dim(CB, func_name, img_handle_arg, full_image_type, image_type, lod_arg);
}

void FloorImageBasePass::emulate_depth_compare(llvm::Value*& dst_vec,
											   llvm::Value* tex_value,
											   const COMPARE_FUNCTION& compare_function,
											   llvm::Value* compare_value_arg) {
	llvm::Value* insert_value = nullptr;
	llvm::Value* condition = nullptr;
	llvm::Constant* false_val = ConstantFP::get(builder->getFloatTy(), 0.0f);
	llvm::Constant* true_val = ConstantFP::get(builder->getFloatTy(), 1.0f);
	
	switch(compare_function) {
		case COMPARE_FUNCTION::NEVER:
			insert_value = false_val;
			break;
		case COMPARE_FUNCTION::ALWAYS:
			insert_value = true_val;
			break;
		case COMPARE_FUNCTION::LESS_OR_EQUAL:
			condition = builder->CreateFCmpOLE(compare_value_arg, tex_value);
			break;
		case COMPARE_FUNCTION::GREATER_OR_EQUAL:
			condition = builder->CreateFCmpOGE(compare_value_arg, tex_value);
			break;
		case COMPARE_FUNCTION::LESS:
			condition = builder->CreateFCmpOLT(compare_value_arg, tex_value);
			break;
		case COMPARE_FUNCTION::GREATER:
			condition = builder->CreateFCmpOGT(compare_value_arg, tex_value);
			break;
		case COMPARE_FUNCTION::EQUAL:
			condition = builder->CreateFCmpOEQ(compare_value_arg, tex_value);
			break;
		case COMPARE_FUNCTION::NOT_EQUAL:
			condition = builder->CreateFCmpONE(compare_value_arg, tex_value);
			break;
		default: llvm_unreachable("invalid compare function");
	}
	
	if(condition != nullptr) {
		insert_value = builder->CreateSelect(condition, true_val, false_val);
	}
	
	dst_vec = builder->CreateInsertElement(dst_vec, insert_value, builder->getInt32(0));
}
