//===- VulkanImage.cpp - Vulkan-specific floor image transformations ------===//
////
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
// This pass implements the SPIR-V-specific floor image transformations, i.e.
// floor.opaque.<read/write function>.* -> SPIR-V image function call
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
#include "llvm/Transforms/LibFloor/VulkanSampling.h"
#include <unordered_map>
using namespace llvm;

#define DEBUG_TYPE "VulkanImage"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

namespace {
	struct VulkanImage : public FloorImageBasePass {
		static char ID; // Pass identification, replacement for typeid
		
		VulkanImage(const uint32_t image_capabilities_ = 0) :
		FloorImageBasePass(ID, IMAGE_TYPE_ID::OPAQUE, image_capabilities_) {
			initializeVulkanImagePass(*PassRegistry::getPassRegistry());
		}
		
		llvm::Function* get_or_create_spirv_function(std::string func_name,
													 llvm::Type* ret_type,
													 const SmallVector<llvm::Type*, 8>& func_arg_types,
													 const bool is_readnone = false) {
			const auto func_type = llvm::FunctionType::get(ret_type, func_arg_types, false);
			auto func = M->getFunction(func_name);
			if(func == nullptr) { // only do this once
				func = dyn_cast<Function>(M->getOrInsertFunction(func_name, func_type,
																 is_readnone ?
																 nounwind_readnone_attr :
																 nounwind_attr).getCallee());
				func->setCallingConv(CallingConv::FLOOR_FUNC);
				// TODO: any other flags here?
			}
			return func;
		}
		
		static const char* type_to_geom(const COMPUTE_IMAGE_TYPE& image_type) {
			switch(image_type) {
				case COMPUTE_IMAGE_TYPE::IMAGE_1D:
					return "11ocl_image1d";
				case COMPUTE_IMAGE_TYPE::IMAGE_1D_ARRAY:
					return "16ocl_image1darray";
				case COMPUTE_IMAGE_TYPE::IMAGE_1D_BUFFER:
					return "17ocl_image1dbuffer";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH:
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_STENCIL:
					return "16ocl_image2ddepth";
				case COMPUTE_IMAGE_TYPE::IMAGE_2D:
					return "11ocl_image2d";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_ARRAY:
					return "21ocl_image2darraydepth";
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_ARRAY:
					return "16ocl_image2darray";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA:
					return "20ocl_image2dmsaadepth";
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA:
					return "15ocl_image2dmsaa";
				case COMPUTE_IMAGE_TYPE::IMAGE_3D:
					return "11ocl_image3d";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA_ARRAY:
					return "25ocl_image2darraymsaadepth";
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA_ARRAY:
					return "20ocl_image2darraymsaa";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE:
					return "18ocl_imagecubedepth";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE_ARRAY:
					return "23ocl_imagecubearraydepth";
				case COMPUTE_IMAGE_TYPE::IMAGE_CUBE:
					return "13ocl_imagecube";
				case COMPUTE_IMAGE_TYPE::IMAGE_CUBE_ARRAY:
					return "18ocl_imagecubearray";
				default:
					return nullptr;
			}
		}
		
