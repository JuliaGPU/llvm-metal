//===-- BitWriter70.cpp -----------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm-c/BitWriter.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;


/*===-- Operations on modules ---------------------------------------------===*/

int LLVMWriteBitcode70ToFile(LLVMModuleRef M, const char *Path) {
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_None);

  if (EC)
    return -1;

  WriteBitcodeToFile(*unwrap(M), OS);
  return 0;
}

int LLVMWriteBitcode70ToFD(LLVMModuleRef M, int FD, int ShouldClose,
                         int Unbuffered) {
  raw_fd_ostream OS(FD, ShouldClose, Unbuffered);

  WriteBitcodeToFile(*unwrap(M), OS);
  return 0;
}

int LLVMWriteBitcode70ToFileHandle(LLVMModuleRef M, int FileHandle) {
  return LLVMWriteBitcode70ToFD(M, FileHandle, true, false);
}

LLVMMemoryBufferRef LLVMWriteBitcode70ToMemoryBuffer(LLVMModuleRef M) {
  std::string Data;
  raw_string_ostream OS(Data);

  WriteBitcodeToFile(*unwrap(M), OS);
  return wrap(MemoryBuffer::getMemBufferCopy(OS.str()).release());
}
