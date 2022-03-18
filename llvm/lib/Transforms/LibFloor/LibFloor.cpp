//===-- LibFloor.cpp ------------------------------------------------------===//
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
// This file implements common infrastructure for libLLVMLibFloor.a, which
// implements various Compute/Graphics backend transformations.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/LibFloor.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/InitializePasses.h"

using namespace llvm;

/// initializeLibFloor - Initialize all passes linked into the
/// LibFloor library.
void llvm::initializeLibFloor(PassRegistry &Registry) {
  initializeAddressSpaceFixPass(Registry);
  initializeEverythingInlinerPass(Registry);
  initializeCUDAImagePass(Registry);
  initializeCUDAFinalPass(Registry);
  initializeMetalFirstPass(Registry);
  initializeMetalFinalPass(Registry);
  initializeMetalFinalModuleCleanupPass(Registry);
  initializeMetalImagePass(Registry);
  initializeSPIRFinalPass(Registry);
  initializeSPIRImagePass(Registry);
  initializeCFGStructurizationPass(Registry);
  initializeVulkanImagePass(Registry);
  initializeVulkanFinalPass(Registry);
  initializeVulkanBuiltinParamHandlingPass(Registry);
  initializeVulkanPreFinalPass(Registry);
  initializeVulkanFinalModuleCleanupPass(Registry);
  initializePropagateRangeInfoPass(Registry);
  initializeFMACombinerPass(Registry);
}

void LLVMAddAddressSpaceFixPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createAddressSpaceFixPass());
}

void LLVMAddCUDAImagePass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createCUDAImagePass());
}

void LLVMAddCUDAFinalPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createCUDAFinalPass());
}

void LLVMAddMetalFirstPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createMetalFirstPass());
}

void LLVMAddMetalFinalPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createMetalFinalPass());
}

void LLVMAddMetalFinalModuleCleanupPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createMetalFinalModuleCleanupPass());
}

void LLVMAddMetalImagePass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createMetalImagePass());
}

void LLVMAddSPIRFinalPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createSPIRFinalPass());
}

void LLVMAddSPIRImagePass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createSPIRImagePass());
}

void LLVMAddCFGStructurizationPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createCFGStructurizationPass());
}

void LLVMAddVulkanImagePass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createVulkanImagePass());
}

void LLVMAddVulkanFinalPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createVulkanFinalPass());
}

void LLVMAddVulkanBuiltinParamHandlingPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createVulkanBuiltinParamHandlingPass());
}

void LLVMAddVulkanPreFinalPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createVulkanPreFinalPass());
}

void LLVMAddVulkanFinalModuleCleanupPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createVulkanFinalModuleCleanupPass());
}

void LLVMAddPropagateRangeInfoPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createPropagateRangeInfoPass());
}

void LLVMAddFMACombinerPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createFMACombinerPass());
}
