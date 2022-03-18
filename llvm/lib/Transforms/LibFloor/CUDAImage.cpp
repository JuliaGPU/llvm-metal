//===- CUDAImage.cpp - CUDA-specific floor image transformations ----------===//
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
// This pass implements the CUDA-specific floor image transformations, i.e.
// floor.cuda.<read/write function>.*
//   -> PTX texture/surface instructions (inline asm)
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
#include "llvm/Transforms/LibFloor/FloorImage.h"
#include <algorithm>
#include <cstdarg>
#include <memory>
#include <array>
#include <optional>
#include <cxxabi.h>
using namespace llvm;

#define DEBUG_TYPE "CUDAImage"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

namespace {
	// CUDAImage
	struct CUDAImage : public FloorImageBasePass {
		static char ID; // Pass identification, replacement for typeid
		
		CUDAImage(const uint32_t image_capabilities_ = 0) :
		FloorImageBasePass(ID, IMAGE_TYPE_ID::CUDA, image_capabilities_) {
			initializeCUDAImagePass(*PassRegistry::getPassRegistry());
		}
		
		static const char* type_to_geom(const COMPUTE_IMAGE_TYPE& image_type) {
			switch (image_type & (COMPUTE_IMAGE_TYPE::__DIM_MASK |
								  COMPUTE_IMAGE_TYPE::FLAG_ARRAY |
								  COMPUTE_IMAGE_TYPE::FLAG_BUFFER |
								  COMPUTE_IMAGE_TYPE::FLAG_CUBE |
								  COMPUTE_IMAGE_TYPE::FLAG_DEPTH |
								  COMPUTE_IMAGE_TYPE::FLAG_STENCIL |
								  COMPUTE_IMAGE_TYPE::FLAG_MSAA)) {
				case COMPUTE_IMAGE_TYPE::IMAGE_1D:
				case COMPUTE_IMAGE_TYPE::IMAGE_1D_BUFFER:
					return "1d";
				case COMPUTE_IMAGE_TYPE::IMAGE_1D_ARRAY:
					return "a1d";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH:
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_STENCIL:
				case COMPUTE_IMAGE_TYPE::IMAGE_2D:
					return "2d";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_ARRAY:
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_ARRAY:
					return "a2d";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA:
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA:
					return "2dms";
				case COMPUTE_IMAGE_TYPE::IMAGE_3D:
					return "3d";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE:
				case COMPUTE_IMAGE_TYPE::IMAGE_CUBE:
					return "cube";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE_ARRAY:
				case COMPUTE_IMAGE_TYPE::IMAGE_CUBE_ARRAY:
					return "acube";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA_ARRAY:
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA_ARRAY:
					return "a2dms";
				default:
					return nullptr;
			}
		}
		
		void handle_read_image(Instruction& I,
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
							   const bool is_compare) override {
			SmallVector<llvm::Type*, 16> asm_arg_types;
			SmallVector<llvm::Value*, 16> asm_args;
			std::string constraints_str = "";
			
			// -> return data
			std::string dtype;
			llvm::Type* ret_type, *ret_vec_type;
			if (func_name == "floor.cuda.read_image.float") {
				dtype = "f32";
				constraints_str = "=f,=f,=f,=f";
				ret_type = llvm::StructType::get(*ctx, std::vector<llvm::Type*> {{
					llvm::Type::getFloatTy(*ctx),
					llvm::Type::getFloatTy(*ctx),
					llvm::Type::getFloatTy(*ctx),
					llvm::Type::getFloatTy(*ctx)
				}});
				ret_vec_type = llvm::FixedVectorType::get(llvm::Type::getFloatTy(*ctx), 4);
			} else if (func_name == "floor.cuda.read_image.half") {
				dtype = "f16";
				constraints_str = "=h,=h,=h,=h";
				ret_type = llvm::StructType::get(*ctx, std::vector<llvm::Type*> {{
					llvm::Type::getHalfTy(*ctx),
					llvm::Type::getHalfTy(*ctx),
					llvm::Type::getHalfTy(*ctx),
					llvm::Type::getHalfTy(*ctx)
				}});
				ret_vec_type = llvm::FixedVectorType::get(llvm::Type::getHalfTy(*ctx), 4);
			} else if (func_name == "floor.cuda.read_image.int") {
				dtype = "s32";
				constraints_str = "=r,=r,=r,=r";
				ret_type = llvm::StructType::get(*ctx, std::vector<llvm::Type*> {{
					llvm::Type::getInt32Ty(*ctx),
					llvm::Type::getInt32Ty(*ctx),
					llvm::Type::getInt32Ty(*ctx),
					llvm::Type::getInt32Ty(*ctx)
				}});
				ret_vec_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 4);
			} else if (func_name == "floor.cuda.read_image.uint") {
				dtype = "u32";
				constraints_str = "=r,=r,=r,=r";
				ret_type = llvm::StructType::get(*ctx, std::vector<llvm::Type*> {{
					llvm::Type::getInt32Ty(*ctx),
					llvm::Type::getInt32Ty(*ctx),
					llvm::Type::getInt32Ty(*ctx),
					llvm::Type::getInt32Ty(*ctx)
				}});
				ret_vec_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 4);
			}
			// unknown -> ignore
			else return;
			
