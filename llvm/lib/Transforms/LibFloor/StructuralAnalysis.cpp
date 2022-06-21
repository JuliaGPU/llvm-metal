//===- StructuralAnalysis.cpp - -------------------------------------------===//
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
// \brief   The source file for the StructuralAnalysis pass.
//===----------------------------------------------------------------------===//
//
// This file defines the class of Structural Analysis which will return the
// control tree and unstructured branches of a function
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/LibFloor/StructuralAnalysis.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/FileSystem.h"
#include <algorithm>

namespace llvm {

void StructuralAnalysis::dumpCFGDot(const std::string &filename,
                                    const Function &F) {
  std::error_code EC;
  raw_fd_ostream File(filename, EC, sys::fs::OF_Text);

  if (!EC)
    WriteGraph(File, (const Function *)&F);
  else
    errs() << "failed to dump cfg!";
}

void StructuralAnalysis::dumpIR(const std::string &filename,
                                const Function &F) {
  std::error_code EC;
  raw_fd_ostream File(filename, EC, sys::fs::OF_Text);

  if (!EC)
    File << F;
  else
    errs() << "failed to dump IR!";
}

StructuralAnalysis::NodeTy::~NodeTy() {
  for (auto n = childNode.begin(); n != childNode.end(); ++n) {
    delete *n;
  }
}

static StructuralAnalysis::NodeTy *
get_first_node_or_null(const std::vector<StructuralAnalysis::NodeTy *> &nodes) {
  if (nodes.empty()) {
    return nullptr;
  }
  return nodes[0];
}

// buildSimpleCFG - Build a Simple CFG out of the LLVM CFG
void StructuralAnalysis::buildSimpleCFG(NodeVecTy &Nodes) {
  // Create a simple CFG node for every Basic Block
  for (BasicBlock &i : *_function) {
    NodeTy *n = new NodeTy();
    n->BB = &i;
    n->containedBB.emplace_back(&i);
    Nodes.emplace_back(n);
    BB2NodeMap[&i] = n;
  }

  // Setup the edges of the simple CFG
  for (BasicBlock &block : *_function) {
    NodeTy *n = BB2NodeMap[&block];

    // Setup the predecessor of every node
    for (BasicBlock *pred : predecessors(&block)) {
      n->predNode.emplace_back(BB2NodeMap[pred]);
    }

    // Setup the successor of every node
    for (BasicBlock *succ : successors(&block)) {
      n->succNode.emplace_back(BB2NodeMap[succ]);
    }
  }

  // Remove unreachable node
  NodeTy *entry = BB2NodeMap[&_function->getEntryBlock()];

  deleteUnreachableNodes(Nodes, entry);
}

// structuralAnalysis - Follow Fig 7.39 of Muchnick book
void StructuralAnalysis::structuralAnalysis(NodeVecTy &Nodes, NodeTy *entry) {
  NodeTy *n = nullptr, *p = nullptr, *entryNode = nullptr, *exitNode = nullptr;
  RegionTy rType;
  NodeVecTy nodeSet, reachUnder;

  unstructuredBRVec.clear();

  // Handle the case if the Function has only one Basic Block
  if (Nodes.size() == 1) {
    NodeTy *node = new NodeTy();
    NodeTy *singleNode = Nodes.front();
    node->isCombined = true;
    node->childNode.emplace_back(singleNode);
    node->entryNode = singleNode;
    node->exitBB = singleNode->BB;
    node->containedBB.emplace_back(singleNode->BB);
    node->nodeType = Block;

    singleNode->parentNode = node;

    Nodes[0] = node; // replace singleNode with node

    return;
  }

  do {
    bool change = false;

    post.clear();
    preTree.clear();
    postTree.clear();

    visit.clear();
    visitPath.clear();
    postMax = 0;
    postCtr = 1;
    preMax = 0;

    for (NodeTy *node : Nodes) {
      preTree[node] = 0;
      postTree[node] = 0;
    }

    DFSPostorder(Nodes, entry);

    while (Nodes.size() > 1 && postCtr <= postMax) {
      n = post[postCtr];

      if (!containsNode(Nodes, n))
        continue;

      // Locate an acyclic region, if present
      if (n->isLoopHeader && n->loopExitNode) {
        visitPath.clear();

        if (path(n, n->loopExitNode, Nodes, nullptr)) {
          NodeTy *tmpNode = n->loopExitNode;

          while (tmpNode->parentNode) {
            tmpNode = tmpNode->parentNode;
          }

          n->remove_successor(tmpNode);
          tmpNode->remove_predecessor(n);
          n->loopExitNode = tmpNode;
        } else
          n->loopExitNode = nullptr;
      }

      rType =
          acyclicRegionType(Nodes, n, nodeSet, &entryNode, &exitNode, entry);

      if (n->isLoopHeader && n->loopExitNode) {
        n->add_successor(n->loopExitNode);
        n->loopExitNode->add_predecessor(n);
      }

      if (rType == Improper) {
        change = true;

        break;
      } else if (rType != Nil) {
        p = reduce(Nodes, rType, nodeSet, entryNode, exitNode);
        change = true;

        if (containsNode(nodeSet, entry)) {
          entry = p;
        }

        break;
      } else {
        if (NodeTy *backEdgeNode = pathBack(n, Nodes, reachUnder)) {
          rType = cyclicRegionType(Nodes, reachUnder, n, backEdgeNode,
                                   &exitNode, entry);

          if (rType == Improper) {
            change = true;

            break;
          } else if (rType != Nil) {
            change = true;
            p = reduce(Nodes, rType, reachUnder, n, exitNode);

            if (containsNode(reachUnder, entry)) {
              entry = p;
            }

            break;
          } else
            postCtr++;
        } else
          postCtr++;
      }
    }

    if (!change) {
      for (uint32_t i = 1; i <= postMax; i++) {
        NodeTy *node = post[i];

        if (node->predNode.size() > 1 && node->succNode.empty()) {
          uint32_t min = postMax + 1;

          for (NodeTy *predNode : node->predNode) {
            if (postTree[predNode] < min)
              min = postTree[predNode];
          }

          for (NodeTy *predNode : node->predNode) {
            if (postTree[predNode] != min) {
              if (isStillReachableFromEntry(Nodes, entry, node, predNode)) {
                findUnstructuredBR(predNode, node, true, true);
                change = true;
              }
            }
          }

          if (change)
            break;
        }
      }
    }

    if (!change) {
      for (uint32_t i = 1; i <= postMax; i++) {
        NodeTy *node = post[i];

        if (node->predNode.size() > 1 && !node->isBackEdge) {
          NodeTy *tmpNode = nullptr;
          bool processThisNode = true;

          for (NodeTy *predNode : node->predNode) {
            if (edge2ClassMap[std::make_pair(predNode, node)] == BACK) {
              processThisNode = false;

              break;
            }

            if (tmpNode == nullptr)
              tmpNode = predNode;
            else {
              visitPath.clear();

              if (path(tmpNode, predNode, Nodes, node))
                continue;
              else {
                visitPath.clear();

                if (path(predNode, tmpNode, Nodes, node))
                  tmpNode = predNode;
                else {
                  processThisNode = false;

                  break;
                }
              }
            }
          }

          if (processThisNode) {
            for (NodeTy *predNode : node->predNode) {
              if (predNode == tmpNode) {
                if (isStillReachableFromEntry(Nodes, entry, node, predNode)) {
                  findUnstructuredBR(predNode, node, true, true);
                  change = true;
                }
              }
            }

            if (change)
              break;
          }
        }
      }
    }

    if (!change) {
      for (uint32_t i = 1; i <= postMax; i++) {
        NodeTy *node = post[i];

        if (node->predNode.size() > 1 && !node->isBackEdge) {
          bool processThisNode = true;
          uint32_t min = postMax + 1;

          for (NodeTy *predNode : node->predNode) {
            if (edge2ClassMap[std::make_pair(predNode, node)] == BACK) {
              processThisNode = false;

              break;
            }

            if (postTree[predNode] < min)
              min = postTree[predNode];
          }

          if (processThisNode) {

            for (NodeTy *predNode : node->predNode) {
              if (postTree[predNode] != min) {
                if (isStillReachableFromEntry(Nodes, entry, node, predNode)) {
                  findUnstructuredBR(predNode, node, true, true);
                  change = true;
                }
              }
            }

            if (change)
              break;
          }
        }
      }
    }

    // TODO: properly report this instead of asserting
    if (!change) {
      StructuralAnalysis::dumpCFGDot("failed_function.dot", *_function);
    }
    assert(change != false && "Cannot reduce any more in structural analysis");
    if (!change)
      break;
  } while (Nodes.size() != 1);
}

// DFSPostorder - Follow Fig 7.40 of Muchnick book
void StructuralAnalysis::DFSPostorder(const NodeVecTy &Nodes, NodeTy *x) {
  visit.emplace(x);
  preTree[x] = ++preMax;

  for (NodeTy *y : x->succNode) {
    if (visit.count(y) == 0) {
      DFSPostorder(Nodes, y);
      edge2ClassMap[std::make_pair(x, y)] = TREE;
    } else if (preTree[x] < preTree[y]) {
      edge2ClassMap[std::make_pair(x, y)] = FORWARD;
    } else if (postTree[y] == 0 || preTree[x] == preTree[y]) {
      edge2ClassMap[std::make_pair(x, y)] = BACK;
    } else {
      edge2ClassMap[std::make_pair(x, y)] = CROSS;
    }
  }

  postMax++;
  post[postMax] = x;
  postTree[x] = postMax;
}

// acyclicRegionType - Follow Fig 7.41 of Muchnick book
StructuralAnalysis::RegionTy
StructuralAnalysis::acyclicRegionType(const NodeVecTy &Nodes, NodeTy *node,
                                      NodeVecTy &nset, NodeTy **entryNode,
                                      NodeTy **exitNode, NodeTy *entry) {
  NodeTy *m, *n;
  bool p, s;

  nset.clear();

  // Check for a block containing node
  NodeTy *firstNode, *lastNode;
  firstNode = lastNode = n = node;
  p = true;
  s = (n->succNode.size() == 1);

  while (p && s) {
    lastNode = n;

    if (!containsNode(nset, n)) {
      nset.emplace_back(n);
    } else {
      return Nil;
    }

    n = get_first_node_or_null(n->succNode);
    p = (n->predNode.size() == 1);
    s = (n->succNode.size() == 1);
  }

  if (p) {
    if (!containsNode(nset, n)) {
      nset.emplace_back(n);
      lastNode = n;
    } else {
      return Nil;
    }
  }

  n = node;
  p = (n->predNode.size() == 1);
  s = true;

  while (p && s) {
    firstNode = n;

    if (!containsNode(nset, n) || n == node) {
      insertNode(nset, n);
    } else {
      return Nil;
    }
    n = get_first_node_or_null(n->predNode);
    p = (n->predNode.size() == 1);
    s = (n->succNode.size() == 1);
  }

  if (s) {
    if (!containsNode(nset, n) || n == node) {
      firstNode = n;
      insertNode(nset, n);
    } else {
      return Nil;
    }
  }

  if (firstNode->has_predecessor(lastNode)) {
    if (nset.size() == 2) {
      return Nil;
    } else {
      eraseNode(nset, firstNode);
    }
  }

  *entryNode = n;

  if (nset.size() >= 2) {
    if (!containsNode(nset, *entryNode)) {
      for (NodeTy *succ : (*entryNode)->succNode) {
        if (containsNode(nset, succ)) {
          *entryNode = succ;
        }
      }
    }

    *exitNode = lastNode;

    return Block;
  }

  *entryNode = node;

  if ((*entryNode)->succNode.size() == 2) {
    auto i = (*entryNode)->succNode.begin();
    m = *i;
    ++i;
    n = *i;

    if (m == *entryNode || n == *entryNode)
      return Nil;

    if (edge2ClassMap[std::make_pair(*entryNode, m)] == BACK)
      return Nil;
    if (edge2ClassMap[std::make_pair(*entryNode, n)] == BACK)
      return Nil;

    // Check for a normal IfThenElse
    if (m->succNode.size() == 1 && n->succNode.size() == 1 &&
        m->predNode.size() == 1 && n->predNode.size() == 1 &&
        get_first_node_or_null(m->succNode) ==
            get_first_node_or_null(n->succNode) &&
        get_first_node_or_null(m->succNode) != *entryNode) {

      if (edge2ClassMap[std::make_pair(m, *entryNode)] == BACK)
        return Nil;
      if (edge2ClassMap[std::make_pair(n, *entryNode)] == BACK)
        return Nil;

      insertNode(nset, *entryNode);
      insertNode(nset, m);
      insertNode(nset, n);
      *exitNode = get_first_node_or_null(m->succNode);

      return IfThenElse;
    }
    // Check for an IfThenElse with no exit block
    if (m->succNode.empty() && n->succNode.empty() && m->predNode.size() == 1 &&
        n->predNode.size() == 1) {
      insertNode(nset, *entryNode);
      insertNode(nset, m);
      insertNode(nset, n);
      *exitNode = nullptr;

      return IfThenElse;
    }
    // Check for an IfThen
    // n is the Then part
    else if (n->succNode.size() == 1 && n->predNode.size() == 1 &&
             m == get_first_node_or_null(n->succNode)) {
      if (edge2ClassMap[std::make_pair(n, m)] != BACK) {
        if (edge2ClassMap[std::make_pair(n, *entryNode)] == BACK) {
          return Nil;
        }

        insertNode(nset, *entryNode);
        insertNode(nset, n);
        *exitNode = m;

        return IfThen;
      }
    }
    // m is the Then part
    else if (m->succNode.size() == 1 && m->predNode.size() == 1 &&
             n == get_first_node_or_null(m->succNode)) {
      if (edge2ClassMap[std::make_pair(m, n)] != BACK) {
        if (edge2ClassMap[std::make_pair(m, *entryNode)] == BACK) {
          return Nil;
        }

        insertNode(nset, *entryNode);
        insertNode(nset, m);
        *exitNode = n;

        return IfThen;
      }
    }
    // n is the Then part w/o exiting edge
    else if (n->succNode.empty() && n->predNode.size() == 1) {
      visitPath.clear();

      if (!path(m, *entryNode, Nodes, nullptr)) {
        insertNode(nset, *entryNode);
        insertNode(nset, n);
        *exitNode = nullptr;

        return IfThen;
      }
    }
    // m is the Then part w/o exiting edge
    else if (m->succNode.empty() && m->predNode.size() == 1) {
      visitPath.clear();

      if (!path(n, *entryNode, Nodes, nullptr)) {
        insertNode(nset, *entryNode);
        insertNode(nset, m);
        *exitNode = nullptr;

        return IfThen;
      }
    }
    // Check for an IfThenElse with incoming edges
    else if (m->succNode.size() == 1 && n->succNode.size() == 1 &&
             get_first_node_or_null(m->succNode) ==
                 get_first_node_or_null(n->succNode) &&
             get_first_node_or_null(m->succNode) != *entryNode) {

      if (edge2ClassMap[std::make_pair(m, *entryNode)] == BACK)
        return Nil;
      if (edge2ClassMap[std::make_pair(n, *entryNode)] == BACK)
        return Nil;

      if (!n->has_predecessor(get_first_node_or_null(n->succNode)) &&
          !m->has_predecessor(get_first_node_or_null(m->succNode))) {

        if (m->has_predecessor(get_first_node_or_null(m->succNode)) ||
            n->has_predecessor(get_first_node_or_null(n->succNode))) {
          return Nil;
        }

        bool improperFlag = false;

        if (m->predNode.size() > 1) {
          for (auto pi = m->predNode.begin(), pe = m->predNode.end(); pi != pe;
               ++pi) {
            if (*pi != *entryNode &&
                isStillReachableFromEntry(Nodes, entry, m, *pi) &&
                edge2ClassMap[std::make_pair(*pi, m)] != BACK) {
              findUnstructuredBR(*pi, m, true, true);
              improperFlag = true;
            }
          }
        }

        if (n->predNode.size() > 1) {
          for (auto pi = n->predNode.begin(), pe = n->predNode.end(); pi != pe;
               ++pi) {
            if (*pi != *entryNode &&
                isStillReachableFromEntry(Nodes, entry, n, *pi) &&
                edge2ClassMap[std::make_pair(*pi, n)] != BACK) {
              findUnstructuredBR(*pi, n, true, true);
              improperFlag = true;
            }
          }
        }

        if (improperFlag)
          return Improper;
      }
    }
    // Check for an IfThen with incoming edges
    // n is the Then part
    else if (n->succNode.size() == 1 && n->predNode.size() > 1 &&
             m == get_first_node_or_null(n->succNode)) {
      if (edge2ClassMap[std::make_pair(n, *entryNode)] == BACK)
        return Nil;

      if (edge2ClassMap[std::make_pair(n, m)] != BACK) {
        if (n->has_predecessor(m))
          return Nil;

        bool improperFlag = false;

        for (auto pi = n->predNode.begin(), pe = n->predNode.end(); pi != pe;
             ++pi) {
          if (*pi != *entryNode &&
              isStillReachableFromEntry(Nodes, entry, n, *pi) &&
              edge2ClassMap[std::make_pair(*pi, n)] != BACK) {
            findUnstructuredBR(*pi, n, true, true);
            improperFlag = true;
          }
        }

        if (improperFlag)
          return Improper;
      }
    }
    // m is the Then part
    else if (m->succNode.size() == 1 && m->predNode.size() > 1 &&
             n == get_first_node_or_null(m->succNode)) {
      if (edge2ClassMap[std::make_pair(m, *entryNode)] == BACK)
        return Nil;

      if (edge2ClassMap[std::make_pair(m, n)] != BACK) {
        if (m->has_predecessor(n))
          return Nil;

        bool improperFlag = false;

        for (auto pi = m->predNode.begin(), pe = m->predNode.end(); pi != pe;
             ++pi) {
          if (*pi != *entryNode &&
              isStillReachableFromEntry(Nodes, entry, m, *pi) &&
              edge2ClassMap[std::make_pair(*pi, m)] != BACK) {
            findUnstructuredBR(*pi, m, true, true);
            improperFlag = true;
          }
        }

        if (improperFlag)
          return Improper;
      }
    }
    // Check for an IfThenElse (w/o exit block) with incoming edges
    else if (m->succNode.empty() && n->succNode.empty()) {
      bool improperFlag = false;

      if (m->predNode.size() > 1) {
        for (auto pi = m->predNode.begin(), pe = m->predNode.end(); pi != pe;
             ++pi) {
          if (*pi != *entryNode &&
              isStillReachableFromEntry(Nodes, entry, m, *pi)) {
            findUnstructuredBR(*pi, m, true, true);
            improperFlag = true;
          }
        }
      }

      if (n->predNode.size() > 1) {
        for (auto pi = n->predNode.begin(), pe = n->predNode.end(); pi != pe;
             ++pi) {
          if (*pi != *entryNode &&
              isStillReachableFromEntry(Nodes, entry, n, *pi)) {
            findUnstructuredBR(*pi, n, true, true);
            improperFlag = true;
          }
        }
      }

      if (improperFlag)
        return Improper;
    }
    // n is the Then part (w/o exiting edge) with incoming edges
    else if (n->succNode.empty() && n->predNode.size() > 1) {
      visitPath.clear();

      if (!path(m, *entryNode, Nodes, nullptr)) {
        if (n->has_predecessor(m))
          return Nil;

        bool improperFlag = false;

        for (auto pi = n->predNode.begin(), pe = n->predNode.end(); pi != pe;
             ++pi) {
          if (*pi != *entryNode &&
              isStillReachableFromEntry(Nodes, entry, n, *pi)) {
            findUnstructuredBR(*pi, n, true, true);
            improperFlag = true;
          }
        }

        if (improperFlag)
          return Improper;
      }
    }
    // m is the Then part w/o exiting edge with incoming edges
    else if (m->succNode.empty() && m->predNode.size() > 1) {
      visitPath.clear();

      if (!path(n, *entryNode, Nodes, nullptr)) {
        if (m->has_predecessor(n))
          return Nil;

        bool improperFlag = false;

        for (auto pi = m->predNode.begin(), pe = m->predNode.end(); pi != pe;
             ++pi) {
          if (*pi != *entryNode &&
              isStillReachableFromEntry(Nodes, entry, n, *pi)) {
            findUnstructuredBR(*pi, m, true, true);
            improperFlag = true;
          }
        }

        if (improperFlag)
          return Improper;
      }
    }
  }
  // Check for Case
  else if ((*entryNode)->succNode.size() > 2) {
    if (isCaseWithDefault(Nodes, *entryNode, exitNode, entry)) {
      insertNode(nset, *entryNode);

      for (auto i = (*entryNode)->succNode.begin(),
                e = (*entryNode)->succNode.end();
           i != e; ++i) {
        insertNode(nset, *i);
      }

      return Case;
    } else if (isCaseWithoutDefault(Nodes, *entryNode, exitNode, entry)) {
      insertNode(nset, *entryNode);

      for (auto i = (*entryNode)->succNode.begin(),
                e = (*entryNode)->succNode.end();
           i != e; ++i) {
        if (*i != *exitNode) {
          insertNode(nset, *i);
        }
      }

      return Case;
    } else if (isImproperCaseWithDefault(Nodes, *entryNode, entry)) {
      return Improper;
    } else if (isImproperCaseWithoutDefault(Nodes, *entryNode, exitNode,
                                            entry)) {
      return Improper;
    }
  }

  return Nil;
}

// isCaseWithDefault - Check if node leads a case block
bool StructuralAnalysis::isCaseWithDefault(const NodeVecTy &Nodes,
                                           NodeTy *entryNode, NodeTy **exitNode,
                                           NodeTy *entry) {
  *exitNode = nullptr;

  for (auto i = entryNode->succNode.begin(), e = entryNode->succNode.end();
       i != e; ++i) {
    // Check if every successor node has only one successor
    if ((*i)->succNode.size() > 1)
      return false;

    if (edge2ClassMap[std::make_pair(entryNode, *i)] == BACK)
      return false;

    // If successor has only one predessor, it has to be the entry node
    if ((*i)->predNode.size() == 1) {
      if (entryNode != get_first_node_or_null((*i)->predNode))
        return false;
    }
    // If successor has two predessors, one has to be the entry node
    // and the other has to be another successor node
    else if ((*i)->predNode.size() == 2) {
      auto pi = (*i)->predNode.begin();
      NodeTy *predNode1 = *pi;
      ++pi;
      NodeTy *predNode2 = *pi;

      if (predNode1 != entryNode || !entryNode->has_successor(predNode2))
        if (!entryNode->has_successor(predNode1) || predNode2 != entryNode)
          return false;
    }
    // The predecessor node number has to be less than 3
    else
      return false;

    NodeTy *succNode = get_first_node_or_null((*i)->succNode);

    if (succNode == nullptr)
      continue;

    if (succNode == entryNode)
      return false;

    // Check if the successor of the successor node is not another successor
    if (!entryNode->has_successor(succNode)) {
      // Check if the successor of the successor is the only exit node
      if (!*exitNode)
        *exitNode = succNode;
      else if (*exitNode != succNode)
        return false;
    }
    // There is no loop between successors
    else if (succNode->has_successor(*i))
      return false;
  }

  for (auto i = entryNode->succNode.begin(), e = entryNode->succNode.end();
       i != e; ++i) {
    if ((*i)->succNode.empty()) {
      visitPath.clear();

      if (path(*exitNode, entryNode, Nodes, nullptr))
        return false;
    }
  }

  return true;
}

// isImproperCaseWithDefault - Check if node leads a case block
bool StructuralAnalysis::isImproperCaseWithDefault(const NodeVecTy &Nodes,
                                                   NodeTy *entryNode,
                                                   NodeTy *entry) {
  NodeTy *exitNode = nullptr;
  EdgeSetTy improperEdgeSet;

  for (auto i = entryNode->succNode.begin(), e = entryNode->succNode.end();
       i != e; ++i) {
    // Check if every successor node has only one successor
    if ((*i)->succNode.size() > 1)
      return false;

    if (edge2ClassMap[std::make_pair(entryNode, *i)] == BACK)
      return false;

    NodeTy *succNode = get_first_node_or_null((*i)->succNode);

    if (succNode) {
      if (succNode == entryNode)
        return false;

      // Check if the successor of the successor node
      // is not another successor node
      if (!entryNode->has_successor(succNode)) {
        // Is the successor of the successor node is the only exit node?
        if (!exitNode)
          exitNode = succNode;
        else if (exitNode != succNode)
          return false;
      }
      // There is no loop between successors
      else if (succNode->has_successor(*i))
        return false;
    }

    // If successor has only one predessor, it has to be the entry node
    if ((*i)->predNode.size() == 1) {
      if (entryNode != get_first_node_or_null((*i)->predNode))
        return false;
    }
    // If successor has two predessors, one has to be the entry node
    // and the other has to be another successor node
    else if ((*i)->predNode.size() == 2) {
      auto pi = (*i)->predNode.begin();
      NodeTy *predNode1 = *pi;
      ++pi;
      NodeTy *predNode2 = *pi;

      if (predNode1 != entryNode || !entryNode->has_successor(predNode2))
        if (!entryNode->has_successor(predNode1) || predNode2 != entryNode)
          return false;
    }
    // The predecessor node number has to be less than 3
    else {
      int insideIncomingNum = 0;

      for (auto pi = (*i)->predNode.begin(), pe = (*i)->predNode.end();
           pi != pe; ++pi) {

        if (edge2ClassMap[std::make_pair(*pi, *i)] != BACK && *pi != exitNode &&
            *pi != entryNode) {
          if (!entryNode->has_successor(*pi))
            improperEdgeSet.insert(std::make_pair(*pi, *i));
          else {
            insideIncomingNum++;

            if (insideIncomingNum > 1)
              improperEdgeSet.insert(std::make_pair(*pi, *i));
          }
        } else
          return false;
      }
    }
  }

  for (auto i = entryNode->succNode.begin(), e = entryNode->succNode.end();
       i != e; ++i) {
    if ((*i)->succNode.empty()) {
      visitPath.clear();

      if (path(exitNode, entryNode, Nodes, nullptr))
        return false;
    }
  }

  bool improperFlag = false;

  for (EdgeSetTy::iterator i = improperEdgeSet.begin(),
                           e = improperEdgeSet.end();
       i != e; ++i)
    if (isStillReachableFromEntry(Nodes, entry, i->second, i->first)) {
      findUnstructuredBR(i->first, i->second, true, true);
      improperFlag = true;
    }

  return improperFlag;
}

// isCaseWithoutDefault - Check if node leads a case block
bool StructuralAnalysis::isCaseWithoutDefault(const NodeVecTy &Nodes,
                                              NodeTy *entryNode,
                                              NodeTy **exitNode,
                                              NodeTy *entry) {
  // Find the exit node first
  *exitNode = nullptr;

  for (auto i = entryNode->succNode.begin(), e = entryNode->succNode.end();
       i != e; ++i) {
    NodeTy *node1 = *i;
    bool foundExit = true;

    // all of successors of exit node are not within the switch block
    for (auto si = node1->succNode.begin(), se = node1->succNode.end();
         si != se; ++si) {
      NodeTy *succNode = *si;

      if (succNode) {
        if (entryNode->has_successor(succNode)) {
          foundExit = false;

          break;
        } else if (succNode == entryNode)
          return false;
      }
    }

    if (!foundExit)
      continue;

    foundExit = false;

    // at least one of predcessors of exit node comes from switch block
    for (auto pi = node1->predNode.begin(), pe = node1->predNode.end();
         pi != pe; ++pi) {
      NodeTy *predNode = *pi;

      if (predNode != entryNode && entryNode->has_successor(predNode)) {
        foundExit = true;
      }
    }

    if (foundExit) {
      *exitNode = node1;

      break;
    }
  }

  if (!(*exitNode))
    return false;

  for (auto i = entryNode->succNode.begin(), e = entryNode->succNode.end();
       i != e; ++i) {
    if (*i == *exitNode)
      continue;

    // Check if every successor node has only one successor
    if ((*i)->succNode.size() > 1)
      return false;

    NodeTy *succNode = get_first_node_or_null((*i)->succNode);

    if (succNode) {
      if (succNode == nullptr)
        continue;

      if (edge2ClassMap[std::make_pair(entryNode, *i)] == BACK)
        return false;

      // The successor of the successor node should be the the another
      // successor node of node
      if (!entryNode->has_successor(succNode))
        return false;
      // There is no loop between successors
      else if (succNode != *exitNode && succNode->has_successor(*i))
        return false;
    }

    // If successor has only one predessor, it has to be the entry node
    if ((*i)->predNode.size() == 1) {
      if (entryNode != get_first_node_or_null((*i)->predNode))
        return false;
    }
    // If successor has two predessors, one has to be the entry node
    // and the other has to be another successor node
    else if ((*i)->predNode.size() == 2) {
      auto pi = (*i)->predNode.begin();
      NodeTy *predNode1 = *pi;
      ++pi;
      NodeTy *predNode2 = *pi;

      if (predNode1 != entryNode || !entryNode->has_successor(predNode2))
        if (!entryNode->has_successor(predNode1) || predNode2 != entryNode)
          return false;
    }
    // The predecessor node number has to be less than 3
    else
      return false;
  }

  for (auto i = entryNode->succNode.begin(), e = entryNode->succNode.end();
       i != e; ++i) {
    if ((*i)->succNode.empty()) {
      visitPath.clear();

      if (path(*exitNode, entryNode, Nodes, nullptr))
        return false;
    }
  }

  return true;
}

// isImproperCaseoutWithDefault - Check if node leads a case block with incoming
// edges
bool StructuralAnalysis::isImproperCaseWithoutDefault(const NodeVecTy &Nodes,
                                                      NodeTy *entryNode,
                                                      NodeTy **exitNode,
                                                      NodeTy *entry) {
  EdgeSetTy improperEdgeSet;

  // Find the exit node first
  *exitNode = nullptr;

  for (auto i = entryNode->succNode.begin(), e = entryNode->succNode.end();
       i != e; ++i) {
    NodeTy *node1 = *i;
    bool foundExit = true;

    // all of successors of exit node are not within the switch block
    for (auto si = node1->succNode.begin(), se = node1->succNode.end();
         si != se; ++si) {
      NodeTy *succNode = *si;

      if (succNode) {
        if (entryNode->has_successor(succNode)) {
          foundExit = false;

          break;
        } else if (succNode == entryNode)
          return false;
      }
    }

    if (!foundExit)
      continue;

    foundExit = false;

    // at least one of predcessors of exit node comes from switch block
    for (auto pi = node1->predNode.begin(), pe = node1->predNode.end();
         pi != pe; ++pi) {
      NodeTy *predNode = *pi;

      if (predNode != entryNode && entryNode->has_successor(predNode)) {
        foundExit = true;
      }
    }

    if (foundExit) {
      *exitNode = node1;

      break;
    }
  }

  if (!(*exitNode))
    return false;

  for (auto i = entryNode->succNode.begin(), e = entryNode->succNode.end();
       i != e; ++i) {
    if (*i == *exitNode)
      continue;

    // Check if every successor node has only one successor
    if ((*i)->succNode.size() > 1)
      return false;

    if (edge2ClassMap[std::make_pair(entryNode, *i)] == BACK)
      return false;

    NodeTy *succNode = get_first_node_or_null((*i)->succNode);

    if (succNode) {
      if (succNode == nullptr)
        continue;

      // The successor of the successor node should be the the another
      // successor node of node
      if (!entryNode->has_successor(succNode))
        return false;
      // There is no loop between successors
      else if (succNode != *exitNode && succNode->has_successor(*i))
        return false;
    }

    // If successor has only one predessor, it has to be the entry node
    if ((*i)->predNode.size() == 1) {
      if (entryNode != get_first_node_or_null((*i)->predNode))
        return false;
    }
    // If successor has two predessors, one has to be the entry node
    // and the other has to be another successor node
    else if ((*i)->predNode.size() == 2) {
      auto pi = (*i)->predNode.begin();
      NodeTy *predNode1 = *pi;
      ++pi;
      NodeTy *predNode2 = *pi;

      if (predNode1 != entryNode || !entryNode->has_successor(predNode2))
        if (!entryNode->has_successor(predNode1) || predNode2 != entryNode)
          return false;

      if (predNode1 == *exitNode)
        return false;

      if (predNode2 == *exitNode)
        return false;
    }
    // The predecessor node number has to be less than 3
    else {
      int insideIncomingNum = 0;

      for (auto pi = (*i)->predNode.begin(), pe = (*i)->predNode.end();
           pi != pe; ++pi) {

        if (edge2ClassMap[std::make_pair(*pi, *i)] != BACK &&
            (*i) != *exitNode && (*pi) != entryNode && (*pi) != *exitNode) {
          if (!entryNode->has_successor(*pi))
            improperEdgeSet.insert(std::make_pair(*pi, *i));
          else {
            insideIncomingNum++;

            if (insideIncomingNum > 1)
              improperEdgeSet.insert(std::make_pair(*pi, *i));
          }
        } else
          return false;
      }
    }
  }

  for (auto i = entryNode->succNode.begin(), e = entryNode->succNode.end();
       i != e; ++i) {
    if ((*i)->succNode.empty()) {
      visitPath.clear();

      if (path(*exitNode, entryNode, Nodes, nullptr))
        return false;
    }
  }

  bool improperFlag = false;

  for (EdgeSetTy::iterator i = improperEdgeSet.begin(),
                           e = improperEdgeSet.end();
       i != e; ++i) {
    if (isStillReachableFromEntry(Nodes, entry, i->second, i->first)) {
      findUnstructuredBR(i->first, i->second, true, true);
      improperFlag = true;
    }
  }

  return improperFlag;
}

// cyclicRegionType - Follow Fig 7.42 of Muchnick book
StructuralAnalysis::RegionTy StructuralAnalysis::cyclicRegionType(
    const NodeVecTy &Nodes, NodeVecTy &nset, NodeTy *loopHeaderNode,
    NodeTy *backEdgeNode, NodeTy **exitNode, NodeTy *entry) {
  // Check for a SelfLoop
  if (nset.size() == 1) {
    if (loopHeaderNode == backEdgeNode) {
      *exitNode = get_first_node_or_null(backEdgeNode->succNode);

      return SelfLoop;
    } else
      return Nil;
  }

  if (isImproper(Nodes, nset, loopHeaderNode, backEdgeNode, exitNode, entry))
    // It is an Improper region
    return Improper;

  if (nset.size() == 2) {
    if (backEdgeNode->succNode.size() == 1) {
      for (NodeTy *pred : loopHeaderNode->predNode) {
        if (pred != backEdgeNode) {
          if (edge2ClassMap[std::make_pair(pred, loopHeaderNode)] == BACK)
            return Nil;
        }
      }

      for (NodeTy *succ : loopHeaderNode->succNode) {
        if (succ != backEdgeNode) {
          if (edge2ClassMap[std::make_pair(loopHeaderNode, succ)] == BACK)
            return Nil;
        }
      }

      if (backEdgeNode->predNode.size() != 1 ||
          backEdgeNode->succNode.size() != 1)
        return Nil;

      return NaturalLoop;
    } else if (backEdgeNode->succNode.size() > 1) {
      for (NodeTy *pred : loopHeaderNode->predNode) {
        if (pred != backEdgeNode) {
          if (edge2ClassMap[std::make_pair(pred, loopHeaderNode)] == BACK)
            return Nil;
        }
      }

      for (NodeTy *succ : loopHeaderNode->succNode) {
        if (succ != backEdgeNode) {
          if (edge2ClassMap[std::make_pair(loopHeaderNode, succ)] == BACK)
            return Nil;
        }
      }

      if (backEdgeNode->predNode.size() != 1)
        return Nil;

      return NaturalLoop;
    }
  }

  return Nil;
}

// reduce - Follow Fig 7.43 of Muchnick book
StructuralAnalysis::NodeTy *
StructuralAnalysis::reduce(NodeVecTy &N, RegionTy rType, NodeVecTy &nodeSet,
                           NodeTy *entryNode, NodeTy *exitNode) {
  NodeTy *node = new NodeTy();

  node->isCombined = true;

  if (entryNode) {
    node->entryNode = entryNode;
    node->entryBB = findEntryBB(entryNode);
  }

  replace(N, node, nodeSet /*, addSelfEdge*/);

  node->isLoopHeader = false;
  node->loopExitNode = nullptr;
  node->isBackEdge = false;
  node->parentNode = nullptr;

  if (exitNode)
    node->exitBB = findEntryBB(exitNode);
  else
    node->exitBB = nullptr;

  for (auto &i : nodeSet) {
    findBB(i, node->containedBB);
  }

  node->nodeType = rType;

  return node;
}

// replace - Follow Fig 7.44 of Muchnick book
void StructuralAnalysis::replace(NodeVecTy &N, NodeTy *node,
                                 NodeVecTy &nodeSet /*, bool addSelfEdge*/) {
  // Link region node into abstract flowgraph, adjust the postorder traversal
  // and predecessor and successor functions, and augment the control tree
  compact(N, node, nodeSet /*, addSelfEdge*/);

  for (auto i = nodeSet.begin(), e = nodeSet.end(); i != e; ++i) {
    insertNode(node->childNode, *i);
    (*i)->parentNode = node;
  }
}

// isImproper - Follow Fig 7.45 of Muchnick book
bool StructuralAnalysis::isImproper(const NodeVecTy &Nodes, NodeVecTy &nset,
                                    NodeTy *loopHeaderNode,
                                    NodeTy *backEdgeNode, NodeTy **exitNode,
                                    NodeTy *entry) {
  bool improperFlag = false;

  // Check loopHeaderNode first
  for (auto i = loopHeaderNode->predNode.begin(),
            e = loopHeaderNode->predNode.end();
       i != e; ++i) {
    NodeTy *predNode = *i;

    if (edge2ClassMap[std::make_pair(predNode, loopHeaderNode)] == BACK) {
      if (!containsNode(nset, predNode) &&
          isStillReachableFromEntry(Nodes, entry, loopHeaderNode, predNode)) {
        findUnstructuredBR(predNode, loopHeaderNode, true, true);
        improperFlag = true;
      } else if (containsNode(nset, predNode) && predNode != backEdgeNode) {
        findUnstructuredBR(predNode, loopHeaderNode, false, false);
        improperFlag = true;
      }
    }
  }

  // Check the incoming edges
  for (auto i = nset.begin(), e = nset.end(); i != e; ++i) {
    NodeTy *node = *i;

    if (node != loopHeaderNode)
      for (auto ii = node->predNode.begin(), ee = node->predNode.end();
           ii != ee; ++ii) {
        if (!containsNode(nset, *ii)
            /*&& isStillReachableFromEntry(N, entry, node, *ii)*/) {
          improperFlag = true;

          findUnstructuredBR(*ii, node, false, true);
          deleteUnreachableNodes(const_cast<NodeVecTy&>(Nodes), entry);
        }
      }
  }

  EdgeSetTy exitEdgeSet;
  NodeTy *exitNodeOfHeader = nullptr;
  NodeTy *exitNodeOfBackEdge = nullptr;
  NodeTy *mainExitNode = nullptr;

  for (auto i = nset.begin(), e = nset.end(); i != e; ++i) {
    NodeTy *node = *i;

    for (auto ii = node->succNode.begin(), ee = node->succNode.end(); ii != ee;
         ++ii) {
      if (!containsNode(nset, *ii)) {
        exitEdgeSet.insert(std::make_pair(node, *ii));

        if (node == loopHeaderNode) {
          if (exitNodeOfHeader == nullptr)
            exitNodeOfHeader = *ii;
        } else if (node == backEdgeNode) {
          if (exitNodeOfBackEdge == nullptr)
            exitNodeOfBackEdge = *ii;
        }
      }
    }
  }

  if (exitNodeOfHeader)
    mainExitNode = exitNodeOfHeader;
  else if (exitNodeOfBackEdge)
    mainExitNode = exitNodeOfBackEdge;

  for (EdgeSetTy::iterator i = exitEdgeSet.begin(), e = exitEdgeSet.end();
       i != e; ++i) {
    EdgeTy exitEdge = *i;

    if (exitEdge.second != mainExitNode) {
      findUnstructuredBR(exitEdge.first, exitEdge.second, false, true);
      deleteUnreachableNodes(const_cast<NodeVecTy&>(Nodes), entry);
      improperFlag = true;
    }
  }

  if (exitNodeOfHeader) {
    for (EdgeSetTy::iterator i = exitEdgeSet.begin(), e = exitEdgeSet.end();
         i != e; ++i) {
      if (i->first != loopHeaderNode && (*i).second == mainExitNode) {
        findUnstructuredBR(i->first, i->second, false, false);
        improperFlag = true;
      }
    }
  } else if (exitNodeOfBackEdge) {
    for (EdgeSetTy::iterator i = exitEdgeSet.begin(), e = exitEdgeSet.end();
         i != e; ++i) {
      if (i->first != backEdgeNode && i->second == mainExitNode) {
        findUnstructuredBR(i->first, i->second, false, false);
        improperFlag = true;
      }
    }
  }

  *exitNode = mainExitNode;
  loopHeaderNode->isLoopHeader = true;
  backEdgeNode->isBackEdge = true;
  loopHeaderNode->loopExitNode = mainExitNode;

  return improperFlag;
}

// pathBack - Check if there is a node k such that there is a path from
// m to k that does not pass through n and an edge k->n that is a back edge
StructuralAnalysis::NodeTy *
StructuralAnalysis::pathBack(NodeTy *n, NodeVecTy &N, NodeVecTy &reachUnder) {
  NodeTy *backEdgeNode = nullptr;

  reachUnder.clear();

  // Find backedge first
  for (NodeTy *predNode : n->predNode) {
    if (edge2ClassMap[std::make_pair(predNode, n)] == BACK) {
      if (!containsNode(reachUnder, predNode)) {
        backEdgeNode = predNode;

        // Locate a cyclic region, if present
        reachUnder.clear();
        insertNode(reachUnder, n);
        insertNode(reachUnder, backEdgeNode);

        for (NodeTy *m : N) {
          // Check if there is a path from m to loop exit node
          visitPath.clear();
          if (path(m, backEdgeNode, N, n)) {
            visitPath.clear();

            if (path(n, m, N, backEdgeNode)) {
              insertNode(reachUnder, m);
            }
          }
        }
      }
    }
  }

  return backEdgeNode;
}

// path(n, m, I) - Return true if there is a path from from n to m
// such that all the nodes in it are in I and false otherwise
bool StructuralAnalysis::path(NodeTy *n, NodeTy *m, const NodeVecTy &Nodes,
                              NodeTy *esc) {
  visitPath.emplace(n);

  if (n == esc || m == esc)
    return false;

  if (n == m)
    return true;

  for (auto i = n->succNode.begin(), e = n->succNode.end(); i != e; ++i) {
    if (containsNode(Nodes, *i) && *i != esc && visitPath.count(*i) == 0) {
      if (*i == m) {
        return true;
      } else if (path(*i, m, Nodes, esc)) {
        return true;
      }
    }
  }

  return false;
}

// path(n, m, I, src, dst) - Return true if there is a path from from n to m
// such that all the nodes in it are in I without going through edge src->dst
// and false otherwise
bool StructuralAnalysis::path(NodeTy *n, NodeTy *m, const NodeVecTy &Nodes,
                              NodeTy *src, NodeTy *dst) {
  visitPath.emplace(n);

  if (n == m)
    return true;

  for (auto i = n->succNode.begin(), e = n->succNode.end(); i != e; ++i)
    if (containsNode(Nodes, *i) && visitPath.count(*i) == 0) {
      if (*i == dst && n == src)
        continue;

      if (*i == m)
        return true;
      else if (path(*i, m, Nodes, src, dst))
        return true;
    }

  return false;
}

// compact - Compact nodes in nset into n;
void StructuralAnalysis::compact(NodeVecTy &N, NodeTy *n,
                                 NodeVecTy &nset /*, bool addSelfEdge*/) {
  // Adds node n to N
  insertNode(N, n);

  // Remove the nodes in nset from both N and post()
  for (auto nset_node : nset) {
    for (auto succ : nset_node->succNode) {
      if (!containsNode(nset, succ)) {
        n->add_successor(succ);
        succ->add_predecessor(n);
        succ->remove_predecessor(nset_node);
      }
    }

    for (auto pred : nset_node->predNode) {
      if (!containsNode(nset, pred)) {
        n->add_predecessor(pred);
        pred->add_successor(n);
        pred->remove_successor(nset_node);
      }
    }

    eraseNode(N, nset_node);
  }
}

// mapNode2BB - Return the corresponding BasicBlock* of the node
BasicBlock *StructuralAnalysis::mapNode2BB(const NodeTy *node) const {
  const NodeTy *tmpNode = node;
  while (tmpNode->isCombined) {
    tmpNode = tmpNode->entryNode;
  }
  return tmpNode->BB;
}

// mapBB2Node - Return the corresponding sturcture node of the basic block
StructuralAnalysis::NodeTy *StructuralAnalysis::mapBB2Node(BasicBlock *bb) {
  NodeTy *node, *tmpNode;

  node = BB2NodeMap[bb];

  while ((tmpNode = node->parentNode) != nullptr)
    node = tmpNode;

  return node;
}

// dumpCTNode - dump one CT node
void StructuralAnalysis::dumpCTNode(llvm::raw_ostream &stream,
                                    NodeTy *node) const {
  if (!node->isCombined)
    return;

  stream << "\t";

  switch (node->nodeType) {
  case Block:
    stream << "Block      ";
    break;
  case IfThen:
    stream << "IfThen     ";
    break;
  case IfThenElse:
    stream << "IfThenElse ";
    break;
  case Case:
    stream << "Case       ";
    break;
  case SelfLoop:
    stream << "SelfLoop   ";
    break;
  case NaturalLoop:
    stream << "NaturalLoop";
    break;
  default:
    break;
  }

  stream << "\t";

  dumpNode(stream, node);
  stream << " -> exit: "
         << (node->exitBB != nullptr ? node->exitBB->getName().str() : "null");

  stream << '\n';

  for (auto i = node->childNode.begin(), e = node->childNode.end(); i != e;
       ++i) {
    dumpCTNode(stream, *i);
  }
}

// dumpNode - dump one node
void StructuralAnalysis::dumpNode(llvm::raw_ostream &stream,
                                  NodeTy *node) const {
  BBVecTy BBVec;

  BasicBlock *this_block = mapNode2BB(node);
  stream << "[" << this_block->getName().str() << "]\t";

  findBB(node, BBVec);
  for (BasicBlock *BB : BBVec) {
    if (BB == this_block)
      continue;
    stream << BB->getName().str() << "\t";
  }
}

// findUnstructuredBR - Record the branch and remove it from CFG
void StructuralAnalysis::findUnstructuredBR(NodeTy *srcNode, NodeTy *dstNode,
                                            bool needForwardCopy, bool isGoto) {
  BBVecTy srcNodeVec, dstNodeVec;
  findBB(srcNode, srcNodeVec);
  findBB(dstNode, dstNodeVec);

  for (BasicBlock *srcBB : srcNodeVec) {
    for (BasicBlock *succ : successors(srcBB)) {
      for (BasicBlock *dstBB : dstNodeVec) {
        if (dstBB == succ) {
          if (isGoto) {
            if (checkUnique(unstructuredBRVec, srcBB, dstBB)) {
              unstructuredBRVec.push_back(std::make_pair(srcBB, dstBB));
            }
          }

          if (needForwardCopy) {
            if (checkUnique(dstNode->incomingForwardBR, srcBB, dstBB)) {
              dstNode->incomingForwardBR.push_back(
                  std::make_pair(srcBB, dstBB));
            }
          }
        }
      }
    }
  }

  srcNode->remove_successor(dstNode);
  dstNode->remove_predecessor(srcNode);

  if (edge2ClassMap[std::make_pair(srcNode, dstNode)] == BACK) {
    dstNode->isLoopHeader = false;
    dstNode->loopExitNode = nullptr;
  }
}

// findBB - put all Basic Blocks in node into nodeVec
void StructuralAnalysis::findBB(NodeTy *node, BBVecTy &nodeVec) const {
  if (!node->isCombined) {
    if (find(nodeVec.cbegin(), nodeVec.cend(), node->BB) != nodeVec.cend())
      return;
    nodeVec.emplace_back(node->BB);
  } else {
    for (NodeTy *child : node->childNode)
      findBB(child, nodeVec);
  }
}

// dumpUnstructuredBR - Dump all found unstructured branches
void StructuralAnalysis::dumpUnstructuredBR(llvm::raw_ostream &stream) const {
  stream << "\nUnstructured Branches:\n";

  for (EdgeVecTy::const_iterator i = unstructuredBRVec.begin(),
                                 e = unstructuredBRVec.end();
       i != e; ++i) {
    stream << "\t" << i->first->getName().str() << "\t"
           << i->second->getName().str() << "\n";
  }

  stream << "\n";
}

void StructuralAnalysis::dumpForwardCopy(llvm::raw_ostream &stream) const {
  stream << "\nneed forward copy:\n";

  const std::function<void(const NodeTy *)> node_iter =
      [&node_iter, &stream](const NodeTy *node) {
        for (const auto &edge : node->incomingForwardBR) {
          stream << "\t" << edge.first->getName().str() << "\t"
                 << edge.second->getName().str() << "\n";
        }

        for (const auto &child : node->childNode) {
          node_iter(child);
        }
      };

  if (!Net.empty()) {
    node_iter(Net.front());
  }

  stream << "\n";
}

// True if after erasing edge src->dst, dst is still reachable from entry
bool StructuralAnalysis::isStillReachableFromEntry(const NodeVecTy &Nodes,
                                                   NodeTy *entry,
                                                   NodeTy *dstNode,
                                                   NodeTy *srcNode) {
  visitPath.clear();

  return path(entry, dstNode, Nodes, srcNode, dstNode);
}

// findEntryBB - find the entry Basic Block of the node
BasicBlock *StructuralAnalysis::findEntryBB(NodeTy *node) {
  if (!node->isCombined)
    return node->BB;
  else
    return findEntryBB(node->entryNode);
}

void StructuralAnalysis::cleanupUnreachable() {
  for (auto i = unreachableNodeSet.begin(), e = unreachableNodeSet.end();
       i != e; ++i) {
    cleanup(*i);
  }
}

// clean - fill in the element of incoming branches and outgoing branches
void StructuralAnalysis::cleanup(NodeTy *node) {
  if (!node->isCombined)
    return;
  else {
    if ((node->nodeType == NaturalLoop || node->nodeType == SelfLoop) &&
        node->containedBB.size() > 1) {
      for (auto i = node->containedBB.begin(), e = node->containedBB.end();
           i != e; ++i) {
        BasicBlock *BB = *i;

        if (BB != node->entryBB) {
          for (BasicBlock *Pred : predecessors(BB)) {
            if (!containsBB(node->containedBB, Pred)) {
              node->incomingBR.push_back(std::make_pair(Pred, BB));
            }
          }

          for (BasicBlock *Succ : successors(BB)) {
            if (!containsBB(node->containedBB, Succ) && Succ != node->exitBB) {
              node->outgoingBR.push_back(std::make_pair(BB, Succ));
            }
          }
        }
      }
    }

    NodeVecTy nodeSet = node->childNode;

    for (auto i = nodeSet.begin(), e = nodeSet.end(); i != e; ++i) {
      cleanup(*i);
    }
  }
}

// deleteUnreachableNode - delete nodes that are no longer reachable from the
// entry
void StructuralAnalysis::deleteUnreachableNodes(NodeVecTy &Nodes,
                                                NodeTy *entry) {
  for (auto iter = Nodes.begin(); iter != Nodes.end();) {
    visitPath.clear();
    NodeTy *node = *iter;

    if (!path(entry, node, Nodes, nullptr)) {
      for (auto pi = node->predNode.begin(), pe = node->predNode.end();
           pi != pe; ++pi) {
        (*pi)->remove_successor(node);
      }

      for (auto si = node->succNode.begin(), se = node->succNode.end();
           si != se; ++si) {
        (*si)->remove_predecessor(node);
      }

      insertNode(unreachableNodeSet, node);

      iter = Nodes.erase(iter);
    } else
      ++iter;
  }
}

void StructuralAnalysis::reconstructUnreachable() {
BEGIN:
  bool merge = false;

  for (auto i = unreachableNodeSet.begin(), e = unreachableNodeSet.end();
       i != e; ++i) {
    NodeTy *node1 = *i;

    for (auto ii = unreachableNodeSet.begin(), ee = unreachableNodeSet.end();
         ii != ee; ++ii) {
      NodeTy *node2 = *i;

      if (node1 == node2)
        continue;

      for (auto pi = node1->predNode.begin(), pe = node1->predNode.end();
           pi != pe; ++pi) {
        NodeTy *pred = *pi;

        if ((pred->isCombined &&
             containsBB(node2->containedBB, pred->entryBB)) ||
            (!pred->isCombined && containsBB(node2->containedBB, pred->BB))) {
          pred->add_successor(node1);
          merge = true;
        }
      }

      for (auto si = node1->succNode.begin(), se = node1->succNode.end();
           si != se; ++si) {
        NodeTy *succ = *si;

        if ((succ->isCombined &&
             containsBB(node2->containedBB, succ->entryBB)) ||
            (!succ->isCombined && containsBB(node2->containedBB, succ->BB))) {
          succ->add_predecessor(node1);
          merge = true;
        }
      }

      if (merge) {
        NodeVecTy nodeSet;

        insertNode(nodeSet, node1);
        insertNode(nodeSet, node2);

        reduce(unreachableNodeSet, Unreachable, nodeSet, nullptr, nullptr);

        goto BEGIN;
      }
    }
  }
}

StructuralAnalysis::~StructuralAnalysis() {
  for (auto n = Net.begin(); n != Net.end(); ++n) {
    delete *n;
  }
}

void StructuralAnalysis::analyze(Function &F) {
  _function = &F;

  // build a Simple CFG out of the LLVM CFG
  buildSimpleCFG(Net);

  NodeTy *entry = BB2NodeMap[&_function->getEntryBlock()];

  // Follow the Fig 7.39 of Muchnick book
  structuralAnalysis(Net, entry);

  if (!Net.empty()) {
    cleanup(*(Net.begin()));
  }

  reconstructUnreachable();

  cleanupUnreachable();
}

void StructuralAnalysis::write(llvm::raw_ostream &stream) const {
  stream << _function->getName().str() << ":\n";

  if (!Net.empty()) {
    dumpCTNode(stream, *(Net.begin()));
  }

  dumpUnstructuredBR(stream);
  dumpForwardCopy(stream);
}

bool StructuralAnalysis::checkUnique(EdgeVecTy &edgeVec, BasicBlock *srcBB,
                                     BasicBlock *dstBB) {
  for (EdgeVecTy::iterator i = edgeVec.begin(), e = edgeVec.end(); i != e;
       ++i) {

    if (i->first == srcBB && i->second == dstBB)
      return false;
  }

  return true;
}

bool StructuralAnalysis::containsNode(const NodeVecTy &Nodes,
                                      const NodeTy *node) {
  return find(cbegin(Nodes), cend(Nodes), node) != cend(Nodes);
}

bool StructuralAnalysis::containsBB(const BBVecTy &BBs, const BasicBlock *BB) {
  return find(cbegin(BBs), cend(BBs), BB) != cend(BBs);
}

bool StructuralAnalysis::insertNode(NodeVecTy &Nodes, NodeTy *node) {
  if (containsNode(Nodes, node)) {
    return false;
  }
  Nodes.emplace_back(node);
  return true;
}

bool StructuralAnalysis::eraseNode(NodeVecTy &Nodes, NodeTy *node) {
  const auto iter = find(cbegin(Nodes), cend(Nodes), node);
  if (iter == cend(Nodes)) {
    return false;
  }
  Nodes.erase(iter);
  return true;
}
}
