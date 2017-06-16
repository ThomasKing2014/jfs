//===----------------------------------------------------------------------===//
//
//                        JFS - The JIT Fuzzing Solver
//
// Copyright 2017 Daniel Liew
//
// This file is distributed under the MIT license.
// See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
#ifndef JFS_CXX_FUZZING_BACKEND_FUZZING_SOLVER_H
#define JFS_CXX_FUZZING_BACKEND_FUZZING_SOLVER_H
#include "jfs/FuzzingCommon/FuzzingSolver.h"

namespace jfs {
namespace cxxfb {

class CXXFuzzingSolverImpl;

// This solver emits a CXX program and fuzzes it to find a satisfying
// assignment.
class CXXFuzzingSolver : public jfs::fuzzingCommon::FuzzingSolver {
private:
  std::unique_ptr<CXXFuzzingSolverImpl> impl;

protected:
  std::unique_ptr<jfs::core::SolverResponse>
  fuzz(jfs::core::Query &q, bool produceModel,
       std::shared_ptr<jfs::fuzzingCommon::FuzzingAnalysisInfo> info) override;

public:
  CXXFuzzingSolver(const jfs::core::SolverOptions &);
  ~CXXFuzzingSolver();
  llvm::StringRef getName() const;
};
}
}
#endif