			constraints_str += ",l"; // u64 tex handle
			asm_arg_types.push_back(llvm::Type::getInt64Ty(*ctx));
			asm_args.push_back(img_handle_arg);
			
			// -> geom
			const auto geom_cstr = type_to_geom(image_type);
			if (!geom_cstr) {
				ctx->emitError(&I, "unknown or incorrect image type");
				return;
			}
			std::string geom = geom_cstr; // .1d, .2d, .3d, .a1d, .a2d, .cube, .acube, .2dms, .a2dms
			const auto is_array = has_flag<COMPUTE_IMAGE_TYPE::FLAG_ARRAY>(image_type);
			const auto is_msaa = has_flag<COMPUTE_IMAGE_TYPE::FLAG_MSAA>(image_type);
			const auto is_cube = has_flag<COMPUTE_IMAGE_TYPE::FLAG_CUBE>(image_type);
			
			// -> coords
			auto coord_vec_type = dyn_cast_or_null<FixedVectorType>(coord_arg->getType());
			if(!coord_vec_type) {
				ctx->emitError(&I, "invalid image coordinate argument (cast to vector failed)");
				return;
			}
			const auto coord_dim = coord_vec_type->getNumElements();
			
			const auto coord_type = coord_vec_type->getElementType();
			if(is_msaa && !coord_type->isIntegerTy()) {
				ctx->emitError(&I, "coordinate type must be integer for msaa images");
				return;
			}
			
			// TODO: add s/w support for reading cube maps with integer coords (u, v, face, *layer)
			if(is_cube && !coord_type->isFloatTy()) {
				ctx->emitError(&I, "coordinate type must be float for cube images");
				return;
			}
			
			std::string ctype = (coord_type->isFloatTy() ? "f32" : "s32");
			std::string coords_placeholders;
			const std::string coord_type_str = (coord_type->isFloatTy() ? "f" : "r");
			uint32_t asm_arg_idx = 5;
			if(is_msaa) {
				asm_arg_types.push_back(sample_arg->getType());
				asm_args.push_back(sample_arg);
				constraints_str += ",r";
				coords_placeholders += " $";
				coords_placeholders += std::to_string(asm_arg_idx++);
			}
			if(is_array) {
				asm_arg_types.push_back(layer_arg->getType());
				asm_args.push_back(layer_arg);
				constraints_str += ",r";
				coords_placeholders += (!is_msaa ? " $" : ", $");
				coords_placeholders += std::to_string(asm_arg_idx++);
			}
			for(uint32_t i = 0; i < coord_dim; ++i) {
				asm_arg_types.push_back(coord_type);
				asm_args.push_back(builder->CreateExtractElement(coord_arg, builder->getInt32(i)));
				constraints_str += "," + coord_type_str;
				coords_placeholders += (i == 0 && asm_arg_idx == 5 ? " $" : ", $");
				coords_placeholders += std::to_string(asm_arg_idx++);
			}
			
			// append (ignored) 0 coordinate if #coordinates == 3
			if((coord_dim + (is_msaa ? 1 : 0) + (is_array ? 1 : 0)) == 3) {
				coords_placeholders += (coord_type->isFloatTy() ? ", 0.0" : ", 0");
			}
			
			// -> lod
			std::string mipmap_prefix = "";
			std::string lod_str = "";
			if(is_lod_or_bias) {
				mipmap_prefix = "level.";
				lod_str = ", ";
				// NOTE: lod type must match coord elem type, cast if necessary
				if(const auto const_lod = dyn_cast_or_null<ConstantInt>(lod_or_bias_arg)) {
					if(coord_type->isIntegerTy()) {
						lod_str += std::to_string(const_lod->getSExtValue());
					}
					else {
						// convert to float
						lod_str += std::to_string((float)const_lod->getSExtValue());
					}
				}
				else if(const auto const_lod = dyn_cast_or_null<ConstantFP>(lod_or_bias_arg)) {
					if(coord_type->isFloatTy()) {
						lod_str += std::to_string(const_lod->getValueAPF().convertToFloat());
					}
					else {
						// convert to int
						lod_str += std::to_string((int32_t)round(const_lod->getValueAPF().convertToFloat()));
					}
				}
				else {
					asm_arg_types.push_back(coord_type);
					if(coord_type == lod_or_bias_arg->getType()) {
						asm_args.push_back(lod_or_bias_arg);
					}
					else {
						// convert to appropriate type
						asm_args.push_back(coord_type->isIntegerTy() ?
										   builder->CreateFPToSI(lod_or_bias_arg, coord_type) :
										   builder->CreateSIToFP(lod_or_bias_arg, coord_type));
					}
					lod_str += "$" + std::to_string(asm_arg_idx++);
					constraints_str += (coord_type->isIntegerTy() ? ",r" : ",f");
				}
			}
			
