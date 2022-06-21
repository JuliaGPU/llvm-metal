//===-- MetalTypes.h - base class for image transformations------*- C++ -*-===//
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
// This header file contains all globally used Metal types.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LIBFLOOR_METALTYPES_H
#define LLVM_TRANSFORMS_LIBFLOOR_METALTYPES_H

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <string>
#include <array>
#include <unordered_map>

namespace metal {

#define make_tag_type(a, b, c, d) ((uint32_t(d) << 24u) | (uint32_t(c) << 16u) | (uint32_t(b) << 8u) | uint32_t(a))
//! NOTE: tag types are always 32-bit
//! NOTE: tag types are always followed by a uint16_t that specifies the length of the tag data
enum TAG_TYPE : uint32_t {
	// used in initial header section
	NAME		= make_tag_type('N', 'A', 'M', 'E'),
	TYPE		= make_tag_type('T', 'Y', 'P', 'E'),
	HASH		= make_tag_type('H', 'A', 'S', 'H'),
	MD_SIZE		= make_tag_type('M', 'D', 'S', 'Z'),
	OFFSET		= make_tag_type('O', 'F', 'F', 'T'),
	VERSION		= make_tag_type('V', 'E', 'R', 'S'),
	SOFF        = make_tag_type('S', 'O', 'F', 'F'),
	// used in reflection section
	CNST        = make_tag_type('C', 'N', 'S', 'T'),
	VATT        = make_tag_type('V', 'A', 'T', 'T'),
	VATY        = make_tag_type('V', 'A', 'T', 'Y'),
	RETR        = make_tag_type('R', 'E', 'T', 'R'),
	ARGR        = make_tag_type('A', 'R', 'G', 'R'),
	// used in debug section
	DEBI        = make_tag_type('D', 'E', 'B', 'I'),
	DEPF        = make_tag_type('D', 'E', 'P', 'F'),
	// additional metadata
	HSRD        = make_tag_type('H', 'S', 'R', 'D'),
	UUID        = make_tag_type('U', 'U', 'I', 'D'),
	// used for source code/archive
	SARC        = make_tag_type('S', 'A', 'R', 'C'),
	// TODO/TBD
	LAYR        = make_tag_type('L', 'A', 'Y', 'R'),
	TESS        = make_tag_type('T', 'E', 'S', 'S'),
	// generic end tag
	END         = make_tag_type('E', 'N', 'D', 'T'),
};
#undef make_tag_type

enum class DATA_TYPE : uint8_t {
	INVALID = 0,
	
	STRUCT = 1,
	ARRAY = 2,
	
	FLOAT1 = 3,
	FLOAT2 = 4,
	FLOAT3 = 5,
	FLOAT4 = 6,
	FLOAT2X2 = 7,
	FLOAT2X3 = 8,
	FLOAT2X4 = 9,
	FLOAT3X2 = 10,
	FLOAT3X3 = 11,
	FLOAT3X4 = 12,
	FLOAT4X2 = 13,
	FLOAT4X3 = 14,
	FLOAT4X4 = 15,
	
	HALF1 = 16,
	HALF2 = 17,
	HALF3 = 18,
	HALF4 = 19,
	HALF2X2 = 20,
	HALF2X3 = 21,
	HALF2X4 = 22,
	HALF3X2 = 23,
	HALF3X3 = 24,
	HALF3X4 = 25,
	HALF4X2 = 26,
	HALF4X3 = 27,
	HALF4X4 = 28,
	
	INT1 = 29,
	INT2 = 30,
	INT3 = 31,
	INT4 = 32,
	
	UINT1 = 33,
	UINT2 = 34,
	UINT3 = 35,
	UINT4 = 36,
	
	SHORT1 = 37,
	SHORT2 = 38,
	SHORT3 = 39,
	SHORT4 = 40,
	
	USHORT1 = 41,
	USHORT2 = 42,
	USHORT3 = 43,
	USHORT4 = 44,
	
