//===- StructuralAnalysis.h - ---------------------------------------------===//
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
// \brief   The header file for the StructuralAnalysis pass.
//===----------------------------------------------------------------------===//
//
// This file defines the class of Structural Analysis which will return the
// control tree and unstructured branches of a function
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

#ifndef LLVM_TRANSFORMS_LIBFLOOR_STRUCTURALANALYSIS_H
#define LLVM_TRANSFORMS_LIBFLOOR_STRUCTURALANALYSIS_H

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// TODO: modernize/c++ify this

namespace llvm {

// StructuralAnalysis - This class holds all the methods and data structures
class StructuralAnalysis {
public:
  enum EdgeClass { TREE, FORWARD, BACK, CROSS };
  typedef std::pair<BasicBlock *, BasicBlock *> EdgeLLVMTy;
  typedef std::vector<EdgeLLVMTy> EdgeVecTy;
  typedef std::vector<BasicBlock *> BBVecTy;

  // Types defined in Fig 7.38 of Muchnick book
  enum RegionTy {
    Nil,
    Block,
    IfThen,
    IfThenElse,
    Case,
    SelfLoop,
    NaturalLoop,
    Improper,
    Unreachable
  };

  // NodeTy - This type is used for the CFG node
  struct NodeTy {
    // Whether it is an original or combined from original
    bool isCombined{false};
    // Map to the corresponding BasicBlock* if it is original
    BasicBlock *BB{nullptr};
    std::vector<NodeTy *> predNode; // Predecessor of the node
    std::vector<NodeTy *> succNode; // Successor of the node
    // If isCombined is true, it points to the entry of the nodeset
    NodeTy *entryNode{nullptr};
    NodeTy *parentNode{nullptr};     // Parent Node in Control Tree
    std::vector<NodeTy *> childNode; // Child Nodes in Control Tree
    EdgeVecTy outgoingBR;            // Outgoing Branches of a loop
    EdgeVecTy incomingBR;            // Incoming Branches of a loop
    BasicBlock *entryBB{nullptr};    // The entry Basic Block
    BasicBlock *exitBB{nullptr};     // The exit Basic Block
    BBVecTy containedBB;             // BasicBlock*s contained in this node
    EdgeVecTy incomingForwardBR;     // The shared code of unstructured branch
    RegionTy nodeType{Nil};          // The type of the node
    bool isLoopHeader{false};
    bool isBackEdge{false};
    NodeTy *loopExitNode{nullptr};

    void remove_predecessor(NodeTy *pred) {
      predNode.erase(std::remove(predNode.begin(), predNode.end(), pred),
                     predNode.end());
    }
    bool has_predecessor(NodeTy *pred) const {
      const auto iter = std::find(predNode.cbegin(), predNode.cend(), pred);
      return (iter != predNode.cend());
    }
    bool add_predecessor(NodeTy *pred) {
      if (has_predecessor(pred)) {
        return false;
      }
      predNode.emplace_back(pred);
      return true;
    }
    void remove_successor(NodeTy *succ) {
      succNode.erase(std::remove(succNode.begin(), succNode.end(), succ),
                     succNode.end());
    }
    bool has_successor(NodeTy *succ) const {
      const auto iter = std::find(succNode.cbegin(), succNode.cend(), succ);
      return (iter != succNode.cend());
    }
    bool add_successor(NodeTy *succ) {
      if (has_successor(succ)) {
        return false;
      }
      succNode.emplace_back(succ);
      return true;
    }

    ~NodeTy();
  };

  // NodeVecTy - used to holds nodes in a set
  typedef std::vector<NodeTy *> NodeVecTy;
  typedef std::unordered_map<BasicBlock *, NodeTy *> BB2NodeMapTy;
  typedef std::pair<NodeTy *, NodeTy *> EdgeTy;
  typedef std::map<EdgeTy, EdgeClass> Edge2ClassMapTy;
  typedef std::set<EdgeTy> EdgeSetTy;
  typedef std::set<NodeTy *> VisitSetTy;

public:
  ~StructuralAnalysis();
  void analyze(Function &F);

  // Get a text representation of the analysis
  void write(llvm::raw_ostream &stream) const;

public:
  NodeVecTy Net;

  // unstructuredBRVec - store the detected unstructured branches
  EdgeVecTy unstructuredBRVec;

  // BB2NodeMap - This var is used to find the Node from BasicBlock*
  BB2NodeMapTy BB2NodeMap;

  NodeVecTy unreachableNodeSet;

private:
  Function *_function;

  // postorder traversal of the flowgraph
  uint32_t postCtr, postMax, preMax;
  std::map<uint32_t, NodeTy *> post;
  VisitSetTy visit, visitPath;
  std::map<NodeTy *, uint32_t> preTree, postTree;

  // edge2ClassMap - map the edge to its class
  Edge2ClassMapTy edge2ClassMap;

public:
  // buildSimpleCFG - Build a Simple CFG out of the LLVM CFG
  void buildSimpleCFG(NodeVecTy &Nodes);

  // structuralAnalysis - Follow Fig 7.39 of Muchnick book
  void structuralAnalysis(NodeVecTy &Nodes, NodeTy *entry);

  // DFSPostorder - Follow Fig 7.40 of Muchnick book
  void DFSPostorder(const NodeVecTy &Nodes, NodeTy *x);

  // acyclicRegionType - Follow Fig 7.41 of Muchnick book
  RegionTy acyclicRegionType(const NodeVecTy &Nodes, NodeTy *node,
                             NodeVecTy &nset, NodeTy **entryNode,
                             NodeTy **exitNode, NodeTy *entry);