			// -> gradient
			std::string gradient_str = "";
			if(is_gradient) {
				mipmap_prefix = "grad.";
				
				// dpdx
				gradient_str += ", { ";
				for(uint32_t i = 0; i < coord_dim; ++i) {
					asm_arg_types.push_back(builder->getFloatTy());
					asm_args.push_back(builder->CreateExtractElement(dpdx_arg, builder->getInt32(i)));
					gradient_str += (i == 0 ? "$" : ", $") + std::to_string(asm_arg_idx++);
					constraints_str += ",f";
				}
				if(coord_dim == 3) gradient_str += ", 0.0";
				
				// dpdy
				gradient_str += " }, { ";
				for(uint32_t i = 0; i < coord_dim; ++i) {
					asm_arg_types.push_back(builder->getFloatTy());
					asm_args.push_back(builder->CreateExtractElement(dpdy_arg, builder->getInt32(i)));
					gradient_str += (i == 0 ? "$" : ", $") + std::to_string(asm_arg_idx++);
					constraints_str += ",f";
				}
				if(coord_dim == 3) gradient_str += ", 0.0";
				gradient_str += " }";
			}
			
			// -> offset
			std::string offset_str = "";
			if(is_offset) {
				for(uint32_t i = 0; i < coord_dim; ++i) {
					if(i != 0) offset_str += ", ";
					if(const auto const_offset_elem = dyn_cast_or_null<ConstantInt>(offset_elems[i])) {
						offset_str += std::to_string(const_offset_elem->getSExtValue());
					}
					else {
						asm_arg_types.push_back(llvm::Type::getInt32Ty(*ctx));
						asm_args.push_back(offset_elems[i]);
						offset_str += "$" + std::to_string(asm_arg_idx++);
						constraints_str += ",r";
					}
				}
				
				// append ignored 0 offset
				if(coord_dim == 3) {
					offset_str += ", 0";
				}
			}
			
			// -> compare
			std::string compare_str = "";
			llvm::Value* compare_override = nullptr;
			const bool has_hw_depth_compare = has_flag<IMAGE_CAPABILITY::DEPTH_COMPARE>(image_capabilities);
			if(is_compare && has_hw_depth_compare) {
				// must have float coords with depth compare
				if(!coord_type->isFloatTy()) {
					ctx->emitError(&I, "coordinate type must be float when using depth compare");
					return;
				}
				
				// check for unsupported image types
				switch(image_type) {
					case COMPUTE_IMAGE_TYPE::IMAGE_1D:
					case COMPUTE_IMAGE_TYPE::IMAGE_1D_ARRAY:
					case COMPUTE_IMAGE_TYPE::IMAGE_1D_BUFFER:
					case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH:
					case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_STENCIL:
					case COMPUTE_IMAGE_TYPE::IMAGE_2D:
					case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_ARRAY:
					case COMPUTE_IMAGE_TYPE::IMAGE_2D_ARRAY:
					case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE:
					case COMPUTE_IMAGE_TYPE::IMAGE_CUBE:
					case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE_ARRAY:
					case COMPUTE_IMAGE_TYPE::IMAGE_CUBE_ARRAY:
						// all supported
						break;
						
					case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA:
					case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA:
					case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA_ARRAY:
					case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA_ARRAY:
					case COMPUTE_IMAGE_TYPE::IMAGE_3D:
					default:
						ctx->emitError(&I, "image type does not support depth compare");
						return;
				}
				
				asm_arg_types.push_back(builder->getFloatTy());
				asm_args.push_back(compare_value_arg);
				constraints_str += ",f";
				compare_str = "$" + std::to_string(asm_arg_idx++);
				
				// directly skip/eval NEVER and ALWAYS compare functions
				// while this would be DCE'ed anyways, this is somewhat cleaner
				// NOTE: we still want all of the usual error messages and checking, which is why we're not doing this earlier
				if(compare_function == COMPARE_FUNCTION::NEVER) {
					compare_override = ConstantFP::get(builder->getFloatTy(), 0.0f);
				}
				else if(compare_function == COMPARE_FUNCTION::ALWAYS) {
					compare_override = ConstantFP::get(builder->getFloatTy(), 1.0f);
				}
			}
			
