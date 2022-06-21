//===-- CFGStructurization.cpp - CFG Structurizer -------------------------===//
//
//  Flo's Open libRary (floor)
//  Copyright (C) 2004 - 2022 Florian Ziesche
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; version 2 of the License only.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License along
//  with this program; if not, write to the Free Software Foundation, Inc.,
//  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//
//==-----------------------------------------------------------------------===//
//
// Transforms the CFG into a structurized CFG.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/LibFloor.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <algorithm>
#include <cstdarg>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <array>

#include "llvm/Transforms/LibFloor/StructuralAnalysis.h"
#include "llvm/Transforms/LibFloor/StructuralTransform.h"
#include "llvm/Transforms/LibFloor/cfg/cfg_structurizer.hpp"
#include "llvm/Transforms/LibFloor/cfg/cfg_translator.hpp"

using namespace llvm;

#define DEBUG_TYPE "cfg-structurization"

#if 1
#define DBG(x)
#else
#define DBG(x) x
#endif

namespace {
	class CFGStructurization : public FunctionPass {
	public:
		static char ID; // Pass identification, replacement for typeid
		
		typedef StructuralAnalysis SA;
		
		Module* M { nullptr };
		LLVMContext* ctx { nullptr };
		Function* func { nullptr };
		
		DominatorTree* DT;
		
		bool was_modified { false };

		CFGStructurization() : FunctionPass(ID) {
			initializeCFGStructurizationPass(*PassRegistry::getPassRegistry());
		}
		
		StringRef getPassName() const override {
			return "CFG structurization";
		}

		void getAnalysisUsage(AnalysisUsage &AU) const override {
			AU.addRequired<DominatorTreeWrapperPass>();
		}
		
		// returns true if the BB only contains a terminator and doesn't have a successor
		static bool is_simple_bb(const BasicBlock* bb) {
			if(!succ_empty(bb)) return false;
			
			const Instruction* term = bb->getTerminator();
			if(&bb->front() == term) {
				return true;
			}
			// also need to handle special terminations (discard is a func call + unreachable/term inst)
			if(const CallInst* CI = dyn_cast<CallInst>(&bb->front())) {
				if(const Function* CF = CI->getCalledFunction()) {
					if(CF->hasName() && CF->getName() == "floor.discard_fragment") {
						// next must be term
						if(&*std::next(bb->begin()) == term) {
							return true;
						}
						assert(false && "invalid discard_fragment call (not followed by term instr)");
					}
				}
			}
			return false;
		}
		
		CallInst* insert_merge_block_marker(BasicBlock* merge_block) {
			Function* merge_block_func = M->getFunction("floor.merge_block");
			if(merge_block_func == nullptr) {
				FunctionType* merge_block_type = FunctionType::get(llvm::Type::getVoidTy(*ctx), false);
				merge_block_func = (Function*)M->getOrInsertFunction("floor.merge_block", merge_block_type).getCallee();
				merge_block_func->setCallingConv(CallingConv::FLOOR_FUNC);
				merge_block_func->setCannotDuplicate();
				merge_block_func->setDoesNotThrow();
				merge_block_func->setNotConvergent();
				merge_block_func->setDoesNotRecurse();
			}
			CallInst* merge_block_call = CallInst::Create(merge_block_func, "", merge_block->getTerminator());
			merge_block_call->setCallingConv(CallingConv::FLOOR_FUNC);
			return merge_block_call;
		}
		
		CallInst* insert_continue_block_marker(BasicBlock* continue_block) {
			Function* continue_block_func = M->getFunction("floor.continue_block");
			if(continue_block_func == nullptr) {
				FunctionType* continue_block_type = FunctionType::get(llvm::Type::getVoidTy(*ctx), false);
				continue_block_func = (Function*)M->getOrInsertFunction("floor.continue_block", continue_block_type).getCallee();
				continue_block_func->setCallingConv(CallingConv::FLOOR_FUNC);
				continue_block_func->setCannotDuplicate();
				continue_block_func->setDoesNotThrow();
				continue_block_func->setNotConvergent();
				continue_block_func->setDoesNotRecurse();
			}
			CallInst* continue_block_call = CallInst::Create(continue_block_func, "", continue_block->getTerminator());
			continue_block_call->setCallingConv(CallingConv::FLOOR_FUNC);
			return continue_block_call;
		}
		
