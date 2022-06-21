//===-- FloorImage.h - base class for image transformations------*- C++ -*-===//
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
// This header file and class define and implement the base class for all
// image transformations (CUDA and opaque, as used for Metal, OpenCL and Vulkan).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LIBFLOOR_FLOORIMAGE_H
#define LLVM_TRANSFORMS_LIBFLOOR_FLOORIMAGE_H

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <string>
#include <array>

#include "llvm/Pass.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "FloorImageType.h"

namespace llvm {
	struct FloorImageBasePass : public FunctionPass, InstVisitor<FloorImageBasePass> {
		friend class InstVisitor<FloorImageBasePass>;
		
		enum class IMAGE_TYPE_ID {
			CUDA,
			OPAQUE
		};
		
		explicit FloorImageBasePass(char &ID,
									const IMAGE_TYPE_ID& image_type_id,
									const uint32_t& image_capabilities);
		
		bool runOnFunction(Function &F) override;
		
		using InstVisitor<FloorImageBasePass>::visit;
		void visit(Instruction& I);
		void visitCallBase(CallBase& CB);
		
		void handle_image(CallBase& CB, const StringRef& func_name);
		void handle_image_query(CallBase& CB, const StringRef& func_name);
		
		virtual void handle_read_image(Instruction& I,
									   const StringRef& func_name,
									   llvm::Value* img_handle_arg,
									   const COMPUTE_IMAGE_TYPE& image_type,
									   llvm::ConstantInt* const_sampler_arg,
									   llvm::Value* dyn_sampler_arg,
									   llvm::Value* coord_arg,
									   llvm::Value* layer_arg,
									   llvm::Value* sample_arg,
									   llvm::Value* offset_arg,
									   const SmallVector<llvm::Value*, 3>& offset_elems,
									   const bool is_offset,
									   llvm::Value* lod_or_bias_arg,
									   const bool is_lod_or_bias, // true: lod, false: bias
									   llvm::Value* dpdx_arg,
									   llvm::Value* dpdy_arg,
									   const bool is_gradient,
									   const COMPARE_FUNCTION& compare_function,
									   llvm::Value* compare_value_arg,
									   const bool is_compare) = 0;
		
		virtual void handle_write_image(Instruction& I,
										const StringRef& func_name,
										llvm::Value* img_handle_arg,
										const COMPUTE_IMAGE_TYPE& full_image_type,
										const COMPUTE_IMAGE_TYPE& image_type,
										const COMPUTE_IMAGE_TYPE& format_type,
										const COMPUTE_IMAGE_TYPE& data_type,
										llvm::Value* rt_image_type,
										const bool& is_normalized,
										const uint32_t& image_channel_count,
										llvm::Value* coord_arg,
										llvm::Value* layer_arg,
										llvm::Value* lod_arg,
										const bool is_lod,
										llvm::Value* data_arg) = 0;
		
		virtual void handle_get_image_dim(Instruction& I,
										  const StringRef& func_name,
										  llvm::Value* img_handle_arg,
										  const COMPUTE_IMAGE_TYPE& full_image_type,
										  const COMPUTE_IMAGE_TYPE& image_type,
										  llvm::Value* lod_arg) = 0;
		
	protected:
		const IMAGE_TYPE_ID image_type_id;
		const char* image_read_prefix;
		const char* image_write_prefix;
		const char* image_get_dim_prefix;
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		Instruction* alloca_insert { nullptr };
		std::shared_ptr<IRBuilder<>> builder;
		bool was_modified { false };
		bool is_fragment_shader { false };
		bool is_metal { false };
		bool is_metal_2_3 { false };
		bool is_metal_2_4 { false };
		IMAGE_CAPABILITY image_capabilities { IMAGE_CAPABILITY::NONE };
		
		llvm::AttributeList nounwind_readnone_attr;
		llvm::AttributeList nounwind_attr;
		
		// depth compare s/w emulation if not supported by backend h/w or s/w
		void emulate_depth_compare(llvm::Value*& dst_vec,
								   llvm::Value* tex_value,
								   const COMPARE_FUNCTION& compare_function,
								   llvm::Value* compare_value_arg);
		
	};
}

#endif