			// -> build asm string
			std::string asm_str = "tex." + mipmap_prefix + geom + ".v4." + dtype + "." + ctype;
			asm_str += " { $0, $1, $2, $3 },";
			asm_str += " [$4, {" + coords_placeholders + " }]";
			if(is_lod_or_bias) {
				asm_str += lod_str;
			}
			if(is_gradient) {
				asm_str += gradient_str;
			}
			if(is_offset) {
				asm_str += ", { " + offset_str + " }";
			}
			if(is_compare && has_hw_depth_compare) {
				asm_str += ", " + compare_str;
			}
			asm_str += ";";
			
			const auto asm_func_type = FunctionType::get(ret_type, asm_arg_types, false);
			auto asm_func = InlineAsm::get(asm_func_type, asm_str, constraints_str, false /* non-volatile */);
			auto asm_call = builder->CreateCall(asm_func, asm_args);
			asm_call->setDoesNotAccessMemory(); // all reads are readnone (can be optimized away if unused)
			asm_call->setDebugLoc(I.getDebugLoc()); // keep debug loc
			asm_call->setNotConvergent();
			
			//
			llvm::Value* dst_vec = UndefValue::get(ret_vec_type);
			if(compare_override == nullptr) {
				// -> normal color read or h/w depth compare
				if(!is_compare || has_hw_depth_compare) {
					for(uint32_t i = 0; i < 4; ++i) {
						auto scalar = builder->CreateExtractValue(asm_call, i);
						dst_vec = builder->CreateInsertElement(dst_vec, scalar, builder->getInt32(i));
					}
				}
				// -> s/w depth compare
				else {
					emulate_depth_compare(dst_vec, builder->CreateExtractValue(asm_call, 0), compare_function, compare_value_arg);
				}
			}
			// -> compare override (NONE/NEVER/ALWAYS), don't emit tex instruction
			else {
				dst_vec = builder->CreateInsertElement(dst_vec, compare_override, builder->getInt32(0));
			}
			
