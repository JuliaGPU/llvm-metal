//===- MetalLibWriterPass.cpp - Metal Library writing pass ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// MetalLibWriterPass implementation.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Triple.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "../Writer50/ValueEnumerator50.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/LibFloor/MetalTypes.h"
#include "sha256.hpp"
#define BZ_NO_STDIO 1
#include "bzip2/bzlib.h"
#include "tar/microtar.h"
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <random>
#include <fstream>
#include <sstream>
using namespace llvm;
using namespace std;
using namespace metal;

// workaround Windows stupidity ...
#if defined(uuid_t)
#undef uuid_t
#endif

PreservedAnalyses MetalLibWriterPass::run(Module &M, ModuleAnalysisManager &) {
  WriteMetalLibToFile(M, OS);
  return PreservedAnalyses::all();
}

namespace {
class WriteMetalLibPass : public ModulePass {
  raw_ostream &OS; // raw_ostream to print on

public:
  static char ID; // Pass identification, replacement for typeid
  explicit WriteMetalLibPass(raw_ostream &o) : ModulePass(ID), OS(o) {}

  StringRef getPassName() const override { return "Metal Library Writer"; }

  bool runOnModule(Module &M) override {
    WriteMetalLibToFile(M, OS);
    return false;
  }
};
} // namespace

char WriteMetalLibPass::ID = 0;

ModulePass *llvm::createMetalLibWriterPass(raw_ostream &Str) {
  return new WriteMetalLibPass(Str);
}

//
struct __attribute__((packed)) metallib_version {
  // container/file version
  uint16_t container_version_major : 15;
  uint16_t is_macos_target : 1;
  uint16_t container_version_minor;
  uint16_t container_version_bugfix;

  // flags
  uint8_t file_type : 7;
  uint8_t is_stub : 1;
  uint8_t platform : 7;
  uint8_t is_64_bit : 1;

