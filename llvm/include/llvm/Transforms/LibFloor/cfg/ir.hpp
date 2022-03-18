/*
 * Copyright 2019-2021 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
//==-----------------------------------------------------------------------===//
//
// dxil-spirv CFG structurizer adopted for LLVM use
// ref: https://github.com/HansKristian-Work/dxil-spirv
// @ 189cc855b471591763d9951d63e51c72649037ab
//
//===----------------------------------------------------------------------===//

#pragma once

#include <assert.h>
#include <initializer_list>
#include <stdint.h>
#include <vector>
#include <optional>
#include "llvm/IR/CFG.h"

// A simple IR representation which allows the CFGStructurizer to do some simple
// rewrites of blocks, PHI nodes in particular.

namespace llvm {
class ConstantInt;

enum class MergeType { None, Loop, Selection };

struct CFGNode;

struct MergeInfo {
  MergeType merge_type = MergeType::None;
  CFGNode *merge_block = nullptr;
  CFGNode *continue_block = nullptr;
};

struct IncomingValue {
  CFGNode *block = nullptr;
  Value *value = nullptr;
};

struct PHI {
  PHINode *phi = nullptr;
  std::vector<IncomingValue> incoming;
};

struct Terminator {
  enum class Type { Unreachable, Branch, Condition, Switch, Return, Kill };

  // NOTE: this may be nullptr for newly created conditions
  Instruction *terminator = nullptr;

  Type type = Type::Unreachable;

  // Branch
  CFGNode *direct_block = nullptr;

  // Conditional Branch and Switch
  Value *condition = nullptr;

  // Condition
  CFGNode *true_block = nullptr;
  CFGNode *false_block = nullptr;

  // Switch
  struct Case {
    CFGNode *node = nullptr;
    uint64_t global_order = 0;
    ConstantInt *value = nullptr;
  };
  std::vector<Case> cases;
  CFGNode *default_node = nullptr;

  // Return
  Value *return_value = nullptr;
};

struct IRBlock {
  std::vector<PHI> phi;
  std::vector<Instruction *> operations;
  MergeInfo merge_info;
  Terminator terminator;
};

} // namespace llvm
