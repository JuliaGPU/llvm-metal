//===-- FloorGPUTTI.h - LibFloor GPU TTI implementation ---------*- C++ -*-===//
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
// TODO !
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LIBFLOOR_FLOORGPUTTI_H
#define LLVM_TRANSFORMS_LIBFLOOR_FLOORGPUTTI_H

#include "clang/Basic/CodeGenOptions.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Module.h"

namespace llvm {

//! (dummy) libfloor GPU target machine
class LibFloorGPUTargetMachine : public LLVMTargetMachine {
public:
	// TODO: DataLayout, TargetOptions
	explicit LibFloorGPUTargetMachine() :
	LLVMTargetMachine({}, "", {}, "", "", {},
					  Reloc::Static, CodeModel::Tiny, CodeGenOpt::Aggressive) {}
	
	static LibFloorGPUTargetMachine* get_instance() {
		static LibFloorGPUTargetMachine instance;
		return &instance;
	}
	
	bool isNoopAddrSpaceCast(unsigned FromAS, unsigned ToAS) const override {
		return (FromAS == ToAS);
	}
	
};

//! (dummy) libfloor GPU sub-target
class LibFloorGPUSubTarget : public TargetSubtargetInfo {
public:
	// TODO: Triple, "CPU"?
	LibFloorGPUSubTarget() :
	TargetSubtargetInfo({}, "", "", "", {}, {}, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) {}
	
	static LibFloorGPUSubTarget* get_instance() {
		static LibFloorGPUSubTarget instance;
		return &instance;
	}
	
	// TODO: getSchedModel, getCacheSize, getCacheLineSize, getPrefetchDistance, getMinPrefetchStride, getMaxPrefetchIterationsAhead, enableWritePrefetching, getCacheAssociativity, useAA,
	
};

//! libfloor GPU target lowering
class LibFloorGPUTargetLowering : public TargetLowering {
protected:
	const bool is_metal { false };
	const bool is_vulkan { false };
	
public:
	explicit LibFloorGPUTargetLowering(const bool is_metal_, const bool is_vulkan_) :
	TargetLowering(*LibFloorGPUTargetMachine::get_instance()), is_metal(is_metal_), is_vulkan(is_vulkan_) {}
	
	static LibFloorGPUTargetLowering* get_instance(const bool is_metal_, const bool is_vulkan_) {
		static LibFloorGPUTargetLowering instance(is_metal_, is_vulkan_);
		return &instance;
	}
	
	//! Metal: allow misaligned access under certain pre-conditions
	//! Vulkan: TBD
	bool impl_allowsMisalignedMemoryAccesses(unsigned BitWidth, unsigned AddressSpace, Align Alignment, bool *Fast) const {
		if (is_metal) {
			// both alignment and #bytes must be 1 or divisble by 2
			const auto align = Alignment.value();
			if (align > 1u && align % 2u != 0) {
				return false;
			}
			const auto bytes = BitWidth / 8u;
			if (bytes % 2u != 0) {
				return false;
			}
			// alignment must be smaller or equal to #bytes
			if (bytes < align) {
				return false;
			}
			// bytes must be a multiple of alignment
			if (((bytes / align) * align) != bytes) {
				return false;
			}
			// always flag as fast
			if (Fast) {
				*Fast = true;
			}
			return true;
		}
		return false;
	}
	
	bool allowsMisalignedMemoryAccesses(EVT evt, unsigned AddrSpace = 0, Align Alignment = Align(1),
										MachineMemOperand::Flags Flags = MachineMemOperand::MONone,
										bool *Fast = nullptr) const override {
		return impl_allowsMisalignedMemoryAccesses(evt.getSizeInBits(), AddrSpace, Alignment, Fast);
	}
	
	bool allowsMisalignedMemoryAccesses(LLT llt, unsigned AddrSpace = 0, Align Alignment = Align(1),
										MachineMemOperand::Flags Flags = MachineMemOperand::MONone,
										bool *Fast = nullptr) const override {
		return impl_allowsMisalignedMemoryAccesses(llt.getSizeInBits(), AddrSpace, Alignment, Fast);
	}
	
};

//! libfloor target transform implementation
class LibFloorGPUTTIImpl : public TargetTransformInfoImplCRTPBase<LibFloorGPUTTIImpl> {
public:
	using crtp_base_class = TargetTransformInfoImplCRTPBase<LibFloorGPUTTIImpl>;
	friend crtp_base_class;
	using TTI = TargetTransformInfo;

	explicit LibFloorGPUTTIImpl(const Function& F, const clang::CodeGenOptions& CodeGenOpts_,
								const clang::TargetOptions& TargetOpts_,
								const clang::LangOptions& LangOpts_,
								Module& M_) :
	crtp_base_class(F.getParent()->getDataLayout()),
	CodeGenOpts(CodeGenOpts_), TargetOpts(TargetOpts_), LangOpts(LangOpts_), M(M_),
	is_metal(Triple(M.getTargetTriple()).getArch() == Triple::ArchType::air64),
	is_vulkan(Triple(M.getTargetTriple()).getArch() == Triple::ArchType::spir64 &&
			  Triple(M.getTargetTriple()).getEnvironment() == Triple::EnvironmentType::Vulkan) {}
	
