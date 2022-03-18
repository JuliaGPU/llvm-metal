//===- VulkanSampling.h - Vulkan-specific sampler/image info --------------===//
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
// TODO
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LIBFLOOR_VULKANSAMPLING_H
#define LLVM_TRANSFORMS_LIBFLOOR_VULKANSAMPLING_H

namespace vulkan_sampling {
	// vulkan immutable/fixed sampler type
	// -> libfloor: device/vulkan_image.hpp
	struct sampler {
		enum FILTER_MODE : uint32_t {
			__FILTER_MODE_MASK			= (0x00000001u),
			__FILTER_MODE_SHIFT			= (0u),
			NEAREST						= (0u << __FILTER_MODE_SHIFT),
			LINEAR						= (1u << __FILTER_MODE_SHIFT),
		};
		enum ADDRESS_MODE : uint32_t {
			__ADDRESS_MODE_MASK			= (0x00000002u),
			__ADDRESS_MODE_SHIFT		= (1u),
			CLAMP_TO_EDGE				= (0u << __ADDRESS_MODE_SHIFT),
			REPEAT						= (1u << __ADDRESS_MODE_SHIFT),
		};
		enum COMPARE_FUNCTION : uint32_t {
			__COMPARE_FUNCTION_MASK		= (0x0000001Cu),
			__COMPARE_FUNCTION_SHIFT	= (2u),
			NEVER						= (0u << __COMPARE_FUNCTION_SHIFT),
			LESS						= (1u << __COMPARE_FUNCTION_SHIFT),
			EQUAL						= (2u << __COMPARE_FUNCTION_SHIFT),
			LESS_OR_EQUAL				= (3u << __COMPARE_FUNCTION_SHIFT),
			GREATER						= (4u << __COMPARE_FUNCTION_SHIFT),
			NOT_EQUAL					= (5u << __COMPARE_FUNCTION_SHIFT),
			GREATER_OR_EQUAL			= (6u << __COMPARE_FUNCTION_SHIFT),
			ALWAYS						= (7u << __COMPARE_FUNCTION_SHIFT),
		};
		// NOTE: this should be the MSB, because we won't actually be creating samplers for pixel addressing
		enum COORD_MODE : uint32_t {
			__COORD_MODE_MASK			= (0x00000020u),
			__COORD_MODE_SHIFT			= (5u),
			NORMALIZED					= (0u << __COORD_MODE_SHIFT),
			PIXEL						= (1u << __COORD_MODE_SHIFT),
		};
		
		uint32_t value;
	};
	
	enum class LOD_TYPE : uint32_t {
		NO_LOD = 0,
		IMPLICIT_LOD = 1, // fragment shader only
		IMPLICIT_LOD_WITH_BIAS = 2, // fragment shader only
		EXPLICIT_LOD = 3,
		GRADIENT = 4,
		__MAX_LOD_TYPE = GRADIENT,
		INVALID = ~0u,
	};
	
}

#endif
