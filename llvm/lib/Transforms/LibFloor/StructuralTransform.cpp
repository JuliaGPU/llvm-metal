//===- StructuralTransform.cpp - ------------------------------------------===//
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
// \brief   The source file for the StructuralTransform pass.
//===----------------------------------------------------------------------===//
//
// This file implements an Structural Transform based on Zhang's paper
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/LibFloor/StructuralTransform.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/IR/Verifier.h"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

#define report(x) DBG(errs() << x << "\n"; errs().flush();)

namespace llvm {

typedef StructuralAnalysis SA;

// Algorithm 2 of Zhang's paper -- elimination of outgoing branches
bool StructuralTransform::Cut(NodeTy *N) {
  report("Applying cut transform");
  bool change = false;

  for (auto *child : N->childNode) {
    change |= Cut(child);
  }

  if (!stopCut) {
    if (N->isCombined &&
        (N->nodeType == SA::NaturalLoop || N->nodeType == SA::SelfLoop) &&
        N->containedBB.size() > 1 && !N->outgoingBR.empty()) {
      change = true;

      BasicBlock *TopExitBB = N->exitBB;

      for (auto &edge : N->outgoingBR) {
        // 1. Before loop, insert fp = false
        auto fp = new AllocaInst(condition_type, 0, nullptr, "fp", alloca_insert);

        // TODO: "before loop" doesn't equal "in the entry block", or does it?
        // what if it is contained by another loop?
        new StoreInst(ConstantInt::get(condition_type, 0), fp,
                      condition_init_insert);

        // 2. replace branch to targe t by if (B) then {fp = true; exit}
        new StoreInst(ConstantInt::get(condition_type, 1), fp,
                      edge.first->getFirstNonPHI());

        for (BasicBlock *succ : successors(edge.first)) {
          if (!SA::containsBB(N->containedBB, succ)) {
            continue;
          }

          new StoreInst(ConstantInt::get(condition_type, 0), fp,
                        succ->getFirstNonPHI());
        }

        // 3. After loop, insert if (fp) goto t
        BasicBlock *NewCmpBB = BasicBlock::Create(
            _function->getContext(), "NewCmpBB", _function, nullptr);

        // ValueMap
        BBMapper ValueMap;
        ValueMap[edge.second] = NewCmpBB;

        if (TopExitBB != nullptr) {
          ValueMap[TopExitBB] = NewCmpBB;
        }

        // For each BasicBlock
        // Remap Values here for Instructions
        for (BasicBlock *BB : N->containedBB) {
          // TODO: handle switches
          assert(!isa<ReturnInst>(BB->getTerminator()) &&
                 "terminator can not be a return instruction");
          BranchInst *branch = dyn_cast<BranchInst>(BB->getTerminator());
          assert(branch != nullptr &&
                 "terminator must be a branch instruction");

          if (branch->isUnconditional()) {
            auto it = ValueMap.find(branch->getSuccessor(0));

            // found in ValueMap
            if (it != ValueMap.end()) {
              DBG(errs() << "changing successor of " << BB->getName()
                         << " from " << branch->getSuccessor(0)->getName()
                         << " to " << it->second->getName() << "\n";)
              branch->setSuccessor(0, it->second);
            }
          } else if (branch->isConditional()) {
            auto it1 = ValueMap.find(branch->getSuccessor(0));
            auto it2 = ValueMap.find(branch->getSuccessor(1));

            const bool found1 = (it1 != ValueMap.end());
            const bool found2 = (it2 != ValueMap.end());

            // both found in ValueMap
            if (found1 && found2) {
              branch->eraseFromParent();
              BranchInst::Create(it1->second, BB);
            }
            // edge 1 is found in ValueMap & edge 2 is not
            else if (found1 && !found2) {
              branch->setSuccessor(0, it1->second);
            }
            // edge 2 is found in ValueMap & edge 1 is not
            else if (!found1 && found2) {
              branch->setSuccessor(1, it2->second);
            }
          }
        }

        if (TopExitBB != nullptr) {
          const auto insert_point = NewCmpBB->getFirstNonPHI();
          auto rhs = ConstantInt::get(condition_type, 1);
          Value *cmp = nullptr;
          if (insert_point != nullptr) {
            auto lhs = new LoadInst(condition_type, fp, "fp_load", false, insert_point);
            cmp = new ICmpInst(insert_point, CmpInst::ICMP_EQ, lhs, rhs,
                               "fp_cmp");
          } else {
            // no instructions in bb yet -> insert at BB end
            auto lhs = new LoadInst(condition_type, fp, "fp_load", false, NewCmpBB);
            cmp = new ICmpInst(*NewCmpBB, CmpInst::ICMP_EQ, lhs, rhs, "fp_cmp");
          }
          BranchInst::Create(edge.second, TopExitBB, cmp, NewCmpBB);
        } else {
          BranchInst::Create(edge.second, NewCmpBB);
        }

        TopExitBB = NewCmpBB;
      }

      if (N->exitBB == nullptr) {
        // TODO: can this be an unconditional? also: handle switch?
        BranchInst *branch = dyn_cast<BranchInst>(N->entryBB->getTerminator());
        assert(branch != nullptr && branch->isUnconditional() &&
               "invalid branch in entryBB");
        branch->setSuccessor(0, TopExitBB);
      }

      stopCut = true;
      return change;
    }
  }

  return change;
}

// Algorithm 3 of Zhang's paper -- elimination of backward branches
bool StructuralTransform::BackwardCopy(NodeTy *N) {
  // report("Applying backward copy");
  assert(false && "BackwardCopy not implemented yet");
  // TODO: !
  return false;
}

// Algorithm 4 of Zhang's paper -- elimination of Forward branches
bool StructuralTransform::ForwardCopy(NodeTy *N, uint32_t level) {
  DBG(errs() << "Applying forward copy: " << std::string(level, ' ')
             << SA.findEntryBB(N)->getName() << "\n";)
  for (auto *child : N->childNode) {
    // we can only safely apply one forward copy, so abort if this has happened
    if (ForwardCopy(child, level + 1)) {
      return true;
    }
  }

  if (!N->incomingForwardBR.empty()) {
    DBG(errs() << ">> @" << SA.findEntryBB(N)->getName() << ": needs forward copy\n";)
    for (auto &edge : N->incomingForwardBR) {
      BBMapper BBMap; // local

      // Clone BasicBlocks to the new function
      for (BasicBlock *BB : N->containedBB) {
        auto vmap = std::make_unique<ValueToValueMapTy>();
        BasicBlock *ClonedBB = CloneBasicBlock(BB, *vmap.get(), ".scfg.cloned",
                                               _function, nullptr);

        // NOTE: instructions will be remapped later

        const auto vmap_iter = ClonedVMap.find(ClonedBB);
        if (vmap_iter == ClonedVMap.end()) {
          ClonedVMap[ClonedBB] = std::move(vmap);
        } else {
          // can this ever occur?
          errs() << "\033[31m MARK: did occur \033[m\n";
          auto insert_vmap_ptr = vmap_iter->second.get();
          for (const auto &elem : *vmap.get()) {
            insert_vmap_ptr->insert(elem);
          }
        }
        BBMap[BB] = ClonedBB;
      }

      // update global map
      for (auto &bb_entry : BBMap) {
        ClonedBBMap[bb_entry.first] = bb_entry.second;
      }

      // general value remapping function (handling remappings across bbs)
      const auto map_value = [this, &BBMap](
          Value *value, const ValueToValueMapTy *direct_vmap) -> Value * {
        // not mapping BBs, constants or function arguments
        if (isa<BasicBlock>(value) || isa<Constant>(value) ||
            isa<Argument>(value)) {
          return value;
        }

        const auto direct_iter = direct_vmap->find(value);
        // direct BB mapping, i.e. instruction is defined in this BB
        if (direct_iter != direct_vmap->end()) {
          DBG(errs() << "\033[32m direct mapping: " << *value << " -> "
                     << *direct_iter->second << " \033[m\n";)
          return direct_iter->second;
        }
        // indirect BB mapping, i.e. instruction is defined in another cloned BB
        else if (Instruction *instr = dyn_cast<Instruction>(value)) {
          const auto orig_par_BB = instr->getParent();
          const auto orig_par_BB_iter = BBMap.find(orig_par_BB);
          if (orig_par_BB_iter != BBMap.end()) {
            // map it to the cloned BB
            const auto par_vmap_iter =
                ClonedVMap.find(orig_par_BB_iter->second);
            if (par_vmap_iter != ClonedVMap.end()) {
              const auto mapped_iter = par_vmap_iter->second->find(instr);
              if (mapped_iter != par_vmap_iter->second->end()) {
                // found it
                DBG(errs() << "\033[32m mapping instr from other BB: " << *instr
                           << " -> " << *mapped_iter->second << " \033[m\n";)
                return mapped_iter->second;
              }
            }
          }
        }
        DBG(errs() << "\033[31m can't map: " << *value << "\033[m\n";)
        return value;
      };

      // replace instruction uses of instructions that were cloned in the cloned
      // block (CloneBasicBlock will only clone the "lhs", not the uses/operands
      // inside instructions)
      // note that we also need to replace/map instructions that come from other
      // BBs that were also cloned
      // -> need to do this after cloning everything and having created the
      // value/bb maps for everything
      for (BasicBlock *BB : N->containedBB) {
        const auto cloned_bb_iter = BBMap.find(BB);
        if (cloned_bb_iter == BBMap.end()) {
          assert(false && "failed to find cloned BB");
        }
        auto ClonedBB = cloned_bb_iter->second;

        const auto vmap_iter = ClonedVMap.find(ClonedBB);
        if (vmap_iter == ClonedVMap.end()) {
          assert(false && "failed to find vmap for cloned BB");
        }
        const auto vmap = vmap_iter->second.get();

        for (Instruction &instr : *ClonedBB) {
          for (uint32_t i = 0, count = instr.getNumOperands(); i < count; ++i) {
            instr.setOperand(i, map_value(instr.getOperand(i), vmap));
          }
        }
      }

      // For each BasicBlock
      // Remap Values here for Instructions
      for (auto &bb_entry : BBMap) {
        // nothing to do here
        if (isa<ReturnInst>(bb_entry.first->getTerminator())) {
          continue;
        }

        BranchInst *branch =
            dyn_cast<BranchInst>(bb_entry.first->getTerminator());
        assert(branch != nullptr && "terminator must be a branch instruction");
        BranchInst *sec_branch =
            dyn_cast<BranchInst>(bb_entry.second->getTerminator());
        assert(sec_branch != nullptr && "invalid branch");

        if (branch->isUnconditional()) {
          auto it = BBMap.find(branch->getSuccessor(0));

          BasicBlock *new_sec_branch_target = nullptr;
          // found in BBMap
          if (it != BBMap.end()) {
            new_sec_branch_target = it->second;
          }
          // not found in BBMap
          else {
            new_sec_branch_target = branch->getSuccessor(0);
          }

          // transform conditional branch to an unconditional one
          if (sec_branch->isConditional()) {
            BranchInst::Create(new_sec_branch_target, sec_branch);
            sec_branch->eraseFromParent();
          }
          // if already unconditional, simply set the new target
          else {
            sec_branch->setSuccessor(0, new_sec_branch_target);
          }
        } else if (branch->isConditional()) {
          auto it1 = BBMap.find(branch->getSuccessor(0));
          auto it2 = BBMap.find(branch->getSuccessor(1));

          // both found in BBMap
          if (it1 != BBMap.end() && it2 != BBMap.end()) {
            sec_branch->setSuccessor(0, it1->second);
            sec_branch->setSuccessor(1, it2->second);
          }
          // edge 1 is found in BBMap & edge 2 is not
          else if (it1 != BBMap.end() && it2 == BBMap.end()) {
            sec_branch->setSuccessor(0, it1->second);
            sec_branch->setSuccessor(1, branch->getSuccessor(1));
          }
          // edge 2 is found in BBMap & edge 1 is not
          else if (it1 == BBMap.end() && it2 != BBMap.end()) {
// TODO: correct order? or reverse here?
#if 1
            sec_branch->setSuccessor(0, it2->second);
            sec_branch->setSuccessor(1, branch->getSuccessor(0));
#else
            sec_branch->setSuccessor(0, branch->getSuccessor(0));
            sec_branch->setSuccessor(1, it2->second);
#endif
          }
          // neither is in BBMap
          else if (it1 == BBMap.end() && it2 == BBMap.end()) {
            sec_branch->setSuccessor(0, branch->getSuccessor(0));
            sec_branch->setSuccessor(1, branch->getSuccessor(1));
          }
        }
      }

      BranchInst *branch = dyn_cast<BranchInst>(edge.first->getTerminator());
      BasicBlock *newDst = BBMap[edge.second];
      assert(branch != nullptr && "terminator must be a branch instruction");
      if (branch->getSuccessor(0) == edge.second) {
        branch->setSuccessor(0, newDst);
      } else if (branch->isConditional() &&
                 branch->getSuccessor(1) == edge.second) {
        branch->setSuccessor(1, newDst);
      } else {
        assert(false && "initial edge doesn't point to edge target");
      }

      // update phi nodes of cloned blocks + successors preds / phi nodes
      for (auto &bb_entry : BBMap) {
        BasicBlock *orig = bb_entry.first;
        BasicBlock *clone = bb_entry.second;
        const auto update_phis = [this, &map_value](BasicBlock *bb,
                                                    BasicBlock *orig_bb) {
          // update incoming
          std::unordered_set<BasicBlock *> preds;
          for (BasicBlock *pred : predecessors(bb)) {
            preds.emplace(pred);
          }
          for (Instruction *instr = &bb->front(),
                           *next_instr = instr->getNextNode(),
                           *non_phi = bb->getFirstNonPHI();
               instr != non_phi;
               instr = next_instr, next_instr = next_instr->getNextNode()) {
            if (PHINode *phi = dyn_cast<PHINode>(instr)) {
              std::unordered_set<BasicBlock *> rem_bbs;
              std::unordered_set<BasicBlock *> existing_bbs;
              for (BasicBlock *bb_in : phi->blocks()) {
                existing_bbs.emplace(bb_in);
                if (preds.count(bb_in) == 0) {
                  rem_bbs.emplace(bb_in);
                }
              }

              // add incoming phi edges for non-existing preds (map based on
              // original BB value -> clone vmap)
              for (BasicBlock *pred : preds) {
                if (existing_bbs.count(pred) == 0) {
                  // find original bb
                  const auto orig_bb_iter = ClonedBBMap.find_value(pred);
                  if (orig_bb_iter == ClonedBBMap.end()) {
                    assert(false && "no original BB for cloned BB!");
                  }

                  const auto bb_index =
                      phi->getBasicBlockIndex(orig_bb_iter->first);
                  assert(bb_index != -1 &&
                         "original BB not part of this phi node?");

                  const auto orig_val = phi->getIncomingValue(bb_index);
                  auto mapped_value =
                      map_value(orig_val, ClonedVMap[pred].get());

                  // add new incoming edge
                  phi->addIncoming(mapped_value, pred);
                }
              }

              // remove incoming BB values from BBs that no longer point to this
              for (BasicBlock *rem_bb : rem_bbs) {
                phi->removeIncomingValue(rem_bb);
              }
            }
          }
        };
        update_phis(orig, nullptr);
        update_phis(clone, orig);
      }

      // update outgoing / successors
      for (auto &bb_entry : BBMap) {
        BasicBlock *orig_bb = bb_entry.first;
        BasicBlock *bb = bb_entry.second;

        for (BasicBlock *succ : successors(bb)) {
          if (BBMap.find_value(succ) != BBMap.end()) {
            // already handled
            continue;
          }

          for (Instruction *instr = &succ->front(),
                           *non_phi = succ->getFirstNonPHI();
               instr != non_phi; instr = instr->getNextNode()) {
            if (PHINode *phi = dyn_cast<PHINode>(instr)) {
              // add new incoming
              const auto orig_val = phi->getIncomingValueForBlock(orig_bb);
              assert(orig_val != nullptr &&
                     "original block was not using this phi/block?");
              auto mapped_value = map_value(orig_val, ClonedVMap[bb].get());

              // add new incoming edge
              phi->addIncoming(mapped_value, bb);
            }
          }
        }
      }

      // cleanup / optimize
      for (auto &bb_entry : BBMap) {
        DeleteDeadPHIs(bb_entry.first);
        DeleteDeadPHIs(bb_entry.second);

        if (bb_entry.first->getUniquePredecessor() != nullptr) {
          FoldSingleEntryPHINodes(bb_entry.first);
        }
        if (bb_entry.second->getUniquePredecessor() != nullptr) {
          FoldSingleEntryPHINodes(bb_entry.second);
        }
      }

#if 0
      static uint32_t transform_idx = 0;
      errs() << "## dumping #" << transform_idx << " ...\n";
      StructuralAnalysis::dumpIR("func_transform_" +
                                     std::to_string(transform_idx) + ".ll",
                                 *_function);
      StructuralAnalysis::dumpCFGDot("func_transform_" +
                                         std::to_string(transform_idx) + ".dot",
                                     *_function);
      ++transform_idx;

      errs() << "verifying ...\n";
      assert(!verifyFunction(*_function, &errs()) && "verification failed");
      errs() << "verifying done\n";
#endif
    }

    return true; // applied forward copy
  }

  return false; // nothing happened
}

bool StructuralTransform::transform(Function &F) {
  _function = &F;

  alloca_insert = &F.getEntryBlock().front();
  condition_init_insert = F.getEntryBlock().getTerminator();

  // TODO: can we merge multiple conditions to a larger integer and then use a
  // switch instead?
  if (condition_type == nullptr) {
    condition_type = llvm::Type::getInt1Ty(F.getContext());
  }

  SA.unstructuredBRVec.clear();
  SA.unreachableNodeSet.clear();
  SA.Net.clear();
  SA.analyze(F);

  DBG(SA.write(errs());)

  bool was_modified = false;
  while (!SA.unstructuredBRVec.empty()) {
    NodeTy *entry = *(SA.Net.begin());

    stopCut = false;

    for (NodeTy *node : SA.unreachableNodeSet) {
      if (Cut(node)) {
        was_modified = true;
        goto ANALYSIS;
      }
    }

    if (ForwardCopy(entry)) {
      was_modified = true;
      goto ANALYSIS;
    }

    if (Cut(entry)) {
      was_modified = true;
      goto ANALYSIS;
    }

    if (BackwardCopy(entry)) {
      was_modified = true;
      goto ANALYSIS;
    }

  ANALYSIS:
    DBG(errs() << "#### NEW ANALYSIS ####\n";)
    SA.unstructuredBRVec.clear();
    SA.unreachableNodeSet.clear();
    SA.Net.clear();
    SA.analyze(*_function);
  }

  SA.Net.clear();
  // SA.analyze(*_function); // TODO: needed?

  return was_modified;
}

const BasicBlock *StructuralTransform::bb(NodeTy *node) const {
  return (node->isCombined ? nullptr : node->BB);
}

const StructuralTransform::NodeListTy &
StructuralTransform::children(const NodeTy *node) const {
  NodeListTy *nList = new NodeListTy;
  NodeTy *tmp = nullptr;

  switch (node->nodeType) {
  case SA::Block:
    tmp = node->entryNode;
    nList->push_back(tmp);

    while (tmp->succNode.size() == 1) {
      tmp = *(tmp->succNode.begin());
      nList->push_back(tmp);
    }

    break;
  case SA::IfThen:
    nList->push_back(cond(node));
    nList->push_back(*(node->childNode.begin()));

    break;
  case SA::IfThenElse:
    nList->push_back(cond(node));
    nList->push_back(ifTrue(node));
    nList->push_back(ifFalse(node));

    break;
  case SA::SelfLoop:
    nList->push_back(*(node->childNode.begin()));
    break;
  case SA::NaturalLoop:
    tmp = node->entryNode;
    nList->push_back(tmp);

    tmp = *(tmp->succNode.begin());
    nList->push_back(tmp);

    break;
  default:
    break;
  }

  return *nList;
}

const SA::NodeTy *StructuralTransform::cond(const NodeTy *node) const {
  return node->entryNode;
}

const SA::NodeTy *StructuralTransform::ifTrue(const NodeTy *node) const {
  if (node->nodeType == SA::IfThen) {
    return *(node->childNode.begin());
  }

  const NodeTy *lastChild = node;

  while (!lastChild->isCombined) {
    lastChild = children(lastChild).back();
  }

  BranchInst *branch = dyn_cast<BranchInst>(lastChild->BB->getTerminator());
  assert(branch != nullptr && "terminator must be a branch instruction");

  auto tmpNode = node->childNode.begin();
  NodeTy *childNode1 = *tmpNode;
  ++tmpNode;
  NodeTy *childNode2 = *tmpNode;

  if (branch->getSuccessor(0) == childNode1->entryBB) {
    return childNode2;
  } else {
    return childNode1;
  }
}

const SA::NodeTy *StructuralTransform::ifFalse(const NodeTy *node) const {
  const NodeTy *lastChild = node;

  while (!lastChild->isCombined) {
    lastChild = children(lastChild).back();
  }

  BranchInst *branch = dyn_cast<BranchInst>(lastChild->BB->getTerminator());
  assert(branch != nullptr && "terminator must be a branch instruction");

  auto tmpNode = node->childNode.begin();
  NodeTy *childNode1 = *tmpNode;
  ++tmpNode;
  NodeTy *childNode2 = *tmpNode;

  if (branch->getSuccessor(0) == childNode1->entryBB) {
    return childNode1;
  } else {
    return childNode2;
  }
}
}
