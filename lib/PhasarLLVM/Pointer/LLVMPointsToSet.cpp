/******************************************************************************
 * Copyright (c) 2020 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

#include <cassert>
#include <iostream>
#include <fstream>
#include <memory>
#include <type_traits>
#include <unordered_set>

#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/ErrorHandling.h"

#include "phasar/DB/ProjectIRDB.h"
#include "phasar/PhasarLLVM/Pointer/LLVMBasedPointsToAnalysis.h"
#include "phasar/PhasarLLVM/Pointer/LLVMPointsToSet.h"
#include "phasar/PhasarLLVM/Pointer/LLVMPointsToUtils.h"
#include "phasar/Utils/LLVMShorthands.h"
#include "phasar/Utils/Logger.h"

using namespace std;
using namespace psr;

namespace psr {

void traverseIRDB(ProjectIRDB &IRDB,
                 const std::function<void(const llvm::Value*)>&ValueFunc)
{
  for (llvm::Module *M : IRDB.getAllModules()) {
    for (auto &G : M->globals()) {
      ValueFunc(&G);
    }
    for (auto &F : M->functions()) {
      ValueFunc(&F);
      for (auto &I : F.args()) {
        if (I.getType()->isPointerTy()) {
          ValueFunc(&I);
        }
      }
      for (llvm::inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        ValueFunc(&*I);
      }
    }
  }
}

LLVMPointsToSet::LLVMPointsToSet(ProjectIRDB &IRDB, bool UseLazyEvaluation,
                                 PointerAnalysisType PATy)
{
  PTA = std::make_shared<LLVMBasedPointsToAnalysis>(IRDB, UseLazyEvaluation, PATy);

  for (llvm::Module *M : IRDB.getAllModules()) {
    // compute points-to information for all globals
    for (const auto &G : M->globals()) {
      computeValuesPointsToSet(&G);
    }
    for (const auto &F : M->functions()) {
      computeValuesPointsToSet(&F);
    }
    if (!UseLazyEvaluation) {
      // compute points-to information for all functions
      for (auto &F : *M) {
        if (!F.isDeclaration()) {
          computeFunctionsPointsToSet(&F);
        }
      }
    }
  }
}

LLVMPointsToSet::LLVMPointsToSet(ProjectIRDB &IRDB,
                                 const std::string& PointsToSetFile)
{
  load(PointsToSetFile, IRDB);
}

void LLVMPointsToSet::save(const std::string &PointsToSetFile, ProjectIRDB &IRDB) {
  std::unordered_map<const llvm::Value *, unsigned long> ValueToIdMap;
  std::ofstream OS(PointsToSetFile);


  // ValueIds segment is just for debug. It will not been loaded.
  OS << "[ValueIds]" << std::endl;
  unsigned int NextID = 0;

  // Traverse all values in the IRDB in a fixed order, and assign ID to each value.
  traverseIRDB(IRDB, [&NextID, &ValueToIdMap, &OS](const llvm::Value *V) {
    ValueToIdMap[V] = NextID;
    OS << NextID << ": " << llvmIRToString(V) << std::endl;
    ++NextID;
  });

  std::set<std::shared_ptr<std::unordered_set<const llvm::Value *>>> Printed;

  OS << "[AnalyzedFunctions]" << std::endl;
  for (auto &F : AnalyzedFunctions) {
    OS << ValueToIdMap[F] << " ";
  }
  OS << std::endl;

  OS << "[PointsToSets]" << std::endl;
  for (auto &[_, S] : PointsToSets) {
    if (Printed.find(S) != Printed.end()) {
      // Print each set only once
      continue;
    }

    Printed.insert(S);
    for (const auto &V : *S) {
      OS << ValueToIdMap[V] << " ";
    }

    OS << std::endl;
  }
}

void LLVMPointsToSet::load(const std::string &PointsToSetFile, ProjectIRDB &IRDB) {
  std::ifstream IS(PointsToSetFile);
  std::vector<const llvm::Value *> IdToValueMap;

  traverseIRDB(IRDB, [&IdToValueMap](const llvm::Value *V) {
    IdToValueMap.push_back(V);
  });

  std::string Line;
  std::shared_ptr<std::unordered_set<const llvm::Value *>> PointersSet;
  while(std::getline(IS, Line)) {
    if (Line == "[AnalyzedFunctions]") {
      break;
    }
  }

  while(std::getline(IS, Line)) {
    if (Line == "[PointsToSets]") {
      break;
    }

    std::string Cell;
    std::istringstream LineStream(Line);
    while(std::getline(LineStream, Cell, ' ')) {
      unsigned long ID = atol(Cell.c_str());
      const llvm::Function* Func = llvm::dyn_cast<llvm::Function>(IdToValueMap[ID]);
      AnalyzedFunctions.insert(Func);
    }
  }

  while(std::getline(IS, Line)) {
    std::string Cell;
    std::istringstream LineStream(Line);
    PointersSet = make_shared<std::unordered_set<const llvm::Value *>>();
    while(std::getline(LineStream, Cell, ' ')) {
      unsigned long ID = atol(Cell.c_str());
      const llvm::Value* value = IdToValueMap[ID];
      PointersSet->insert(value);
      PointsToSets[value] = PointersSet;
    }
  }
}

void LLVMPointsToSet::computeValuesPointsToSet(const llvm::Value *V) {
  if (!isInterestingPointer(V)) {
    // don't need to do anything
    return;
  }
  // Add set for the queried value if none exists, yet
  addSingletonPointsToSet(V);
  if (const auto *G = llvm::dyn_cast<llvm::GlobalObject>(V)) {
    // A global object can be a function or a global variable. We need to
    // consider functions here, too, because function pointer magic may be
    // used by the target program. Add a set for global object.
    // A global object may be used in multiple functions.
    for (const auto *User : G->users()) {
      if (const auto *Inst = llvm::dyn_cast<llvm::Instruction>(User)) {
        // The may be no corresponding function when the instruction is used in
        // a vtable, for instance.
        if (Inst->getParent()) {
          computeFunctionsPointsToSet(
              const_cast<llvm::Function *>(Inst->getFunction()));
          if (!llvm::isa<llvm::Function>(G) && isInterestingPointer(User)) {
            mergePointsToSets(User, G);
          } else if (const auto *Store =
                         llvm::dyn_cast<llvm::StoreInst>(User)) {
            if (isInterestingPointer(Store->getValueOperand())) {
              // Store->getPointerOperand() doesn't require checking: it is
              // always an interesting pointer
              mergePointsToSets(Store->getValueOperand(),
                                Store->getPointerOperand());
            }
          }
        }
      }
    }
  } else {
    const auto *VF = retrieveFunction(V);
    computeFunctionsPointsToSet(const_cast<llvm::Function *>(VF));
  }
}

void LLVMPointsToSet::addSingletonPointsToSet(const llvm::Value *V) {
  if (PointsToSets.find(V) != PointsToSets.end()) {
    PointsToSets[V]->insert(V);
  } else {
    PointsToSets[V] = std::make_shared<std::unordered_set<const llvm::Value *>>(
        std::unordered_set<const llvm::Value *>{V});
  }
}

void LLVMPointsToSet::mergePointsToSets(const llvm::Value *V1,
                                        const llvm::Value *V2) {
  auto SearchV1 = PointsToSets.find(V1);
  assert(SearchV1 != PointsToSets.end());
  auto SearchV2 = PointsToSets.find(V2);
  assert(SearchV2 != PointsToSets.end());
  const auto *V1Ptr = SearchV1->first;
  const auto *V2Ptr = SearchV2->first;
  if (V1Ptr == V2Ptr) {
    return;
  }
  auto V1Set = SearchV1->second;
  auto V2Set = SearchV2->second;
  // check if we need to merge the sets
  if (V1Set->find(V2) != V1Set->end()) {
    return;
  }
  std::shared_ptr<std::unordered_set<const llvm::Value *>> SmallerSet;
  std::shared_ptr<std::unordered_set<const llvm::Value *>> LargerSet;
  if (V1Set->size() <= V2Set->size()) {
    SmallerSet = V1Set;
    LargerSet = V2Set;
  } else {
    SmallerSet = V2Set;
    LargerSet = V1Set;
  }
  // add smaller set to larger one
  LargerSet->insert(SmallerSet->begin(), SmallerSet->end());
  // reindex the contents of the smaller set
  for (const auto *Ptr : *SmallerSet) {
    PointsToSets[Ptr] = LargerSet;
  }
  // get rid of the smaller set
  SmallerSet->clear();
}

bool LLVMPointsToSet::interIsReachableAllocationSiteTy(const llvm::Value *V,
                                                       const llvm::Value *P) {
  // consider the full inter-procedural points-to/alias information

  if (llvm::isa<llvm::AllocaInst>(P)) {
    return true;
  }
  if (llvm::isa<llvm::CallInst>(P) || llvm::isa<llvm::InvokeInst>(P)) {
    const llvm::CallBase *CS = llvm::dyn_cast<llvm::CallBase>(P);
    if (CS->getCalledFunction() != nullptr &&
        CS->getCalledFunction()->hasName() &&
        HeapAllocatingFunctions.count(CS->getCalledFunction()->getName())) {
      return true;
    }
  }

  return false;
}

bool LLVMPointsToSet::intraIsReachableAllocationSiteTy(
    const llvm::Value *V, const llvm::Value *P, const llvm::Function *VFun,
    const llvm::GlobalObject *VG) {
  // consider the function-local, i.e. intra-procedural, points-to/alias
  // information only

  // We may not be able to retrieve a function for the given value since some
  // pointer values can exist outside functions, for instance, in case of
  // vtables, etc.
  if (const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(P)) {
    // only add function local allocation sites
    if ((VFun && VFun == Alloca->getFunction())) {
      return true;
    }
    if (VG) {
      return true;
    }
  } else if (llvm::isa<llvm::CallInst>(P) || llvm::isa<llvm::InvokeInst>(P)) {
    const llvm::CallBase *CS = llvm::dyn_cast<llvm::CallBase>(P);
    if (CS->getCalledFunction() != nullptr &&
        CS->getCalledFunction()->hasName() &&
        HeapAllocatingFunctions.count(CS->getCalledFunction()->getName())) {
      if (VFun && VFun == CS->getFunction()) {
        return true;
      } else if (VG) {
        return true;
      }
    }
  }

  return false;
}

void LLVMPointsToSet::computeFunctionsPointsToSet(llvm::Function *F) {
  // F may be null
  if (!F) {
    return;
  }
  // check if we already analyzed the function
  if (AnalyzedFunctions.find(F) != AnalyzedFunctions.end()) {
    return;
  }
  LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), DEBUG)
                << "Analyzing function: " << F->getName().str());
  AnalyzedFunctions.insert(F);
  llvm::AAResults &AA = *PTA->getAAResults(F);
  bool EvalAAMD = true;

  // taken from llvm/Analysis/AliasAnalysisEvaluator.cpp
  const llvm::DataLayout &DL = F->getParent()->getDataLayout();

  llvm::SetVector<llvm::Value *> Pointers;
  llvm::SmallSetVector<llvm::CallBase *, 16> Calls;
  llvm::SetVector<llvm::Value *> Loads;
  llvm::SetVector<llvm::Value *> Stores;

  for (auto &I : F->args()) {
    if (I.getType()->isPointerTy()) { // Add all pointer arguments.
      Pointers.insert(&I);
    }
  }

  for (llvm::inst_iterator I = inst_begin(*F), E = inst_end(*F); I != E; ++I) {
    if (I->getType()->isPointerTy()) { // Add all pointer instructions.
      Pointers.insert(&*I);
    }
    if (EvalAAMD && llvm::isa<llvm::LoadInst>(&*I)) {
      Loads.insert(&*I);
    }
    if (EvalAAMD && llvm::isa<llvm::StoreInst>(&*I)) {
      Stores.insert(&*I);
      auto *Store = llvm::cast<llvm::StoreInst>(&*I);
      auto *SVO = Store->getValueOperand();
      auto *SPO = Store->getPointerOperand();
      if (SVO->getType()->isPointerTy()) {
        if (llvm::isa<llvm::Function>(SVO)) {
          addSingletonPointsToSet(SVO);
          addSingletonPointsToSet(SPO);
          mergePointsToSets(SVO, SPO);
        }
        if (auto *SVOCE = llvm::dyn_cast<llvm::ConstantExpr>(SVO)) {

          std::unique_ptr<llvm::Instruction, decltype(&deleteValue)> AsI(
              SVOCE->getAsInstruction(), &deleteValue);
          if (auto *BC = llvm::dyn_cast<llvm::BitCastInst>(AsI.get())) {
            auto *RHS = BC->getOperand(0);
            addSingletonPointsToSet(RHS);
            addSingletonPointsToSet(SVOCE);
            addSingletonPointsToSet(SPO);
            mergePointsToSets(RHS, SPO);
            mergePointsToSets(SVOCE, SPO);
          }
        }
      }
    }
    llvm::Instruction &Inst = *I;
    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(&Inst)) {
      llvm::Value *Callee = Call->getCalledOperand();
      // Skip actual functions for direct function calls.
      if (!llvm::isa<llvm::Function>(Callee) && isInterestingPointer(Callee)) {
        Pointers.insert(Callee);
      }
      // Consider formals.
      for (llvm::Use &DataOp : Call->data_ops()) {
        if (isInterestingPointer(DataOp)) {
          Pointers.insert(DataOp);
        }
      }
      Calls.insert(Call);
    } else {
      // Consider all operands.
      for (llvm::Instruction::op_iterator OI = Inst.op_begin(),
                                          OE = Inst.op_end();
           OI != OE; ++OI) {
        if (isInterestingPointer(*OI)) {
          Pointers.insert(*OI);
        }
      }
    }
  }
  // Consider globals
  for (auto &Global : F->getParent()->globals()) {
    if (auto *GlobalVariable = llvm::dyn_cast<llvm::GlobalVariable>(&Global)) {
      Pointers.insert(GlobalVariable);
    }
  }
  // introduce a singleton set for each pointer
  // those sets will be merged as we discover aliases
  for (auto *Pointer : Pointers) {
    addSingletonPointsToSet(Pointer);
  }

  const int kWarningPointers = 100;
  if (Pointers.size() > kWarningPointers) {
    LOG_IF_ENABLE(BOOST_LOG_SEV(lg::get(), WARNING)
                  << "Large number of pointers detected - Perf is O(N^2) here: "
                  << Pointers.size() << " for "
                  << llvm::demangle(F->getName().str()));
  }

  // iterate over the worklist, and run the full (n^2)/2 disambiguations
  for (auto I1 = Pointers.begin(), E = Pointers.end(); I1 != E; ++I1) {
    llvm::Type *I1ElTy =
        llvm::cast<llvm::PointerType>((*I1)->getType())->getElementType();
    const uint64_t I1Size = I1ElTy->isSized()
                                ? DL.getTypeStoreSize(I1ElTy)
                                : llvm::MemoryLocation::UnknownSize;
    for (auto I2 = Pointers.begin(); I2 != I1; ++I2) {
      llvm::Type *I2ElTy =
          llvm::cast<llvm::PointerType>((*I2)->getType())->getElementType();
      const uint64_t I2Size = I2ElTy->isSized()
                                  ? DL.getTypeStoreSize(I2ElTy)
                                  : llvm::MemoryLocation::UnknownSize;
      switch (AA.alias(*I1, I1Size, *I2, I2Size)) {
      case llvm::AliasResult::NoAlias:
        // both pointers already have corresponding points-to sets, we are
        // fine
        break;
      case llvm::AliasResult::MayAlias: // NOLINT
        [[fallthrough]];
      case llvm::AliasResult::PartialAlias: // NOLINT
        [[fallthrough]];
      case llvm::AliasResult::MustAlias:
        // merge points to sets
        mergePointsToSets(*I1, *I2);
        break;
      }
    }
  }
  // we no longer need the LLVM representation
  PTA->erase(F);
}

AliasResult LLVMPointsToSet::alias(const llvm::Value *V1, const llvm::Value *V2,
                                   const llvm::Instruction *I) {
  // if V1 or V2 is not an interesting pointer those values cannot alias
  if (!isInterestingPointer(V1) || !isInterestingPointer(V2)) {
    return AliasResult::NoAlias;
  }
  computeValuesPointsToSet(V1);
  computeValuesPointsToSet(V2);
  return PointsToSets[V1]->count(V2) ? AliasResult::MustAlias
                                     : AliasResult::NoAlias;
}

std::shared_ptr<std::unordered_set<const llvm::Value *>>
LLVMPointsToSet::getPointsToSet(const llvm::Value *V,
                                const llvm::Instruction *I) {
  // if V is not a (interesting) pointer we can return an empty set
  if (!isInterestingPointer(V)) {
    return std::make_shared<std::unordered_set<const llvm::Value *>>();
  }
  // compute V's points-to set
  computeValuesPointsToSet(V);
  if (PointsToSets.find(V) == PointsToSets.end()) {
    // if we still can't find its value return an empty set
    return std::make_shared<std::unordered_set<const llvm::Value *>>();
  }
  return PointsToSets[V];
}

std::shared_ptr<std::unordered_set<const llvm::Value *>>
LLVMPointsToSet::getReachableAllocationSites(const llvm::Value *V,
                                             bool IntraProcOnly,
                                             const llvm::Instruction *I) {
  // if V is not a (interesting) pointer we can return an empty set
  if (!isInterestingPointer(V)) {
    return std::make_shared<std::unordered_set<const llvm::Value *>>();
  }
  computeValuesPointsToSet(V);
  auto AllocSites = std::make_shared<std::unordered_set<const llvm::Value *>>();
  const auto PTS = PointsToSets[V];
  // consider the full inter-procedural points-to/alias information
  if (!IntraProcOnly) {
    for (const auto *P : *PTS) {
      if (interIsReachableAllocationSiteTy(V, P)) {
        AllocSites->insert(P);
      }
    }
  } else {
    // consider the function-local, i.e. intra-procedural, points-to/alias
    // information only
    const auto *VFun = retrieveFunction(V);
    const auto *VG = llvm::dyn_cast<llvm::GlobalObject>(V);
    // We may not be able to retrieve a function for the given value since some
    // pointer values can exist outside functions, for instance, in case of
    // vtables, etc.
    for (const auto *P : *PTS) {
      if (intraIsReachableAllocationSiteTy(V, P, VFun, VG)) {
        AllocSites->insert(P);
      }
    }
  }
  return AllocSites;
}

bool LLVMPointsToSet::isInReachableAllocationSites(
    const llvm::Value *V, const llvm::Value *PotentialValue, bool IntraProcOnly,
    const llvm::Instruction *I) {
  // if V is not a (interesting) pointer we can return an empty set
  if (!isInterestingPointer(V)) {
    return false;
  }
  computeValuesPointsToSet(V);

  bool PVIsReachableAllocationSiteType = false;
  if (IntraProcOnly) {
    const auto *VFun = retrieveFunction(V);
    const auto *VG = llvm::dyn_cast<llvm::GlobalObject>(V);
    PVIsReachableAllocationSiteType =
        intraIsReachableAllocationSiteTy(V, PotentialValue, VFun, VG);
  } else {
    PVIsReachableAllocationSiteType =
        interIsReachableAllocationSiteTy(V, PotentialValue);
  }

  if (PVIsReachableAllocationSiteType) {
    const auto PTS = PointsToSets[V];
    return PTS->count(PotentialValue);
  }

  return false;
}

void LLVMPointsToSet::mergeWith(const PointsToInfo &PTI) {
  const auto *OtherPTI = dynamic_cast<const LLVMPointsToSet *>(&PTI);
  if (!OtherPTI) {
    llvm::report_fatal_error(
        "LLVMPointsToSet can only be merged with another LLVMPointsToSet!");
  }
  // merge analyzed functions
  AnalyzedFunctions.insert(OtherPTI->AnalyzedFunctions.begin(),
                           OtherPTI->AnalyzedFunctions.end());
  // merge points-to sets
  for (const auto &[KeyPtr, Set] : OtherPTI->PointsToSets) {
    bool FoundElemPtr = false;
    for (const auto *ElemPtr : *Set) {
      // check if a pointer of other is already present in this
      auto Search = PointsToSets.find(ElemPtr);
      if (Search != PointsToSets.end()) {
        // if so, copy its elements
        FoundElemPtr = true;
        Search->second->insert(Set->begin(), Set->end());
        // and reindex its elements
        for (const auto *ElemPtr : *Set) {
          PointsToSets.insert({ElemPtr, Search->second});
        }
        break;
      }
    }
    // if none of the pointers of a set of other is known in this, we need to
    // perform a copy
    if (!FoundElemPtr) {
      PointsToSets.insert(
          {KeyPtr,
           std::make_shared<std::unordered_set<const llvm::Value *>>(*Set)});
    }
  }
}

void LLVMPointsToSet::introduceAlias(const llvm::Value *V1,
                                     const llvm::Value *V2,
                                     const llvm::Instruction *I,
                                     AliasResult Kind) {
  //  only introduce aliases if both values are interesting pointer
  if (!isInterestingPointer(V1) || !isInterestingPointer(V2)) {
    return;
  }
  // before introducing additional aliases make sure we initially computed
  // the aliases for V1 and V2
  computeValuesPointsToSet(V1);
  computeValuesPointsToSet(V2);
  mergePointsToSets(V1, V2);
}

nlohmann::json LLVMPointsToSet::getAsJson() const { return ""_json; }

void LLVMPointsToSet::printAsJson(std::ostream &OS) const {}

void LLVMPointsToSet::print(std::ostream &OS) const {
  for (const auto &[V, PTS] : PointsToSets) {
    OS << "V: " << llvmIRToString(V) << '\n';
    for (const auto &Ptr : *PTS) {
      OS << "\tpoints to -> " << llvmIRToString(Ptr) << '\n';
    }
  }
}

void LLVMPointsToSet::peakIntoPointsToSet(
    const PointsToSetMap::value_type &ValueSetPair, int Peak) {
  llvm::outs() << "Value: ";
  ValueSetPair.first->print(llvm::outs());
  llvm::outs() << '\n';
  int PeakCounter = 0;
  llvm::outs() << "aliases with: {\n";
  for (const llvm::Value *I : *ValueSetPair.second) {
    I->print(llvm::outs());
    llvm::outs() << '\n';
    PeakCounter++;
    if (PeakCounter > Peak) {
      llvm::outs() << llvm::formatv("... and {0} more\n",
                                    ValueSetPair.second->size() - Peak);
      break;
    }
  }
  llvm::outs() << "}\n";
}

void LLVMPointsToSet::drawPointsToSetsDistribution(int Peak) const {
  std::vector<std::pair<size_t, unsigned>> SizeAmountPairs;

  for (const auto &ValueSetPair : PointsToSets) {
    auto Search =
        std::find_if(SizeAmountPairs.begin(), SizeAmountPairs.end(),
                     [&ValueSetPair](const auto &Entry) {
                       return Entry.first == ValueSetPair.second->size();
                     });
    if (Search != SizeAmountPairs.end()) {
      Search->second++;
    } else {
      SizeAmountPairs.emplace_back(ValueSetPair.second->size(), 1);
    }
  }

  std::sort(SizeAmountPairs.begin(), SizeAmountPairs.end(),
            [](const auto &KVPair1, const auto &KVPair2) {
              return KVPair1.first < KVPair2.first;
            });

  int TotalValues = std::accumulate(
      SizeAmountPairs.begin(), SizeAmountPairs.end(), 0,
      [](int Current, const auto &KVPair) { return Current + KVPair.second; });

  llvm::outs() << llvm::formatv("{0,10}  {1,-=50} {2,10}\n", "PtS Size",
                                "Distribution", "Number of sets");
  for (auto &KV : SizeAmountPairs) {
    std::string PeakBar(static_cast<double>(KV.second) * 50 /
                            static_cast<double>(TotalValues),
                        '*');
    llvm::outs() << llvm::formatv("{0,10} |{1,-50} {2,-10}\n", KV.first,
                                  PeakBar, KV.second);
  }
  llvm::outs() << "\n";

  if (Peak) {
    for (const auto &ValueSetPair : PointsToSets) {
      if (ValueSetPair.second->size() == SizeAmountPairs.back().first) {
        llvm::outs() << "Peak into one of the biggest points sets.\n";
        peakIntoPointsToSet(ValueSetPair, Peak);
        return;
      }
    }
  }
}

} // namespace psr