		void create_loop_merge(Instruction* insert_before, BasicBlock* bb_merge, BasicBlock* bb_continue) {
			Function* loop_merge_func = M->getFunction("floor.loop_merge");
			if(loop_merge_func == nullptr) {
				llvm::Type* loop_merge_arg_types[] { llvm::Type::getLabelTy(*ctx), llvm::Type::getLabelTy(*ctx) };
				FunctionType* loop_merge_type = FunctionType::get(llvm::Type::getVoidTy(*ctx), loop_merge_arg_types, false);
				loop_merge_func = (Function*)M->getOrInsertFunction("floor.loop_merge", loop_merge_type).getCallee();
				loop_merge_func->setCallingConv(CallingConv::FLOOR_FUNC);
				loop_merge_func->setCannotDuplicate();
				loop_merge_func->setDoesNotThrow();
				loop_merge_func->setNotConvergent();
				loop_merge_func->setDoesNotRecurse();
			}
			llvm::Value* merge_vars[] { bb_merge, bb_continue };
			CallInst* loop_merge_call = CallInst::Create(loop_merge_func, merge_vars, "", insert_before);
			loop_merge_call->setCallingConv(CallingConv::FLOOR_FUNC);
			
			insert_merge_block_marker(bb_merge);
			insert_continue_block_marker(bb_continue);
		}
		
		void create_selection_merge(Instruction* insert_before, BasicBlock* merge_block) {
			Function* sel_merge_func = M->getFunction("floor.selection_merge");
			if(sel_merge_func == nullptr) {
				llvm::Type* sel_merge_arg_types[] { llvm::Type::getLabelTy(*ctx) };
				FunctionType* sel_merge_type = FunctionType::get(llvm::Type::getVoidTy(*ctx), sel_merge_arg_types, false);
				sel_merge_func = (Function*)M->getOrInsertFunction("floor.selection_merge", sel_merge_type).getCallee();
				sel_merge_func->setCallingConv(CallingConv::FLOOR_FUNC);
				sel_merge_func->setCannotDuplicate();
				sel_merge_func->setDoesNotThrow();
				sel_merge_func->setNotConvergent();
				sel_merge_func->setDoesNotRecurse();
			}
			llvm::Value* merge_vars[] { merge_block };
			CallInst* sel_merge_call = CallInst::Create(sel_merge_func, merge_vars, "", insert_before);
			sel_merge_call->setCallingConv(CallingConv::FLOOR_FUNC);
			
			insert_merge_block_marker(merge_block);
		}
		
		// recursively traverses a StructuralAnalysis node tree, applying the specified handler function on each combined node
		template <typename F>
		void node_recurse(const SA::NodeTy* node, SA& cfg_analysis, F&& handler) {
			if(!node->isCombined) return;
			
			BasicBlock* this_block = cfg_analysis.mapNode2BB(node);
			handler(node, this_block);
			
			for(const auto* child : node->childNode) {
				node_recurse(child, cfg_analysis, std::forward<F&&>(handler));
			}
		}
		
		// this applies simple fixes on the CFG:
		//  * split SelfLoops (single loops) into a header and body block
		//  * clone / split off trivial exit blocks (to somewhat mitigate the "single merge block" problem)
		void restructure_simple() {
			DBG(errs() << "> simple CFG restructuring\n";)
			StructuralAnalysis cfg_analysis;
			cfg_analysis.analyze(*func);
			DBG(cfg_analysis.write(errs());)
			
			node_recurse(*cfg_analysis.Net.begin(), cfg_analysis,
						 [this](const SA::NodeTy* node, BasicBlock* this_block) {
				switch(node->nodeType) {
					case SA::SelfLoop: {
						DBG(errs() << ">> splitting self loop: " << this_block->getName() << "\n";)
						SplitBlock(this_block, this_block->getFirstNonPHI());
						was_modified = true;
						break;
					}
					case SA::IfThen:
					case SA::IfThenElse: {
						BranchInst* branch = dyn_cast<BranchInst>(this_block->getTerminator());
						assert(branch != nullptr && "if-then/if-then-else block must have a branch instruction");
						if(node->exitBB != nullptr &&
						   is_simple_bb(node->exitBB) &&
						   branch->isConditional()) {
							// only do this for immediate branches to a trivial exit block, otherwise continue
							BasicBlock* branches[] { branch->getSuccessor(0), branch->getSuccessor(1) };
							const bool simple_bb[] {
								is_simple_bb(branches[0]),
								is_simple_bb(branches[1]),
							};
							if(simple_bb[0] || simple_bb[1]) {
								const uint32_t simple_idx = (simple_bb[0] ? 0 : 1);
								
								DBG(errs() << ">> added new trivial exit: in: " << this_block->getName() << ", exit: " << branches[simple_idx]->getName() << "\n";)
								
								ValueToValueMapTy dummy_vmap;
								auto new_exit = CloneBasicBlock(branches[simple_idx], dummy_vmap, ".exit", func);
								branch->setSuccessor(simple_idx, new_exit);
								was_modified = true;
							}
						}
						break;
					}
					default: break;
				}
			});
		}
		
