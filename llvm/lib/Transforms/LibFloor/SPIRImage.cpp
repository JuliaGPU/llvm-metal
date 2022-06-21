//===- SPIRImage.cpp - SPIR-specific floor image transformations ----------===//
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
// This pass implements the SPIR-specific floor image transformations, i.e.
// floor.opaque.<read/write function>.* -> spir image function call
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
#include <unordered_map>
using namespace llvm;

#define DEBUG_TYPE "SPIRImage"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

namespace {
	struct SPIRImage : public FloorImageBasePass {
		static char ID; // Pass identification, replacement for typeid
		bool enable_intel_workarounds { false };
		
		SPIRImage(const uint32_t image_capabilities_ = 0,
				  const bool enable_intel_workarounds_ = false) :
		FloorImageBasePass(ID, IMAGE_TYPE_ID::OPAQUE, image_capabilities_),
		enable_intel_workarounds(enable_intel_workarounds_) {
			initializeSPIRImagePass(*PassRegistry::getPassRegistry());
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
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE_ARRAY:
				case COMPUTE_IMAGE_TYPE::IMAGE_CUBE:
				case COMPUTE_IMAGE_TYPE::IMAGE_CUBE_ARRAY:
				default:
					return nullptr;
			}
		}
		
		llvm::Function* get_or_create_spir_function(std::string func_name,
													llvm::Type* ret_type,
													const SmallVector<llvm::Type*, 8>& func_arg_types,
													const bool is_readnone = false) {
			if(enable_intel_workarounds) {
				// intel name mangling is a bit off sometimes, so use their name mangling instead
				// correct -> intel
				static const std::unordered_map<std::string, std::string> intel_func_repl {
					{ "_Z11read_imagef11ocl_image2d11ocl_samplerDv2_fDv2_fDv2_f", "_Z11read_imagef11ocl_image2d11ocl_samplerDv2_fS_S_" },
					{ "_Z11read_imagei11ocl_image2d11ocl_samplerDv2_fDv2_fDv2_f", "_Z11read_imagei11ocl_image2d11ocl_samplerDv2_fS_S_" },
					{ "_Z12read_imageui11ocl_image2d11ocl_samplerDv2_fDv2_fDv2_f", "_Z12read_imageui11ocl_image2d11ocl_samplerDv2_fS_S_" },
					{ "_Z11read_imagef16ocl_image2ddepth11ocl_samplerDv2_fDv2_fDv2_f", "_Z11read_imagef16ocl_image2ddepth11ocl_samplerDv2_fS_S_" },
					{ "_Z11read_imagef11ocl_image3d11ocl_samplerDv4_fDv4_fDv4_f", "_Z11read_imagef11ocl_image3d11ocl_samplerDv4_fS_S_" },
					{ "_Z11read_imagei11ocl_image3d11ocl_samplerDv4_fDv4_fDv4_f", "_Z11read_imagei11ocl_image3d11ocl_samplerDv4_fS_S_" },
					{ "_Z12read_imageui11ocl_image3d11ocl_samplerDv4_fDv4_fDv4_f", "_Z12read_imageui11ocl_image3d11ocl_samplerDv4_fS_S_" },
					{ "_Z12write_imagei11ocl_image3dDv4_iDv4_i", "_Z12write_imagei11ocl_image3dDv4_iS_" },
					{ "_Z12write_imagei11ocl_image3dDv4_iiDv4_i", "_Z12write_imagei11ocl_image3dDv4_iiS_" },
					{ "_Z12write_imagei16ocl_image2darrayDv4_iDv4_i", "_Z12write_imagei16ocl_image2darrayDv4_iS_" },
					{ "_Z12write_imagei16ocl_image2darrayDv4_iiDv4_i", "_Z12write_imagei16ocl_image2darrayDv4_iiS_" },
				};
				const auto iter = intel_func_repl.find(func_name);
				if(iter != intel_func_repl.end()) {
					func_name = iter->second;
				}
			}
			
			const auto func_type = llvm::FunctionType::get(ret_type, func_arg_types, false);
			auto func = M->getFunction(func_name);
			if(func == nullptr) { // only do this once
				func = dyn_cast<Function>(M->getOrInsertFunction(func_name, func_type,
																 is_readnone ?
																 nounwind_readnone_attr :
																 nounwind_attr).getCallee());
				func->setCallingConv(CallingConv::FLOOR_FUNC);
			}
			return func;
		}
		
