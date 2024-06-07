//===-- Bitcode/LegacyWriter/ModuleRewriter70.h - Rewrite IR ----*- C++ -*-===//
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

#ifndef LLVM_LIB_BITCODE_LEGACYWRITER_MODULEREWRITER70_H
#define LLVM_LIB_BITCODE_LEGACYWRITER_MODULEREWRITER70_H

namespace llvm {

class Module;

class ModuleRewriter70 {
public:
  ModuleRewriter70(Module &M) : M(M) {}

  bool run();

private:
  Module &M;
};


} // End llvm namespace

#endif
