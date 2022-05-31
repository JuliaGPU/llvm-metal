//===-- Metal.cpp - Library for converting LLVM code to C --------------===//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// The original implementation is from libfloor (https://github.com/a2flo/floor)
// Copyright (C) 2004 - 2021 Florian Ziesche
//
//===----------------------------------------------------------------------===//

#include "Metal.h"

#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/SHA256.h"

#include <unordered_map>

using namespace llvm;
using namespace std;

namespace {
class WriteMetalLibPass : public ModulePass {
  raw_ostream &OS; // raw_ostream to print on

public:
  static char ID; // Pass identification, replacement for typeid
  explicit WriteMetalLibPass(raw_ostream &o) : ModulePass(ID), OS(o) {}

  StringRef getPassName() const override { return "Metal Library Writer"; }

  bool runOnModule(Module &M) override;
};
}

char WriteMetalLibPass::ID = 0;

ModulePass *llvm::createMetalLibWriterPass(raw_ostream &Str) {
  return new WriteMetalLibPass(Str);
}

//
struct __attribute__((packed)) metallib_version {
  // version of the container (NOTE: per-program Metal/AIR version info is
  // extra)
  uint32_t container_version_major : 8;
  uint32_t container_version_rev : 4;   // lo
  uint32_t container_version_minor : 4; // hi

  // unknown: always 2, 0, 0
  // TODO: might be dwarf version?
  uint32_t unknown_version_major : 8;
  uint32_t unknown_version_rev : 4;   // lo
  uint32_t unknown_version_minor : 4; // hi

  // unknown: always 2 (macOS < 10.15), 3 (iOS and macOS 10.15),
  // 5 (macOS 10.16+/11.0)
  uint32_t unkown_version;

  // unknown: always 0
  uint32_t zero;
};
static_assert(sizeof(metallib_version) == 12, "invalid version header length");

struct __attribute__((packed)) metallib_header_control {
  uint64_t programs_offset;
  uint64_t programs_length;
  uint64_t reflection_offset;
  uint64_t reflection_length;
  uint64_t debug_offset;
  uint64_t debug_length;
  uint64_t bitcode_offset;
  uint64_t bitcode_length;
  uint32_t program_count;
};
static_assert(sizeof(metallib_header_control) == 68,
              "invalid program info length");

struct __attribute__((packed)) metallib_header {
  const char magic[4]; // == metallib_magic
  const metallib_version version;
  const uint64_t file_length;
  const metallib_header_control header_control;
};
static_assert(sizeof(metallib_header) == 4 + sizeof(metallib_version) +
                                             sizeof(uint64_t) +
                                             sizeof(metallib_header_control),
              "invalid metallib header size");

struct metallib_program_info {
// NOTE: tag types are always 32-bit
// NOTE: tag types are always followed by a uint16_t that specifies the length
// of the tag data
#define make_tag_type(a, b, c, d)                                              \
  ((uint32_t(d) << 24u) | (uint32_t(c) << 16u) | (uint32_t(b) << 8u) |         \
   uint32_t(a))
  enum TAG_TYPE : uint32_t {
    // used in initial header section
    NAME = make_tag_type('N', 'A', 'M', 'E'),
    TYPE = make_tag_type('T', 'Y', 'P', 'E'),
    HASH = make_tag_type('H', 'A', 'S', 'H'),
    MD_SIZE = make_tag_type('M', 'D', 'S', 'Z'),
    OFFSET = make_tag_type('O', 'F', 'F', 'T'),
    VERSION = make_tag_type('V', 'E', 'R', 'S'),
    // used in reflection section
    CNST = make_tag_type('C', 'N', 'S', 'T'),
    VATT = make_tag_type('V', 'A', 'T', 'T'),
    VATY = make_tag_type('V', 'A', 'T', 'Y'),
    RETR = make_tag_type('R', 'E', 'T', 'R'),
    ARGR = make_tag_type('A', 'R', 'G', 'R'),
    // used in debug section
    DEBI = make_tag_type('D', 'E', 'B', 'I'),
    // TODO/TBD
    LAYR = make_tag_type('L', 'A', 'Y', 'R'),
    TESS = make_tag_type('T', 'E', 'S', 'S'),
    SOFF = make_tag_type('S', 'O', 'F', 'F'),
    // generic end tag
    END = make_tag_type('E', 'N', 'D', 'T'),
  };
#undef make_tag_type