		bool simplify_cfg(Function& F) {
			DBG(errs() << "> CFG simplification\n";)
			bool modified = false;
			
			std::vector<BasicBlock*> bbs;
			for(auto bb_iter = F.begin(); bb_iter != F.end(); ++bb_iter) {
				bbs.emplace_back(&*bb_iter);
			}
			
			for(const auto& BB : bbs) {
				modified |= DeleteDeadPHIs(BB);
				
				// if bb has a single predecessor, fold single entry phi nodes
				if(BB->getUniquePredecessor() != nullptr) {
					FoldSingleEntryPHINodes(BB);
				}
				
				DBG(const auto bb_name = BB->getName();)
				const auto did_merge = MergeBlockIntoPredecessor(BB, nullptr, nullptr, nullptr);
				DBG(if(did_merge) { errs() << ">>> merged bb into pred: " << bb_name << "\n"; })
				
				modified |= did_merge;
			}
			
			return modified;
		}
		
		void restructure_and_annotate() {
			DBG(errs() << "> final CFG restructuring and annotating\n";)
			StructuralAnalysis cfg_analysis;
			cfg_analysis.analyze(*func);
			DBG(cfg_analysis.write(errs());)
			
			// compute LoopInfo from the DT (ideally we could use the StructuralAnalysis info for this,
			// but it doesn't provide (correct) back-edge info)
			if(was_modified) {
				DT->recalculate(*func);
			}
			LoopInfo LI(*DT);
			
			// first: gather all merge block information
			struct merge_info {
				BasicBlock* header_block;
				BasicBlock* continue_block; // nullptr if not a loop (TODO: still needed with node?)
				const SA::NodeTy* node;
			};
			std::unordered_map<BasicBlock*, std::vector<merge_info>> merge_blocks;
			std::unordered_set<BasicBlock*> loop_header_blocks, selection_header_blocks;
			node_recurse(*cfg_analysis.Net.begin(), cfg_analysis, [this, /*&cfg_analysis,*/
                                                             &loop_header_blocks,
                                                             &selection_header_blocks,
                                                             &LI, &merge_blocks](const SA::NodeTy* node,
                                                                                 BasicBlock* this_block) {
				switch(node->nodeType) {
					case SA::NaturalLoop: {
						assert(node->exitBB != nullptr && "must have an exit block");
						DBG(errs() << ">> @loop: " << this_block->getName() << "\n";)
						Loop* loop = LI.getLoopFor(this_block);
						assert(loop != nullptr && "must have LoopInfo for this loop BB!");
						BasicBlock* back_edge = loop->getLoopLatch();
						if(back_edge == nullptr) {
							report_fatal_error("did not find the loop back-edge");
							llvm_unreachable("did not find the loop back-edge");
						}
						DBG(errs() << "loop back-edge: " << back_edge->getName() << "\n";)
						DBG(errs() << "loop merge: " << node->exitBB->getName() << "\n";)
						loop_header_blocks.emplace(this_block);
						merge_blocks[node->exitBB].emplace_back(merge_info { this_block, back_edge, node });
						break;
					}
					case SA::IfThen:
					case SA::IfThenElse: {
						BranchInst* branch = dyn_cast<BranchInst>(this_block->getTerminator());
						assert(branch != nullptr && "if-then/if-then-else block must have a branch instruction");
						if(branch->isConditional()) {
							// if this selection header block is also a loop header block, split it before the branch,
							// because blocks can't be both (according to the current spec)
							// NOTE: will always encounter NaturalLoop before IfThen/IfThenElse of the same block
							if(loop_header_blocks.count(this_block) > 0) {
								this_block = SplitBlock(this_block, this_block->getTerminator(), DT, &LI);
								was_modified = true;
							}
							
							// check if either successor is a simple return block (only this block is it's predecessor)
							// if so, make the other block the merge block (outcome doesn't matter if both return)
							const bool is_simple_return_bb[] {
								succ_empty(branch->getSuccessor(0)) &&
								branch->getSuccessor(0)->getSinglePredecessor() == this_block,
								succ_empty(branch->getSuccessor(1)) &&
								branch->getSuccessor(1)->getSinglePredecessor() == this_block,
							};
							if(is_simple_return_bb[0] || is_simple_return_bb[1]) {
								merge_blocks[is_simple_return_bb[0] ?
											 branch->getSuccessor(1) :
											 branch->getSuccessor(0)].emplace_back(merge_info { this_block, nullptr, node });
							}
							// if not, make the exit block the merge block
							else {
								assert(node->exitBB != nullptr && "must have an exit block");
								merge_blocks[node->exitBB].emplace_back(merge_info { this_block, nullptr, node });
							}
							selection_header_blocks.emplace(this_block);
						}
						// else: don't need to handle unconditionals
						break;
					}
					case SA::SelfLoop: {
						report_fatal_error("CFG should no longer contain self-loops!");
						llvm_unreachable("CFG should no longer contain self-loops!");
					}
					// TODO: how to handle other invalid types?
					default: break;
				}
			});
			
			// then: add (loop|selection) merge annotations + possibly restructure the CFG if a merge block has multiple users
			for(auto& merge_block : merge_blocks) {
				// simple case: single-user merge block
				if(merge_block.second.size() == 1) {
					// loop
					const auto& info = merge_block.second[0];
					if(info.continue_block != nullptr) {
						create_loop_merge(info.header_block->getTerminator(),
										  merge_block.first, info.continue_block);
					}
					// selection
					else {
						create_selection_merge(info.header_block->getTerminator(), merge_block.first);
					}
					continue;
				}
				
				// will always modify something after this
				was_modified = true;
				DBG(errs() << ">> multi-user merge block: " << merge_block.first->getName() << "\n";
				for(const auto& info : merge_block.second) {
					errs() << "\tuser: " << info.header_block->getName() << "\n";
				}
				errs() << "\n";)
				
				// figure out the merge tree we need to create
				// (due to the merge-block dominator requirements we can't just simply create a chain or binary tree)
				// NOTE: there will always be a single parent node and it will always be the first entry in
				//       merge_block.second (unstructured -> structured cfg transformation makes sure of this)
				struct merge_node {
					const merge_info& info;
					std::vector<merge_node> children;
				};
				const std::function<merge_node(const merge_info&)> merge_tree_recurse =
					[&merge_tree_recurse, &merge_block/* , &cfg_analysis TODO: if needed */](const merge_info& info) {
						merge_node node { info, {} };
						for(auto* direct_child : info.node->childNode) {
							if(!direct_child->isCombined) continue;
							
							std::deque<SA::NodeTy*> child_stack;
							child_stack.emplace_back(direct_child);
							while(!child_stack.empty()) {
								auto* child = child_stack[0];
								child_stack.pop_front();
								if(!child->isCombined) continue;
								
								const auto iter = std::find_if(merge_block.second.cbegin(), merge_block.second.cend(),
															   [child](const merge_info& entry) {
																   return (entry.node == child);
															   });
								if(iter != merge_block.second.cend()) {
									node.children.emplace_back(merge_tree_recurse(*iter));
									break;
								}
								else {
									for(auto* indirect_child : child->childNode) {
										child_stack.emplace_back(indirect_child);
									}
								}
							}
						}
						return node;
					};
				merge_node merge_tree = merge_tree_recurse(merge_block.second[0]);
				
				const std::function<void(const merge_node&, const uint32_t)> dump_merge_tree =
					[&dump_merge_tree, &loop_header_blocks](const merge_node& node, const uint32_t level) {
						for(uint32_t i = 0; i < level; ++i) errs() << "  ";
						errs() << node.info.header_block->getName() << "(loop: " << (node.info.continue_block != nullptr ? 1 : 0) << ", " << loop_header_blocks.count(node.info.header_block) << ")\n";
						for(const auto& child : node.children) {
							dump_merge_tree(child, level + 1);
						}
					};
				DBG(errs() << "## merge-tree:\n";
				dump_merge_tree(merge_tree, 1);
				errs() << "####\n";)
				
				// create the actual merge tree + insert resp. merge instructions
				BasicBlock* merge_bb = merge_block.first;
				const std::function<void(const merge_node&,
										 BasicBlock*,
										 const uint32_t,
										 const uint32_t)> create_cfg_merge_tree =
					[this, &create_cfg_merge_tree](const merge_node& node,
												   BasicBlock* this_merge_block,
												   const uint32_t level,
												   const uint32_t child_idx) {
						BasicBlock* header = node.info.header_block;
						
						std::vector<BasicBlock*> child_merge_bbs;
						for(const auto& child : node.children) {
							std::unordered_set<BasicBlock*> preds, visited;
							// TODO: this is inefficient
							const std::function<void(BasicBlock* bb)> get_preds =
								[&get_preds, &preds, &visited, &this_merge_block](BasicBlock* bb) {
									if(visited.count(bb) > 0) return;
									visited.emplace(bb);
									for(auto* succ : successors(bb)) {
										if(succ == this_merge_block) {
											preds.emplace(bb);
										}
										else {
											get_preds(succ);
										}
									}
								};
							get_preds(child.info.header_block);
							if(preds.empty()) continue;
							
							std::vector<BasicBlock*> preds_vec(preds.cbegin(), preds.cend());
							DBG(errs() << "@" << header->getName() << ", split off:\n";
							for(const auto* pred : preds_vec) {
								errs() << "\tpred: " << pred->getName() << "\n";
							})
							
							child_merge_bbs.emplace_back(SplitBlockPredecessors(this_merge_block, preds_vec,
                                                                  ".merge", DT, nullptr, nullptr, false));
						}
						
						uint32_t cidx = 0;
						for(const auto& child : node.children) {
							create_cfg_merge_tree(child, child_merge_bbs[cidx], level + 1, cidx);
							cidx++;
						}
						
						// add merge annotation
						if(node.info.continue_block != nullptr) {
							create_loop_merge(header->getTerminator(), this_merge_block, node.info.continue_block);
						}
						else {
							create_selection_merge(header->getTerminator(), this_merge_block);
						}
						
						return;
					};
				create_cfg_merge_tree(merge_tree, merge_bb, 0, 0);
			}
		}
		
