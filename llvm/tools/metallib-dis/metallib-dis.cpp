//===-- metallib-dis.cpp - The low-level MetalLib + LLVM disassembler -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This utility may be invoked in the following manner:
//  metallib-dis [options]            - Read MetalLib from stdin, write asm
//                                      to stdout
//  metallib-dis [options] x.metallib - Read MetalLib from the x.metallib file,
//                                      write asm to the x.txt file.
//  Options:
//      --help   - Output information about command line switches
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/LLVMContext.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/AssemblyAnnotationWriter.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/WithColor.h"
#include <system_error>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
using namespace llvm;
using namespace std;

static cl::opt<std::string>
InputFilename(cl::Positional, cl::desc("<input metallib>"), cl::init("-"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Override output filename"),
               cl::value_desc("filename"));

static cl::opt<bool>
Force("f", cl::desc("Enable binary output on terminals"));

static cl::opt<bool>
DontPrint("disable-output", cl::desc("Don't output the .txt file"), cl::Hidden);

static cl::opt<bool>
ShowAnnotations("show-annotations",
                cl::desc("Add informational comments to the LLVM IR output"));

static cl::opt<bool> PreserveAssemblyUseListOrder(
    "preserve-ll-uselistorder",
    cl::desc("Preserve use-list order when writing LLVM assembly."),
    cl::init(false), cl::Hidden);

//
static constexpr const char metallib_magic[4] { 'M', 'T', 'L', 'B' };

struct __attribute__((packed)) metallib_version {
	// version of the container (NOTE: per-program Metal/AIR version info is extra)
	uint32_t container_version_major : 8;
	uint32_t container_version_rev : 4; // lo
	uint32_t container_version_minor : 4; // hi
	
	// unknown: always 2, 0, 0
	// TODO: might be dwarf version?
	uint32_t unknown_version_major : 8;
	uint32_t unknown_version_rev : 4; // lo
	uint32_t unknown_version_minor : 4; // hi

	// unknown: always 2 (macOS), 3 (iOS), 5 (macOS since 10.16/11.0)
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
static_assert(sizeof(metallib_header_control) == 68, "invalid program info length");

struct __attribute__((packed)) metallib_header {
	const char magic[4]; // == metallib_magic
	const metallib_version version;
	const uint64_t file_length;
	const metallib_header_control header_control;
};
static_assert(sizeof(metallib_header) == 4 + sizeof(metallib_version) + sizeof(uint64_t) + sizeof(metallib_header_control), "invalid metallib header size");

struct metallib_program_info {
	uint32_t length; // including length itself
	
	// NOTE: tag types are always 32-bit
	// NOTE: tag types are always followed by a uint16_t that specifies the length of the tag data
#define make_tag_type(a, b, c, d) ((uint32_t(d) << 24u) | (uint32_t(c) << 16u) | (uint32_t(b) << 8u) | uint32_t(a))
	enum TAG_TYPE : uint32_t {
		// used in initial header section
		NAME		= make_tag_type('N', 'A', 'M', 'E'),
		TYPE		= make_tag_type('T', 'Y', 'P', 'E'),
		HASH		= make_tag_type('H', 'A', 'S', 'H'),
		MD_SIZE		= make_tag_type('M', 'D', 'S', 'Z'),
		OFFSET		= make_tag_type('O', 'F', 'F', 'T'),
		VERSION		= make_tag_type('V', 'E', 'R', 'S'),
		// used in reflection section
		CNST        = make_tag_type('C', 'N', 'S', 'T'),
		VATT        = make_tag_type('V', 'A', 'T', 'T'),
		VATY        = make_tag_type('V', 'A', 'T', 'Y'),
		RETR        = make_tag_type('R', 'E', 'T', 'R'),
		ARGR        = make_tag_type('A', 'R', 'G', 'R'),
		// used in debug section
		DEBI        = make_tag_type('D', 'E', 'B', 'I'),
		// TODO/TBD
		LAYR        = make_tag_type('L', 'A', 'Y', 'R'),
		TESS        = make_tag_type('T', 'E', 'S', 'S'),
		SOFF        = make_tag_type('S', 'O', 'F', 'F'),
		// generic end tag
		END         = make_tag_type('E', 'N', 'D', 'T'),
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
	
	struct offset_info {
		// NOTE: these are all relative offsets -> add to metallib_header_control offsets to get absolute offsets
		uint64_t reflection_offset;
		uint64_t debug_offset;
		uint64_t bitcode_offset;
	};
	
	struct entry {
		uint32_t length;
		string name; // NOTE: limited to 65536 - 1 ('\0')
		PROGRAM_TYPE type { PROGRAM_TYPE::NONE };
		uint8_t sha256_hash[32] {
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
			0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		};
		offset_info offset { 0, 0, 0 };
		uint64_t bitcode_size { 0 }; // always 8 bytes
		version_info metal_version { 0, 0, 0 };
		version_info metal_language_version { 0, 0, 0 };
		uint8_t tess_info { 0 };
		uint64_t soffset { 0 };
	};
	vector<entry> entries;
};

//
namespace {

static void printDebugLoc(const DebugLoc &DL, formatted_raw_ostream &OS) {
  OS << DL.getLine() << ":" << DL.getCol();
  if (DILocation *IDL = DL.getInlinedAt()) {
    OS << "@";
    printDebugLoc(IDL, OS);
  }
}
class CommentWriter : public AssemblyAnnotationWriter {
public:
  void emitFunctionAnnot(const Function *F,
                         formatted_raw_ostream &OS) override {
    OS << "; [#uses=" << F->getNumUses() << ']';  // Output # uses
    OS << '\n';
  }
  void printInfoComment(const Value &V, formatted_raw_ostream &OS) override {
    bool Padded = false;
    if (!V.getType()->isVoidTy()) {
      OS.PadToColumn(50);
      Padded = true;
      // Output # uses and type
      OS << "; [#uses=" << V.getNumUses() << " type=" << *V.getType() << "]";
    }
    if (const Instruction *I = dyn_cast<Instruction>(&V)) {
      if (const DebugLoc &DL = I->getDebugLoc()) {
        if (!Padded) {
          OS.PadToColumn(50);
          Padded = true;
          OS << ";";
        }
        OS << " [debug line = ";
        printDebugLoc(DL,OS);
        OS << "]";
      }
      if (const DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(I)) {
        if (!Padded) {
          OS.PadToColumn(50);
          OS << ";";
        }
        OS << " [debug variable = " << DDI->getVariable()->getName() << "]";
      }
      else if (const DbgValueInst *DVI = dyn_cast<DbgValueInst>(I)) {
        if (!Padded) {
          OS.PadToColumn(50);
          OS << ";";
        }
        OS << " [debug variable = " << DVI->getVariable()->getName() << "]";
      }
    }
  }
};

struct MetalLibDisDiagnosticHandler : public DiagnosticHandler {
  char *Prefix;
  MetalLibDisDiagnosticHandler(char *PrefixPtr) : Prefix(PrefixPtr) {}
  bool handleDiagnostics(const DiagnosticInfo &DI) override {
    raw_ostream &OS = errs();
    OS << Prefix << ": ";
    switch (DI.getSeverity()) {
      case DS_Error: WithColor::error(OS); break;
      case DS_Warning: WithColor::warning(OS); break;
      case DS_Remark: OS << "remark: "; break;
      case DS_Note: WithColor::note(OS); break;
    }

    DiagnosticPrinterRawOStream DP(OS);
    DI.print(DP);
    OS << '\n';

    if (DI.getSeverity() == DS_Error)
      exit(1);
    return true;
  }
};
} // end anon namespace

static Expected<bool> openInputFile(LLVMContext &Context, std::unique_ptr<ToolOutputFile>& Out) {
	auto& os = Out->os();
	
	//
	ErrorOr<std::unique_ptr<MemoryBuffer>> input_data = MemoryBuffer::getFileOrSTDIN(InputFilename);
	if (!input_data) {
		return errorCodeToError(input_data.getError());
	}
	const auto& buffer = (*input_data)->getBuffer();
	const auto& data = buffer.data();
	
	// sanity check
	if(buffer.size() < sizeof(metallib_header)) {
		return make_error<StringError>("invalid header size", inconvertibleErrorCode());
	}
	
	//
	const auto& header = *(const metallib_header*)data;
	if(memcmp(metallib_magic, header.magic, 4) != 0) {
		return make_error<StringError>("invalid magic", inconvertibleErrorCode());
	}
	
	// dump
	os << "[header]" << '\n';
	os << "container version: " << header.version.container_version_major << "." << header.version.container_version_minor << "." << header.version.container_version_rev << '\n';
	os << "unknown version: " << header.version.unknown_version_major << "." << header.version.unknown_version_minor << "." << header.version.unknown_version_rev << '\n';
	os << "unknown: " << header.version.unkown_version << '\n';
	os << "zero?: " << header.version.zero << '\n';
	os << "length: " << header.file_length << '\n';
	
	os << '\n';
	os << "programs_offset: " << header.header_control.programs_offset << '\n';
	os << "programs_length: " << header.header_control.programs_length << '\n';
	os << "reflection_offset: " << header.header_control.reflection_offset << '\n';
	os << "reflection_length: " << header.header_control.reflection_length << '\n';
	os << "debug_offset: " << header.header_control.debug_offset << '\n';
	os << "debug_length: " << header.header_control.debug_length << '\n';
	os << "bitcode_offset: " << header.header_control.bitcode_offset << '\n';
	os << "bitcode_length: " << header.header_control.bitcode_length << '\n';
	os << "program_count: " << header.header_control.program_count << '\n';
	
	// read programs info
	if(buffer.size() < header.header_control.programs_offset + header.header_control.programs_length + 4u) {
		return make_error<StringError>("invalid size", inconvertibleErrorCode());
	}
	
	metallib_program_info info;
	auto program_ptr = &data[header.header_control.programs_offset + 4];
	
	info.entries.resize(header.header_control.program_count);
	for(uint32_t i = 0; i < header.header_control.program_count; ++i) {
		auto& entry = info.entries[i];
		
		entry.length = *(const uint32_t*)program_ptr; program_ptr += 4;
		
		bool found_end_tag = false;
		while(!found_end_tag) {
			const auto tag = *(const metallib_program_info::TAG_TYPE*)program_ptr; program_ptr += 4;
			uint32_t tag_length = 0;
			if(tag != metallib_program_info::TAG_TYPE::END) {
				tag_length = *(const uint16_t*)program_ptr;
				program_ptr += 2;
				
				if(tag_length == 0) {
					return make_error<StringError>("tag " + to_string(uint32_t(tag)) + " should not be empty",
												   inconvertibleErrorCode());
				}
			}
			
			switch(tag) {
				case metallib_program_info::TAG_TYPE::NAME: {
					entry.name = string((const char*)program_ptr, tag_length - 1u);
					break;
				}
				case metallib_program_info::TAG_TYPE::TYPE: {
					entry.type = *(const metallib_program_info::PROGRAM_TYPE*)program_ptr;
					break;
				}
				case metallib_program_info::TAG_TYPE::HASH: {
					if(tag_length != 32) {
						return make_error<StringError>("invalid hash size: " + to_string(tag_length),
													   inconvertibleErrorCode());
					}
					memcpy(entry.sha256_hash, program_ptr, 32u);
					break;
				}
				case metallib_program_info::TAG_TYPE::OFFSET: {
					entry.offset = *(const metallib_program_info::offset_info*)program_ptr;
					break;
				}
				case metallib_program_info::TAG_TYPE::VERSION: {
					entry.metal_version = *(const metallib_program_info::version_info*)program_ptr;
					entry.metal_language_version = *((const metallib_program_info::version_info*)program_ptr + 1u);
					break;
				}
				case metallib_program_info::TAG_TYPE::MD_SIZE: {
					// TODO: this might not exist?
					entry.bitcode_size = *(const uint64_t*)program_ptr;
					break;
				}
				case metallib_program_info::TAG_TYPE::TESS: {
					entry.tess_info = *(const uint8_t*)program_ptr;
					break;
				}
				case metallib_program_info::TAG_TYPE::SOFF: {
					if(tag_length != 8) {
						return make_error<StringError>("invalid SOFF size: " + to_string(tag_length),
													   inconvertibleErrorCode());
					}
					entry.soffset = *(const uint64_t*)program_ptr;
					break;
				}
				case metallib_program_info::TAG_TYPE::END: {
					found_end_tag = true;
					break;
				}
				default:
					return make_error<StringError>("invalid tag: " + to_string((uint32_t)tag),
												   inconvertibleErrorCode());
			}
			program_ptr += tag_length;
		}
	}
	if(info.entries.size() != header.header_control.program_count) {
		return make_error<StringError>("invalid entry count", inconvertibleErrorCode());
	}
	
	//
	for(const auto& prog : info.entries) {
		os << '\n';
		os << "################################################################################\n";
		os << '\n';
		os << "[program]" << '\n';
		os << "\tname: " << prog.name << '\n';
		os << "\ttype: ";
		switch(prog.type) {
			case metallib_program_info::PROGRAM_TYPE::FRAGMENT:
				os << "fragment";
				break;
			case metallib_program_info::PROGRAM_TYPE::VERTEX:
				os << "vertex";
				break;
			case metallib_program_info::PROGRAM_TYPE::KERNEL:
				os << "kernel";
				break;
			case metallib_program_info::PROGRAM_TYPE::NONE:
				os << "NONE";
				break;
		}
		os << '\n';
		os << "\tversion: " << prog.metal_version.major << "." << prog.metal_version.minor << "." << prog.metal_version.rev << '\n';
		os << "\tlanguage: " << prog.metal_language_version.major << "." << prog.metal_language_version.minor << "." << prog.metal_language_version.rev << '\n';
		os << "\trel offsets (refl, dbg, bc): " << prog.offset.reflection_offset << ", " << prog.offset.debug_offset << ", " << prog.offset.bitcode_offset << '\n';
		os << "\tbitcode size: " << prog.bitcode_size << '\n';
		os << "\thash: ";
		
		stringstream hash_hex;
		hash_hex << hex << uppercase;
		for(uint32_t i = 0; i < 32; ++i) {
			if(prog.sha256_hash[i] < 0x10) {
				hash_hex << '0';
			}
			hash_hex << uint32_t(prog.sha256_hash[i]);
		}
		os << hash_hex.str() << '\n';
		os << "\ttess info: " << uint32_t(prog.tess_info) << '\n';
		os << "\tsoffset: " << uint32_t(prog.soffset) << '\n';
		
		// output LLVM IR
		// TODO: could use stringref?
		auto bc_mem = WritableMemoryBuffer::getNewUninitMemBuffer(prog.bitcode_size, "bc_module");
		const auto bc_offset = header.header_control.bitcode_offset + prog.offset.bitcode_offset;
		os << "\toffset: " << bc_offset << '\n';
		os << "\tsize: " << bc_mem->getBufferSize() << ", " << prog.bitcode_size << '\n';
		os << '\n';
		memcpy((char*)bc_mem->getBufferStart(), data + bc_offset, bc_mem->getBufferSize());
		
		auto bc_mod = parseBitcodeFile(*bc_mem, Context);
		if(bc_mod) {
			std::unique_ptr<AssemblyAnnotationWriter> Annotator;
			if (ShowAnnotations) {
				Annotator.reset(new CommentWriter());
			}
			
			if ((*bc_mod)->materializeAll()) {
				return make_error<StringError>("failed to materialize", inconvertibleErrorCode());
			}
			(*bc_mod)->print(Out->os(), Annotator.get(), PreserveAssemblyUseListOrder);
			
			if(Out->os().has_error()) {
				Out->os().clear_error();
			}
		} else {
			os << "bc parse error" << '\n';
			// TODO: better error handling
			handleAllErrors(bc_mod.takeError(), [&](ErrorInfoBase &EIB) {
				errs() << "bc module: ";
				EIB.log(errs());
				errs() << '\n';
			});
			return make_error<StringError>("failed to parse bitcode module", inconvertibleErrorCode());
		}
	}
	
	return true;
}

static ExitOnError ExitOnErr;

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  ExitOnErr.setBanner(std::string(argv[0]) + ": error: ");

  LLVMContext Context;
  Context.setDiagnosticHandler(
      std::make_unique<MetalLibDisDiagnosticHandler>(argv[0]));

  cl::ParseCommandLineOptions(argc, argv, ".metallib -> .txt disassembler\n");

  // Just use stdout.  We won't actually print anything on it.
  if (DontPrint)
    OutputFilename = "-";

  if (OutputFilename.empty()) { // Unspecified output, infer it.
    if (InputFilename == "-") {
      OutputFilename = "-";
    } else {
      StringRef IFN = InputFilename;
      OutputFilename = (IFN.endswith(".metallib") ? IFN.drop_back(9) : IFN).str();
      OutputFilename += ".txt";
    }
  }

  std::error_code EC;
  std::unique_ptr<ToolOutputFile> Out(new ToolOutputFile(OutputFilename, EC, sys::fs::OF_None));
  if (EC) {
    errs() << EC.message() << '\n';
    return -1;
  }

  Expected<bool> SuccessOrErr = openInputFile(Context, Out);
  if (!SuccessOrErr) {
    handleAllErrors(SuccessOrErr.takeError(), [&](ErrorInfoBase &EIB) {
      errs() << argv[0] << ": ";
      EIB.log(errs());
      errs() << '\n';
    });
    return 1;
  }

  // Declare success.
  Out->keep();

  return 0;
}