	CHAR1 = 45,
	CHAR2 = 46,
	CHAR3 = 47,
	CHAR4 = 48,
	
	UCHAR1 = 49,
	UCHAR2 = 50,
	UCHAR3 = 51,
	UCHAR4 = 52,
	
	BOOL1 = 53,
	BOOL2 = 54,
	BOOL3 = 55,
	BOOL4 = 56,
	
	_UNUSED_0 = 57,
	
	TEXTURE = 58,
	SAMPLER = 59,
	POINTER = 60,
	
	_UNUSED_1 = 61,
	
	R8UNORM = 62,
	R8SNORM = 63,
	R16UNORM = 64,
	R16SNORM = 65,
	RG8UNORM = 66,
	RG8SNORM = 67,
	RG16UNORM = 68,
	RG16SNORM = 69,
	RGBA8UNORM = 70,
	RGBA8UNORM_SRGB = 71,
	RGBA8SNORM = 72,
	RGBA16UNORM = 73,
	RGBA16SNORM = 74,
	RGB10A2UNORM = 75,
	RG11B10FLOAT = 76,
	RGB9E5FLOAT = 77,
	
	RENDER_PIPELINE = 78,
	COMPUTE_PIPELINE = 79,
	INDIRECT_CMD_BUFFER = 80,
	
	LONG1 = 81,
	LONG2 = 82,
	LONG3 = 83,
	LONG4 = 84,
	
	ULONG1 = 85,
	ULONG2 = 86,
	ULONG3 = 87,
	ULONG4 = 88,
	
	DOUBLE1 = 89,
	DOUBLE2 = 90,
	DOUBLE3 = 91,
	DOUBLE4 = 92,
	
	FLOAT8 = 93,
	FLOAT16 = 94,
	HALF8 = 95,
	HALF16 = 96,
	INT8 = 97,
	INT16 = 98,
	UINT8 = 99,
	UINT16 = 100,
	SHORT8 = 101,
	SHORT16 = 102,
	USHORT8 = 103,
	USHORT16 = 104,
	CHAR8 = 105,
	CHAR16 = 106,
	UCHAR8 = 107,
	UCHAR16 = 108,
	LONG8 = 109,
	LONG16 = 110,
	ULONG8 = 111,
	ULONG16 = 112,
	DOUBLE8 = 113,
	DOUBLE16 = 114,
	
	VISIBLE_FUNCTION_TABLE = 115,
	INTERSECTION_FUNCTION_TABLE = 116,
	PRIMITIVE_ACCELERATION_STRUCTURE = 117,
	INSTANCE_ACCELERATION_STRUCTURE = 118,
	