  enum class PROGRAM_TYPE : uint8_t {
    VERTEX = 0,
    FRAGMENT = 1,
    KERNEL = 2,
    // TODO: tessellation?
    NONE = 255
  };

  struct version_info {
    uint32_t major : 16;
    uint32_t minor : 8;
    uint32_t rev : 8;
  };
  static_assert(sizeof(version_info) == sizeof(uint32_t),
                "invalid offset_info size");

  struct offset_info {
    // NOTE: these are all relative offsets -> add to metallib_header_control
    // offsets to get absolute offsets
    uint64_t reflection_offset;
    uint64_t debug_offset;
    uint64_t bitcode_offset;
  };
  static_assert(sizeof(offset_info) == 3 * sizeof(uint64_t),
                "invalid offset_info size");

  struct entry {
    uint32_t length{0};

    string name; // NOTE: limited to 65536 - 1 ('\0')

    PROGRAM_TYPE type{PROGRAM_TYPE::NONE};

    array<uint8_t, 32> hash;

    offset_info offset{0, 0, 0};

    // we need a separate stream for the actual bitcode data, since we need to
    // know
    // the size of each module/file (no way to know this beforehand)
    string bitcode_data{""}; // -> used via raw_string_ostream later on
    uint64_t bitcode_size{0};

    // same for reflection and debug data
    string reflection_data{""};
    uint64_t reflection_size{0};
    string debug_data{""};
    uint64_t debug_size{0};

    version_info metal_version;
    version_info metal_language_version;

    // output in same order as Apple:
    //  * NAME
    //  * TYPE
    //  * HASH
    //  * MDSZ
    //  * OFFT
    //  * VERS
    //  * SOFF (TODO)
    //  * ENDT
    void update_length() {
      length = 4;                     // length info itself
      length += 7 * sizeof(TAG_TYPE); // 7 tags
      length += 6 * sizeof(uint16_t); // tag lengths (except ENDT)

      length += name.size() + 1;          // name length + \0
      length += 1;                        // type
      length += sizeof(hash);             // hash
      length += sizeof(uint64_t);         // module size, always 8 bytes
      length += sizeof(offset_info);      // offset
      length += sizeof(version_info) * 2; // both versions

      bitcode_size = bitcode_data.size();
      reflection_size = reflection_data.size();
      debug_size = debug_data.size();
    }
    void update_offsets(uint64_t &running_refl_size, uint64_t &running_dbg_size,
                        uint64_t &running_bc_size) {
      offset.reflection_offset = running_refl_size;
      offset.debug_offset = running_dbg_size;
      offset.bitcode_offset = running_bc_size;

      running_refl_size += reflection_size;
      running_dbg_size += debug_size;
      running_bc_size += bitcode_size;
    }

    template <typename data_type>
    static inline void write_value(raw_ostream &OS, const data_type &value) {
      OS.write((const char *)&value, sizeof(data_type));
    }

    void write_header(raw_ostream &OS) const {
      write_value(OS, length);

      // NAME
      write_value(OS, TAG_TYPE::NAME);
      write_value(OS, uint16_t(name.size() + 1));
      OS << name << '\0';

      // TYPE
      write_value(OS, TAG_TYPE::TYPE);
      write_value(OS, uint16_t(sizeof(uint8_t)));
      write_value(OS, uint8_t(type));

      // HASH
      write_value(OS, TAG_TYPE::HASH);
      write_value(OS, uint16_t(sizeof(hash)));
      write_value(OS, hash);

      // MDSZ
      write_value(OS, TAG_TYPE::MD_SIZE);
      write_value(OS, uint16_t(sizeof(uint64_t)));
      write_value(OS, uint64_t(bitcode_data.size()));

      // OFFT
      write_value(OS, TAG_TYPE::OFFSET);
      write_value(OS, uint16_t(sizeof(offset_info)));
      write_value(OS, offset.reflection_offset);
      write_value(OS, offset.debug_offset);
      write_value(OS, offset.bitcode_offset);

      // VERS
      write_value(OS, TAG_TYPE::VERSION);
      write_value(OS, uint16_t(2 * sizeof(version_info)));
      write_value(OS, *(const uint32_t *)&metal_version);
      write_value(OS, *(const uint32_t *)&metal_language_version);

      // ENDT
      write_value(OS, TAG_TYPE::END);
    }

