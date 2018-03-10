/******************************************************************************
 * Copyright (c) 2017 Philipp Schubert.
 * All rights reserved. This program and the accompanying materials are made
 * available under the terms of LICENSE.txt.
 *
 * Contributors:
 *     Philipp Schubert and others
 *****************************************************************************/

/*
 * IFDSConstAnalysis.h
 *
 *  Created on: 07.06.2017
 *      Author: rleer
 */
#ifndef ANALYSIS_IFDS_IDE_PROBLEMS_IFDS_CONST_ANALYSIS_IFDSCONSTANALYSIS_H_
#define ANALYSIS_IFDS_IDE_PROBLEMS_IFDS_CONST_ANALYSIS_IFDSCONSTANALYSIS_H_

#include "../../../lib/LLVMShorthands.h"
#include "../../../utils/Logger.h"
#include "../../../utils/utils.h"
#include "../../control_flow/LLVMBasedICFG.h"
#include "../../ifds_ide/DefaultIFDSTabulationProblem.h"
#include "../../ifds_ide/DefaultSeeds.h"
#include "../../ifds_ide/FlowFunction.h"
#include "../../ifds_ide/SpecialSummaries.h"
#include "../../ifds_ide/ZeroValue.h"
#include "../../ifds_ide/flow_func/Gen.h"
#include "../../ifds_ide/flow_func/GenAll.h"
#include "../../ifds_ide/flow_func/Identity.h"
#include "../../ifds_ide/flow_func/Kill.h"
#include "../../ifds_ide/flow_func/KillAll.h"
#include <algorithm>
#include <llvm/IR/CallSite.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
using namespace std;

class IFDSConstAnalysis : public DefaultIFDSTabulationProblem<
                              const llvm::Instruction *, const llvm::Value *,
                              const llvm::Function *, LLVMBasedICFG &> {
private:
  PointsToGraph &ptg;
  //  IFDSSummaryPool<const llvm::Value *, const llvm::Instruction *> dynSum;
  vector<string> EntryPoints;
  /// Holds all initialized variables and objects.
  set<const llvm::Value *> Initialized;

public:
  IFDSConstAnalysis(LLVMBasedICFG &icfg, vector<string> EntryPoints = {"main"});

  virtual ~IFDSConstAnalysis() = default;

  shared_ptr<FlowFunction<const llvm::Value *>>
  getNormalFlowFunction(const llvm::Instruction *curr,
                        const llvm::Instruction *succ) override;

  shared_ptr<FlowFunction<const llvm::Value *>>
  getCallFlowFunction(const llvm::Instruction *callStmt,
                      const llvm::Function *destMthd) override;

  shared_ptr<FlowFunction<const llvm::Value *>>
  getRetFlowFunction(const llvm::Instruction *callSite,
                     const llvm::Function *calleeMthd,
                     const llvm::Instruction *exitStmt,
                     const llvm::Instruction *retSite) override;

  shared_ptr<FlowFunction<const llvm::Value *>>
  getCallToRetFlowFunction(const llvm::Instruction *callSite,
                           const llvm::Instruction *retSite) override;

  shared_ptr<FlowFunction<const llvm::Value *>>
  getSummaryFlowFunction(const llvm::Instruction *callStmt,
                         const llvm::Function *destMthd) override;

  map<const llvm::Instruction *, set<const llvm::Value *>>
  initialSeeds() override;

  const llvm::Value *createZeroValue() override;

  bool isZeroValue(const llvm::Value *d) const override;

  string DtoString(const llvm::Value *d) override;

  string NtoString(const llvm::Instruction *n) override;

  string MtoString(const llvm::Function *m) override;

  /**
   * @note Global Variables are always intialized in llvm IR, and therefore
   * not part of the Initialized set.
   * @brief Checks if the given Value is initialized
   * @return True, if d is initialized or a Global Variable.
   */
  bool isInitialized(const llvm::Value *d) const;

  void markAsInitialized(const llvm::Value *d);

  void printInitilizedSet();

  /**
   * Only interested in points-to information within the function scope, i.e.
   *   -local instructions
   *   -function args of parent function
   *   -global variable/pointer
   * TODO add additional missing checks
   * @brief Refines the given points-to information to only context-relevant
   * points-to information.
   * @param PointsToSet that is refined.
   * @param Context dictates which points-to information is relevant.
   */
  set<const llvm::Value *>
  getContextRelevantPointsToSet(set<const llvm::Value *> &PointsToSet,
                                const llvm::Function *Context);
};

#endif /* ANALYSIS_IFDS_IDE_PROBLEMS_IFDS_CONST_ANALYSIS_IFDSCONSTANALYSIS_H_  \
        */
