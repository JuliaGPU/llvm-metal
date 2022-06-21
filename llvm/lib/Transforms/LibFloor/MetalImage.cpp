//===- MetalImage.cpp - Metal-specific floor image transformations --------===//
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
// This pass implements the Metal-specific floor image transformations, i.e.
// floor.opaque.<read/write function>.* -> air.<read/write function>.*
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

#define DEBUG_TYPE "MetalImage"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

namespace {
	struct MetalImage : public FloorImageBasePass {
		static char ID; // Pass identification, replacement for typeid
		
		static constexpr const uint32_t Metal_ConstantAS = 2;
		
		MetalImage(const uint32_t image_capabilities_ = 0) :
		FloorImageBasePass(ID, IMAGE_TYPE_ID::OPAQUE, image_capabilities_) {
			initializeMetalImagePass(*PassRegistry::getPassRegistry());
		}
		
		static const char* type_to_geom(const COMPUTE_IMAGE_TYPE& image_type) {
			switch(image_type) {
				case COMPUTE_IMAGE_TYPE::IMAGE_1D:
					return "texture_1d";
				case COMPUTE_IMAGE_TYPE::IMAGE_1D_ARRAY:
					return "texture_1d_array";
				case COMPUTE_IMAGE_TYPE::IMAGE_1D_BUFFER:
					return "texture_1d_buffer";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH:
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_STENCIL:
					return "depth_2d";
				case COMPUTE_IMAGE_TYPE::IMAGE_2D:
					return "texture_2d";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_ARRAY:
					return "depth_2d_array";
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_ARRAY:
					return "texture_2d_array";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA:
					return "depth_2d_ms";
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA:
					return "texture_2d_ms";
				case COMPUTE_IMAGE_TYPE::IMAGE_3D:
					return "texture_3d";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE:
					return "depth_cube";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE_ARRAY:
					return "depth_cube_array";
				case COMPUTE_IMAGE_TYPE::IMAGE_CUBE:
					return "texture_cube";
				case COMPUTE_IMAGE_TYPE::IMAGE_CUBE_ARRAY:
					return "texture_cube_array";
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA_ARRAY: // since Metal 2.1
					return "depth2d_ms_array";
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA_ARRAY: // since Metal 2.1
					return "texture2d_ms_array";
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
			SmallVector<llvm::Type*, 16> func_arg_types;
			SmallVector<llvm::Value*, 16> func_args;
			
			// -> return data
			std::string dtype;
			llvm::Type* ret_type;
			if(func_name.endswith(".float")) {
				dtype = "v4f32";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getFloatTy(*ctx), 4);
			}
			else if(func_name.endswith(".int")) {
				dtype = "s.v4i32";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 4);
			}
			else if(func_name.endswith(".uint")) {
				dtype = "u.v4i32";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 4);
			}
			else if(func_name.endswith(".half")) {
				dtype = "v4f16";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getHalfTy(*ctx), 4);
			}
			else if(func_name.endswith(".short")) {
				dtype = "s.v4i16";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*ctx), 4);
			}
			else if(func_name.endswith(".ushort")) {
				dtype = "u.v4i16";
				ret_type = llvm::FixedVectorType::get(llvm::Type::getInt16Ty(*ctx), 4);
			}
			// unknown -> ignore
			else return;
			
			// -> geom
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
			
			// -> coord type
			auto coord_vec_type = dyn_cast_or_null<FixedVectorType>(coord_arg->getType());
			if(!coord_vec_type) {
				ctx->emitError(&I, "invalid image coordinate argument (cast to vector failed)");
				return;
			}
			
			const auto coord_type = coord_vec_type->getElementType();
			if(is_msaa && !coord_type->isIntegerTy()) {
				ctx->emitError(&I, "coordinate type must be integer for msaa images");
				return;
			}
			if(is_cube && !coord_type->isFloatTy()) {
				ctx->emitError(&I, "coordinate type must be float for cube images");
				return;
			}
			
			// air.read_* or air.sample_*?
			// for msaa: always read
			// for all else: read if int coords, sample if float coords
			const bool is_sample_call = (!is_msaa && !coord_type->isIntegerTy());
			
			// img handle and sampler
			func_arg_types.push_back(img_handle_arg->getType());
			func_args.push_back(img_handle_arg);
			
			// only add the sampler arg if this is a sample call
			bool set_sampler_attrs = false;
			uint32_t sampler_param_idx = 0;
			if(is_sample_call) {
				// sampler handling since Metal 2.0+ is more complex
				// -> need to transform sampler into a global constant
				// -> need to add this constant to !air.sampler_states metadata
				if (dyn_sampler_arg == nullptr) {
					ctx->emitError(&I, "sampler arg should be dynamic");
					return;
				}
				
				auto ce_sampler = dyn_cast<ConstantExpr>(dyn_sampler_arg);
				if (ce_sampler == nullptr) {
					ctx->emitError(&I, "sampler arg must be a constant expression");
					return;
				}
				
				if (!ce_sampler->isCast() || ce_sampler->getOpcode() != llvm::Instruction::IntToPtr) {
					ctx->emitError(&I, "sampler arg must be a constant inttoptr expression");
					return;
				}
				
				auto sampler_constant_value = dyn_cast<ConstantInt>(ce_sampler->getOperand(0));
				if (sampler_constant_value == nullptr) {
					ctx->emitError(&I, "sampler arg must contain a constant value");
					return;
				}
				
				// create global sampler state
				// NOTE: since we're often using the same sampler -> cache them
				static std::unordered_map<uint64_t, GlobalVariable*> sample_state_cache;
				auto sampler_constant_value_u64 = sampler_constant_value->getZExtValue();
				auto cache_iter = sample_state_cache.find(sampler_constant_value_u64);
				GlobalVariable* sampler_state = nullptr;
				if (cache_iter != sample_state_cache.end()) {
					sampler_state = cache_iter->second;
				} else {
					sampler_state = new GlobalVariable(*M,
													   sampler_constant_value->getType(),
													   true,
													   GlobalVariable::InternalLinkage,
													   sampler_constant_value,
													   "__air_sampler_state",
													   nullptr,
													   GlobalValue::NotThreadLocal,
													   Metal_ConstantAS);
					sampler_state->setAlignment(MaybeAlign { 8u }); // always 8-byte aligned
					sample_state_cache.emplace(sampler_constant_value_u64, sampler_state);
					
					// insert metadata
					auto ctx = &M->getContext();
					SmallVector<llvm::Metadata*, 2> sampler_md_info;
					sampler_md_info.push_back(llvm::MDString::get(*ctx, "air.sampler_state"));
					sampler_md_info.push_back(llvm::ConstantAsMetadata::get(sampler_state));
					
					auto samplers_md = M->getOrInsertNamedMetadata("air.sampler_states");
					samplers_md->addOperand(llvm::MDNode::get(*ctx, sampler_md_info));
				}
				
				// still need to bitcast to %struct._sampler_t*
				auto sampler_type = dyn_cast<PointerType>(dyn_sampler_arg->getType());
				auto cast_sampler_state = ConstantExpr::getBitCast(sampler_state, sampler_type);
				
				func_arg_types.push_back(cast_sampler_state->getType());
				func_args.push_back(cast_sampler_state);
				
				set_sampler_attrs = true;
				sampler_param_idx = func_args.size() - 1;
			}
			
			if(is_depth) {
				// must always add the depth type 1 (== float)
				func_arg_types.push_back(llvm::Type::getInt32Ty(*ctx));
				func_args.push_back(builder->getInt32(1));
				
				// depth return type is always a float
				ret_type = llvm::Type::getFloatTy(*ctx);
				dtype = "f32";
				
				if(is_compare) {
					if(!is_sample_call) {
						ctx->emitError(&I, "compare must be a sample call");
						return;
					}
				}
			}
			else {
				if(is_compare) {
					ctx->emitError(&I, "compare is only allowed with depth types");
					return;
				}
			}
			
			// handle offset
			llvm::Value* offset_coord_arg = coord_arg;
			if(!is_sample_call && is_offset) {
				offset_coord_arg = builder->CreateAdd(coord_arg, offset_arg);
			}
			
			// -> coords: coords, sample, face, layer
			if(coord_vec_type->getNumElements() == 1) {
				// 1D coord, make it scalar
				func_arg_types.push_back(coord_type);
				func_args.push_back(builder->CreateExtractElement(offset_coord_arg, builder->getInt32(0)));
			}
			else {
				// normal coord arg
				if(!is_cube || coord_type->isFloatTy()) {
					func_arg_types.push_back(offset_coord_arg->getType());
					func_args.push_back(offset_coord_arg);
				}
				// cube with int coords
				else {
					// extract the face integer from the coords
					const auto face_arg = builder->CreateExtractElement(coord_arg, builder->getInt32(2));
					
					// create new int2 arg
					llvm::Value* coord_i2_arg = UndefValue::get(llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 2));
					coord_i2_arg = builder->CreateInsertElement(coord_i2_arg, builder->CreateExtractElement(offset_coord_arg, builder->getInt32(0)), builder->getInt32(0));
					coord_i2_arg = builder->CreateInsertElement(coord_i2_arg, builder->CreateExtractElement(offset_coord_arg, builder->getInt32(1)), builder->getInt32(1));
					
					func_arg_types.push_back(coord_i2_arg->getType());
					func_args.push_back(coord_i2_arg);
					
					func_arg_types.push_back(face_arg->getType());
					func_args.push_back(face_arg);
				}
			}
			
			if(is_msaa) {
				func_arg_types.push_back(sample_arg->getType());
				func_args.push_back(sample_arg);
			}
			
			if(is_array) {
				func_arg_types.push_back(layer_arg->getType());
				func_args.push_back(layer_arg);
			}
			
			if(is_compare) {
				func_arg_types.push_back(compare_value_arg->getType());
				func_args.push_back(compare_value_arg);
			}
			
			// -> additional args: lod, bias, gradient, offset
			if(!is_sample_call) {
				// -> read
				if(!is_msaa) { // msaa is always lod 0, hence needs no arg
					// -> lod
					if(is_lod_or_bias) {
						// for read, only int lods are allowed
						if(!lod_or_bias_arg->getType()->isIntegerTy()) {
							// convert to int
							const auto int_lod = builder->CreateFPToSI(lod_or_bias_arg, llvm::Type::getInt32Ty(*ctx));
							func_arg_types.push_back(int_lod->getType());
							func_args.push_back(int_lod);
						}
						else {
							func_arg_types.push_back(lod_or_bias_arg->getType());
							func_args.push_back(lod_or_bias_arg);
						}
					}
					else {
						// if no lod is specified (bias is not allowed here), add a 0 lod
						func_arg_types.push_back(llvm::Type::getInt32Ty(*ctx));
						func_args.push_back(builder->getInt32(0));
					}
				}
				
				if (is_metal_2_3) {
					// Metal 2.3+ has an additional (unknown) i32 0 argument
					func_arg_types.push_back(llvm::Type::getInt32Ty(*ctx));
					func_args.push_back(builder->getInt32(0));
				}
			}
			else {
				// -> sample
				
				// -> gradient
				if(is_gradient) {
					func_arg_types.push_back(dpdx_arg->getType());
					func_args.push_back(dpdx_arg);
					func_arg_types.push_back(dpdy_arg->getType());
					func_args.push_back(dpdy_arg);
					
					if (is_metal_2_4) {
						// Metal 2.4 has an additional "min LOD clamp" f32 argument -> set to 0.0 for now
						func_arg_types.push_back(llvm::Type::getFloatTy(*ctx));
						func_args.push_back(ConstantFP::get(llvm::Type::getFloatTy(*ctx), 0.0f));
					}
				}
				
				// -> offset
				if(!is_cube) { // cube allows no offset
					func_arg_types.push_back(llvm::Type::getInt1Ty(*ctx));
					func_args.push_back(builder->getInt1(is_offset));
					
					func_arg_types.push_back(offset_arg->getType());
					func_args.push_back(offset_arg);
				}
				
				// -> lod / bias
				if(!is_gradient) {
					// lod or bias?
					func_arg_types.push_back(llvm::Type::getInt1Ty(*ctx));
					func_args.push_back(builder->getInt1(is_lod_or_bias));
					
					// for sample, only float lods are allowed
					if(!lod_or_bias_arg->getType()->isFloatTy()) {
						// convert to float
						const auto float_lod = builder->CreateSIToFP(lod_or_bias_arg, llvm::Type::getFloatTy(*ctx));
						func_arg_types.push_back(float_lod->getType());
						func_args.push_back(float_lod);
					}
					else {
						func_arg_types.push_back(lod_or_bias_arg->getType());
						func_args.push_back(lod_or_bias_arg);
					}
				}
				
				if (is_metal_2_3) {
					// Metal 2.3 has an additional (unknown) f32 0.0 argument
					// NOTE: do not emit this for Metal 2.4+ when a gradient is used
					if (!is_metal_2_4 || (is_metal_2_4 && !is_gradient)) {
						func_arg_types.push_back(llvm::Type::getFloatTy(*ctx));
						func_args.push_back(ConstantFP::get(llvm::Type::getFloatTy(*ctx), 0.0f));
					}
				}
				
				// Metal 2.0+ has an additional (unknown) i32 0 argument
				func_arg_types.push_back(llvm::Type::getInt32Ty(*ctx));
				func_args.push_back(builder->getInt32(0));
			}
			
			// Metal 2.3+: return type is now always { <sample/read-type>, i8 }
			if (is_metal_2_3) {
				ret_type = llvm::StructType::get(*ctx, { ret_type, llvm::Type::getInt8Ty(*ctx) }, false /* !packed */);
				assert(((llvm::StructType*)ret_type)->isLiteral() && "must be literal");
			}
			
			// -> build read func name
			std::string read_func_name = "air.";
			
			read_func_name += (is_sample_call ? "sample_" : "read_");
			if(is_compare) read_func_name += "compare_";
			read_func_name += geom;
			if(is_gradient) read_func_name += "_grad";
			
			read_func_name += '.' + dtype;
			
			AttrBuilder attr_builder;
			attr_builder.addAttribute(llvm::Attribute::Convergent);
			attr_builder.addAttribute(llvm::Attribute::ArgMemOnly);
			attr_builder.addAttribute(llvm::Attribute::NoUnwind);
			attr_builder.addAttribute(llvm::Attribute::ReadOnly);
			auto func_attrs = AttributeList::get(*ctx, ~0, attr_builder);
			
			// create the air call
			const auto func_type = llvm::FunctionType::get(ret_type, func_arg_types, false);
			llvm::CallInst* read_call = builder->CreateCall(M->getOrInsertFunction(read_func_name, func_type, func_attrs), func_args);
			read_call->setConvergent();
			read_call->setOnlyAccessesArgMemory();
			read_call->setDoesNotThrow();
			read_call->setOnlyReadsMemory(); // all reads are readonly (can be optimized away if unused)
			read_call->setDebugLoc(I.getDebugLoc()); // keep debug loc
			
			// set sampler* cast attributes if necessary
			if (set_sampler_attrs) {
				read_call->addParamAttr(sampler_param_idx, Attribute::NoCapture);
				read_call->addParamAttr(sampler_param_idx, Attribute::ReadOnly);
			}
			
			// if this is a depth read/sample, the return type is a float -> create a float4
			llvm::Value* read_call_result = read_call;
			if (is_metal_2_3) {
				// we always ignore the i8 return value -> extract are wanted result
				read_call_result = builder->CreateExtractValue(read_call_result, { 0 });
			}
			if(is_depth) {
				auto undef_vec = UndefValue::get(llvm::FixedVectorType::get(llvm::Type::getFloatTy(*ctx), 4));
				read_call_result = builder->CreateInsertElement(undef_vec, read_call_result, builder->getInt32(0));
				// rest is undef/zero (and will be stripped away again anyways)
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
			SmallVector<llvm::Type*, 16> func_arg_types;
			SmallVector<llvm::Value*, 16> func_args;
			
			//// more arg checking
			auto coord_vec_type = dyn_cast_or_null<FixedVectorType>(coord_arg->getType());
			if(!coord_vec_type) {
				ctx->emitError(&I, "invalid image coordinate argument (cast to vector failed)");
				return;
			}
			
			const auto coord_type = coord_vec_type->getElementType();
			if(!coord_type->isIntegerTy()) {
				ctx->emitError(&I, "coordinate type must be integer");
				return;
			}
			
			std::string dtype;
			if (func_name.endswith(".float.depth")) {
				dtype = "f32";
			} else if (func_name.endswith(".float")) {
				dtype = "v4f32";
			} else if (func_name.endswith(".int")) {
				dtype = "s.v4i32";
			} else if (func_name.endswith(".uint")) {
				dtype = "u.v4i32";
			} else if (func_name.endswith(".half")) {
				dtype = "v4f16";
			} else if (func_name.endswith(".short")) {
				dtype = "s.v4i16";
			} else if (func_name.endswith(".ushort")) {
				dtype = "u.v4i16";
			}
			// unknown -> ignore
			else return;
			
			//// func replacement
			func_arg_types.push_back(img_handle_arg->getType());
			func_args.push_back(img_handle_arg);
			
			// -> geom
			const auto geom_cstr = type_to_geom(image_type);
			if (!geom_cstr) {
				ctx->emitError(&I, "unknown or incorrect image type");
				return;
			}
			std::string geom = geom_cstr;
			const auto is_array = has_flag<COMPUTE_IMAGE_TYPE::FLAG_ARRAY>(image_type);
			const auto is_cube = has_flag<COMPUTE_IMAGE_TYPE::FLAG_CUBE>(image_type);
			const auto is_depth = has_flag<COMPUTE_IMAGE_TYPE::FLAG_DEPTH>(image_type);
			
			// filter types that are not allowed, b/c they can't be directly written to
			switch (image_type) {
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA:
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA:
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE:
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_CUBE_ARRAY:
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_MSAA_ARRAY:
				case COMPUTE_IMAGE_TYPE::IMAGE_2D_MSAA_ARRAY:
					ctx->emitError(&I, "invalid image type - type is not writable");
					return;
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH:
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_STENCIL:
				case COMPUTE_IMAGE_TYPE::IMAGE_DEPTH_ARRAY:
					// TODO/NOTE: not officially exposed by Metal headers, but exists in metalfe -> figure out what the min version is
					break;
				default:
					break;
			}
			
			// -> coords: coords, face, layer
			if(coord_vec_type->getNumElements() == 1) {
				// 1D coord, make it scalar
				func_arg_types.push_back(coord_type);
				func_args.push_back(builder->CreateExtractElement(coord_arg, builder->getInt32(0)));
			}
			else {
				// normal coord arg
				if(!is_cube) {
					func_arg_types.push_back(coord_vec_type);
					func_args.push_back(coord_arg);
				}
				// cube with int coords
				else {
					// extract the face integer from the coords
					const auto face_arg = builder->CreateExtractElement(coord_arg, builder->getInt32(2));
					
					// create new int2 arg
					llvm::Value* coord_i2_arg = UndefValue::get(llvm::FixedVectorType::get(llvm::Type::getInt32Ty(*ctx), 2));
					coord_i2_arg = builder->CreateInsertElement(coord_i2_arg, builder->CreateExtractElement(coord_arg, builder->getInt32(0)), builder->getInt32(0));
					coord_i2_arg = builder->CreateInsertElement(coord_i2_arg, builder->CreateExtractElement(coord_arg, builder->getInt32(1)), builder->getInt32(1));
					
					func_arg_types.push_back(coord_i2_arg->getType());
					func_args.push_back(coord_i2_arg);
					
					func_arg_types.push_back(face_arg->getType());
					func_args.push_back(face_arg);
				}
			}
			
			if(is_array) {
				func_arg_types.push_back(layer_arg->getType());
				func_args.push_back(layer_arg);
			}
			
			// -> data (a 4-component vector or a single float if depth)
			func_arg_types.push_back(data_arg->getType());
			func_args.push_back(data_arg);
			
			// -> lod
			func_arg_types.push_back(llvm::Type::getInt32Ty(*ctx));
			if (!is_lod) {
				func_args.push_back(builder->getInt32(0));
			} else {
				func_args.push_back(lod_arg);
			}
			
			if (is_metal_2_3) {
				// Metal 2.3+ has an additional rounding mode argument (always set to 0 for now, or 2 if depth)
				func_arg_types.push_back(llvm::Type::getInt32Ty(*ctx));
				func_args.push_back(builder->getInt32(!is_depth ? 0 : 2));
			}
			
			// -> build write func name
			const std::string write_func_name = "air.write_" + geom + '.' + dtype;
			
			// create the air call
			const auto func_type = llvm::FunctionType::get(builder->getVoidTy(), func_arg_types, false);
			llvm::CallInst* write_call = builder->CreateCall(M->getOrInsertFunction(write_func_name, func_type, nounwind_attr), func_args);
			write_call->setDebugLoc(I.getDebugLoc()); // keep debug loc
			
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

			// query/get function base
			const auto query_image = [&](const std::string& query_name) {
				SmallVector<llvm::Type*, 2> func_arg_types;
				SmallVector<llvm::Value*, 2> func_args;
				func_arg_types.push_back(img_handle_arg->getType());
				func_args.push_back(img_handle_arg);
				func_arg_types.push_back(lod_arg->getType());
				func_args.push_back(lod_arg);
				
				// -> build get func name
				const std::string get_func_name = "air.get_" + query_name + '_' + geom;
				
				AttrBuilder attr_builder;
				attr_builder.addAttribute(llvm::Attribute::Convergent);
				attr_builder.addAttribute(llvm::Attribute::ArgMemOnly);
				attr_builder.addAttribute(llvm::Attribute::NoUnwind);
				attr_builder.addAttribute(llvm::Attribute::ReadOnly);
				auto func_attrs = AttributeList::get(*ctx, ~0, attr_builder);
				
				// create the air call
				const auto ret_type = llvm::Type::getInt32Ty(*ctx);
				const auto func_type = llvm::FunctionType::get(ret_type, func_arg_types, false);
				llvm::CallInst* get_call = builder->CreateCall(M->getOrInsertFunction(get_func_name, func_type, func_attrs), func_args);
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

char MetalImage::ID = 0;
INITIALIZE_PASS_BEGIN(MetalImage, "MetalImage", "MetalImage Pass", false, false)
INITIALIZE_PASS_END(MetalImage, "MetalImage", "MetalImage Pass", false, false)

FunctionPass *llvm::createMetalImagePass(const uint32_t image_capabilities) {
	return new MetalImage(image_capabilities);
}