    void write_module(raw_ostream &OS) const {
      OS.write(bitcode_data.data(), bitcode_data.size());
    }

    void write_reflection(raw_ostream &OS) const {
      OS.write(reflection_data.data(), reflection_data.size());
    }

    void write_debug(raw_ostream &OS) const {
      OS.write(debug_data.data(), debug_data.size());
    }
  };
  vector<entry> entries;
};

//
static bool is_used_in_function(const Function *F, const Value *V) {
  for (const auto &user : V->users()) {
    if (const auto instr = dyn_cast<Instruction>(user)) {
      if (instr->getParent()->getParent() == F) {
        return true;
      }
    } else if (const auto const_expr = dyn_cast<ConstantExpr>(user)) {
      for (const auto &ce_user : const_expr->users()) {
        if (const auto ce_instr = dyn_cast<Instruction>(ce_user)) {
          if (ce_instr->getParent()->getParent() == F) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

// version -> { AIR version, language version }
static const unordered_map<uint32_t,
                           pair<array<uint32_t, 3>, array<uint32_t, 3>>>
    metal_versions{
        {200, {{{2, 0, 0}}, {{2, 0, 0}}}}, {210, {{{2, 1, 0}}, {{2, 1, 0}}}},
        {220, {{{2, 2, 0}}, {{2, 2, 0}}}}, {230, {{{2, 3, 0}}, {{2, 3, 0}}}},
        {240, {{{2, 4, 0}}, {{2, 4, 0}}}},
    };

//
bool WriteMetalLibPass::runOnModule(Module &M) {
  // get metal version
  Triple TT(M.getTargetTriple());
  uint32_t target_air_version = 200;
  if (M.getSDKVersion() != VersionTuple()) {
    // it's possible we already have a SDK Version module flag,
    // e.g. when re-processing IR extracted from a metallib file.
  } else if (TT.isiOS()) {
    uint32_t ios_major, ios_minor, ios_micro;
    TT.getiOSVersion(ios_major, ios_minor, ios_micro);
    if (ios_major <= 11) {
      target_air_version = 200;
    } else if (ios_major == 12) {
      target_air_version = 210;
    } else if (ios_major == 13) {
      target_air_version = 220;
    } else if (ios_major >= 14) {
      target_air_version = 230;
    } else if (ios_major >= 15) {
      target_air_version = 240;
    }

    M.setSDKVersion(VersionTuple{ios_major, ios_minor});
  } else {
    uint32_t osx_major, osx_minor, osx_micro;
    TT.getMacOSXVersion(osx_major, osx_minor, osx_micro);
    if (osx_major == 10 && osx_minor <= 13) {
      target_air_version = 200;
    } else if (osx_major == 10 && osx_minor == 14) {
      target_air_version = 210;
    } else if (osx_major == 10 && osx_minor == 15) {
      target_air_version = 220;
    } else if ((osx_major == 11 && osx_minor >= 0) ||
               (osx_major == 10 && osx_minor >= 16)) {
      target_air_version = 230;
    } else if ((osx_major == 12 && osx_minor >= 0) || osx_major > 12) {
      target_air_version = 240;
    }

    M.setSDKVersion(VersionTuple{osx_major, osx_minor});
  }
  const auto &metal_version = *metal_versions.find(target_air_version);

  // gather entry point functions that we want to clone/emit
  unordered_map<string, metallib_program_info::PROGRAM_TYPE> function_set;
  // -> first pass to gather all entry points specified in metadata lists
  for (uint32_t i = 0; i < 3; ++i) {
    const auto func_type = (metallib_program_info::PROGRAM_TYPE)i;
    const NamedMDNode *func_list = nullptr;
    switch (func_type) {
    case metallib_program_info::PROGRAM_TYPE::KERNEL:
      func_list = M.getNamedMetadata("air.kernel");
      break;
    case metallib_program_info::PROGRAM_TYPE::VERTEX:
      func_list = M.getNamedMetadata("air.vertex");
      break;
    case metallib_program_info::PROGRAM_TYPE::FRAGMENT:
      func_list = M.getNamedMetadata("air.fragment");
      break;
    case metallib_program_info::PROGRAM_TYPE::NONE:
      llvm_unreachable("invalid type");
    }
    if (func_list == nullptr) {
      // no functions of this type
      continue;
    }

    for (const auto &op : func_list->operands()) {
      const auto &op_0 = op->getOperand(0);
      if (auto const_md = dyn_cast<ConstantAsMetadata>(op_0)) {
        if (auto func = dyn_cast<Function>(const_md->getValue())) {
          function_set.emplace(func->getName().str(), func_type);
        }
      }
    }
  }
  // -> second pass to actually gather all functions
  // NOTE: we do it this way so that we maintain the order of functions
  vector<pair<const Function *, metallib_program_info::PROGRAM_TYPE>> functions;
  for (const auto &func : M.functions()) {
    if (!func.hasName()) {
      continue;
    }

    const auto func_iter = function_set.find(func.getName().str());
    if (func_iter == function_set.end()) {
      continue; // not an entry point
    }
    functions.emplace_back(&func, func_iter->second);
  }
  const uint32_t function_count = uint32_t(functions.size());

  // program info
  metallib_header_control ctrl;
  metallib_program_info prog_info;
  ctrl.program_count = function_count;
  prog_info.entries.resize(function_count);

  // create per-function modules and fill entries
  uint64_t entries_size = 0;
  uint64_t reflection_data_size = 0;
  uint64_t debug_data_size = 0;
  uint64_t bitcode_data_size = 0;
  for (uint32_t i = 0; i < function_count; ++i) {
    auto &entry = prog_info.entries[i];
    auto &func = functions[i].first;

    entry.type = functions[i].second;
    entry.name = func->getName().str();
    entry.metal_version.major = metal_version.second.first[0];
    entry.metal_version.minor = metal_version.second.first[1];
    entry.metal_version.rev = metal_version.second.first[2];
    entry.metal_language_version.major = metal_version.second.second[0];
    entry.metal_language_version.minor = metal_version.second.second[1];
    entry.metal_language_version.rev = metal_version.second.second[2];

    // clone the module with the current entry point function
    // and any global values (variables, functions) that we need
    ValueToValueMapTy VMap;
    auto cloned_mod = CloneModule(M, VMap, [&func](const GlobalValue *GV) {
      if (GV == func)
        return true;
      return is_used_in_function(func, GV);
    });

    // update data layout
    if (target_air_version >= 230) {
      cloned_mod->setDataLayout(
          "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-"
          "f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:"
          "128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:"
          "1024-n8:16:32");
    }

    // remove all unused functions and global vars, since CloneModule only sets
    // unused vars to external linkage and unused funcs are declarations only
    // NOTE: this also removes entry points that are now unused (metadata is
    // removed later)
    for (auto I = cloned_mod->begin(), E = cloned_mod->end(); I != E;) {
      Function &F = *I++;
      if (F.isDeclaration() && F.use_empty()) {
        F.eraseFromParent();
        continue;
      }
    }

    for (auto I = cloned_mod->global_begin(), E = cloned_mod->global_end();
         I != E;) {
      GlobalVariable &GV = *I++;
      if (GV.isDeclaration() && GV.use_empty()) {
        GV.eraseFromParent();
        continue;
      }
    }

    // clean up metadata
    // * metadata of all entry points that no longer exist
    static constexpr const std::array<const char *, 3> entry_point_md_names{{
        "air.kernel",
        "air.vertex",
        "air.fragment",
    }};
    for (const auto &entry_point_md_name : entry_point_md_names) {
      if (auto func_entries =
              cloned_mod->getNamedMetadata(entry_point_md_name)) {
        vector<MDNode *> kept_nodes;
        for (auto op_iter = func_entries->op_begin();
             op_iter != func_entries->op_end(); ++op_iter) {
          MDNode *node = *op_iter;
          if (node->getNumOperands() < 3) {
            continue;
          }
          if (node->getOperand(0).get() == nullptr) {
            continue;
          }
          kept_nodes.emplace_back(node);
        }

        // need to drop all references to existing nodes, b/c we can't directly
        // remove operands
        func_entries->dropAllReferences();
        if (kept_nodes.empty()) {
          // remove air.sampler_states altogether
          func_entries->eraseFromParent();
        } else {
          // now, only add ones we want to keep
          for (auto node : kept_nodes) {
            func_entries->addOperand(node);
          }
        }
      }
    }
    // * sample states of entry points that no longer exist
    if (auto sampler_states =
            cloned_mod->getNamedMetadata("air.sampler_states")) {
      vector<MDNode *> kept_nodes;
      for (auto op_iter = sampler_states->op_begin();
           op_iter != sampler_states->op_end(); ++op_iter) {
        MDNode *node = *op_iter;
        if (node->getNumOperands() != 2 ||
            node->getOperand(1).get() == nullptr) {
          continue;
        }
        kept_nodes.emplace_back(node);
      }

      // need to drop all references to existing nodes, b/c we can't directly
      // remove operands
      sampler_states->dropAllReferences();
      if (kept_nodes.empty()) {
        // remove air.sampler_states altogether
        sampler_states->eraseFromParent();
      } else {
        // now, only add ones we want to keep
        for (auto node : kept_nodes) {
          sampler_states->addOperand(node);
        }
      }
    }
    // * set fake compiler ident
    if (target_air_version >= 230) {
      if (auto llvm_ident = cloned_mod->getNamedMetadata("llvm.ident")) {
        if (MDNode *ident_op = llvm_ident->getOperand(0)) {
          static const std::unordered_map<uint32_t, std::string> ident_versions {
            { 230, "Apple LLVM version 31001.143 (metalfe-31001.143)" },
            { 240, "Apple metal version 31001.322 (metalfe-31001.322.1)" },
          };
          ident_op->replaceOperandWith(
              0, llvm::MDString::get(
                     cloned_mod->getContext(),
                     ident_versions.at(target_air_version)));
        }
      }
    }

    // modify local and constant memory GVs
    const auto &DL = cloned_mod->getDataLayout();
    for (auto I = cloned_mod->global_begin(), E = cloned_mod->global_end();
         I != E;) {
      GlobalVariable &GV = *I++;
      if (GV.getAddressSpace() == 2 /* constant memory */ ||
          GV.getAddressSpace() == 3 /* local memory */) {
        auto value_type = GV.getValueType();
        if (value_type && value_type->isSized() &&
            DL.getTypeStoreSize(value_type) >= 16 && GV.getAlignment() < 16) {
          // use at least 16-byte alignment
          GV.setAlignment(MaybeAlign { 16u });
        }
      }
      if (GV.getAddressSpace() == 3 /* local memory */) {
        // always use undef initializer (instead of zeroinitializer)
        GV.setInitializer(UndefValue::get(GV.getValueType()));
      }
    }

    // write module / bitcode
    raw_string_ostream bitcode_stream{entry.bitcode_data};
    WriteBitcode50ToFile(cloned_mod.get(), bitcode_stream);
    bitcode_stream.flush();

    // hash module
    entry.hash = SHA256::hash(
      makeArrayRef((const uint8_t *)entry.bitcode_data.data(),
                   entry.bitcode_data.size()));

    // write reflection and debug data (just ENDT right now)
    const auto end_tag = metallib_program_info::TAG_TYPE::END;
    const uint32_t tag_length = sizeof(metallib_program_info::TAG_TYPE);

    raw_string_ostream refl_stream{entry.reflection_data};
    refl_stream.write((const char *)&tag_length, sizeof(uint32_t));
    refl_stream.write((const char *)&end_tag, tag_length);
    refl_stream.flush();

    raw_string_ostream dbg_stream{entry.debug_data};
    dbg_stream.write((const char *)&tag_length, sizeof(uint32_t));
    dbg_stream.write((const char *)&end_tag, tag_length);
    dbg_stream.flush();

    // finish
    entry.update_length();
    entries_size += entry.length;
    reflection_data_size += entry.reflection_size;
    debug_data_size += entry.debug_size;
    bitcode_data_size += entry.bitcode_size;
  }

  // now that we have created all data/info, update all offsets
  uint64_t running_refl_size = 0, running_dbg_size = 0, running_bc_size = 0;
  for (uint32_t i = 0; i < function_count; ++i) {
    auto &entry = prog_info.entries[i];
    entry.update_offsets(running_refl_size, running_dbg_size, running_bc_size);
  }

  //// start writing
  // header
  OS.write("MTLB", 4);

  metallib_version header;
  if (TT.isiOS()) {
    header.container_version_major = 1; // always 1.0.0 right now
    header.container_version_minor = 0;
  } else { // macOS
    header.container_version_major = 1; // always 1.8.0 right now
    header.container_version_minor = 8;
  }
  header.container_version_rev = 0;
  header.unknown_version_major = 2;
  header.unknown_version_minor = 0;
  header.unknown_version_rev = 0;
  if (TT.isMacOSX() && !TT.isOSVersionLT(10, 16, 0)) {
    header.unkown_version = 5;
  } else if (TT.isiOS() || (TT.isMacOSX() && !TT.isOSVersionLT(10, 15, 0))) {
    header.unkown_version = 3;
  } else { // macOS
    header.unkown_version = 2;
  }
  header.zero = 0;

  OS.write((const char *)&header, sizeof(metallib_version));

  // file length
  const uint64_t header_size = sizeof(metallib_header);
  const uint64_t initial_offset = header_size + entries_size;
  const uint64_t dummy_ENDT_size =
      sizeof(metallib_program_info::TAG_TYPE);
  const uint64_t file_length = initial_offset + dummy_ENDT_size +
                               reflection_data_size + debug_data_size +
                               bitcode_data_size;
  OS.write((const char *)&file_length, sizeof(uint64_t));

  // header control
  ctrl.programs_offset = sizeof(metallib_header) - 4 /* header or count? */;
  ctrl.programs_length = entries_size;
  ctrl.reflection_offset = initial_offset + dummy_ENDT_size;
  ctrl.reflection_length = reflection_data_size;
  ctrl.debug_offset = initial_offset + dummy_ENDT_size + reflection_data_size;
  ctrl.debug_length = debug_data_size;
  ctrl.bitcode_offset =
      initial_offset + dummy_ENDT_size + reflection_data_size + debug_data_size;
  ctrl.bitcode_length = bitcode_data_size;
  OS.write((const char *)&ctrl, sizeof(metallib_header_control));

  // write entry headers/info
  for (const auto &entry : prog_info.entries) {
    entry.write_header(OS);
  }

  // write dummy ENDT
  // NOTE: no idea why this is needed and this is definitively not included by
  // the "programs_length",
  //       but iOS 12.0+ requires it, so just write it like this
  const auto dummy_ENDT = metallib_program_info::TAG_TYPE::END;
  OS.write((const char *)&dummy_ENDT, sizeof(metallib_program_info::TAG_TYPE));

  // write reflection data
  for (const auto &entry : prog_info.entries) {
    entry.write_reflection(OS);
  }

  // write debug data
  for (const auto &entry : prog_info.entries) {
    entry.write_debug(OS);
  }

  // write bitcode data
  for (const auto &entry : prog_info.entries) {
    entry.write_module(OS);
  }

  return false;
}

// Pin MetalTargetObjectFile's vtables to this file.
MetalTargetObjectFile::~MetalTargetObjectFile() {}

MCSection *MetalTargetObjectFile::SelectSectionForGlobal(
    const GlobalObject *GO, SectionKind Kind, const TargetMachine &TM) const {
  return getDataSection();
}

