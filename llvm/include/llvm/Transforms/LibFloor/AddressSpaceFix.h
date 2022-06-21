//==- AddressSpaceFix.cpp - OpenCL/Metal/Vulkan and related addrspace fixes -=//
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
// This file implements an address space fixer for OpenCL/Metal/Vulkan.
//
// This is implemented as a module pass that iterates over all functions, then
// over all call instructions in there, fixing all calls that require a
// different address space then what is provided by the called function.
// Since this requires the address space information "from the top",
// this can't be implemented as a bottom-up SCC pass.
// Note that this will duplicate any functions that don't have matching address
// space parameters and thus heavily depends on proper inlining later on.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LIBFLOOR_ADDRESSSPACEFIX_H
#define LLVM_TRANSFORMS_LIBFLOOR_ADDRESSSPACEFIX_H

namespace llvm {
	//! fixes the address space of all users of the specified instruction
	//! NOTE: call instructions will *not* be fixed by this (there should be no functions/calls when calling this)
	void fix_instruction_users(LLVMContext &ctx,
	                           Instruction &instr,
	                           Value &parent,
	                           const uint32_t address_space,
	                           std::vector<ReturnInst *> &returns);
} // namespace llvm

#endif //LLVM_TRANSFORMS_ADDRESSSPACEFIX_HPP
