//===- InlineEverything.cpp - Code to inline all functions ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements a custom inliner that inlines everything, unless it was
// marked "noinline".
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/PassRegistry.h"
#include "llvm/Transforms/IPO/Inliner.h"

using namespace llvm;

#define DEBUG_TYPE "inline"

namespace {

/// \brief Inliner pass which inlines everything unless it was marked "noinline".
class EverythingInliner : public LegacyInlinerBase {

public:
  EverythingInliner() : LegacyInlinerBase(ID, /*InsertLifetime*/ true) {
    initializeEverythingInlinerPass(*PassRegistry::getPassRegistry());
  }

  EverythingInliner(bool InsertLifetime)
      : LegacyInlinerBase(ID, InsertLifetime) {
    initializeEverythingInlinerPass(*PassRegistry::getPassRegistry());
  }

  /// Main run interface method.  We override here to avoid calling skipSCC().
  bool runOnSCC(CallGraphSCC &SCC) override { return inlineCalls(SCC); }

  static char ID; // Pass identification, replacement for typeid

  InlineCost getInlineCost(CallBase &CB) override;

  using llvm::Pass::doFinalization;
  bool doFinalization(CallGraph &CG) override {
    return removeDeadFunctions(CG, /*AlwaysInlineOnly=*/ false);
  }
};

}

char EverythingInliner::ID = 0;
INITIALIZE_PASS_BEGIN(EverythingInliner, "everything-inline",
					  "everything inliner", false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(EverythingInliner, "everything-inline",
                    "everything inliner", false, false)

Pass *llvm::createEverythingInlinerPass() { return new EverythingInliner(); }

Pass *llvm::createEverythingInlinerPass(bool InsertLifetime) {
  return new EverythingInliner(InsertLifetime);
}

InlineCost EverythingInliner::getInlineCost(CallBase &CB) {
  Function *Callee = CB.getCalledFunction();

  if (Callee && !Callee->isDeclaration() &&
      (CB.hasFnAttr(Attribute::NoInline) ||
	   Callee->hasFnAttribute(Attribute::NoInline) ||
	   !isInlineViable(*Callee).isSuccess())) {
    return InlineCost::getNever("everything inliner");
  }

  return InlineCost::getAlways("everything inliner");
}
