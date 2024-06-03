//===-- Bitcode/Writer50/ModuleRewriter50.h - Rewrite IR -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class rewrites module contents to make them compatible with LLVM 5.0.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_BITCODE_WRITER50_MODULEREWRITER50_H
#define LLVM_LIB_BITCODE_WRITER50_MODULEREWRITER50_H

namespace llvm {

class Module;

class ModuleRewriter50 {
public:
  ModuleRewriter50(Module &M) : M(M) {}

  void run();

private:
    Module &M;

    bool removeFreeze();
    bool convertPointers();
};


} // End llvm namespace

#endif
