/*
 * Copyright 2021 Florian Ziesche
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
// LLVM compat/translator for the dxil-spirv CFG structurizer
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <cstdint>
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/Transforms/LibFloor/cfg/node.hpp"
#include "llvm/Transforms/LibFloor/cfg/ir.hpp"

namespace llvm {

class cfg_translator {
public:
  //! initializes/connects the CFG from/with an LLVM function
  cfg_translator(Function &F_, LLVMContext &ctx_, CFGNodePool &pool_);
  CFGNode *get_entry_block() const { return entry; }

  //! on CFG transform/structurization completion:
  //! translate any CFG changes back to LLVM IR
  //! since the entry block may change, "updated_entry_block" sets the new entry
  //! block
  void cfg_to_llvm_ir(CFGNode *updated_entry_block,
                      const bool add_merge_annotations);

protected:
  Function &F;
  Module &M;
  LLVMContext &ctx;
  CFGNodePool &pool;

  CFGNode *entry = nullptr;
  std::unordered_map<BasicBlock *, CFGNode *> bb_map;

  void run();
  void translate_bb(CFGNode &node);

  void add_or_update_terminator(CFGNode &node);

  CallInst *insert_merge_block_marker(BasicBlock *merge_block);
  CallInst *insert_continue_block_marker(BasicBlock *continue_block);
  void create_loop_merge(Instruction *insert_before, BasicBlock *bb_merge,
                         BasicBlock *bb_continue);
  void create_selection_merge(Instruction *insert_before,
                              BasicBlock *merge_block);
};

} // namespace llvm