		void handle_vk_coord(Instruction& I,
							 llvm::Value* coord_arg,
							 llvm::Value* layer_arg,
							 const bool is_array,
							 const bool is_msaa,
							 const bool is_non_cube_array_depth_compare,
							 // must have: true: int coords, false: float coords
							 const bool must_have_int_args,
							 std::string& vk_func_name,
							 SmallVector<llvm::Type*, 8>& func_arg_types,
							 SmallVector<llvm::Value*, 8>& func_args) {
			auto coord_vec_type = dyn_cast_or_null<FixedVectorType>(coord_arg->getType());
			const auto coord_dim = coord_vec_type->getNumElements();
			if(!coord_vec_type) {
				ctx->emitError(&I, "invalid image coordinate argument (cast to vector failed)");
				return;
			}
			
			const auto coord_type = coord_vec_type->getElementType();
			const auto is_int_coord = coord_type->isIntegerTy();
			if(!is_int_coord && !coord_type->isFloatTy()) {
				ctx->emitError(&I, "invalid coordinate type (neither int nor float)");
				return;
			}
			
			auto vk_coord_dim = coord_dim + (is_array ? 1 : 0) + (is_non_cube_array_depth_compare ? 1 : 0);
			assert(vk_coord_dim <= 4 && "can't have a coord vector type with dim > 4");
			const auto vk_coord_scalar_type = (must_have_int_args ? llvm::Type::getInt32Ty(*ctx) : llvm::Type::getFloatTy(*ctx));
			const auto vk_coord_type = (vk_coord_dim == 1 ?
										vk_coord_scalar_type :
										llvm::FixedVectorType::get(vk_coord_scalar_type, vk_coord_dim));
			
			//
			bool convert_to_int = false, convert_to_float = false;
			if(must_have_int_args && !is_int_coord) {
				convert_to_int = true;
			}
			else if(!must_have_int_args && is_int_coord) {
				convert_to_float = true;
			}
			const auto convert_val = [&convert_to_int, &convert_to_float,
									  &vk_coord_scalar_type, this](llvm::Value* val) {
				return (convert_to_int ?
						builder->CreateFPToSI(val, vk_coord_scalar_type) :
						(convert_to_float ?
						 builder->CreateSIToFP(val, vk_coord_scalar_type) :
						 val));
			};
			
			// start with the specified coord arg, there are some cases where we can just use it without rebuilding
			auto vk_coord_arg = coord_arg;
			if(vk_coord_type != coord_vec_type) {
				if(vk_coord_dim == 1) {
					// just a scalar
					vk_coord_arg = convert_val(builder->CreateExtractElement(coord_arg, builder->getInt32(0)));
				}
				else {
					// create a new tmp coord, then copy coord elements (keep unused undef)
					vk_coord_arg = UndefValue::get(vk_coord_type);
					uint32_t coord_idx = 0;
					for(; coord_idx < coord_dim; ++coord_idx) {
						vk_coord_arg = builder->CreateInsertElement(vk_coord_arg,
																	convert_val(builder->CreateExtractElement(coord_arg,
																											  builder->getInt32(coord_idx))),
																	builder->getInt32(coord_idx));
					}
					
					// need to pull the layer index into the coordinate, including possible int -> float conversion
					if(is_array) {
						auto layer = layer_arg;
						if(!must_have_int_args) {
							// need to convert
							layer = builder->CreateUIToFP(layer_arg, vk_coord_scalar_type);
						}
						vk_coord_arg = builder->CreateInsertElement(vk_coord_arg, layer, builder->getInt32(coord_idx++));
					}
					
					// workaround a bug in nvidia drivers where another coord component is expected when doing depth comparison
					// (this seems to be a remnant of glsl where Dref was stored as the last component instead of an additional argument)
					if(is_non_cube_array_depth_compare) {
						vk_coord_arg = builder->CreateInsertElement(vk_coord_arg,
																	ConstantFP::get(llvm::Type::getFloatTy(*ctx), 0.0f),
																	builder->getInt32(coord_idx++));
					}
				}
			}
			func_arg_types.push_back(vk_coord_arg->getType());
			func_args.push_back(vk_coord_arg);
			
			if(vk_coord_dim > 1) {
				vk_func_name += "Dv" + std::to_string(vk_coord_dim) + "_";
			}
			vk_func_name += (must_have_int_args ? "i" : "f");
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
			SmallVector<llvm::Type*, 8> func_arg_types;
			SmallVector<llvm::Value*, 8> func_args;
			auto coord_vec_type = dyn_cast_or_null<FixedVectorType>(coord_arg->getType());
			
			// NOTE: read call will be constructed as follows ([arg] are optional args):
			// read(image, sampler_idx, coord_with_layer,
			//      lod_type, [lod_arg_0], [lod_arg_1],
			//      bool is_offset, [offset],
			//      [sample_idx],
			//      [compare_val])
			// -> this will use cxx mangling, since we still need to differentiate the calls later on
			
			// TODO: properly handle cube images -> must either use explicit/implicit sampling or read, not fetch
			// TODO: properly handle depth compare -> must use explicit/implicit sampling, not fetch
			
			// must be constant/constexpr for now
			if(const_sampler_arg == nullptr) {
				ctx->emitError(&I, "sampler must be a constant");
				return;
			}
			const vulkan_sampling::sampler sampler_val { (uint32_t)const_sampler_arg->getZExtValue() };
			const auto is_fetch = ((sampler_val.value & vulkan_sampling::sampler::COORD_MODE::__COORD_MODE_MASK) ==
								   vulkan_sampling::sampler::COORD_MODE::PIXEL);
			
			// get geom string / mangled name + flags
			const auto geom_cstr = type_to_geom(image_type);
			if (!geom_cstr) {
				ctx->emitError(&I, "unknown or incorrect image type");
				return;
			}
			std::string geom = geom_cstr;
			const auto is_array = has_flag<COMPUTE_IMAGE_TYPE::FLAG_ARRAY>(image_type);
			const auto is_msaa = has_flag<COMPUTE_IMAGE_TYPE::FLAG_MSAA>(image_type);
			const auto is_cube = has_flag<COMPUTE_IMAGE_TYPE::FLAG_CUBE>(image_type);
			const auto is_depth = has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(image_type);
			
			// -> return data and vulkan function name
			// NOTE: we don't have a c++ mangling support in here, so do it manually
			// (this is actually easy enough, since everything is very static)
			std::string vk_func_name;
			llvm::Type* ret_type;
			if(func_name.endswith(".float")) {
				vk_func_name = "_Z11read_imagef";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getFloatTy(*ctx), 4);
			}
			else if(func_name.endswith(".int")) {
				vk_func_name = "_Z11read_imagei";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 4);
			}
			else if(func_name.endswith(".uint")) {
				vk_func_name = "_Z12read_imageui";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 4);
			}
			else if(func_name.endswith(".half")) {
				vk_func_name = "_Z11read_imageh";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getHalfTy(*ctx), 4);
			}
			// unknown -> ignore
			else return;
			
			// -> geom/image
			vk_func_name += geom;
			func_arg_types.push_back(img_handle_arg->getType());
			func_args.push_back(img_handle_arg);
			
			// -> sampler
			vk_func_name += "11ocl_sampler"; // technically "i"
			func_arg_types.push_back(const_sampler_arg->getType());
			func_args.push_back(const_sampler_arg);
			
			// -> coord
			handle_vk_coord(I,
							coord_arg,
							layer_arg,
							is_array,
							is_msaa,
							is_depth && is_compare && !(is_cube && is_array),
							// fetch: always int coords, sample: always float coords
							is_fetch,
							vk_func_name,
							func_arg_types,
							func_args);
			
			// -> lod / bias / gradient
			vulkan_sampling::LOD_TYPE lod_type = vulkan_sampling::LOD_TYPE::INVALID;
			const auto const_fp_lod_or_bias = dyn_cast<ConstantFP>(lod_or_bias_arg);
			if(!is_msaa) {
				// -> gradient
				if(is_gradient) {
					lod_type = vulkan_sampling::LOD_TYPE::GRADIENT;
				}
				// -> lod / bias
				else {
					// -> lod
					if(is_lod_or_bias) {
						lod_type = vulkan_sampling::LOD_TYPE::EXPLICIT_LOD;
					}
					// -> bias
					else {
						// if this is inside a fragment shader and this isn't a fetch, use implicit lod
						if(is_fragment_shader && !is_fetch) {
							// if bias is 0, only use implicit lod as-is
							// NOTE: also makes sure that bias is a float value
							if(const_fp_lod_or_bias != nullptr &&
							   const_fp_lod_or_bias->isZero()) {
								lod_type = vulkan_sampling::LOD_TYPE::IMPLICIT_LOD;
							}
							// if bias is != 0 and we're not in fetch mode, specify the bias
							else if(!is_fetch && lod_or_bias_arg->getType()->isFloatTy()) {
								lod_type = vulkan_sampling::LOD_TYPE::IMPLICIT_LOD_WITH_BIAS;
							}
							// else: convert to explicit lod
							else {
								lod_type = vulkan_sampling::LOD_TYPE::EXPLICIT_LOD;
							}
						}
						// for all others: convert to an explicit lod
						else {
							lod_type = vulkan_sampling::LOD_TYPE::EXPLICIT_LOD;
						}
					}
				}
			}
			else {
				// msaa allows no lod
				lod_type = vulkan_sampling::LOD_TYPE::NO_LOD;
			}
			assert(lod_type != vulkan_sampling::LOD_TYPE::INVALID && "invalid lod type");
			
			vk_func_name += "i";
			auto lod_type_arg = ConstantInt::get(Type::getInt32Ty(*ctx), (uint32_t)lod_type);
			func_arg_types.push_back(lod_type_arg->getType());
			func_args.push_back(lod_type_arg);
			
			const auto is_int_lod = (is_fetch);
			switch(lod_type) {
				case vulkan_sampling::LOD_TYPE::NO_LOD:
				case vulkan_sampling::LOD_TYPE::IMPLICIT_LOD:
					// nothing to emit here
					break;
				case vulkan_sampling::LOD_TYPE::GRADIENT: {
					// gradient coord_dim must be equal to image coord dim (- layer)
					const auto coord_dim = coord_vec_type->getNumElements();
					if(coord_dim == 1) {
						// extract scalar
						vk_func_name += "ff";
						func_arg_types.push_back(builder->getFloatTy());
						func_args.push_back(builder->CreateExtractElement(dpdx_arg, builder->getInt32(0)));
						func_arg_types.push_back(builder->getFloatTy());
						func_args.push_back(builder->CreateExtractElement(dpdy_arg, builder->getInt32(0)));
					}
					else if(coord_dim == 2) {
						// just pass-through
						vk_func_name += "Dv2_fDv2_f";
						func_arg_types.push_back(dpdx_arg->getType());
						func_args.push_back(dpdx_arg);
						func_arg_types.push_back(dpdy_arg->getType());
						func_args.push_back(dpdy_arg);
					}
					else if(coord_dim == 3) {
						// just pass-through
						vk_func_name += "Dv3_fDv3_f";
						func_arg_types.push_back(dpdx_arg->getType());
						func_args.push_back(dpdx_arg);
						func_arg_types.push_back(dpdy_arg->getType());
						func_args.push_back(dpdy_arg);
					}
					else llvm_unreachable("invalid coord dim");
					
					break;
				}
				case vulkan_sampling::LOD_TYPE::IMPLICIT_LOD_WITH_BIAS:
					vk_func_name += "f";
					func_arg_types.push_back(lod_or_bias_arg->getType());
					func_args.push_back(lod_or_bias_arg);
					break;
				case vulkan_sampling::LOD_TYPE::EXPLICIT_LOD: {
					Value* lod = nullptr;
					if(is_int_lod && lod_or_bias_arg->getType()->isFloatTy()) {
						// convert to int
						lod = builder->CreateFPToSI(lod_or_bias_arg, llvm::Type::getInt32Ty(*ctx));
					}
					else if(!is_int_lod && lod_or_bias_arg->getType()->isIntegerTy()) {
						// convert to float
						lod = builder->CreateSIToFP(lod_or_bias_arg, llvm::Type::getFloatTy(*ctx));
					}
					else lod = lod_or_bias_arg;
					
					vk_func_name += (is_int_lod ? "i" : "f");
					func_arg_types.push_back(lod->getType());
					func_args.push_back(lod);
					break;
				}
				default: llvm_unreachable("invalid lod type");
			}
			
			// -> offset
			vk_func_name += "b";
			const auto is_offset_arg = (is_offset ? ConstantInt::getTrue(*ctx) : ConstantInt::getFalse(*ctx));
			func_arg_types.push_back(is_offset_arg->getType());
			func_args.push_back(is_offset_arg);
			
			if(is_offset) {
				// offset coord_dim must be equal to image coord dim (- layer)
				const auto coord_dim = coord_vec_type->getNumElements();
				if(coord_dim == 1) {
					// extract scalar
					vk_func_name += "i";
					auto extracted_arg = builder->CreateExtractElement(offset_arg, builder->getInt32(0));
					func_arg_types.push_back(extracted_arg->getType());
					func_args.push_back(extracted_arg);
				}
				else if(coord_dim == 2) {
					// just pass-through
					vk_func_name += "Dv2_i";
					func_arg_types.push_back(offset_arg->getType());
					func_args.push_back(offset_arg);
				}
				else if(coord_dim == 3) {
					// just pass-through
					vk_func_name += "Dv3_i";
					func_arg_types.push_back(offset_arg->getType());
					func_args.push_back(offset_arg);
				}
				else llvm_unreachable("invalid coord dim");
			}
			// else: no arg
			
			// -> sample idx
			if(is_msaa) {
				if(!sample_arg->getType()->isIntegerTy()) {
					ctx->emitError(&I, "msaa sample index must be integer");
					return;
				}
				
				vk_func_name += "i";
				func_arg_types.push_back(sample_arg->getType());
				func_args.push_back(sample_arg);
			}
			
			// -> depth compare
			if(is_depth && is_compare) {
				if(!compare_value_arg->getType()->isFloatTy()) {
					ctx->emitError(&I, "compare value must be float");
					return;
				}
				
				// return type is always a scalar float
				ret_type = Type::getFloatTy(*ctx);
				
				vk_func_name += "f";
				func_arg_types.push_back(compare_value_arg->getType());
				func_args.push_back(compare_value_arg);
			}
			
			// create the vulkan call
			// NOTE: always returns a vector4
			auto read_func = get_or_create_spirv_function(vk_func_name, ret_type, func_arg_types, true);
			llvm::CallInst* read_call = builder->CreateCall(read_func, func_args);
			read_call->setConvergent();
			read_call->setOnlyAccessesArgMemory();
			read_call->setDoesNotThrow();
			read_call->setOnlyReadsMemory(); // all reads are readonly (can be optimized away if unused)
			read_call->setDebugLoc(I.getDebugLoc()); // keep debug loc
			read_call->setCallingConv(CallingConv::FLOOR_FUNC);
			
			// if this is a depth compare, the return type is a float -> create a float4
			llvm::Value* read_call_result = read_call;
			if(is_depth && is_compare) {
				read_call_result = UndefValue::get(llvm::FixedVectorType::get(llvm::Type::getFloatTy(*ctx), 4));
				read_call_result = builder->CreateInsertElement(read_call_result, read_call, builder->getInt32(0));
				// NOTE: rest of vector is undef/zero (and will be stripped away again anyways)
			}
			
			//
			I.replaceAllUsesWith(read_call_result);
			I.eraseFromParent();
			
			//
			simplify_image_handle(img_handle_arg);
		}
		
		void handle_write_image(Instruction& I,
								const StringRef& func_name,
								llvm::Value* img_handle_arg,
								const COMPUTE_IMAGE_TYPE& full_image_type,
								const COMPUTE_IMAGE_TYPE& image_type,
								const COMPUTE_IMAGE_TYPE& format_type,
								const COMPUTE_IMAGE_TYPE& data_type,
								llvm::Value* /* rt_image_type */,
								const bool& is_normalized,
								const uint32_t& image_channel_count,
								llvm::Value* coord_arg,
								llvm::Value* layer_arg,
								llvm::Value* lod_arg,
								const bool is_lod,
								llvm::Value* data_arg) override {
			SmallVector<llvm::Type*, 8> func_arg_types;
			SmallVector<llvm::Value*, 8> func_args;
			
			// NOTE: write call will be constructed as follows ([arg] are optional args):
			// write(image, coord_with_layer, data,
			//       // NOTE: only explicit lod or no lod
			//       lod_type, [lod_arg_0])
			
			//// more arg checking
			auto coord_vec_type = dyn_cast_or_null<VectorType>(coord_arg->getType());
			if(!coord_vec_type) {
				ctx->emitError(&I, "invalid image coordinate argument (cast to vector failed)");
				return;
			}
			
			const auto coord_type = coord_vec_type->getElementType();
			if(!coord_type->isIntegerTy()) {
				ctx->emitError(&I, "coordinate type must be integer");
				return;
			}
			
			std::string vk_func_name, dtype;
			if(func_name.endswith(".float")) {
				vk_func_name = "_Z12write_imagef";
				dtype = "f";
			}
			else if(func_name.endswith(".int")) {
				vk_func_name = "_Z12write_imagei";
				dtype = "i";
			}
			else if(func_name.endswith(".uint")) {
				vk_func_name = "_Z13write_imageui";
				dtype = "j";
			}
			else if(func_name.endswith(".half")) {
				vk_func_name = "_Z12write_imageh";
				dtype = "h";
			}
			// unknown -> ignore
			else return;
			
			//// func replacement
			// -> geom
			const auto geom_cstr = type_to_geom(image_type);
			if (!geom_cstr) {
				ctx->emitError(&I, "unknown or incorrect image type");
				return;
			}
			std::string geom = geom_cstr;
			const auto is_array = has_flag<COMPUTE_IMAGE_TYPE::FLAG_ARRAY>(image_type);
			const auto is_msaa = has_flag<COMPUTE_IMAGE_TYPE::FLAG_MSAA>(image_type);
			//const auto is_cube = has_flag<COMPUTE_IMAGE_TYPE::FLAG_CUBE>(image_type);
			//const auto is_depth = has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(image_type);
			
			vk_func_name += geom;
			
			// TODO: vulkan/spir-v can write cube, depth, msaa images ?
			
			func_arg_types.push_back(img_handle_arg->getType());
			func_args.push_back(img_handle_arg);
			
			// -> coord
			handle_vk_coord(I,
							coord_arg,
							layer_arg,
							is_array,
							is_msaa,
							false,
							true, // must always have int coords for writes
							vk_func_name,
							func_arg_types,
							func_args);
			
			// -> data
			// data is always a vector4
			vk_func_name += "Dv4_" + dtype;
			func_arg_types.push_back(data_arg->getType());
			func_args.push_back(data_arg);
			
			// -> lod
			vk_func_name += "i";
			func_arg_types.push_back(Type::getInt32Ty(*ctx));
			func_args.push_back(ConstantInt::get(Type::getInt32Ty(*ctx),
												 uint32_t(is_lod ?
														  vulkan_sampling::LOD_TYPE::EXPLICIT_LOD :
														  vulkan_sampling::LOD_TYPE::NO_LOD)));
			if(is_lod) {
				// always int
				vk_func_name += "i";
				func_arg_types.push_back(lod_arg->getType());
				func_args.push_back(lod_arg);
			}
			
			// -> sample idx
			// TODO: !
			
			// create the vulkan call
			auto write_func = get_or_create_spirv_function(vk_func_name, builder->getVoidTy(), func_arg_types, false);
			llvm::CallInst* write_call = builder->CreateCall(write_func, func_args);
			write_call->setDebugLoc(I.getDebugLoc()); // keep debug loc
			write_call->setCallingConv(CallingConv::FLOOR_FUNC);
			
			//
			I.replaceAllUsesWith(write_call);
			I.eraseFromParent();
			
			//
			simplify_image_handle(img_handle_arg);
		}
		
		// in cases where the image handle is acquired through image arrays,
		// replace any (bitcast-)gep-(bitcast-)load(-inttoptr) chains with a floor.image_array_load call
		// this is necessary, because we need to handle these specially on the SPIR-V side
		void simplify_image_handle(llvm::Value* handle) {
			llvm::Value* img_handle = handle;
			
			// loaded handle might be "int-to-ptr" casted
			IntToPtrInst* ITPtrI = dyn_cast_or_null<IntToPtrInst>(handle);
			if (ITPtrI) {
				img_handle = ITPtrI->getOperand(0);
			}
			
			// directly abort if not a load (array element is always loaded)
			LoadInst* LI = dyn_cast<LoadInst>(img_handle);
			if(!LI) return;
			
			// load operand might be bitcasted
			auto load_ptr_op = LI->getPointerOperand();
			BitCastInst* load_ptr_op_bc = dyn_cast_or_null<BitCastInst>(load_ptr_op);
			if (load_ptr_op_bc) {
				load_ptr_op = load_ptr_op_bc->getOperand(0);
			}
			
			// array element ptr must have come from a GEP
			GetElementPtrInst* array_elem_gep = dyn_cast<GetElementPtrInst>(load_ptr_op);
			if(!array_elem_gep) return;
			
			// GEP src pointer
			auto src_ptr = array_elem_gep->getPointerOperand();
			auto array_type_ = src_ptr->getType()->getPointerElementType();
			if(!array_type_->isArrayTy()) return;
			ArrayType* array_type = cast<ArrayType>(array_type_);
			
			// check if this is a 2D array, in which case there is another load
			LoadInst* outer_load = dyn_cast<LoadInst>(src_ptr);
			GetElementPtrInst* outer_array_elem_gep = nullptr;
			bool is_2d_array = false;
			if (outer_load) {
				is_2d_array = true;
				auto outer_load_ptr_op = outer_load->getPointerOperand();
				// might be bitcasted as well
				BitCastInst* outer_load_ptr_op_bc = dyn_cast_or_null<BitCastInst>(outer_load_ptr_op);
				if (outer_load_ptr_op_bc) {
					outer_load_ptr_op = outer_load_ptr_op_bc->getOperand(0);
				}
				
				// again: array element ptr must have come from a GEP
				outer_array_elem_gep = dyn_cast<GetElementPtrInst>(outer_load_ptr_op);
				if (!outer_array_elem_gep) return;
				
				// actual GEP src pointer
				src_ptr = outer_array_elem_gep->getPointerOperand();
				array_type_ = src_ptr->getType()->getPointerElementType();
				if (!array_type_->isArrayTy()) return;
				array_type = cast<ArrayType>(array_type_);
			}
			
			// check if it started out from a bitcast (used by dynamic indexing)
			BitCastInst* BC = dyn_cast<BitCastInst>(src_ptr);
			ArrayType* src_array_type = nullptr;
			Value* img_array = nullptr;
			if(BC) {
				// abort if bitcast src isn't [N * something]*
				img_array = BC->getOperand(0);
				auto src_array_ptr_type = BC->getSrcTy();
				if(!src_array_ptr_type->isPointerTy() ||
				   !src_array_ptr_type->getPointerElementType()->isArrayTy()) {
					return;
				}
				src_array_type = cast<ArrayType>(src_array_ptr_type->getPointerElementType());
				
				// abort if array elem type is a pointer (we're expecting [N x %"class.floor_image::const_image"])
				if(array_type->getArrayElementType()->isPointerTy()) {
					return;
				}
			} else {
				// else: constant indexing (just a gep + load)
				img_array = src_ptr;
				src_array_type = array_type;
			}
			
			// abort if not an opaque ptr type in an address space
			// TODO: opaque type check?
			llvm::Type* img_type = nullptr;
			if (!is_2d_array) {
				if (!src_array_type->getArrayElementType()->isPointerTy() ||
					src_array_type->getArrayElementType()->getPointerAddressSpace() == 0) {
					return;
				}
				img_type = src_array_type->getArrayElementType();
			} else {
				if (!src_array_type->getArrayElementType()->isPointerTy() ||
					!src_array_type->getArrayElementType()->getPointerElementType()->isArrayTy() ||
					!src_array_type->getArrayElementType()->getPointerElementType()->getArrayElementType()->isPointerTy() ||
					src_array_type->getArrayElementType()->getPointerElementType()->getArrayElementType()->getPointerAddressSpace() == 0) {
					return;
				}
				img_type = src_array_type->getArrayElementType()->getPointerElementType()->getArrayElementType();
			}
			
			// create the call
			std::vector<Value*> params {
				img_array
			};
			// GEP ptr, 0, (%outer_idx,) %idx, ... -> want third operand(s)
			if (is_2d_array) {
				params.push_back(outer_array_elem_gep->getOperand(2));
			}
			params.push_back(array_elem_gep->getOperand(2));
			
			std::vector<Type*> param_types;
			for (const auto& param : params) {
				param_types.push_back(param->getType());
			}
			
			const auto func_type = llvm::FunctionType::get(img_type, param_types, false);
			
			auto handle_instr = cast<Instruction>(handle);
			std::string elem_counts_str = "." + std::to_string(src_array_type->getNumElements());
			if (is_2d_array) {
				auto inner_arr_type = src_array_type = cast<ArrayType>(src_array_type->getArrayElementType()->getPointerElementType());
				elem_counts_str += "." + std::to_string(inner_arr_type->getNumElements());
			}
			auto CI = CallInst::Create(M->getOrInsertFunction("floor.image_array_load." +
															  img_type->getPointerElementType()->getStructName().str() +
															  elem_counts_str,
															  func_type),
									   params, "imgarrld", handle_instr);
			
			// if dynamic access (bitcast), then bitcast has the debug location, use gep location otherwise
			CI->setDebugLoc(BC ? BC->getDebugLoc() : array_elem_gep->getDebugLoc());
			
			handle_instr->replaceAllUsesWith(CI);
			handle_instr->eraseFromParent();
		}
		
		void handle_get_image_dim(Instruction& I,
								  const StringRef& func_name,
								  llvm::Value* img_handle_arg,
								  const COMPUTE_IMAGE_TYPE& /* full_image_type */,
								  const COMPUTE_IMAGE_TYPE& image_type,
								  llvm::Value* lod_arg) override {
			// gather info
			const auto dim_count = image_dim_count(image_type);
			const auto is_array = has_flag<COMPUTE_IMAGE_TYPE::FLAG_ARRAY>(image_type);

			const auto geom_cstr = type_to_geom(image_type);
			if (!geom_cstr) {
				ctx->emitError(&I, "unknown or incorrect image type");
				return;
			}
			const std::string geom = geom_cstr;

			// query/get function base
			const auto query_image = [&](const std::string& query_name) {
				SmallVector<llvm::Type*, 8> func_arg_types;
				SmallVector<llvm::Value*, 8> func_args;
				func_arg_types.push_back(img_handle_arg->getType());
				func_args.push_back(img_handle_arg);
				func_arg_types.push_back(lod_arg->getType());
				func_args.push_back(lod_arg);
				
				// -> build get func name
				const std::string get_func_name = query_name + geom + "i";
				
				// create the air call
				const auto ret_type = llvm::Type::getInt32Ty(*ctx);
				auto get_func = get_or_create_spirv_function(get_func_name, ret_type, func_arg_types, true);
				llvm::CallInst* get_call = builder->CreateCall(get_func, func_args);
				get_call->setConvergent();
				get_call->setOnlyAccessesArgMemory();
				get_call->setDoesNotThrow();
				get_call->setOnlyReadsMemory(); // all get_* calls are readonly (can be optimized away if unused)
				get_call->setDebugLoc(I.getDebugLoc()); // keep debug loc
				get_call->setCallingConv(CallingConv::FLOOR_FUNC);
				return get_call;
			};
			
			// we have to a return a full image dim query for all dims of the image (type dependent)
			// order is: width [, height] [, depth], [, layer_count]
			// non-existing dims are set to 0
			const auto ret_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 4);
			llvm::Value* ret_vec = UndefValue::get(ret_type);
			uint32_t ret_vec_idx = 0;
			// all images have a width
			ret_vec = builder->CreateInsertElement(ret_vec, query_image("_Z15get_image_width"), builder->getInt32(ret_vec_idx++));
			if (dim_count >= 2) {
				ret_vec = builder->CreateInsertElement(ret_vec, query_image("_Z16get_image_height"), builder->getInt32(ret_vec_idx++));
			}
			if (dim_count >= 3) {
				ret_vec = builder->CreateInsertElement(ret_vec, query_image("_Z15get_image_depth"), builder->getInt32(ret_vec_idx++));
			}
			if (is_array) {
				ret_vec = builder->CreateInsertElement(ret_vec, query_image("_Z20get_image_array_size"), builder->getInt32(ret_vec_idx++));
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

char VulkanImage::ID = 0;
INITIALIZE_PASS_BEGIN(VulkanImage, "VulkanImage", "VulkanImage Pass", false, false)
INITIALIZE_PASS_END(VulkanImage, "VulkanImage", "VulkanImage Pass", false, false)

FunctionPass *llvm::createVulkanImagePass(const uint32_t image_capabilities) {
	return new VulkanImage(image_capabilities);
}
