//===-- FloorImageType.h - base class for image transformations--*- C++ -*-===//
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

#ifndef LLVM_TRANSFORMS_LIBFLOOR_FLOORIMAGETYPE_H
#define LLVM_TRANSFORMS_LIBFLOOR_FLOORIMAGETYPE_H

#include <cstdarg>
#include <cstdint>
#include <type_traits>

// condensed version of the COMPUTE_IMAGE_TYPE defined by floor
enum class COMPUTE_IMAGE_TYPE : uint64_t {
	//! invalid/uninitialized
	NONE					= (0ull),
	
	//////////////////////////////////////////
	// -> image flags and types
	
	//! bits 60-63: extended type flags
	__EXT_FLAG_MASK			= (0xF000'0000'0000'0000ull),
	__EXT_FLAG_SHIFT		= (60ull),
	//! extended type: in combination with FLAG_MSAA, an MSAA image can be made transient, i.e. does not need to be stored in memory
	//! NOTE: only applicable for Metal and Vulkan
	FLAG_TRANSIENT			= (1ull << (__EXT_FLAG_SHIFT + 0ull)),
	//! extended optional type: internal flag that signals that we want to sample an image as a 16-bit type (e.g. half)
	//! NOTE: only used internally, serves no purpose on the user-side
	FLAG_16_BIT_SAMPLING	= (1ull << (__EXT_FLAG_SHIFT + 1ull)),
	__UNUSED_EXT_FLAG_2		= (1ull << (__EXT_FLAG_SHIFT + 2ull)),
	__UNUSED_EXT_FLAG_3		= (1ull << (__EXT_FLAG_SHIFT + 3ull)),
	
	//! bits 38-59: unused
	
	//! bits 35-37: anisotropy (stored as power-of-two)
	__ANISOTROPY_MASK		= (0x0000'0038'0000'0000ull),
	__ANISOTROPY_SHIFT		= (35ull),
	ANISOTROPY_1			= (0ull << __ANISOTROPY_SHIFT),
	ANISOTROPY_2			= (1ull << __ANISOTROPY_SHIFT),
	ANISOTROPY_4			= (2ull << __ANISOTROPY_SHIFT),
	ANISOTROPY_8			= (3ull << __ANISOTROPY_SHIFT),
	ANISOTROPY_16			= (4ull << __ANISOTROPY_SHIFT),
	
	//! bits 32-34: multi-sampling sample count (stored as power-of-two)
	__SAMPLE_COUNT_MASK		= (0x0000'0007'0000'0000ull),
	__SAMPLE_COUNT_SHIFT	= (32ull),
	SAMPLE_COUNT_1			= (0ull << __SAMPLE_COUNT_SHIFT),
	SAMPLE_COUNT_2			= (1ull << __SAMPLE_COUNT_SHIFT),
	SAMPLE_COUNT_4			= (2ull << __SAMPLE_COUNT_SHIFT),
	SAMPLE_COUNT_8			= (3ull << __SAMPLE_COUNT_SHIFT),
	SAMPLE_COUNT_16			= (4ull << __SAMPLE_COUNT_SHIFT),
	SAMPLE_COUNT_32			= (5ull << __SAMPLE_COUNT_SHIFT),
	SAMPLE_COUNT_64			= (6ull << __SAMPLE_COUNT_SHIFT),
	
	//! bits 20-31: type flags
	__FLAG_MASK				= (0x0000'0000'FFFC'0000ull),
	__FLAG_SHIFT			= (20ull),
	//! base type: image is an array (aka has layers)
	FLAG_ARRAY				= (1ull << (__FLAG_SHIFT + 0ull)),
	//! base type: image is a buffer object
	FLAG_BUFFER				= (1ull << (__FLAG_SHIFT + 1ull)),
	//! base type: image uses mutli-sampling (consists of multiple samples)
	FLAG_MSAA				= (1ull << (__FLAG_SHIFT + 2ull)),
	//! base type: image is a cube map
	FLAG_CUBE				= (1ull << (__FLAG_SHIFT + 3ull)),
	//! base type: image is a depth image
	FLAG_DEPTH				= (1ull << (__FLAG_SHIFT + 4ull)),
	//! base type: image is a stencil image
	FLAG_STENCIL			= (1ull << (__FLAG_SHIFT + 5ull)),
	//! base type: image is a render target (Metal) / renderbuffer (OpenGL) / framebuffer attachment (Vulkan)
	//! NOTE: only applicable when using OpenGL sharing, Metal or Vulkan
	FLAG_RENDER_TARGET		= (1ull << (__FLAG_SHIFT + 6ull)),
	//! optional type: image uses mip-mapping, i.e. has multiple LODs
	FLAG_MIPMAPPED			= (1ull << (__FLAG_SHIFT + 7ull)),
	//! optional type: image uses a fixed channel count
	//! NOTE: only used internally, serves no purpose on the user-side
	FLAG_FIXED_CHANNELS		= (1ull << (__FLAG_SHIFT + 8ull)),
	//! optional type: image uses gather sampling (aka tld4/fetch4)
	FLAG_GATHER				= (1ull << (__FLAG_SHIFT + 9ull)),
	//! optional type: when using integer storage formats, the data is normalized in [0, 1]
	FLAG_NORMALIZED			= (1ull << (__FLAG_SHIFT + 10ull)),
	//! optional type: image data contains sRGB data
	FLAG_SRGB				= (1ull << (__FLAG_SHIFT + 11ull)),
	
	//! bits 18-19: channel layout
	__LAYOUT_MASK			= (0x0000'0000'000C'0000ull),
	__LAYOUT_SHIFT			= (18ull),
	LAYOUT_RGBA				= (0ull << __LAYOUT_SHIFT),
	LAYOUT_BGRA				= (1ull << __LAYOUT_SHIFT),
	LAYOUT_ABGR				= (2ull << __LAYOUT_SHIFT),
	LAYOUT_ARGB				= (3ull << __LAYOUT_SHIFT),
	//! layout convenience aliases
	LAYOUT_R				= LAYOUT_RGBA,
	LAYOUT_RG				= LAYOUT_RGBA,
	LAYOUT_RGB				= LAYOUT_RGBA,
	LAYOUT_BGR				= LAYOUT_BGRA,
	
	//! bits 16-17: dimensionality
	//! NOTE: cube maps and arrays use the dimensionality of their underlying image data
	//!       -> 2D for cube maps, 2D for 2D arrays, 1D for 1D arrays
	__DIM_MASK				= (0x0000'0000'0003'0000ull),
	__DIM_SHIFT				= (16ull),
	DIM_1D					= (1ull << __DIM_SHIFT),
	DIM_2D					= (2ull << __DIM_SHIFT),
	DIM_3D					= (3ull << __DIM_SHIFT),
	
	//! bits 14-15: channel count
	__CHANNELS_MASK			= (0x0000'0000'0000'C000ull),
	__CHANNELS_SHIFT		= (14ull),
	CHANNELS_1				= (0ull << __CHANNELS_SHIFT),
	CHANNELS_2				= (1ull << __CHANNELS_SHIFT),
	CHANNELS_3				= (2ull << __CHANNELS_SHIFT),
	CHANNELS_4				= (3ull << __CHANNELS_SHIFT),
	//! channel convenience aliases
	R 						= CHANNELS_1,
	RG 						= CHANNELS_2,
	RGB 					= CHANNELS_3,
	RGBA					= CHANNELS_4,
	
	//! bits 12-13: storage data type
	__DATA_TYPE_MASK		= (0x0000'0000'0000'3000ull),
	__DATA_TYPE_SHIFT		= (12ull),
	INT						= (1ull << __DATA_TYPE_SHIFT),
	UINT					= (2ull << __DATA_TYPE_SHIFT),
	FLOAT					= (3ull << __DATA_TYPE_SHIFT),
	
	//! bits 10-11: access qualifier
	__ACCESS_MASK			= (0x0000'0000'0000'0C00ull),
	__ACCESS_SHIFT			= (10ull),
	//! image is read-only (exluding host operations)
	READ					= (1ull << __ACCESS_SHIFT),
	//! image is write-only (exluding host operations)
	WRITE					= (2ull << __ACCESS_SHIFT),
	//! image is read-write
	//! NOTE: also applies if neither is set
	READ_WRITE				= (READ | WRITE),
	
	//! bits 6-9: compressed formats
	__COMPRESSION_MASK		= (0x0000'0000'0000'03C0),
	__COMPRESSION_SHIFT		= (6ull),
	//! image data is not compressed
	UNCOMPRESSED			= (0ull << __COMPRESSION_SHIFT),
	//! S3TC/DXTn
	BC1						= (1ull << __COMPRESSION_SHIFT),
	BC2						= (2ull << __COMPRESSION_SHIFT),
	BC3						= (3ull << __COMPRESSION_SHIFT),
	//! RGTC1/RGTC2
	RGTC					= (4ull << __COMPRESSION_SHIFT),
	BC4						= RGTC,
	BC5						= RGTC,
	//! BPTC/BPTC_FLOAT
	BPTC					= (5ull << __COMPRESSION_SHIFT),
	BC6H					= BPTC,
	BC7						= BPTC,
	//! PVRTC
	PVRTC					= (6ull << __COMPRESSION_SHIFT),
	//! PVRTC2
	PVRTC2					= (7ull << __COMPRESSION_SHIFT),
	//! EAC/ETC1
	EAC						= (8ull << __COMPRESSION_SHIFT),
	ETC1					= EAC,
	//! ETC2
	ETC2					= (9ull << __COMPRESSION_SHIFT),
	//! ASTC
	ASTC					= (10ull << __COMPRESSION_SHIFT),
	
	//! bits 0-5: formats
	//! NOTE: unless specified otherwise, a format is usable with any channel count
	//! NOTE: not all backends support all formats (for portability, stick to 8-bit/16-bit/32-bit)
	//! NOTE: channel layout / order is determined by LAYOUT_* -> bit/channel order in here can be different to the actual layout
	__FORMAT_MASK			= (0x0000'0000'0000'003Full),
	//! 1 bit per channel
	FORMAT_1				= (1ull),
	//! 2 bits per channel
	FORMAT_2				= (2ull),
	//! 3 channel format: 3-bit/3-bit/2-bit
	FORMAT_3_3_2			= (3ull),
	//! 4 bits per channel or YUV444
	FORMAT_4				= (4ull),
	//! YUV420
	FORMAT_4_2_0			= (5ull),
	//! YUV411
	FORMAT_4_1_1			= (6ull),
	//! YUV422
	FORMAT_4_2_2			= (7ull),
	//! 3 channel format: 5-bit/5-bit/5-bit
	FORMAT_5_5_5			= (8ull),
	//! 4 channel format: 5-bit/5-bit/5-bit/1-bit
	FORMAT_5_5_5_ALPHA_1	= (9ull),
	//! 3 channel format: 5-bit/6-bit/5-bit
	FORMAT_5_6_5			= (10ull),
	//! 8 bits per channel
	FORMAT_8				= (11ull),
	//! 3 channel format: 9-bit/9-bit/9-bit (5-bit exp)
	FORMAT_9_9_9_EXP_5		= (12ull),
	//! 3 or 4 channel format: 10-bit/10-bit/10-bit(/10-bit)
	FORMAT_10				= (13ull),
	//! 4 channel format: 10-bit/10-bit/10-bit/2-bit
	FORMAT_10_10_10_ALPHA_2	= (14ull),
	//! 3 channel format: 11-bit/11-bit/10-bit
	FORMAT_11_11_10			= (15ull),
	//! 3 channel format: 12-bit/12-bit/12-bit
	FORMAT_12_12_12			= (16ull),
	//! 4 channel format: 12-bit/12-bit/12-bit/12-bit
	FORMAT_12_12_12_12		= (17ull),
	//! 16 bits per channel
	FORMAT_16				= (18ull),
	//! 2 channel format: 16-bit/8-bit
	FORMAT_16_8				= (19ull),
	//! 1 channel format: 24-bit
	FORMAT_24				= (20ull),
	//! 2 channel format: 24-bit/8-bit
	FORMAT_24_8				= (21ull),
	//! 32 bits per channel
	FORMAT_32				= (22ull),
	//! 2 channel format: 32-bit/8-bit
	FORMAT_32_8				= (23ull),
	//! 64 bits per channel
	FORMAT_64				= (24ull),
	__FORMAT_MAX			= FORMAT_64,
	
	//////////////////////////////////////////
	// -> base image types
	//! 1D image
	IMAGE_1D				= DIM_1D,
	//! array of 1D images
	IMAGE_1D_ARRAY			= DIM_1D | FLAG_ARRAY,
	//! 1D image buffer (special format on some platforms)
	IMAGE_1D_BUFFER			= DIM_1D | FLAG_BUFFER,
	
	//! 2D image
	IMAGE_2D				= DIM_2D,
	//! array of 2D images
	IMAGE_2D_ARRAY			= DIM_2D | FLAG_ARRAY,
	//! multi-sampled 2D image
	IMAGE_2D_MSAA			= DIM_2D | FLAG_MSAA,
	//! array of multi-sampled 2D images
	IMAGE_2D_MSAA_ARRAY		= DIM_2D | FLAG_MSAA | FLAG_ARRAY,
	
	//! cube map image
	IMAGE_CUBE				= DIM_2D | FLAG_CUBE,
	//! array of cube map images
	IMAGE_CUBE_ARRAY		= DIM_2D | FLAG_CUBE | FLAG_ARRAY,
	
	//! 2D depth image
	IMAGE_DEPTH				= FLAG_DEPTH | CHANNELS_1 | IMAGE_2D,
	//! combined 2D depth + stencil image
	IMAGE_DEPTH_STENCIL		= FLAG_DEPTH | CHANNELS_2 | IMAGE_2D | FLAG_STENCIL,
	//! array of 2D depth images
	IMAGE_DEPTH_ARRAY		= FLAG_DEPTH | CHANNELS_1 | IMAGE_2D_ARRAY,
	//! depth cube map image
	IMAGE_DEPTH_CUBE		= FLAG_DEPTH | CHANNELS_1 | IMAGE_CUBE,
	//! array of depth cube map images
	IMAGE_DEPTH_CUBE_ARRAY	= FLAG_DEPTH | CHANNELS_1 | IMAGE_CUBE | FLAG_ARRAY,
	//! multi-sampled 2D depth image
	IMAGE_DEPTH_MSAA		= FLAG_DEPTH | CHANNELS_1 | IMAGE_2D_MSAA,
	//! array of multi-sampled 2D depth images
	IMAGE_DEPTH_MSAA_ARRAY	= FLAG_DEPTH | CHANNELS_1 | IMAGE_2D_MSAA_ARRAY,
	
	//! 3D image
	IMAGE_3D				= DIM_3D,
	
	//
	BASE_TYPE_MASK			= (__DIM_MASK |
							   FLAG_ARRAY | FLAG_BUFFER | FLAG_CUBE | FLAG_DEPTH | FLAG_MSAA | FLAG_STENCIL),
	
};
__attribute__((always_inline, used)) static constexpr COMPUTE_IMAGE_TYPE operator|(const COMPUTE_IMAGE_TYPE& e0,
																				   const COMPUTE_IMAGE_TYPE& e1) {
	return (COMPUTE_IMAGE_TYPE)((typename std::underlying_type<COMPUTE_IMAGE_TYPE>::type)e0 |
								(typename std::underlying_type<COMPUTE_IMAGE_TYPE>::type)e1);
}
__attribute__((always_inline, used)) static constexpr COMPUTE_IMAGE_TYPE operator&(const COMPUTE_IMAGE_TYPE& e0,
																			 const COMPUTE_IMAGE_TYPE& e1) {
	return (COMPUTE_IMAGE_TYPE)((typename std::underlying_type<COMPUTE_IMAGE_TYPE>::type)e0 &
								(typename std::underlying_type<COMPUTE_IMAGE_TYPE>::type)e1);
}
template <COMPUTE_IMAGE_TYPE flag, typename int_type = typename std::underlying_type<COMPUTE_IMAGE_TYPE>::type>
__attribute__((always_inline, used)) static constexpr bool has_flag(const COMPUTE_IMAGE_TYPE& enum_object) {
	return ((int_type(flag) & int_type(enum_object)) == int_type(flag));
}

//! returns the dimensionality of the specified image type
__attribute__((always_inline, used)) static constexpr uint32_t image_dim_count(const COMPUTE_IMAGE_TYPE& image_type) {
	return uint32_t(image_type & COMPUTE_IMAGE_TYPE::__DIM_MASK) >> uint32_t(COMPUTE_IMAGE_TYPE::__DIM_SHIFT);
}

// compare function used by depth compare reads
enum class COMPARE_FUNCTION : uint32_t {
	NEVER				= 0u,
	LESS				= 1u,
	EQUAL				= 2u,
	LESS_OR_EQUAL		= 3u,
	GREATER				= 4u,
	NOT_EQUAL			= 5u,
	GREATER_OR_EQUAL	= 6u,
	ALWAYS				= 7u,
	__MAX_COMPARE_FUNCTION
};

// device image capabilities
enum class IMAGE_CAPABILITY : uint32_t {
	NONE					= (0u),
	BASIC					= (1u << 0u),
	
	DEPTH_READ				= (1u << 1u),
	DEPTH_WRITE				= (1u << 2u),
	MSAA_READ				= (1u << 3u),
	MSAA_WRITE				= (1u << 4u),
	MSAA_ARRAY_READ			= (1u << 5u),
	MSAA_ARRAY_WRITE		= (1u << 6u),
	CUBE_READ				= (1u << 7u),
	CUBE_WRITE				= (1u << 8u),
	CUBE_ARRAY_READ			= (1u << 9u),
	CUBE_ARRAY_WRITE		= (1u << 10u),
	MIPMAP_READ				= (1u << 11u),
	MIPMAP_WRITE			= (1u << 12u),
	OFFSET_READ				= (1u << 13u),
	OFFSET_WRITE			= (1u << 14u),
	
	DEPTH_COMPARE			= (1u << 16u),
	GATHER					= (1u << 17u),
	READ_WRITE				= (1u << 18u),
};
template <IMAGE_CAPABILITY flag, typename int_type = typename std::underlying_type<IMAGE_CAPABILITY>::type>
__attribute__((always_inline, used)) static constexpr bool has_flag(const IMAGE_CAPABILITY& enum_object) {
	return ((int_type(flag) & int_type(enum_object)) == int_type(flag));
}

#endif