  // platform version
  uint32_t platform_version_major : 16;
  uint32_t platform_version_minor : 8;
  uint32_t platform_version_update : 8;
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
};
static_assert(sizeof(metallib_header_control) == 64,
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
  enum class PROGRAM_TYPE : uint8_t {
    VERTEX = 0,
    FRAGMENT = 1,
    KERNEL = 2,
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

    sha256_hash hash;

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

    bool emit_debug_info{false};
    uint64_t debug_source_offset{0u};

    union tess_data_t {
      struct tess_info_t {
        uint8_t primitive_type : 2;      // 1 = triangle, 2 = quad
        uint8_t control_point_count : 6; // hardware limit is 32
      } info;
      uint8_t data;
    } tess;

    std::vector<vertex_attribute> vertex_attributes;

    // output in same order as Apple:
    //  * NAME
    //  * TYPE
    //  * HASH
    //  * OFFT
    //  * VERS
    //  * MDSZ
    //  * SOFF
    //  * ENDT
    void update_length() {
      length = 4;                     // length info itself
      length += 7 * sizeof(TAG_TYPE); // 7 tags
      length += 6 * sizeof(uint16_t); // tag lengths (except ENDT)
      if (emit_debug_info) {
        length += 1 * (sizeof(TAG_TYPE) + sizeof(uint16_t)); // SOFF tag
      }

      length += name.size() + 1;          // name length + \0
      length += 1;                        // type
      length += sizeof(sha256_hash);      // hash
      length += sizeof(offset_info);      // offset
      length += sizeof(version_info) * 2; // both versions
      length += sizeof(uint64_t);         // module size, always 8 bytes
      if (tess.data != 0u) {
        length += sizeof(TAG_TYPE); // TESS
        length += sizeof(uint16_t); // tag length
        length += sizeof(uint8_t);  // tessellation info
      }
      if (emit_debug_info) {
        length += sizeof(uint64_t); // SOFF, always 8 bytes
      }

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
      write_value(OS, uint16_t(sizeof(sha256_hash)));
      OS.write((const char *)&hash, sizeof(sha256_hash));

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

      if (tess.data != 0u) {
        // TESS
        write_value(OS, TAG_TYPE::TESS);
        write_value(OS, uint16_t(1u));
        write_value(OS, tess.data);
      }

      // MDSZ
      write_value(OS, TAG_TYPE::MD_SIZE);
      write_value(OS, uint16_t(sizeof(uint64_t)));
      write_value(OS, uint64_t(bitcode_data.size()));

      if (emit_debug_info) {
        // SOFF
        write_value(OS, TAG_TYPE::SOFF);
        write_value(OS, uint16_t(sizeof(uint64_t)));
        write_value(OS, debug_source_offset);
      }

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
static bool is_used_in_function(const Function *F, const GlobalVariable *GV) {
  for (const auto &user : GV->users()) {
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

static std::string make_abs_file_name(const std::string &file_name_in) {
  std::string file_name_out = file_name_in;
  SmallVector<char> file_name(file_name_out.size());
  file_name.assign(file_name_out.begin(), file_name_out.end());
  sys::fs::make_absolute(file_name);
  file_name_out.resize(file_name.size(), '\0');
  file_name_out.assign(file_name.begin(), file_name.end());
  return file_name_out;
}

struct tar_stream_t {
  static int read(void *stream, void *data, uint32_t size) {
    assert(false && "should not call this");
    return 0;
  }

  static int seek(void *stream, uint32_t offset) {
    assert(false && "should not call this");
    return MTAR_ESEEKFAIL;
  }

  static int write(void *stream, const void *data, uint32_t size) {
    auto &str = ((tar_stream_t *)stream)->str;
    str.write((const char *)data, size);
    return size;
  }

  static int close(void *stream) {
    (void)stream; // nop
    return MTAR_ESUCCESS;
  }

  static constexpr const mtar_ops_t ops{
      .read = &read,
      .write = &write,
      .seek = &seek,
      .close = &close,
  };

  std::stringstream str;
};
static std::string create_tar(
    const std::unordered_map<std::string /* abs path */,
                             std::unique_ptr<MemoryBuffer> /* data */> &files) {
  // create tar
  tar_stream_t tar_stream;
  mtar_t tar;
  mtar_init(&tar, MTAR_WRITE, &tar_stream_t::ops, &tar_stream);

  for (const auto &file : files) {
    const auto file_size = file.second->getBufferSize();
    mtar_write_file_header(&tar, file.first.c_str(), file_size);
    mtar_write_data(&tar, file.second->getBufferStart(), file_size);
    mtar_end_data(&tar);
  }

  mtar_finalize(&tar);
  mtar_close(&tar);

  return tar_stream.str.str();
}

static std::pair<std::unique_ptr<uint8_t[]>, uint32_t>
compress_source_archive(const void *archive_ptr, const uint32_t archive_size) {
  auto dst_len = archive_size + (archive_size / 50u) +
                 600u; // provision orig + 2% + advised 600 bytes
  auto dst = make_unique<uint8_t[]>(dst_len);
  const auto bz2_ret = BZ2_bzBuffToBuffCompress(
      (char *)dst.get(), &dst_len,
      const_cast<char *>((const char *)archive_ptr), archive_size, 6, 0, 30);
  if (bz2_ret != BZ_OK) {
    errs() << "failed to perform bz2 compression on source archive: " << bz2_ret
           << '\n';
    return {nullptr, 0};
  }
  return {move(dst), dst_len};
}

//
void llvm::WriteMetalLibToFile(Module &M, raw_ostream &OS) {
  // get metal version
  Triple TT(M.getTargetTriple());
  uint32_t target_air_version = 200;
  if (TT.isiOS()) {
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
  prog_info.entries.resize(function_count);

  // absolute source file name
  const std::string src_file_name = make_abs_file_name(M.getSourceFileName());
  const uint32_t src_file_name_length = src_file_name.length() + 1u /* \0 */;

  // if we're building with debug info, emit .metallib specific debug info
  // NOTE: for compat, only do this for Metal 2.4+
  const bool emit_debug_info =
      (M.debug_compile_units_begin() != M.debug_compile_units_end() &&
       target_air_version >= 240);

  // handle source files / archive creation
  std::pair<std::unique_ptr<uint8_t[]>, uint32_t> source_archive_data{nullptr,
                                                                      0};
  std::unordered_map<std::string /* abs path */,
                     std::unique_ptr<MemoryBuffer> /* source code */>
      source_files;
  std::string working_dir;
  std::string linker_cmd;
  std::string dependent_bc_file_name = src_file_name + ".air";
  if (emit_debug_info) {
    // * get the working directory
    if (auto working_dir_md = M.getNamedMetadata("llvm_utils.workingdir");
        working_dir_md) {
      if (working_dir_md->getNumOperands() >= 1) {
        auto md_node = working_dir_md->getOperand(0);
        if (md_node->getNumOperands() >= 1) {
          if (const auto working_dir_str_md =
                  dyn_cast_or_null<llvm::MDString>(md_node->getOperand(0));
              working_dir_str_md) {
            working_dir = working_dir_str_md->getString().str();
          }
        }
      }
    }

    // * gather all source files
    // * ensure that all DIFile metadata entries use absolute file names +
    //   directory is the working directory
    auto &Context = M.getContext();
    // somewhat overkill, but I don't know of a better way
    ValueEnumerator50 VE(M, false);
    for (const auto &md : VE.getMetadataMap()) {
      if (const DIFile *difile_node = dyn_cast_or_null<DIFile>(md.first);
          difile_node) {
        const auto orig_file_name = difile_node->getFilename().str();
        auto abs_file_name = make_abs_file_name(orig_file_name);
        source_files.emplace(abs_file_name, nullptr);
        auto mod_difile_node = const_cast<DIFile *>(difile_node);
        mod_difile_node->replaceOperandWith(
            0, llvm::MDString::get(Context, abs_file_name));
        const auto path_sep = abs_file_name.rfind('/');
        if (path_sep != std::string::npos) {
          abs_file_name.erase(path_sep, abs_file_name.size() - path_sep);
        }
        mod_difile_node->replaceOperandWith(
            1, llvm::MDString::get(Context, working_dir));
      }
    }

    // * read all source code (drop files that we can't read)
    // * emit code for all valid source files ("recompile_info")
    llvm::NamedMDNode *recompile_info =
        M.getOrInsertNamedMetadata("recompile_info");
    SmallVector<llvm::Metadata *, 8> recompile_info_list;
    for (auto src_iter = source_files.begin();
         src_iter != source_files.end();) {
      auto source = MemoryBuffer::getFile(src_iter->first, true /* is text */,
                                          true /* requires \0 */,
                                          false /* not volatile */);
      if (!source) {
        src_iter = source_files.erase(src_iter);
      } else {
        src_iter->second = std::move(*source);
        ++src_iter;
      }
    }
    for (const auto &source_file : source_files) {
      SmallVector<llvm::Metadata *, 2> recompile_info_file;
      recompile_info_file.push_back(
          llvm::MDString::get(Context, source_file.first));
      recompile_info_file.push_back(
          llvm::MDString::get(Context, source_file.second->getBuffer()));
      recompile_info_list.push_back(
          llvm::MDNode::get(Context, recompile_info_file));
    }
    recompile_info->addOperand(llvm::MDNode::get(Context, recompile_info_list));

    // * create additional source archive entries
    if (working_dir.empty()) {
      errs() << "no valid 'llvm_utils.workingdir' metadata entry!\n";
    } else {
      source_files.emplace("metal-working-dir.txt",
                           MemoryBuffer::getMemBuffer(working_dir));
    }

    StringRef cmd_line;
    if (auto cmd_line_md = M.getNamedMetadata("llvm.commandline");
        cmd_line_md) {
      if (cmd_line_md->getNumOperands() >= 1) {
        auto md_node = cmd_line_md->getOperand(0);
        if (md_node->getNumOperands() >= 1) {
          if (auto cmd_line_str_md =
                  dyn_cast_or_null<llvm::MDString>(md_node->getOperand(0));
              cmd_line_str_md) {
            cmd_line = cmd_line_str_md->getString();
          }
        }
      }
    }
    if (cmd_line.empty()) {
      errs() << "no valid 'llvm.commandline' metadata entry!\n";
    } else {
      source_files.emplace("metal-options.txt",
                           MemoryBuffer::getMemBuffer(cmd_line));
      linker_cmd = cmd_line;
    }

    source_files.emplace("original-input-filename.txt",
                         MemoryBuffer::getMemBuffer(dependent_bc_file_name));

    // * create .tar.bz2
    auto tar_data = create_tar(source_files);
#if 0
    {
      error_code ec;
      raw_fd_ostream tar_file(src_file_name + ".tar", ec, sys::fs::CreationDisposition::CD_CreateAlways);
      tar_file.write(tar_data.data(), tar_data.size());
    }
#endif
    source_archive_data =
        compress_source_archive(tar_data.data(), tar_data.size());
#if 0
    {
      error_code ec;
      raw_fd_ostream bz2_file(src_file_name + ".tar.bz2", ec, sys::fs::CreationDisposition::CD_CreateAlways);
      bz2_file.write((const char*)source_archive_data.first.get(), source_archive_data.second);
    }
#endif

    // * export the original complete bitcode file as well
    error_code ec;
    raw_fd_ostream dependent_bc_file(
        dependent_bc_file_name, ec,
        sys::fs::CreationDisposition::CD_CreateAlways);
    if (!ec) {
      WriteBitcode50ToFile(&M, dependent_bc_file);
      dependent_bc_file.flush();
    } else {
      errs() << "failed to write dependent debug file "
             << dependent_bc_file_name << "\n";
      dependent_bc_file_name = "";
    }
  }

  // we can now remove any metadata that we no longer need
  static constexpr array<const char *, 5> drop_mds{
      "llvm_utils.workingdir", "recompile_info",   "air.source_file_name",
      "llvm.linker.options",   "llvm.commandline",
  };
  for (const auto &drop_md : drop_mds) {
    if (auto md = M.getNamedMetadata(drop_md); md) {
      md->dropAllReferences();
      md->eraseFromParent();
    }
  }

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
    entry.emit_debug_info = emit_debug_info;

    // clone the module with the current entry point function and any global
    // vars that we need
    ValueToValueMapTy VMap;
    auto cloned_mod = CloneModule(M, VMap, [&func](const GlobalValue *GV) {
      if (GV == func) {
        return true;
      }
      // only clone global vars if they are needed in a specific function
      if (const GlobalVariable *GVar = dyn_cast<GlobalVariable>(GV)) {
        return is_used_in_function(func, GVar);
      }
      return false;
    });

    // metallib uses the function name as the source file name
    cloned_mod->setSourceFileName(func->getName());

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
          static const std::unordered_map<uint32_t, const char *>
              ident_versions{
                  {230, "Apple LLVM version 31001.143 (metalfe-31001.143)"},
                  {240, "Apple metal version 31001.363 (metalfe-31001.363)"},
              };
          ident_op->replaceOperandWith(
              0, llvm::MDString::get(cloned_mod->getContext(),
                                     ident_versions.at(target_air_version)));
        }
      }
    }
    // * kill other named metadata that is no longer needed
    for (auto nmd_iter = cloned_mod->named_metadata_begin();
         nmd_iter != cloned_mod->named_metadata_end();) {
      if (nmd_iter->getName().startswith("floor.")) {
        auto erase_nmd = &*nmd_iter++;
        cloned_mod->eraseNamedMetadata(erase_nmd);
      } else {
        ++nmd_iter;
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
          GV.setAlignment(MaybeAlign{16u});
        }
      }
      if (GV.getAddressSpace() == 3 /* local memory */) {
        // always use undef initializer (instead of zeroinitializer)
        GV.setInitializer(UndefValue::get(GV.getValueType()));
      }
    }

    // extract tessellation info
    if (auto vertex_md = cloned_mod->getNamedMetadata("air.vertex");
        vertex_md && vertex_md->getNumOperands() > 0) {
      // there should only be one vertex function entry
      assert(vertex_md->getNumOperands() == 1u);
      auto tess_func_md = vertex_md->getOperand(0);
      for (auto op_iter = tess_func_md->op_begin() + 1;
           op_iter != tess_func_md->op_end(); ++op_iter) {
        auto node = dyn_cast_or_null<MDNode>(*op_iter);
        if (!node) {
          continue;
        }
        if (auto first_md = dyn_cast_or_null<MDString>(node->getOperand(0));
            first_md && first_md->getString().equals("air.patch")) {
          auto prim_md = dyn_cast_or_null<MDString>(node->getOperand(1));
          if (!prim_md) {
            continue;
          }
          auto ctrl_pnts_md =
              dyn_cast_or_null<ConstantAsMetadata>(node->getOperand(3));
          if (!ctrl_pnts_md) {
            continue;
          }
          auto ctrl_pnts =
              dyn_cast_or_null<ConstantInt>(ctrl_pnts_md->getValue());
          if (!ctrl_pnts) {
            continue;
          }

          auto prim_str = prim_md->getString();
          if (prim_str.startswith("triangle")) {
            entry.tess.info.primitive_type = 1u;
          } else if (prim_str.startswith("quad")) {
            entry.tess.info.primitive_type = 2u;
          }
          entry.tess.info.control_point_count = min(
              (uint32_t)ctrl_pnts->getValue().getZExtValue(), 63u /* 6 bits */);
        } else if (auto ctrl_pnt_md =
                       dyn_cast_or_null<MDNode>(node->getOperand(0));
                   ctrl_pnt_md) {
          if (ctrl_pnt_md->getNumOperands() < 4) {
            continue;
          }
          // op #0: arg index (should be 0)
          // op #1: "air.patch_control_point_input"
          if (auto patch_ctrl_pnt_str =
                  dyn_cast_or_null<MDString>(ctrl_pnt_md->getOperand(1));
              !patch_ctrl_pnt_str || !patch_ctrl_pnt_str->getString().equals(
                                         "air.patch_control_point_input")) {
            continue;
          }
          // op #2: specifies the patch_control_point_function
          // ops #3 and higher specify all attributes
          for (auto attr_iter = ctrl_pnt_md->op_begin() + 3;
               attr_iter != ctrl_pnt_md->op_end(); ++attr_iter) {
            const auto attr_md = dyn_cast_or_null<MDNode>(*attr_iter);
            if (!attr_md || attr_md->getNumOperands() < 7) {
              continue;
            }
            auto attr_idx =
                dyn_cast_or_null<ConstantAsMetadata>(attr_md->getOperand(1));
            auto attr_type = dyn_cast_or_null<MDString>(attr_md->getOperand(4));
            auto attr_name = dyn_cast_or_null<MDString>(attr_md->getOperand(6));
            if (!attr_idx || !attr_type || !attr_name) {
              continue;
            }
            auto attr_idx_int =
                dyn_cast_or_null<ConstantInt>(attr_idx->getValue());
            if (!attr_idx_int) {
              continue;
            }
            vertex_attribute vattr{
                .name = attr_name->getString().str(),
                .index = (uint32_t)attr_idx_int->getZExtValue(),
                .type = data_type_from_string(attr_type->getString().str()),
                .use = VERTEX_USE::CONRTOL_POINT,
                .active = true, // always flag as active
            };
            if (vattr.type == DATA_TYPE::INVALID) {
              errs() << "invalid data type in control point vertex attribute: "
                     << vattr.name << ", index " << vattr.index << "\n";
              continue;
            }
            entry.vertex_attributes.emplace_back(std::move(vattr));
          }
        }
      }
    }

    // write module / bitcode
    raw_string_ostream bitcode_stream{entry.bitcode_data};
    WriteBitcode50ToFile(cloned_mod.get(), bitcode_stream);
    bitcode_stream.flush();

    // hash module
    entry.hash = compute_sha256_hash((const uint8_t *)entry.bitcode_data.data(),
                                     entry.bitcode_data.size());

    // write reflection and debug data (just ENDT right now)
    static const auto end_tag = TAG_TYPE::END;
    static const auto dbg_tag = TAG_TYPE::DEBI;
    static const auto dep_tag = TAG_TYPE::DEPF;
    static const uint32_t tag_length = sizeof(TAG_TYPE);

    raw_string_ostream refl_stream{entry.reflection_data};
    uint32_t refl_length = tag_length + sizeof(uint32_t);
    if (!entry.vertex_attributes.empty()) {
      const auto attr_count = (uint16_t)entry.vertex_attributes.size();
      refl_length += 2u * (tag_length + sizeof(uint16_t)); // VATT and VATY
      uint16_t vatt_len = sizeof(uint16_t) /* attr count */ +
                          2u * attr_count /* per-attr info */;
      uint16_t vaty_len =
          sizeof(uint16_t) /* attr count */ + attr_count /* per-attr type */;
      for (const auto &vattr : entry.vertex_attributes) {
        vatt_len += vattr.name.size() + 1u;
      }
      refl_length += vatt_len + vaty_len;
      refl_stream.write((const char *)&refl_length, sizeof(uint32_t));

      static const auto vatt_tag = TAG_TYPE::VATT;
      static const auto vaty_tag = TAG_TYPE::VATY;

      // VATT
      refl_stream.write((const char *)&vatt_tag, tag_length);
      refl_stream.write((const char *)&vatt_len, sizeof(vatt_len));
      refl_stream.write((const char *)&attr_count, sizeof(attr_count));
      for (const auto &vattr : entry.vertex_attributes) {
        refl_stream.write(vattr.name.c_str(), vattr.name.size());
        refl_stream.write('\0');

        uint16_t info = vattr.index & 0x1FFFu;
        info |= (uint16_t(vattr.use) & 0x3u) << 13u;
        info |= (vattr.active ? 0x8000u : 0u);
        refl_stream.write((const char *)&info, sizeof(info));
      }

      // VATY
      refl_stream.write((const char *)&vaty_tag, tag_length);
      refl_stream.write((const char *)&vaty_len, sizeof(vaty_len));
      refl_stream.write((const char *)&attr_count, sizeof(attr_count));
      for (const auto &vattr : entry.vertex_attributes) {
        refl_stream.write((const char *)&vattr.type, sizeof(DATA_TYPE));
      }
    } else {
      refl_stream.write((const char *)&refl_length, sizeof(uint32_t));
    }
    refl_stream.write((const char *)&end_tag, tag_length);
    refl_stream.flush();

    raw_string_ostream dbg_stream{entry.debug_data};
    if (emit_debug_info) {
      const uint32_t dep_length =
          (dependent_bc_file_name.empty()
               ? 0
               : tag_length + 2 + dependent_bc_file_name.size());
      const uint32_t dbg_length = 4u /* len */ + tag_length + 2 + 4 /* line */ +
                                  src_file_name_length + dep_length +
                                  tag_length;
      dbg_stream.write((const char *)&dbg_length, sizeof(dbg_length));
      dbg_stream.write((const char *)&dbg_tag, tag_length);
      const uint16_t src_file_info_len = uint16_t(4u + src_file_name_length);
      dbg_stream.write((const char *)&src_file_info_len,
                       sizeof(src_file_info_len));
      uint32_t function_line = 1;
      if (DISubprogram *sub_prog = func->getSubprogram(); sub_prog) {
        function_line = sub_prog->getLine();
      }
      dbg_stream.write((const char *)&function_line, sizeof(function_line));
      dbg_stream.write(src_file_name.c_str(), src_file_name_length - 1u);
      dbg_stream.write('\0');
      if (!dependent_bc_file_name.empty()) {
        dbg_stream.write((const char *)&dep_tag, tag_length);
        const uint16_t dep_info_length =
            uint16_t(dependent_bc_file_name.size() + 1 /* \0 */);
        dbg_stream.write((const char *)&dep_info_length,
                         sizeof(dep_info_length));
        dbg_stream.write(dependent_bc_file_name.c_str(), dep_info_length - 1u);
        dbg_stream.write('\0');
      }
    } else {
      const uint32_t dbg_length = 4u /* len */ + tag_length;
      dbg_stream.write((const char *)&dbg_length, sizeof(dbg_length));
    }
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

  metallib_version header{
      .container_version_major = 1,
      .is_macos_target = TT.isMacOSX(),
      .container_version_minor = 2,
      .container_version_bugfix = 6,
      .file_type = 0,    // always "execute"
      .is_stub = false,  // never stub
      .is_64_bit = true, // always 64-bit
  };
  uint32_t platform_major = 0, platform_minor = 0, platform_update = 0;
  if (TT.isMacOSX()) {
    header.platform = 1u;
    TT.getMacOSXVersion(platform_major, platform_minor, platform_update);
  } else if (TT.isiOS()) {
    header.platform = 2u;
    TT.getiOSVersion(platform_major, platform_minor, platform_update);
  } else if (TT.isTvOS()) {
    header.platform = 3u;
    TT.getiOSVersion(platform_major, platform_minor, platform_update);
  } else if (TT.isWatchOS()) {
    header.platform = 4u;
    TT.getWatchOSVersion(platform_major, platform_minor, platform_update);
  } else {
    header.platform = 0u;
  }
  header.platform_version_major = platform_major;
  header.platform_version_minor = platform_minor;
  header.platform_version_update = platform_update;

  OS.write((const char *)&header, sizeof(metallib_version));

  // file length
  uint64_t ext_program_md_size = sizeof(TAG_TYPE) /* ENDT */;
  if (target_air_version >= 240) {
    ext_program_md_size += (4 + 2 + 16) /* UUID */;
  }
  if (emit_debug_info) {
    ext_program_md_size += (4 + 2 + 16) /* HSRD */;
  }
  const uint32_t src_archive_header_length =
      (emit_debug_info
           ? (sizeof(uint32_t) /* count */ + (linker_cmd.size() + 1) +
              (working_dir.size() + 1) + sizeof(uint32_t) /* length */)
           : 0u);
  const uint32_t src_archive_length =
      (emit_debug_info
           ? (sizeof(TAG_TYPE) /* magic/tag */ +
              sizeof(uint32_t) /* archive length */ +
              sizeof(uint16_t) /* "0" */ + source_archive_data.second +
              sizeof(TAG_TYPE) /* end tag */)
           : 0u);
  const uint64_t file_length =
      (sizeof(metallib_header) + sizeof(uint32_t) /* #programs */ +
       entries_size + ext_program_md_size + reflection_data_size +
       debug_data_size + bitcode_data_size + src_archive_header_length +
       src_archive_length);
  OS.write((const char *)&file_length, sizeof(uint64_t));

  // header control
  ctrl.programs_offset = sizeof(metallib_header);
  ctrl.programs_length = entries_size;
  ctrl.reflection_offset = ctrl.programs_offset + sizeof(uint32_t) +
                           ctrl.programs_length + ext_program_md_size;
  ctrl.reflection_length = reflection_data_size;
  ctrl.debug_offset = ctrl.reflection_offset + ctrl.reflection_length;
  ctrl.debug_length = debug_data_size;
  ctrl.bitcode_offset = ctrl.debug_offset + ctrl.debug_length;
  ctrl.bitcode_length = bitcode_data_size;
  OS.write((const char *)&ctrl, sizeof(metallib_header_control));

  // write entry headers/info
  OS.write((const char *)&function_count, sizeof(function_count));
  for (auto &entry : prog_info.entries) {
    if (emit_debug_info) {
      entry.debug_source_offset = src_archive_header_length;
    }
    entry.write_header(OS);
  }

  // write additional program metadata
  {
    // write embedded source code archive metadata
    if (emit_debug_info) {
      const auto HSRD_tag = TAG_TYPE::HSRD;
      OS.write((const char *)&HSRD_tag, sizeof(TAG_TYPE));
      OS.write(0x10);
      OS.write(0x0);
      const uint64_t src_archives_offset =
          ctrl.bitcode_offset + bitcode_data_size;
      const uint64_t src_archives_length =
          (src_archive_header_length + src_archive_length);
      OS.write((const char *)&src_archives_offset, sizeof(uint64_t));
      OS.write((const char *)&src_archives_length, sizeof(uint64_t));
    }

    // write UUID for Metal 2.4+
    if (target_air_version >= 240) {
      // NOTE: Apple doesn't actually care about UUID variants and versions,
      // so just fill this with random, but still signal variant 1 + version 4
      random_device rd{};
      mt19937 gen{rd()};
      uniform_int_distribution<uint8_t> dist(0u, 0xFFu);
      raw_ostream::uuid_t program_uuid;
      for (auto &ch : program_uuid) {
        ch = dist(gen);
      }
      program_uuid[6] = (4u /* version */ << 4u) | (program_uuid[6] & 0x0Fu);
      program_uuid[8] = (0b10 /* variant */ << 6u) | (program_uuid[8] & 0x3Fu);

      // write
      const auto UUID_tag = TAG_TYPE::UUID;
      OS.write((const char *)&UUID_tag, sizeof(TAG_TYPE));
      OS.write(0x10);
      OS.write(0x0);
      OS.write((const char *)&program_uuid, sizeof(program_uuid));
    }

    // write ENDT
    // NOTE: this is not included by the "programs_length"
    const auto END_tag = TAG_TYPE::END;
    OS.write((const char *)&END_tag, sizeof(TAG_TYPE));
  }

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

  // write embedded source code archives
  if (emit_debug_info) {
    const uint32_t src_archive_count = 1;
    OS.write((const char *)&src_archive_count, sizeof(src_archive_count));

    // linker command + linker working dir (assume same as compiler)
    OS.write(linker_cmd.c_str(), linker_cmd.size());
    OS.write(0);
    OS.write(working_dir.c_str(), working_dir.size());
    OS.write(0);

    OS.write((const char *)&src_archive_length, sizeof(src_archive_length));

    const auto SARC_tag = TAG_TYPE::SARC;
    OS.write((const char *)&SARC_tag, sizeof(TAG_TYPE));

    const uint32_t source_archive_data_size =
        source_archive_data.second + sizeof(uint16_t);
    OS.write((const char *)&source_archive_data_size, sizeof(uint32_t));
    const char archive_number[] = "0";
    OS.write((const char *)&archive_number, 2);
    OS.write((const char *)source_archive_data.first.get(),
             source_archive_data.second);

    const auto END_tag = TAG_TYPE::END;
    OS.write((const char *)&END_tag, sizeof(TAG_TYPE));
  }
}