			//
			I.replaceAllUsesWith(dst_vec);
			I.eraseFromParent();
		}
		
		CallInst* emit_write_image(llvm::Value* img_handle_arg,
								   const COMPUTE_IMAGE_TYPE& format_type,
								   const bool& is_normalized,
								   const uint32_t& image_channel_count,
								   llvm::Value* coord_arg,
								   llvm::Type* coord_type,
								   FixedVectorType* coord_vec_type,
								   llvm::Value* layer_arg,
								   const std::array<llvm::Value*, 4>& orig_data_args,
								   const std::string& geom,
								   const bool is_array,
								   const bool is_float,
								   const bool is_half,
								   const bool is_int,
								   const DebugLoc& debug_loc) {
			SmallVector<llvm::Type*, 16> asm_arg_types;
			SmallVector<llvm::Value*, 16> asm_args;
			
			std::string constraints_str = "l"; // u64 surf handle
			asm_arg_types.push_back(llvm::Type::getInt64Ty(*ctx));
			asm_args.push_back(img_handle_arg);
			
			// -> coords
			const auto coord_dim = coord_vec_type->getNumElements();
			std::string coords_placeholders;
			static const uint32_t coord_start_idx = 1;
			uint32_t coord_idx = 0;
			size_t x_coord_idx = 0;
			if (is_array) {
				asm_arg_types.push_back(layer_arg->getType());
				asm_args.push_back(layer_arg);
				constraints_str += ",r";
				coords_placeholders += " $";
				coords_placeholders += std::to_string(coord_start_idx + coord_idx++);
			}
			for (uint32_t i = 0; i < coord_dim; ++i) {
				asm_arg_types.push_back(coord_type);
				auto coord_elem = builder->CreateExtractElement(coord_arg, builder->getInt32(i));
				if (i == 0) {
					x_coord_idx = asm_args.size();
				}
				asm_args.push_back(coord_elem);
				constraints_str += ",r";
				coords_placeholders += (coord_idx == 0 ? " $" : ", $");
				coords_placeholders += std::to_string(coord_start_idx + coord_idx++);
			}
			
			// append (ignored) 0 coordinate if #coordinates == 3
			if ((coord_dim + (is_array ? 1 : 0)) == 3) {
				coords_placeholders += ", 0";
			}
			
			// -> data
			const auto write_channel_count = (image_channel_count == 3 ? 4 : image_channel_count);
			
			std::array<llvm::Value*, 4> data_args = orig_data_args;
			std::string dtype, rtype;
			if (is_normalized) {
				// need to normalize 32-bit float -> 8-bit or 16-bit unsigned/signed int
				if (format_type != COMPUTE_IMAGE_TYPE::FORMAT_8 && format_type != COMPUTE_IMAGE_TYPE::FORMAT_16) {
					assert(false && "invalid normalized write format - should not be here!");
					ctx->emitError("invalid normalized write format (expected 8-bit or 16-bit dst format");
					return nullptr;
				}
				
				bool is_8_bit = true;
				if (format_type == COMPUTE_IMAGE_TYPE::FORMAT_8) {
					dtype = "b8";
				} else {
					dtype = "b16";
					is_8_bit = false;
				}
				rtype = "h"; // can't go lower than 16-bit
				
				for (uint32_t i = 0; i < write_channel_count; ++i) {
					if (data_args[i]->getType()->isHalfTy()) {
						// using half precision for normalization here would lose precision -> cast to float first
						data_args[i] = builder->CreateFPCast(data_args[i], builder->getFloatTy());
					}
					data_args[i] = builder->CreateFMul(data_args[i],
													   ConstantFP::get(builder->getFloatTy(),
																	   is_int ?
																	   (is_8_bit ? 127.0 : 32767.0) :
																	   (is_8_bit ? 255.0 : 65535.0)));
					data_args[i] = builder->CreateFPToUI(data_args[i],
														 is_8_bit ? builder->getInt8Ty() : builder->getInt16Ty());
				}
			} else {
				switch (format_type) {
					case COMPUTE_IMAGE_TYPE::FORMAT_8:
						dtype = "b8";
						rtype = "h"; // can't go lower than 16-bit
						break;
					case COMPUTE_IMAGE_TYPE::FORMAT_16:
						dtype = "b16";
						rtype = (is_float && !is_half ? "f" : "h");
						break;
					case COMPUTE_IMAGE_TYPE::FORMAT_24:
					case COMPUTE_IMAGE_TYPE::FORMAT_32_8:
					case COMPUTE_IMAGE_TYPE::FORMAT_32:
						dtype = "b32";
						rtype = (is_float || is_half ? "f" : "r");
						break;
					default:
						assert(false && "invalid write format - should not be here!");
						ctx->emitError("invalid write format");
						return nullptr;
				}
				
				// need to trunc 32-bit data to 16-bit (for 8-bit/16-bit int/uint writes)
				if (rtype == "h" && !is_half) {
					for (uint32_t i = 0; i < write_channel_count; ++i) {
						builder->CreateTrunc(data_args[i], builder->getInt16Ty());
					}
				}
			}
			
			// we know the written binary data size now -> update x coordinate offset
			asm_args[x_coord_idx] = builder->CreateMul(asm_args[x_coord_idx],
													   builder->getInt32(write_channel_count *
																		 (dtype == "b16" ? 2 :
																		  (dtype == "b32" ? 4 : 1 /* b8 */))));
			
			std::string data_placeholders;
			uint32_t data_idx = coord_start_idx + coord_idx;
			for (uint32_t i = 0; i < write_channel_count; ++i) {
				asm_arg_types.push_back(data_args[i]->getType());
				asm_args.push_back(data_args[i]);
				constraints_str += "," + rtype;
				data_placeholders += (i == 0 ? " $" : ", $");
				data_placeholders += std::to_string(data_idx++);
			}
			
			// -> build asm string
			std::string asm_str = "sust.b." + geom + ".";
			asm_str += (image_channel_count == 1 ? "" : (image_channel_count == 2 ? "v2." : "v4."));
			asm_str += dtype + ".";
			asm_str += "zero"; // ignore out-of-bounds writes (TODO: might want to trap in debug mode?)
			
			asm_str += " [$0, {" + coords_placeholders + " }],";
			asm_str += " {" + data_placeholders + " };";
			
			const auto asm_func_type = FunctionType::get(builder->getVoidTy(), asm_arg_types, false);
			auto asm_func = InlineAsm::get(asm_func_type, asm_str, constraints_str, true /* volatile */);
			auto asm_call = builder->CreateCall(asm_func, asm_args);
			asm_call->setDebugLoc(debug_loc); // keep debug loc
			asm_call->setNotConvergent();
			
			return asm_call;
		}
		
		void handle_write_image(Instruction& I,
								const StringRef& func_name,
								llvm::Value* img_handle_arg,
								const COMPUTE_IMAGE_TYPE& full_image_type,
								const COMPUTE_IMAGE_TYPE& /* image_type */,
								const COMPUTE_IMAGE_TYPE& /* format_type*/,
								const COMPUTE_IMAGE_TYPE& /* data_type */,
								llvm::Value* rt_image_type,
								const bool& is_normalized,
								const uint32_t& image_channel_count,
								llvm::Value* coord_arg,
								llvm::Value* layer_arg,
								// NOTE: lod is handled on the library side
								llvm::Value* /* lod_arg */,
								const bool /* is_lod */,
								llvm::Value* data_arg) override {
			//// more arg checking
			
			// run-time type must be set
			if (!rt_image_type) {
				ctx->emitError(&I, "missing run-time image type");
				return;
			}
			
			auto coord_vec_type = dyn_cast_or_null<FixedVectorType>(coord_arg->getType());
			if (!coord_vec_type) {
				ctx->emitError(&I, "invalid image coordinate argument (cast to vector failed)");
				return;
			}
			
			const auto coord_type = coord_vec_type->getElementType();
			if (!coord_type->isIntegerTy()) {
				ctx->emitError(&I, "coordinate type must be integer");
				return;
			}
			
			const auto is_float = (func_name == "floor.cuda.write_image.float");
			const auto is_half = (func_name == "floor.cuda.write_image.half");
			const auto is_int = (func_name == "floor.cuda.write_image.int");
			const auto is_uint = (func_name == "floor.cuda.write_image.uint");
			if (!is_float && !is_half && !is_int && !is_uint) {
				return; // unknown -> ignore
			}
			
			// -> geom
			const auto geom_cstr = type_to_geom(full_image_type);
			if (!geom_cstr) {
				ctx->emitError(&I, "unknown or incorrect image type");
				return;
			}
			std::string geom = geom_cstr; // .1d, .2d, .3d, .a1d, .a2d
			const auto is_array = has_flag<COMPUTE_IMAGE_TYPE::FLAG_ARRAY>(full_image_type);
			const auto is_msaa = has_flag<COMPUTE_IMAGE_TYPE::FLAG_MSAA>(full_image_type);
			const auto is_cube = has_flag<COMPUTE_IMAGE_TYPE::FLAG_CUBE>(full_image_type);
			
			// cube and msaa formats are not writable by cuda/ptx
			if (is_cube || is_msaa) {
				ctx->emitError(&I, "invalid image type - type is not writable");
				return;
			}
			
			const auto& debug_loc = I.getDebugLoc();
			const auto img_type_int_type = builder->getInt64Ty();
			
			std::array<llvm::Value*, 4> data_args {{
				builder->CreateExtractElement(data_arg, builder->getInt32(0)),
				builder->CreateExtractElement(data_arg, builder->getInt32(1)),
				builder->CreateExtractElement(data_arg, builder->getInt32(2)),
				builder->CreateExtractElement(data_arg, builder->getInt32(3))
			}};
			
			// emits a large switch -> image asm write, for all valid run-time image types
			const auto emit_writes = [&](const std::optional<uint32_t> image_channel_count) {
				auto mask = uint64_t(COMPUTE_IMAGE_TYPE::__FORMAT_MASK | COMPUTE_IMAGE_TYPE::__DATA_TYPE_MASK | COMPUTE_IMAGE_TYPE::FLAG_NORMALIZED);
				if (!image_channel_count) {
					mask |= uint64_t(COMPUTE_IMAGE_TYPE::__CHANNELS_MASK);
				}
				auto rt_cmp_val = builder->CreateAnd(rt_image_type, mask);
				auto def_unreachable_block = BasicBlock::Create(*ctx, "write_unreachable", func);
				new UnreachableInst(*ctx, def_unreachable_block);
				
				auto start_block = I.getParent();
				auto end_block = start_block->splitBasicBlock(&I, "write_end");
				builder->SetInsertPoint(start_block->getTerminator());
				llvm::SwitchInst* sw = builder->CreateSwitch(rt_cmp_val, def_unreachable_block, is_float || is_half ? 7 : 4);
				start_block->getTerminator()->eraseFromParent();
				
				const bool is_fixed_channel_count = image_channel_count.has_value();
				const auto channel_bit = [&](const uint32_t channel_count) {
					return (is_fixed_channel_count ? COMPUTE_IMAGE_TYPE::NONE :
							COMPUTE_IMAGE_TYPE(uint64_t(channel_count - 1u) << uint64_t(COMPUTE_IMAGE_TYPE::__CHANNELS_SHIFT)));
				};
				const auto emit_block = [&](const std::string& name, COMPUTE_IMAGE_TYPE base_format, COMPUTE_IMAGE_TYPE case_rt_type, const uint32_t channel_count,
											const bool is_float, const bool is_half, const bool is_int) {
					auto block = BasicBlock::Create(*ctx, "write_" + name, func);
					sw->addCase(ConstantInt::get(img_type_int_type, uint64_t(case_rt_type | channel_bit(channel_count))), block);
					builder->SetInsertPoint(block);
					emit_write_image(img_handle_arg, base_format, has_flag<COMPUTE_IMAGE_TYPE::FLAG_NORMALIZED>(case_rt_type), channel_count,
									 coord_arg, coord_type, coord_vec_type, layer_arg, data_args, geom, is_array, is_float, is_half, is_int, debug_loc);
					BranchInst::Create(end_block, block);
					return block;
				};
				
				for (uint32_t channel_count = (image_channel_count ? *image_channel_count : 1); channel_count <= (image_channel_count ? *image_channel_count : 4); ++channel_count) {
					if (is_float || is_half) {
						emit_block("unorm8", COMPUTE_IMAGE_TYPE::FORMAT_8, COMPUTE_IMAGE_TYPE::FORMAT_8 | COMPUTE_IMAGE_TYPE::UINT | COMPUTE_IMAGE_TYPE::FLAG_NORMALIZED, channel_count, false, false, false);
						emit_block("snorm8", COMPUTE_IMAGE_TYPE::FORMAT_8, COMPUTE_IMAGE_TYPE::FORMAT_8 | COMPUTE_IMAGE_TYPE::INT | COMPUTE_IMAGE_TYPE::FLAG_NORMALIZED, channel_count, false, false, true);
						emit_block("unorm16", COMPUTE_IMAGE_TYPE::FORMAT_16, COMPUTE_IMAGE_TYPE::FORMAT_16 | COMPUTE_IMAGE_TYPE::UINT | COMPUTE_IMAGE_TYPE::FLAG_NORMALIZED, channel_count, false, false, false);
						emit_block("snorm16", COMPUTE_IMAGE_TYPE::FORMAT_16, COMPUTE_IMAGE_TYPE::FORMAT_16 | COMPUTE_IMAGE_TYPE::INT | COMPUTE_IMAGE_TYPE::FLAG_NORMALIZED, channel_count, false, false, true);
						emit_block("f16", COMPUTE_IMAGE_TYPE::FORMAT_16, COMPUTE_IMAGE_TYPE::FORMAT_16 | COMPUTE_IMAGE_TYPE::FLOAT, channel_count, is_float, is_half, false);
						auto f32_block = emit_block("f32", COMPUTE_IMAGE_TYPE::FORMAT_32, COMPUTE_IMAGE_TYPE::FORMAT_32 | COMPUTE_IMAGE_TYPE::FLOAT, channel_count, is_float, is_half, false);
						// NOTE: 32-bit float + 8-bit stencil contains exactly the same code as only 32-bit float -> reuse "f32"
						sw->addCase(ConstantInt::get(img_type_int_type, uint64_t(COMPUTE_IMAGE_TYPE::FORMAT_32_8 | COMPUTE_IMAGE_TYPE::FLOAT | channel_bit(channel_count))), f32_block);
					} else if (is_uint) {
						emit_block("ui8", COMPUTE_IMAGE_TYPE::FORMAT_8, COMPUTE_IMAGE_TYPE::FORMAT_8 | COMPUTE_IMAGE_TYPE::UINT, channel_count, false, false, false);
						emit_block("ui16", COMPUTE_IMAGE_TYPE::FORMAT_16, COMPUTE_IMAGE_TYPE::FORMAT_16 | COMPUTE_IMAGE_TYPE::UINT, channel_count, false, false, false);
						emit_block("ui24", COMPUTE_IMAGE_TYPE::FORMAT_24, COMPUTE_IMAGE_TYPE::FORMAT_24 | COMPUTE_IMAGE_TYPE::UINT, channel_count, false, false, false);
						emit_block("ui32", COMPUTE_IMAGE_TYPE::FORMAT_32, COMPUTE_IMAGE_TYPE::FORMAT_32 | COMPUTE_IMAGE_TYPE::UINT, channel_count, false, false, false);
					} else if (is_int) {
						emit_block("i8", COMPUTE_IMAGE_TYPE::FORMAT_8, COMPUTE_IMAGE_TYPE::FORMAT_8 | COMPUTE_IMAGE_TYPE::INT, channel_count, false, false, true);
						emit_block("i16", COMPUTE_IMAGE_TYPE::FORMAT_16, COMPUTE_IMAGE_TYPE::FORMAT_16 | COMPUTE_IMAGE_TYPE::INT, channel_count, false, false, true);
						emit_block("i24", COMPUTE_IMAGE_TYPE::FORMAT_24, COMPUTE_IMAGE_TYPE::FORMAT_24 | COMPUTE_IMAGE_TYPE::INT, channel_count, false, false, true);
						emit_block("i32", COMPUTE_IMAGE_TYPE::FORMAT_32, COMPUTE_IMAGE_TYPE::FORMAT_32 | COMPUTE_IMAGE_TYPE::INT, channel_count, false, false, true);
					}
				}
			};
			
			if (has_flag<COMPUTE_IMAGE_TYPE::FLAG_FIXED_CHANNELS>(full_image_type)) {
				// -> specific channel count is known
				emit_writes(image_channel_count);
			} else {
				// -> channel count is not known, need to emit for { 1, 2, 3, 4 } channels
				emit_writes({});
			}
			
			// NOTE: write must/can not have any uses
			assert(I.getNumUses() == 0);
			I.eraseFromParent();
		}
		
		void handle_get_image_dim(Instruction& I,
								  const StringRef& func_name,
								  llvm::Value* img_handle_arg,
								  const COMPUTE_IMAGE_TYPE& full_image_type,
								  const COMPUTE_IMAGE_TYPE& image_type,
								  llvm::Value* lod_arg) override {
			// gather info
			const auto dim_count = image_dim_count(image_type);
			const auto is_array = has_flag<COMPUTE_IMAGE_TYPE::FLAG_ARRAY>(image_type);
			// tex if readable, surf otherwise
			const auto is_tex = has_flag<COMPUTE_IMAGE_TYPE::READ>(full_image_type);

			// query/get function base
			const auto query_image = [&](const std::string& query_name) {
				SmallVector<llvm::Type*, 2> func_arg_types;
				SmallVector<llvm::Value*, 2> func_args;
				func_arg_types.push_back(img_handle_arg->getType());
				func_args.push_back(img_handle_arg);
				if (is_tex) {
					func_arg_types.push_back(lod_arg->getType());
					func_args.push_back(lod_arg);
				}
				
				// -> build asm call
				std::string asm_str = (is_tex ? "txq.level." : "suq.") + query_name + ".b32 ";
				std::string constraints_str = "=r,l";
				asm_str += "$0, [$1]";
				if (is_tex) {
					asm_str += ", $2";
					constraints_str += ",r";
				}
				asm_str += ";";
				
				// create the asm call
				const auto asm_func_type = FunctionType::get(llvm::Type::getInt32Ty(*ctx), func_arg_types, false);
				auto asm_func = InlineAsm::get(asm_func_type, asm_str, constraints_str, false /* !volatile */);
				auto get_call = builder->CreateCall(asm_func, func_args);
				get_call->setConvergent();
				get_call->setOnlyAccessesArgMemory();
				get_call->setDoesNotThrow();
				get_call->setOnlyReadsMemory(); // all get_* calls are readonly (can be optimized away if unused)
				get_call->setDebugLoc(I.getDebugLoc()); // keep debug loc
				return get_call;
			};
			
			// we have to a return a full image dim query for all dims of the image (type dependent)
			// order is: width [, height] [, depth], [, layer_count]
			// non-existing dims are set to 0
			const auto ret_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 4);
			llvm::Value* ret_vec = UndefValue::get(ret_type);
			uint32_t ret_vec_idx = 0;
			// all images have a width
			ret_vec = builder->CreateInsertElement(ret_vec, query_image("width"), builder->getInt32(ret_vec_idx++));
			if (dim_count >= 2) {
				ret_vec = builder->CreateInsertElement(ret_vec, query_image("height"), builder->getInt32(ret_vec_idx++));
			}
			if (dim_count >= 3) {
				ret_vec = builder->CreateInsertElement(ret_vec, query_image("depth"), builder->getInt32(ret_vec_idx++));
			}
			if (is_array) {
				ret_vec = builder->CreateInsertElement(ret_vec, query_image("array_size"), builder->getInt32(ret_vec_idx++));
			}
			// NOTE: while cube maps technically have 6 layers, this number is not stored in the image dim
			
			// fill remaining components with 0
			for (uint32_t vec_idx = ret_vec_idx; vec_idx < 4; ++vec_idx) {
				ret_vec = builder->CreateInsertElement(ret_vec, builder->getInt32(0), builder->getInt32(ret_vec_idx++));
			}
			
			//
			I.replaceAllUsesWith(ret_vec);
			I.eraseFromParent();
		}
		
	};
}

char CUDAImage::ID = 0;
INITIALIZE_PASS_BEGIN(CUDAImage, "CUDAImage", "CUDAImage Pass", false, false)
INITIALIZE_PASS_END(CUDAImage, "CUDAImage", "CUDAImage Pass", false, false)

FunctionPass *llvm::createCUDAImagePass(const uint32_t image_capabilities) {
	return new CUDAImage(image_capabilities);
}