	BOOL8 = 119,
	BOOL16 = 120,
};

static inline const char* data_type_to_string(const DATA_TYPE type) {
	switch (type) {
		case DATA_TYPE::INVALID:
			return "<invalid>";
		case DATA_TYPE::STRUCT:
			return "struct";
		case DATA_TYPE::ARRAY:
			return "array";
		case DATA_TYPE::FLOAT1:
			return "float";
		case DATA_TYPE::FLOAT2:
			return "float2";
		case DATA_TYPE::FLOAT3:
			return "float3";
		case DATA_TYPE::FLOAT4:
			return "float4";
		case DATA_TYPE::FLOAT2X2:
			return "float2x2";
		case DATA_TYPE::FLOAT2X3:
			return "float2x3";
		case DATA_TYPE::FLOAT2X4:
			return "float2x4";
		case DATA_TYPE::FLOAT3X2:
			return "float3x2";
		case DATA_TYPE::FLOAT3X3:
			return "float3x3";
		case DATA_TYPE::FLOAT3X4:
			return "float3x4";
		case DATA_TYPE::FLOAT4X2:
			return "float4x2";
		case DATA_TYPE::FLOAT4X3:
			return "float4x3";
		case DATA_TYPE::FLOAT4X4:
			return "float4x4";
		case DATA_TYPE::HALF1:
			return "half1";
		case DATA_TYPE::HALF2:
			return "half2";
		case DATA_TYPE::HALF3:
			return "half3";
		case DATA_TYPE::HALF4:
			return "half4";
		case DATA_TYPE::HALF2X2:
			return "half2x2";
		case DATA_TYPE::HALF2X3:
			return "half2x3";
		case DATA_TYPE::HALF2X4:
			return "half2x4";
		case DATA_TYPE::HALF3X2:
			return "half3x2";
		case DATA_TYPE::HALF3X3:
			return "half3x3";
		case DATA_TYPE::HALF3X4:
			return "half3x4";
		case DATA_TYPE::HALF4X2:
			return "half4x2";
		case DATA_TYPE::HALF4X3:
			return "half4x3";
		case DATA_TYPE::HALF4X4:
			return "half4x4";
		case DATA_TYPE::INT1:
			return "int1";
		case DATA_TYPE::INT2:
			return "int2";
		case DATA_TYPE::INT3:
			return "int3";
		case DATA_TYPE::INT4:
			return "int4";
		case DATA_TYPE::UINT1:
			return "uint1";
		case DATA_TYPE::UINT2:
			return "uint2";
		case DATA_TYPE::UINT3:
			return "uint3";
		case DATA_TYPE::UINT4:
			return "uint4";
		case DATA_TYPE::SHORT1:
			return "short1";
		case DATA_TYPE::SHORT2:
			return "short2";
		case DATA_TYPE::SHORT3:
			return "short3";
		case DATA_TYPE::SHORT4:
			return "short4";
		case DATA_TYPE::USHORT1:
			return "ushort1";
		case DATA_TYPE::USHORT2:
			return "ushort2";
		case DATA_TYPE::USHORT3:
			return "ushort3";
		case DATA_TYPE::USHORT4:
			return "ushort4";
		case DATA_TYPE::CHAR1:
			return "char1";
		case DATA_TYPE::CHAR2:
			return "char2";
		case DATA_TYPE::CHAR3:
			return "char3";
		case DATA_TYPE::CHAR4:
			return "char4";
		case DATA_TYPE::UCHAR1:
			return "uchar1";
		case DATA_TYPE::UCHAR2:
			return "uchar2";
		case DATA_TYPE::UCHAR3:
			return "uchar3";
		case DATA_TYPE::UCHAR4:
			return "uchar4";
		case DATA_TYPE::BOOL1:
			return "bool1";
		case DATA_TYPE::BOOL2:
			return "bool2";
		case DATA_TYPE::BOOL3:
			return "bool3";
		case DATA_TYPE::BOOL4:
			return "bool4";
		case DATA_TYPE::TEXTURE:
			return "texture";
		case DATA_TYPE::SAMPLER:
			return "sampler";
		case DATA_TYPE::POINTER:
			return "pointer";
		case DATA_TYPE::R8UNORM:
			return "R8UNORM";
		case DATA_TYPE::R8SNORM:
			return "R8SNORM";
		case DATA_TYPE::R16UNORM:
			return "R16UNORM";
		case DATA_TYPE::R16SNORM:
			return "R16SNORM";
		case DATA_TYPE::RG8UNORM:
			return "RG8UNORM";
		case DATA_TYPE::RG8SNORM:
			return "RG8SNORM";
		case DATA_TYPE::RG16UNORM:
			return "RG16UNORM";
		case DATA_TYPE::RG16SNORM:
			return "RG16SNORM";
		case DATA_TYPE::RGBA8UNORM:
			return "RGBA8UNORM";
		case DATA_TYPE::RGBA8UNORM_SRGB:
			return "RGBA8UNORM_SRGB";
		case DATA_TYPE::RGBA8SNORM:
			return "RGBA8SNORM";
		case DATA_TYPE::RGBA16UNORM:
			return "RGBA16UNORM";
		case DATA_TYPE::RGBA16SNORM:
			return "RGBA16SNORM";
		case DATA_TYPE::RGB10A2UNORM:
			return "RGB10A2UNORM";
		case DATA_TYPE::RG11B10FLOAT:
			return "RG11B10FLOAT";
		case DATA_TYPE::RGB9E5FLOAT:
			return "RGB9E5FLOAT";
		case DATA_TYPE::RENDER_PIPELINE:
			return "render-pipeline";
		case DATA_TYPE::COMPUTE_PIPELINE:
			return "compute-pipeline";
		case DATA_TYPE::INDIRECT_CMD_BUFFER:
			return "indirect-command-buffer";
		case DATA_TYPE::LONG1:
			return "long1";
		case DATA_TYPE::LONG2:
			return "long2";
		case DATA_TYPE::LONG3:
			return "long3";
		case DATA_TYPE::LONG4:
			return "long4";
		case DATA_TYPE::ULONG1:
			return "ulong1";
		case DATA_TYPE::ULONG2:
			return "ulong2";
		case DATA_TYPE::ULONG3:
			return "ulong3";
		case DATA_TYPE::ULONG4:
			return "ulong4";
		case DATA_TYPE::DOUBLE1:
			return "double1";
		case DATA_TYPE::DOUBLE2:
			return "double2";
		case DATA_TYPE::DOUBLE3:
			return "double3";
		case DATA_TYPE::DOUBLE4:
			return "double4";
		case DATA_TYPE::FLOAT8:
			return "float8";
		case DATA_TYPE::FLOAT16:
			return "float16";
		case DATA_TYPE::HALF8:
			return "half8";
		case DATA_TYPE::HALF16:
			return "half16";
		case DATA_TYPE::INT8:
			return "int8";
		case DATA_TYPE::INT16:
			return "int16";
		case DATA_TYPE::UINT8:
			return "uint8";
		case DATA_TYPE::UINT16:
			return "uint16";
		case DATA_TYPE::SHORT8:
			return "short8";
		case DATA_TYPE::SHORT16:
			return "short16";
		case DATA_TYPE::USHORT8:
			return "ushort8";
		case DATA_TYPE::USHORT16:
			return "ushort16";
		case DATA_TYPE::CHAR8:
			return "char8";
		case DATA_TYPE::CHAR16:
			return "char16";
		case DATA_TYPE::UCHAR8:
			return "uchar8";
		case DATA_TYPE::UCHAR16:
			return "uchar16";
		case DATA_TYPE::LONG8:
			return "long8";
		case DATA_TYPE::LONG16:
			return "long16";
		case DATA_TYPE::ULONG8:
			return "ulong8";
		case DATA_TYPE::ULONG16:
			return "ulong16";
		case DATA_TYPE::DOUBLE8:
			return "double8";
		case DATA_TYPE::DOUBLE16:
			return "double16";
		case DATA_TYPE::VISIBLE_FUNCTION_TABLE:
			return "visible-function-table";
		case DATA_TYPE::INTERSECTION_FUNCTION_TABLE:
			return "intersection-function-table";
		case DATA_TYPE::PRIMITIVE_ACCELERATION_STRUCTURE:
			return "primitive-acceleration-structure";
		case DATA_TYPE::INSTANCE_ACCELERATION_STRUCTURE:
			return "instance-acceleration-structure";
		case DATA_TYPE::BOOL8:
			return "bool8";
		case DATA_TYPE::BOOL16:
			return "bool16";
		case DATA_TYPE::_UNUSED_0:
		case DATA_TYPE::_UNUSED_1:
			break;
	}
	return "<unknown-type>";
}

static inline DATA_TYPE data_type_from_string(const std::string& type_str) {
	static const std::unordered_map<std::string, DATA_TYPE> lut {
		{ "double", DATA_TYPE::DOUBLE1 },
		{ "double1", DATA_TYPE::DOUBLE1 },
		{ "double2", DATA_TYPE::DOUBLE2 },
		{ "double3", DATA_TYPE::DOUBLE3 },
		{ "double4", DATA_TYPE::DOUBLE4 },
		{ "float", DATA_TYPE::FLOAT1 },
		{ "float1", DATA_TYPE::FLOAT1 },
		{ "float2", DATA_TYPE::FLOAT2 },
		{ "float3", DATA_TYPE::FLOAT3 },
		{ "float4", DATA_TYPE::FLOAT4 },
		{ "half", DATA_TYPE::HALF1 },
		{ "half1", DATA_TYPE::HALF1 },
		{ "half2", DATA_TYPE::HALF2 },
		{ "half3", DATA_TYPE::HALF3 },
		{ "half4", DATA_TYPE::HALF4 },
		{ "long", DATA_TYPE::LONG1 },
		{ "long1", DATA_TYPE::LONG1 },
		{ "long2", DATA_TYPE::LONG2 },
		{ "long3", DATA_TYPE::LONG3 },
		{ "long4", DATA_TYPE::LONG4 },
		{ "ulong", DATA_TYPE::ULONG1 },
		{ "ulong1", DATA_TYPE::ULONG1 },
		{ "ulong2", DATA_TYPE::ULONG2 },
		{ "ulong3", DATA_TYPE::ULONG3 },
		{ "ulong4", DATA_TYPE::ULONG4 },
		{ "int", DATA_TYPE::INT1 },
		{ "int1", DATA_TYPE::INT1 },
		{ "int2", DATA_TYPE::INT2 },
		{ "int3", DATA_TYPE::INT3 },
		{ "int4", DATA_TYPE::INT4 },
		{ "uint", DATA_TYPE::UINT1 },
		{ "uint1", DATA_TYPE::UINT1 },
		{ "uint2", DATA_TYPE::UINT2 },
		{ "uint3", DATA_TYPE::UINT3 },
		{ "uint4", DATA_TYPE::UINT4 },
		{ "short", DATA_TYPE::SHORT1 },
		{ "short1", DATA_TYPE::SHORT1 },
		{ "short2", DATA_TYPE::SHORT2 },
		{ "short3", DATA_TYPE::SHORT3 },
		{ "short4", DATA_TYPE::SHORT4 },
		{ "ushort", DATA_TYPE::USHORT1 },
		{ "ushort1", DATA_TYPE::USHORT1 },
		{ "ushort2", DATA_TYPE::USHORT2 },
		{ "ushort3", DATA_TYPE::USHORT3 },
		{ "ushort4", DATA_TYPE::USHORT4 },
		{ "char", DATA_TYPE::CHAR1 },
		{ "char1", DATA_TYPE::CHAR1 },
		{ "char2", DATA_TYPE::CHAR2 },
		{ "char3", DATA_TYPE::CHAR3 },
		{ "char4", DATA_TYPE::CHAR4 },
		{ "uchar", DATA_TYPE::UCHAR1 },
		{ "uchar1", DATA_TYPE::UCHAR1 },
		{ "uchar2", DATA_TYPE::UCHAR2 },
		{ "uchar3", DATA_TYPE::UCHAR3 },
		{ "uchar4", DATA_TYPE::UCHAR4 },
		{ "bool", DATA_TYPE::BOOL1 },
		{ "bool1", DATA_TYPE::BOOL1 },
		{ "bool2", DATA_TYPE::BOOL2 },
		{ "bool3", DATA_TYPE::BOOL3 },
		{ "bool4", DATA_TYPE::BOOL4 },
	};
	const auto iter = lut.find(type_str);
	if (iter == lut.end()) {
		return DATA_TYPE::INVALID;
	}
	return iter->second;
}

enum class VERTEX_USE : uint8_t /* only 2 bits */ {
	// normal/default vertex attribute
	STANDARD = 0,
	// per patch vertex attribute
	PER_PATCH = 1,
	// control point
	CONRTOL_POINT = 2,
	_UNUSED = 3,
};
struct vertex_attribute {
	std::string name;
	uint32_t index { 0u };
	DATA_TYPE type { DATA_TYPE::INVALID };
	VERTEX_USE use { VERTEX_USE::STANDARD };
	bool active { false };
};

} // namespace metal

#endif
