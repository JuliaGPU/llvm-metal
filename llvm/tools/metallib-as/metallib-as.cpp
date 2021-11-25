#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::init("-"));

static cl::opt<std::string> OutputFilename("o", cl::desc("Output filename"),
                                           cl::value_desc("filename"));

static cl::opt<std::string>
    TargetTriple("mtriple", cl::desc("Override target triple for module"));

static cl::opt<bool> NoVerify("disable-verify", cl::Hidden,
                              cl::desc("Do not verify input module"));

LLVM_ATTRIBUTE_NORETURN static void reportError(Twine Msg,
                                                StringRef Filename = "") {
  SmallString<256> Prefix;
  if (!Filename.empty()) {
    if (Filename == "-")
      Filename = "<stdin>";
    ("'" + Twine(Filename) + "': ").toStringRef(Prefix);
  }
  WithColor::error(errs(), "llc") << Prefix << Msg << "\n";
  exit(1);
}

static std::unique_ptr<ToolOutputFile> GetOutputStream(const char *TargetName,
                                                       Triple::OSType OS,
                                                       const char *ProgName) {
  // If we don't yet have an output filename, make one.
  if (OutputFilename.empty()) {
    if (InputFilename == "-")
      OutputFilename = "-";
    else {
      // If InputFilename ends in .bc or .ll, remove it.
      StringRef IFN = InputFilename;
      if (IFN.endswith(".bc") || IFN.endswith(".ll"))
        OutputFilename = std::string(IFN.drop_back(3));
      else
        OutputFilename = std::string(IFN);
      OutputFilename += ".metallib";
    }
  }

  // Open the file.
  std::error_code EC;
  sys::fs::OpenFlags OpenFlags = sys::fs::OF_None;
  auto FDOut = std::make_unique<ToolOutputFile>(OutputFilename, EC, OpenFlags);
  if (EC) {
    reportError(EC.message());
    return nullptr;
  }

  return FDOut;
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  LLVMContext Context;

  // initialize the Metal target
  LLVMInitializeMetalTarget();
  LLVMInitializeMetalTargetInfo();
  LLVMInitializeMetalTargetMC();

  cl::ParseCommandLineOptions(argc, argv, "metallib assembler\n");

  SMDiagnostic Err;

  // parse the module
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], WithColor::error(errs(), argv[0]));
    return 1;
  }
  if (!NoVerify && verifyModule(*M, &errs()))
    reportError("input module cannot be verified", InputFilename);

  // override the triple, if requested
  if (!TargetTriple.empty())
    M->setTargetTriple(TargetTriple);
  Triple TheTriple(M->getTargetTriple());

  // get the target
  std::string Error;
  const Target *TheTarget =
      TargetRegistry::lookupTarget(TheTriple.getTriple(), Error);
  if (!TheTarget) {
    WithColor::error(errs(), argv[0]) << Error;
    exit(1);
  }

  // create the target machine
  TargetOptions Options;
  std::unique_ptr<TargetMachine> Target =
      std::unique_ptr<TargetMachine>(TheTarget->createTargetMachine(
          TheTriple.getTriple(), "", "", Options, Reloc::Static));
  assert(Target && "Could not allocate target machine!");

  // Figure out where we are going to send the output.
  std::unique_ptr<ToolOutputFile> Out =
      GetOutputStream(TheTarget->getName(), TheTriple.getOS(), argv[0]);
  if (!Out)
    return 1;
  raw_pwrite_stream *OS = &Out->os();

  // Build up all of the passes that we want to do to the module.
  legacy::PassManager PM;

  // Add an appropriate TargetLibraryInfo pass for the module's triple.
  TargetLibraryInfoImpl TLII(TheTriple);
  PM.add(new TargetLibraryInfoWrapperPass(TLII));

  if (Target->addPassesToEmitFile(PM, *OS, nullptr, CGFT_ObjectFile,
                                  NoVerify)) {
    reportError("target does not support generation of this file type");
  }

  PM.run(*M);

  Out->keep();
  return 0;
}