		bool runOnFunction(Function &F) override {
			M = F.getParent();
			ctx = &M->getContext();
			func = &F;
			was_modified = false;
			
#if 0 // enable to use the old CFG structurization
			was_modified |= run_old_structurization(F);
#else
			was_modified |= run_structurization(F);
#endif
			
			return was_modified;
		}
		
		bool run_old_structurization(Function &F) {
			DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
			
#if 0
			DBG(
				errs() << "#################### func before: " << F.getName() << " ####\n" << F << "\n";
				//StructuralAnalysis::dumpCFGDot("func_initial.dot", F);
			)
#endif
			
			// initial unstructured -> structured control flow fixes
			DBG(errs() << "> fixing unstructured control flow\n";)
			StructuralTransform cfg_transform;
			was_modified |= cfg_transform.transform(F);
			
			// after transformation/structurization (and sometimes in general), we might have very trivial BB branching (unconditional
			// branch to a BB w/o any other incoming edges)
			// -> this can throw off the analysis and following restructuring, so simplify these constructs
			// NOTE: will also take care of folding PHIs
			was_modified |= simplify_cfg(F);
			
			// apply simple cfg fixes
			restructure_simple();
			
			// vulkan/spir-v specific annotations / cfg restructuring
			restructure_and_annotate();
			DBG({
				StructuralAnalysis cfg_analysis;
				cfg_analysis.analyze(F);
				cfg_analysis.write(errs());
			})
			
#if 0
			DBG(
				errs() << "#################### func after: " << F.getName() << " ####\n" << F << "\n";
				//StructuralAnalysis::dumpCFGDot("func_after.dot", F);
			)
#endif
				
			DBG(errs() << "verifying ...\n";)
#if !defined(NDEBUG)
			const auto verify_failed = verifyFunction(F, &errs());
			if (verify_failed) {
				errs().flush();
				assert(!verify_failed && "verification failed");
			}
#endif
			DBG(errs() << "verifying done\n";)
			
			DBG(errs() << "## " << F.getName() << " modified? " << was_modified << "\n";)
			
			return was_modified;
		}
		
		bool run_structurization(Function &F) {
			// skip if we only have a single block that returns
			if (auto ret_instr = dyn_cast_or_null<ReturnInst>(F.getEntryBlock().getTerminator())) {
				return false;
			}
			was_modified = true;
			
			CFGNodePool node_pool(*ctx, F);
			cfg_translator translator(F, *ctx, node_pool);
			
			CFGStructurizer structurizer(translator.get_entry_block(), node_pool, F, *ctx);
			structurizer.run();
			translator.cfg_to_llvm_ir(structurizer.get_entry_block(), true);
			
#if !defined(NDEBUG) || 1 // TODO: disable in release mode later on - for now, always keep it on
			const auto verify_failed = verifyFunction(F, &errs());
			if (verify_failed) {
				errs().flush();
				assert(!verify_failed && "verification failed");
			}
#endif
			
			return was_modified;
		}
	};

}

char CFGStructurization::ID = 0;
FunctionPass *llvm::createCFGStructurizationPass() {
	return new CFGStructurization();
}
INITIALIZE_PASS_BEGIN(CFGStructurization, "CFGStructurization", "CFGStructurization Pass", false, false)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(CFGStructurization, "CFGStructurization", "CFGStructurization Pass", false, false)