	//! restrict to width of 4
	unsigned getMaximumVF(unsigned, unsigned) const { return 4; }
	
	unsigned getLoadStoreVecRegBitWidth(unsigned /* AddrSpace */ ) const {
		return 128;
	}
	unsigned getLoadVectorFactor(unsigned VF, unsigned LoadSize,
								 unsigned ChainSizeInBytes,
								 VectorType *VecTy) const {
		const auto max_bit_width = getLoadStoreVecRegBitWidth(0);
		const auto VecRegBitWidth = VF * LoadSize;
		if (VecRegBitWidth > max_bit_width && VecTy->getScalarSizeInBits() < 32) {
			return max_bit_width / LoadSize;
		}
		return VF;
	}
	unsigned getStoreVectorFactor(unsigned VF, unsigned StoreSize,
								  unsigned ChainSizeInBytes,
								  VectorType *VecTy) const {
		const auto max_bit_width = getLoadStoreVecRegBitWidth(0);
		const auto VecRegBitWidth = VF * StoreSize;
		if (VecRegBitWidth > max_bit_width) {
			return max_bit_width / StoreSize;
		}
		return VF;
	}
	
	//! yes - this is a GPU target
	bool hasBranchDivergence() const { return true; }
	
	//! yes - this is a GPU target
	bool useGPUDivergenceAnalysis() const { return true; }
	
	//! yes - we'll inline everything anyways
	bool areInlineCompatible(const Function*, const Function*) const { return true; }
	
	/*InstructionCost getIntrinsicInstrCost(const IntrinsicCostAttributes &ICA,
										  TTI::TargetCostKind CostKind) {
		;;
	}*/
	InstructionCost getMemcpyCost(const Instruction *I) const {
		return 255;
	}
	
	bool allowsMisalignedMemoryAccesses(LLVMContext &Context, unsigned BitWidth,
										unsigned AddressSpace, Align Alignment,
										bool *Fast) {
		return TLI->impl_allowsMisalignedMemoryAccesses(BitWidth, AddressSpace, Alignment, Fast);
	}
	
	InstructionCost getShuffleCost(TTI::ShuffleKind Kind,
								   VectorType *VT, ArrayRef<int> Mask,
								   int Index, VectorType *SubTp) {
		return 0;
	}
	
	InstructionCost getVectorInstrCost(unsigned Opcode, Type *ValTy, unsigned Index) {
		if (Index == 0 && (Opcode == Instruction::ExtractElement ||
						   Opcode == Instruction::InsertElement)) {
			return 0;
		}
		return crtp_base_class::getVectorInstrCost(Opcode, ValTy, Index);
	}
	
	InstructionCost getArithmeticInstrCost(unsigned Opcode, Type *Ty, TTI::TargetCostKind CostKind,
										   TTI::OperandValueKind Opd1Info = TTI::OK_AnyValue,
										   TTI::OperandValueKind Opd2Info = TTI::OK_AnyValue,
										   TTI::OperandValueProperties Opd1PropInfo = TTI::OP_None,
										   TTI::OperandValueProperties Opd2PropInfo = TTI::OP_None,
										   ArrayRef<const Value *> Args = ArrayRef<const Value *>(),
										   const Instruction *CxtI = nullptr) {
		auto cost = crtp_base_class::getArithmeticInstrCost(Opcode, Ty, CostKind, Opd1Info, Opd2Info,
															Opd1PropInfo, Opd2PropInfo, Args, CxtI);
		if (is_metal && Ty->isVectorTy()) {
			// Metal: for vectors, bias cost so that it equals scalar cost
			if (auto fixed_vec_type = dyn_cast<FixedVectorType>(Ty); fixed_vec_type) {
				cost /= fixed_vec_type->getNumElements();
			}
		}
		return cost;
	}
	
	InstructionCost getArithmeticReductionCost(unsigned Opcode, VectorType *Ty,
											   Optional<FastMathFlags> FMF,
											   TTI::TargetCostKind CostKind) {
		// for now: penalize reduction ops (TODO: all fadd/fmul to combine to dot product)
		return crtp_base_class::getArithmeticReductionCost(Opcode, Ty, FMF, CostKind) * 1024;
	}
	
protected:
	const clang::CodeGenOptions& CodeGenOpts;
	const clang::TargetOptions& TargetOpts;
	const clang::LangOptions& LangOpts;
	const Module& M;
	const bool is_metal { false };
	const bool is_vulkan { false };
	
	const LibFloorGPUSubTarget* ST {
		LibFloorGPUSubTarget::get_instance()
	};
	const LibFloorGPUTargetLowering* TLI {
		LibFloorGPUTargetLowering::get_instance(is_metal, is_vulkan)
	};
	
public:
	const LibFloorGPUSubTarget* getST() const {
		return ST;
	}
	
	const LibFloorGPUTargetLowering* getTLI() const {
		return TLI;
	}
	
};

} // namespace llvm

#endif