		SmallVector<llvm::Value*, 3> get_image_dim(llvm::Value* img_handle_arg,
												   llvm::FixedVectorType* coord_vec_type,
												   const std::string& geom) {
			SmallVector<llvm::Value*, 3> ret;
			
			static const char* img_dim_funcs[] {
				"_Z15get_image_width",
				"_Z16get_image_height",
				"_Z15get_image_depth"
			};
			
			const auto dim = coord_vec_type->getNumElements();
			SmallVector<llvm::Type*, 8> get_dim_arg_types;
			SmallVector<llvm::Value*, 8> get_dim_func_args;
			get_dim_arg_types.push_back(img_handle_arg->getType());
			get_dim_func_args.push_back(img_handle_arg);
			for(uint32_t i = 0; i < dim; ++i) {
				auto get_dim_func = get_or_create_spir_function(img_dim_funcs[i] + geom,
																builder->getInt32Ty(),
																get_dim_arg_types,
																true);
				llvm::CallInst* get_dim_call = builder->CreateCall(get_dim_func, get_dim_func_args);
				get_dim_call->setDoesNotAccessMemory();
				get_dim_call->setConvergent();
				get_dim_call->setDoesNotThrow();
				get_dim_call->setCallingConv(CallingConv::FLOOR_FUNC);
				ret.push_back(get_dim_call);
			}
			
			return ret;
		}
		