  // cyclicRegionType - Follow Fig 7.42 of Muchnick book
  RegionTy cyclicRegionType(const NodeVecTy &Nodes, NodeVecTy &nset,
                            NodeTy *loopHeaderNode, NodeTy *backEdgeNode,
                            NodeTy **exitNode, NodeTy *entry);

  // reduce - Follow Fig 7.43 of Muchnick book
  NodeTy *reduce(NodeVecTy &N, RegionTy rType, NodeVecTy &nodeSet,
                 NodeTy *entryNode, NodeTy *exitNode);

  // replace - Follow Fig 7.44 of Muchnick book
  void replace(NodeVecTy &N, NodeTy *node, NodeVecTy &nodeSet);

  // isImproper - Follow Fig 7.45 of Muchnick book
  bool isImproper(const NodeVecTy &Nodes, NodeVecTy &nset,
                  NodeTy *loopHeaderNode, NodeTy *backEdgeNode,
                  NodeTy **exitNode, NodeTy *entry);

  // pathBack - Check if there is a node k such that there is a path from
  // m to k that does not pass through n and an edge k->n that is a back edge
  NodeTy *pathBack(NodeTy *n, NodeVecTy &N, NodeVecTy &reachUnder);

  // isCaseWithDefault - Check if node leads a case block
  bool isCaseWithDefault(const NodeVecTy &Nodes, NodeTy *entryNode,
                         NodeTy **exitNode, NodeTy *entry);

  // isCaseWithoutDefault - Check if node leads a case block
  bool isCaseWithoutDefault(const NodeVecTy &Nodes, NodeTy *entryNode,
                            NodeTy **exitNode, NodeTy *entry);

  // isImproperCaseWithDefault - Check if node leads
  // a case block with incoming edges
  bool isImproperCaseWithDefault(const NodeVecTy &Nodes, NodeTy *entryNode,
                                 NodeTy *entry);

  // isImproperCaseoutWithDefault - Check if node leads
  //	a case block with incoming edges
  bool isImproperCaseWithoutDefault(const NodeVecTy &Nodes, NodeTy *entryNode,
                                    NodeTy **exitNode, NodeTy *entry);

  // path(n, m, I) - Return true if there is a path from from n to m
  // such that all the nodes in it are in I and false otherwise
  bool path(NodeTy *n, NodeTy *m, const NodeVecTy &Nodes, NodeTy *esc);

  // path(n, m, I, src, dst ) - Return true if there is a path from from
  // n to m such that all the nodes in it are in I without going through edge
  // src->dst and false otherwise
  bool path(NodeTy *n, NodeTy *m, const NodeVecTy &Nodes, NodeTy *src,
            NodeTy *dst);

  // compact - Compact nodes in nset into n;
  void compact(NodeVecTy &N, NodeTy *n, NodeVecTy &nset);

  // mapNode2BB - Return the corresponding BasicBlock* of the node
  BasicBlock *mapNode2BB(const NodeTy *node) const;

  // mapBB2Node - Return the corresponding sturcture node of the basic block
  NodeTy *mapBB2Node(BasicBlock *bb);

  // dumpCTNode - Dump one Control Node
  void dumpCTNode(llvm::raw_ostream &stream, NodeTy *n) const;

  // dumpNode - Dump one node
  void dumpNode(llvm::raw_ostream &stream, NodeTy *node) const;

  // findUnstructuredBR - Record the branch and remove it from CFG
  void findUnstructuredBR(NodeTy *srcNode, NodeTy *dstNode,
                          bool needForwardCopy, bool isGoto);

  // findBB - put all Basic Blocks in node into nodeVec
  void findBB(NodeTy *node, BBVecTy &nodeVec) const;

  // findEntryBB - find the entry Basic Block of the node
  BasicBlock *findEntryBB(NodeTy *node);

  // dumpUnstructuredBR - Dump all found unstructured branches
  void dumpUnstructuredBR(llvm::raw_ostream &stream) const;

  // dump all nodes/edges that need a forward copy
  void dumpForwardCopy(llvm::raw_ostream &stream) const;

  // dumps the CFG of the specified function as a .dot file
  static void dumpCFGDot(const std::string &filename, const Function &F);

  // dumps the IR of the specified function as a .ll file
  static void dumpIR(const std::string &filename, const Function &F);

  // isStillReachableFrom entry -Return true if after erasing
  // edge src->dst, dst is still reachable from entry
  bool isStillReachableFromEntry(const NodeVecTy &Nodes, NodeTy *entry,
                                 NodeTy *dstNode, NodeTy *srcNode);

  // clean - fill in the element of incoming branches and outgoing branches
  void cleanup(NodeTy *node);

  void cleanupUnreachable();

  void reconstructUnreachable();

  // deleteUnreachableNode - delete nodes that are no longer reachable from the
  // entry
  void deleteUnreachableNodes(NodeVecTy &Nodes, NodeTy *entry);

  bool checkUnique(EdgeVecTy &edgeVec, BasicBlock *srcBB, BasicBlock *dstBB);

  // returns true if "Nodes" contains "node"
  static bool containsNode(const NodeVecTy &Nodes, const NodeTy *node);

  // returns true if "BBs" contains "BB"
  static bool containsBB(const BBVecTy &BBs, const BasicBlock *BB);

  // inserts "node" into "Nodes", returns true if successful, false if it
  // already exists
  static bool insertNode(NodeVecTy &Nodes, NodeTy *node);

  // removes "node" from "Nodes", returns true if successful
  static bool eraseNode(NodeVecTy &Nodes, NodeTy *node);
};
}

#endif
