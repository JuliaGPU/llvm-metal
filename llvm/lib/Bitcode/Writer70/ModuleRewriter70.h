//===-- Bitcode/Writer70/ModuleRewriter70.h - Rewrite IR -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class rewrites module contents to make them compatible with LLVM 7.0.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_BITCODE_WRITER70_MODULEREWRITER70_H
#define LLVM_LIB_BITCODE_WRITER70_MODULEREWRITER70_H

namespace llvm {

class Module;

class ModuleRewriter70 {
public:
  ModuleRewriter70(Module &M) : M(M) {}

  void run();

private:
    Module &M;

    bool removeFreeze();
};


} // End llvm namespace

#endif
