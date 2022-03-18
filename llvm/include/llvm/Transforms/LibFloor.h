//===-- LibFloor.h - LibFloor Transformations -------------------*- C++ -*-===//
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
// This header file defines prototypes for accessor functions that expose passes
// in the LibFloor transformations library.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LIBFLOOR_H
#define LLVM_TRANSFORMS_LIBFLOOR_H

#include "llvm/Transforms/Utils/SimplifyCFGOptions.h"
#include <functional>
#include <cstdint>

namespace llvm {

class Function;
class FunctionPass;
class ModulePass;
class Pass;

//===----------------------------------------------------------------------===//
//
// AddressSpaceFix - This pass fixes (intentionally) broken uses of addrspace
// pointers that should be non-addrspace pointers.
//
ModulePass *createAddressSpaceFixPass();

//===----------------------------------------------------------------------===//
//
// CUDAImage - This pass applies CUDA-specific floor image transformations.
//
FunctionPass *createCUDAImagePass(const uint32_t image_capabilities = 0);

//===----------------------------------------------------------------------===//
//
// CUDAFinal - final pass, making CUDA related IR changes.
//
FunctionPass *createCUDAFinalPass();

//===----------------------------------------------------------------------===//
//
// MetalFirst - This pass fixes Metal/AIR issues.
//
FunctionPass *createMetalFirstPass(const bool enable_intel_workarounds = false,
                                   const bool enable_nvidia_workarounds = false);

//===----------------------------------------------------------------------===//
//
// MetalFinal - This pass fixes Metal/AIR issues.
//
FunctionPass *createMetalFinalPass(const bool enable_intel_workarounds = false,
                                   const bool enable_nvidia_workarounds = false);

//===----------------------------------------------------------------------===//
//
// MetalFinalModuleCleanup - This pass removes any calling convention attributes
// and removes unused functions/prototypes/externs.
//
ModulePass *createMetalFinalModuleCleanupPass();

//===----------------------------------------------------------------------===//
//
// MetalImage - This pass applies Metal-specific floor image transformations.
//
FunctionPass *createMetalImagePass(const uint32_t image_capabilities = 0);

//===----------------------------------------------------------------------===//
//
// SPIRFinal - This pass fixes LLVM IR to be SPIR-compliant.
//
FunctionPass *createSPIRFinalPass();

//===----------------------------------------------------------------------===//
//
// SPIRImage - This pass applies SPIR-specific floor image transformations.
//
FunctionPass *createSPIRImagePass(const uint32_t image_capabilities = 0,
                                  const bool enable_intel_workarounds = false);

//===----------------------------------------------------------------------===//
//
// CFGStructurization - This pass transforms the CFG into a structurized CFG.
//
FunctionPass *createCFGStructurizationPass();

//===----------------------------------------------------------------------===//
//
// VulkanImage - This pass applies SPIR-V-specific floor image transformations.
//
FunctionPass *createVulkanImagePass(const uint32_t image_capabilities = 0);

//===----------------------------------------------------------------------===//
//
// VulkanFinal - This pass fixes Vulkan/SPIR-V issues.
//
FunctionPass *createVulkanFinalPass();

//===----------------------------------------------------------------------===//
//
// VulkanBuiltinParamHandling - This pass handles builtin -> parameter
// replacement for Vulkan.
//
FunctionPass *createVulkanBuiltinParamHandlingPass();

//===----------------------------------------------------------------------===//
//
// VulkanPreFinal - This pass fixes Vulkan/SPIR-V issues, prior to CFG
// structurization and VulkanFinal.
//
FunctionPass *createVulkanPreFinalPass();

//===----------------------------------------------------------------------===//
//
// VulkanFinalModuleCleanup - This pass removes unused functions/etc.
//
ModulePass *createVulkanFinalModuleCleanupPass();

//===----------------------------------------------------------------------===//
//
// PropagateRangeInfo - This pass propagates range metadata info.
//
FunctionPass *createPropagateRangeInfoPass();

//===----------------------------------------------------------------------===//
//
// FMACombiner - This pass combines and recombines fmul/fadd/fsub/fneg/fma
// instructions to fma instructions.
//
FunctionPass *createFMACombinerPass();

} // End llvm namespace

#endif
