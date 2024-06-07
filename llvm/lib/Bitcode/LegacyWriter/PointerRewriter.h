//===-- Bitcode/LegacyWriter/PointerRewriter.h - Rewrite pointers -*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class supports writing opaque pointers in typed IR.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_BITCODE_LEGACYWRITER_POINTERREWRITER_H
#define LLVM_LIB_BITCODE_LEGACYWRITER_POINTERREWRITER_H

#include "llvm/ADT/DenseMap.h"

namespace llvm {

class Value;
class Module;
class TypedPointerType;

using PointerTypeMap = DenseMap<const Value *, TypedPointerType *>;

class PointerRewriter {
public:
  PointerRewriter(Module &M) : M(M) {}

  bool run();

  static PointerTypeMap buildPointerMap(const Module &M);

private:
  Module &M;
};


} // End llvm namespace

#endif
