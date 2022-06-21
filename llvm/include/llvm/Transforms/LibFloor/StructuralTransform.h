//===- StructuralTransform.h - --------------------------------------------===//
//
// Copyright (c) 2015, Computer Architecture and Systems Laboratory at Georgia Tech
// Copyright (c) 2016 - 2017, Florian Ziesche (LLVM port + general fixes/cleanup)
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of gpuocelot nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//===----------------------------------------------------------------------===//
// \author  Haicheng Wu <hwu36@gatech.edu>
// \date    Monday April 4, 2011
// \brief   The header file for the StructuralTransform pass.
//===----------------------------------------------------------------------===//
//
// This file implements an Structural Transform based on Zhang's paper
//
// ref: "Using Hammock Graphs to Structure Programs",
// Fubo Zhang and Erik H. D’Hollander
// -> https://biblio.ugent.be/publication/291746/file/451220
//
// ref: "Characterization and Transformation of Unstructured Control Flow in GPU
// Applications", Haicheng Wu, Gregory Diamos, Si Li, and Sudhakar Yalamanchili
// -> http://www.gdiamos.net/papers/caches-paper.pdf
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_LIBFLOOR_STRUCTURALTRANSFORM_H
#define LLVM_TRANSFORMS_LIBFLOOR_STRUCTURALTRANSFORM_H

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include "StructuralAnalysis.h"

#include <list>
#include <unordered_map>

namespace llvm {

/// StructuralTransform - This class holds all the methods and data structures
class StructuralTransform {
public:
  typedef StructuralAnalysis::BBVecTy BBVecTy;
  typedef StructuralAnalysis::NodeTy NodeTy;

public:
  bool transform(Function &F);

private:
  struct BBMapper {
    std::vector<std::pair<BasicBlock *, BasicBlock *>> bb_map;

    BasicBlock *&operator[](const BasicBlock *key) {
      for (auto &elem : bb_map) {
        if (elem.first == key) {
          return elem.second;
        }
      }
      bb_map.emplace_back(const_cast<BasicBlock*>(key), (BasicBlock *)nullptr);
      return bb_map.back().second;
    }

    auto begin() { return bb_map.begin(); }

    auto cbegin() const { return bb_map.cbegin(); }

    auto end() { return bb_map.end(); }

    auto cend() const { return bb_map.cend(); }

    auto find(const BasicBlock *bb) const {
      for (auto iter = bb_map.begin(); iter != bb_map.end(); ++iter) {
        if (iter->first == bb) {
          return iter;
        }
      }
      return bb_map.end();
    }

    auto find_value(const BasicBlock *bb) const {
      for (auto iter = bb_map.begin(); iter != bb_map.end(); ++iter) {
        if (iter->second == bb) {
          return iter;
        }
      }
      return bb_map.end();
    }
  };
  BBMapper ClonedBBMap;
  std::unordered_map<BasicBlock *, std::unique_ptr<ValueToValueMapTy>>
      ClonedVMap;

  // alloca insertion point
  Instruction *alloca_insert{nullptr};

  // insertion point for condition initialization
  Instruction *condition_init_insert{nullptr};

  // type used when creating new conditions (i1)
  llvm::Type *condition_type{nullptr};

  Function *_function;

  // Algorithm 2 of Zhang's paper -- elimination of outgoing branches
  bool Cut(NodeTy *N);

  // Algorithm 3 of Zhang's paper -- elimination of backward branches
  bool BackwardCopy(NodeTy *N);

  // Algorithm 4 of Zhang's paper -- elimination of Forward branches
  bool ForwardCopy(NodeTy *N, uint32_t level = 0);

  bool stopCut;

  StructuralAnalysis SA;

  /// Get iterator to the basic block in the cfg
  const BasicBlock *bb(NodeTy *node) const;

  typedef std::list<const NodeTy *> NodeListTy;

  /// Get the children (returns an ordered container)
  const NodeListTy &children(const NodeTy *node) const;

  /// Get condition node from IfThen and IfThenElse.
  /// The last instruction of the condition node should be a branch.
  const NodeTy *cond(const NodeTy *node) const;
  /// Get if-true node from IfThen and IfThenElse
  const NodeTy *ifTrue(const NodeTy *node) const;
  /// Get if-false node from IfThenElse
  const NodeTy *ifFalse(const NodeTy *node) const;
};
}

#endif