		void handle_cl_coord(Instruction& I,
							 llvm::Value* coord_arg,
							 llvm::Value* layer_arg,
							 const bool is_array,
							 const bool is_msaa,
							 const bool must_have_int_args,
							 std::string& cl_func_name,
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
			if(is_msaa && !is_int_coord) {
				ctx->emitError(&I, "coordinate type must be integer for msaa images");
				return;
			}
			
			if(must_have_int_args && !is_int_coord) {
				ctx->emitError(&I, "coordinate type must be integer");
				return;
			}
			
			// opencl only knows scalar, vector2 and vector4 coordinates -> need to create them if necessary
			auto cl_coord_dim = coord_dim + (is_array ? 1 : 0);
			if(cl_coord_dim == 3) cl_coord_dim = 4;
			const auto cl_coord_scalar_type = (is_int_coord ? llvm::Type::getInt32Ty(*ctx) : llvm::Type::getFloatTy(*ctx));
			const auto cl_coord_type = (cl_coord_dim == 1 ?
										cl_coord_scalar_type :
										llvm::FixedVectorType::get(cl_coord_scalar_type, cl_coord_dim));
			
			// start with the specified coord arg, there are some cases where we can just use it without rebuilding
			auto cl_coord_arg = coord_arg;
			if(cl_coord_type != coord_vec_type) {
				if(cl_coord_dim == 1) {
					// just a scalar
					cl_coord_arg = builder->CreateExtractElement(coord_arg, builder->getInt32(0));
				}
				else {
					// create a new tmp coord, then copy coord elements (keep unused undef)
					cl_coord_arg = UndefValue::get(cl_coord_type);
					uint32_t coord_idx = 0;
					for(; coord_idx < coord_dim; ++coord_idx) {
						cl_coord_arg = builder->CreateInsertElement(cl_coord_arg,
																	builder->CreateExtractElement(coord_arg,
																								  builder->getInt32(coord_idx)),
																	builder->getInt32(coord_idx));
					}
					
					// need to pull the layer index into the coordinate, including possible int -> float conversion
					if(is_array) {
						auto layer = layer_arg;
						if(!is_int_coord) {
							// need to convert
							layer = builder->CreateUIToFP(layer_arg, cl_coord_scalar_type);
						}
						cl_coord_arg = builder->CreateInsertElement(cl_coord_arg, layer, builder->getInt32(coord_idx++));
					}
				}
			}
			func_arg_types.push_back(cl_coord_arg->getType());
			func_args.push_back(cl_coord_arg);
			
			if(cl_coord_dim > 1) {
				cl_func_name += "Dv" + std::to_string(cl_coord_dim) + "_";
			}
			cl_func_name += (is_int_coord ? "i" : "f");
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
							   const bool is_gradient_,
							   const COMPARE_FUNCTION& compare_function,
							   llvm::Value* compare_value_arg,
							   const bool is_compare) override {
			SmallVector<llvm::Type*, 8> func_arg_types;
			SmallVector<llvm::Value*, 8> func_args;
			
			// must be constant/constexpr for now
			if(const_sampler_arg == nullptr) {
				ctx->emitError(&I, "sampler must be a constant");
				return;
			}
			auto sampler_arg = const_sampler_arg;
			
			// NOTE: opencl is rather limited when it comes to image functionality, hence not all types of image reads
			//       are supported, though some can simply be ignored or emulated in s/w
			
			// TODO: add an option to disable advanced image functions and silently fallback to simple ones (at the loss of functionality)
			
			// get geom string / mangled name + flags
			const auto geom_cstr = type_to_geom(image_type);
			if (!geom_cstr) {
				ctx->emitError(&I, "unknown/incorrect/unsupported image type");
				return;
			}
			std::string geom = geom_cstr;
			const auto is_array = has_flag<COMPUTE_IMAGE_TYPE::FLAG_ARRAY>(image_type);
			const auto is_msaa = has_flag<COMPUTE_IMAGE_TYPE::FLAG_MSAA>(image_type);
			const auto is_depth = has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(image_type);
			
			auto coord_vec_type = dyn_cast<FixedVectorType>(coord_arg->getType());
			if (!coord_vec_type) {
				ctx->emitError(&I, "coordinate must be a vector type");
				return;
			}
			
			// -> caps check
			if(is_lod_or_bias) {
				if(!has_flag<IMAGE_CAPABILITY::MIPMAP_READ>(image_capabilities)) {
					ctx->emitError(&I, "lod read not supported by device");
					return;
				}
				
				// *sigh* will be supported with opencl 2.1 though
				// -> convert int coords to float coords and swap out sampler
				if (coord_vec_type->getElementType()->isIntegerTy()) {
					const auto coord_dim = coord_vec_type->getNumElements();
					const auto fp_coord_type = llvm::FixedVectorType::get(llvm::Type::getFloatTy(*ctx), coord_dim);
					
					auto img_dims = get_image_dim(img_handle_arg, coord_vec_type, geom);
					
					llvm::Value* fp_coord = UndefValue::get(fp_coord_type);
					for(uint32_t i = 0; i < coord_dim; ++i) {
						// fp_coord_i = (float(int_coord_i) + 0.5) / float(img_dim_i)
						auto elem = builder->CreateExtractElement(coord_arg, builder->getInt32(i));
						auto fp_elem = builder->CreateSIToFP(elem, builder->getFloatTy());
						auto fp_elem_half = builder->CreateFAdd(fp_elem, ConstantFP::get(builder->getFloatTy(), 0.5));
						auto fp_dim_i = builder->CreateSIToFP(img_dims[i], builder->getFloatTy());
						auto div = builder->CreateFDiv(fp_elem_half, fp_dim_i);
						fp_coord = builder->CreateInsertElement(fp_coord, div, builder->getInt32(i));
					}
					coord_arg = fp_coord;
					
					// sampler: set NORMALIZED/CLK_NORMALIZED_COORDS_TRUE flag
					// NOTE: PIXEL/CLK_NORMALIZED_COORDS_FALSE is 0, so we don't need to clear anything
					sampler_arg = ConstantInt::get(const_sampler_arg->getType(), const_sampler_arg->getZExtValue() | 0x1);
				}
			}
			
			bool is_gradient = is_gradient_;
			if(is_gradient_) {
				if(!has_flag<IMAGE_CAPABILITY::MIPMAP_READ>(image_capabilities)) {
					ctx->emitError(&I, "gradient read not supported by device");
					return;
				}
				
				// again, not supported (also: doesn't make much sense?)
				// -> not going to s/w emulate this for now
				// -> silently ignore for now
#if 0
				if(coord_arg->getType()->getVectorElementType()->isIntegerTy()) {
					ctx->emitError(&I, "gradient read not supported with integer coordinates");
					return;
				}
#else
				if(coord_vec_type->getElementType()->isIntegerTy()) {
					is_gradient = false;
				}
#endif
			}
			
			// -> return data and cl function name
			// NOTE: we don't have a c++ mangling support in here, so do it manually
			// (this is actually easy enough, since everything is very static)
			std::string cl_func_name;
			llvm::Type* ret_type;
			if(func_name.endswith(".float")) {
				cl_func_name = "_Z11read_imagef";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getFloatTy(*ctx), 4);
			}
			else if(func_name.endswith(".int")) {
				cl_func_name = "_Z11read_imagei";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 4);
			}
			else if(func_name.endswith(".uint")) {
				cl_func_name = "_Z12read_imageui";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 4);
			}
			else if(func_name.endswith(".half")) {
				cl_func_name = "_Z11read_imageh";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getHalfTy(*ctx), 4);
			}
			// unknown -> ignore
			else return;
			
			// -> geom
			cl_func_name += geom;
			
			func_arg_types.push_back(img_handle_arg->getType());
			func_args.push_back(img_handle_arg);
			
			if(is_depth) {
				// depth return type is always a float
				ret_type = llvm::Type::getFloatTy(*ctx);
			}
			
			// except for msaa, we always have a sampler
			if(!is_msaa) {
				cl_func_name += "11ocl_sampler";
				func_arg_types.push_back(sampler_arg->getType());
				func_args.push_back(sampler_arg);
			}
			
			// -> offset
			// opencl has no offset support, so always add it
			llvm::Value* offset_coord_arg = coord_arg;
			if(is_offset) {
				if(coord_vec_type->getElementType()->isIntegerTy()) {
					offset_coord_arg = builder->CreateAdd(coord_arg, offset_arg);
				}
				else {
					// need to fallback to s/w for fp coords
					auto offset_dim = coord_vec_type->getNumElements();
					auto img_dims = get_image_dim(img_handle_arg, coord_vec_type, geom);
					
					// float_offset_i = float(offset_i) / float(dim_i)
					llvm::Value* fp_offset = UndefValue::get(coord_vec_type);
					for(uint32_t i = 0; i < offset_dim; ++i) {
						auto offset_i = builder->CreateExtractElement(offset_arg, builder->getInt32(i));
						
						// one offset elem is often 0
						// -> add some special handling since the si->fp conversion and fdiv are unnecessary here
						// (this might later also get rid of unnecessary get_image_* calls)
						if(const auto const_offset_i = dyn_cast_or_null<ConstantInt>(offset_i)) {
							if(const_offset_i->getSExtValue() == 0) {
								builder->CreateInsertElement(fp_offset, ConstantFP::get(builder->getFloatTy(), 0.0),
															 builder->getInt32(i));
								continue;
							}
						}
						
						auto offset_i_fp = builder->CreateSIToFP(offset_i, builder->getFloatTy());
						auto dim_i = builder->CreateSIToFP(img_dims[i], builder->getFloatTy());
						fp_offset = builder->CreateInsertElement(fp_offset,
																 builder->CreateFDiv(offset_i_fp, dim_i),
																 builder->getInt32(i));
					}
					
					// finally: add the compute fp offset
					offset_coord_arg = builder->CreateFAdd(coord_arg, fp_offset);
				}
			}
			
			// -> coord
			handle_cl_coord(I,
							offset_coord_arg,
							layer_arg,
							is_array,
							is_msaa,
							false, // can have either int or float coords
							cl_func_name,
							func_arg_types,
							func_args);
			
			// -> sample
			if(is_msaa) {
				if(!sample_arg->getType()->isIntegerTy()) {
					ctx->emitError(&I, "msaa sample index must be integer");
					return;
				}
				
				cl_func_name += "i";
				func_arg_types.push_back(sample_arg->getType());
				func_args.push_back(sample_arg);
			}
			
			// -> gradient
			if(is_gradient) {
				const auto coord_dim = coord_vec_type->getNumElements();
				if(coord_dim == 1) {
					// extract scalar
					cl_func_name += "ff";
					func_arg_types.push_back(builder->getFloatTy());
					func_args.push_back(builder->CreateExtractElement(dpdx_arg, builder->getInt32(0)));
					func_arg_types.push_back(builder->getFloatTy());
					func_args.push_back(builder->CreateExtractElement(dpdy_arg, builder->getInt32(0)));
				}
				else if(coord_dim == 2) {
					// just pass-through
					cl_func_name += "Dv2_fDv2_f";
					func_arg_types.push_back(dpdx_arg->getType());
					func_args.push_back(dpdx_arg);
					func_arg_types.push_back(dpdy_arg->getType());
					func_args.push_back(dpdy_arg);
				}
				else if(coord_dim == 3) {
					// need to create a vector4
					cl_func_name += "Dv4_fDv4_f";
					
					const auto grad_type = llvm::FixedVectorType::get(builder->getFloatTy(), 4);
					func_arg_types.push_back(grad_type);
					func_arg_types.push_back(grad_type);
					
					llvm::Value* dpdx4 = UndefValue::get(grad_type);
					llvm::Value* dpdy4 = UndefValue::get(grad_type);
					for(uint32_t i = 0; i < 3; ++i) {
						auto idx = builder->getInt32(i);
						dpdx4 = builder->CreateInsertElement(dpdx4, builder->CreateExtractElement(dpdx_arg, idx), idx);
						dpdy4 = builder->CreateInsertElement(dpdy4, builder->CreateExtractElement(dpdy_arg, idx), idx);
					}
					func_args.push_back(dpdx4);
					func_args.push_back(dpdy4);
				}
				else llvm_unreachable("invalid coord dim");
			}
			
			// -> lod
			// NOTE: bias is never supported
			if(is_lod_or_bias) {
				cl_func_name += "f";
				
				auto lod = lod_or_bias_arg;
				if(lod_or_bias_arg->getType()->isIntegerTy()) {
					// only float is supported, convert it
					lod = builder->CreateSIToFP(lod_or_bias_arg, builder->getFloatTy());
				}
				func_arg_types.push_back(lod->getType());
				func_args.push_back(lod);
			}
			
			// create the opencl call
			auto read_func = get_or_create_spir_function(cl_func_name, ret_type, func_arg_types, true);
			llvm::CallInst* read_call = builder->CreateCall(read_func, func_args);
			read_call->setDoesNotAccessMemory(); // all reads are readnone (can be optimized away if unused)
			read_call->setConvergent();
			read_call->setDoesNotThrow();
			read_call->setDebugLoc(I.getDebugLoc()); // keep debug loc
			read_call->setCallingConv(CallingConv::FLOOR_FUNC);
			
			// if this is a depth read/sample, the return type is a float -> create a float4
			llvm::Value* read_call_result = read_call;
			if(is_depth) {
				read_call_result = UndefValue::get(llvm::FixedVectorType::get(llvm::Type::getFloatTy(*ctx), 4));
				if(!is_compare) {
					read_call_result = builder->CreateInsertElement(read_call_result, read_call, builder->getInt32(0));
				}
				// -> compare
				else {
					emulate_depth_compare(read_call_result, read_call, compare_function, compare_value_arg);
				}
				// NOTE: rest of vector is undef/zero (and will be stripped away again anyways)
			}
			
			//
			I.replaceAllUsesWith(read_call_result);
			I.eraseFromParent();
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
			
			std::string cl_func_name, dtype;
			if(func_name.endswith(".float")) {
				cl_func_name = "_Z12write_imagef";
				dtype = "f";
			}
			else if(func_name.endswith(".int")) {
				cl_func_name = "_Z12write_imagei";
				dtype = "i";
			}
			else if(func_name.endswith(".uint")) {
				cl_func_name = "_Z13write_imageui";
				dtype = "j";
			}
			else if(func_name.endswith(".half")) {
				cl_func_name = "_Z12write_imageh";
				dtype = "h";
			}
			// unknown -> ignore
			else return;
			
			//// func replacement
			// -> geom
			const auto geom_cstr = type_to_geom(image_type);
			if (!geom_cstr) {
				ctx->emitError(&I, "unknown/incorrect/unsupported image type");
				return;
			}
			std::string geom = geom_cstr;
			const auto is_array = has_flag<COMPUTE_IMAGE_TYPE::FLAG_ARRAY>(image_type);
			const auto is_depth = has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(image_type);
			
			// filter types that are not allowed, b/c they can't be directly written to
			switch (image_type) {
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA:
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA:
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA_ARRAY:
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA_ARRAY:
					ctx->emitError(&I, "invalid image type - type is not writable");
					return;
				default:
					break;
			}
			
			cl_func_name += geom;
			
			func_arg_types.push_back(img_handle_arg->getType());
			func_args.push_back(img_handle_arg);
			
			// -> coord
			handle_cl_coord(I,
							coord_arg,
							layer_arg,
							is_array,
							false, // no msaa
							true, // must always have int coords for writes
							cl_func_name,
							func_arg_types,
							func_args);
			
			// -> lod
			if(is_lod) {
				// always int
				cl_func_name += "i";
				func_arg_types.push_back(lod_arg->getType());
				func_args.push_back(lod_arg);
			}
			
			// -> data
			// data is always a vector4, unless we're writing depth
			if(!is_depth) {
				cl_func_name += "Dv4_";
				func_arg_types.push_back(data_arg->getType());
				func_args.push_back(data_arg);
			}
			else {
				// extract and use depth elem
				auto depth_arg = builder->CreateExtractElement(data_arg, builder->getInt32(0));
				func_arg_types.push_back(depth_arg->getType());
				func_args.push_back(depth_arg);
			}
			cl_func_name += dtype;
			
			// create the opencl call
			auto write_func = get_or_create_spir_function(cl_func_name, builder->getVoidTy(), func_arg_types, false);
			llvm::CallInst* write_call = builder->CreateCall(write_func, func_args);
			write_call->setDebugLoc(I.getDebugLoc()); // keep debug loc
			write_call->setCallingConv(CallingConv::FLOOR_FUNC);
			
			//
			I.replaceAllUsesWith(write_call);
			I.eraseFromParent();
			
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
			
			// TODO/NOTE: LOD arg is not supported with OpenCL/SPIR, only with OpenCL/SPIR-V (but not properly handled yet)
			const bool with_lod = false;

			// query/get function base
			const auto query_image = [&](const std::string& query_name) {
				SmallVector<llvm::Type*, 8> func_arg_types;
				SmallVector<llvm::Value*, 8> func_args;
				func_arg_types.push_back(img_handle_arg->getType());
				func_args.push_back(img_handle_arg);
				if (with_lod) {
					func_arg_types.push_back(lod_arg->getType());
					func_args.push_back(lod_arg);
				}
				
				// -> build get func name
				const std::string get_func_name = query_name + geom + (with_lod ? "i" : "");
				
				// create the air call
				const auto ret_type = llvm::Type::getInt32Ty(*ctx);
				auto get_func = get_or_create_spir_function(get_func_name, ret_type, func_arg_types, true);
				llvm::CallInst* get_call = builder->CreateCall(get_func, func_args);
				get_call->setConvergent();
				get_call->setDoesNotThrow();
				get_call->setDoesNotAccessMemory(); // all get_* calls are readnone (can be optimized away if unused)
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

char SPIRImage::ID = 0;
INITIALIZE_PASS_BEGIN(SPIRImage, "SPIRImage", "SPIRImage Pass", false, false)
INITIALIZE_PASS_END(SPIRImage, "SPIRImage", "SPIRImage Pass", false, false)

FunctionPass *llvm::createSPIRImagePass(const uint32_t image_capabilities,
										const bool intel_workarounds) {
	return new SPIRImage(image_capabilities, intel_workarounds);
}
