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
#include "jfs/CXXFuzzingBackend/CXXFuzzingSolver.h"
#include "jfs/CXXFuzzingBackend/CXXFuzzingSolverOptions.h"
#include "jfs/CXXFuzzingBackend/CXXProgramBuilderPass.h"
#include "jfs/CXXFuzzingBackend/ClangInvocationManager.h"
#include "jfs/CXXFuzzingBackend/ClangOptions.h"
#include "jfs/Core/IfVerbose.h"
#include "jfs/FuzzingCommon/LibFuzzerInvocationManager.h"
#include "jfs/FuzzingCommon/SortConformanceCheckPass.h"
#include "jfs/FuzzingCommon/WorkingDirectoryManager.h"
#include "jfs/Transform/QueryPass.h"
#include "jfs/Transform/QueryPassManager.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_set>

using namespace jfs::core;
using namespace jfs::fuzzingCommon;
using namespace jfs::transform;

namespace jfs {
namespace cxxfb {

class CXXFuzzingSolverResponse : public SolverResponse {
public:
  CXXFuzzingSolverResponse(SolverResponse::SolverSatisfiability sat)
      : SolverResponse(sat) {}
  std::shared_ptr<Model> getModel() override {
    // TODO: Figure out how to do model generation
    return nullptr;
  }
};

class CXXFuzzingSolverImpl {
  std::mutex cancellablePassesMutex; // protects `cancellablePasses`
  std::unordered_set<jfs::transform::QueryPass*> cancellablePasses;
  std::atomic<bool> cancelled;
  JFSContext& ctx;
  // Raw pointer because we don't own the storage.
  CXXFuzzingSolverOptions* options;
  ClangInvocationManager cim;
  LibFuzzerInvocationManager lim;
  WorkingDirectoryManager* wdm;

public:
  friend class CXXFuzzingSolver;
  CXXFuzzingSolverImpl(JFSContext& ctx, CXXFuzzingSolverOptions* options,
                       WorkingDirectoryManager* wdm)
      : cancelled(false), ctx(ctx), options(options), cim(ctx), lim(ctx),
        wdm(wdm) {
    assert(this->wdm != nullptr);
    assert(this->options != nullptr);
    // Check paths
    bool clangPathsOkay = options->getClangOptions()->checkPaths(ctx);
    if (!clangPathsOkay) {
      ctx.raiseFatalError("One or more Clang paths do not exist");
    }
  }
  ~CXXFuzzingSolverImpl() {}

  llvm::StringRef getName() { return "CXXFuzzingSolver"; }
  void cancel() {
    cancelled = true;
    // Cancel any active passes
    {
      std::lock_guard<std::mutex> lock(cancellablePassesMutex);
      for (const auto& pass : cancellablePasses) {
        pass->cancel();
      }
    }
    // Cancel active Clang invocation
    cim.cancel();
    // Cancel active LibFuzzer invocation
    lim.cancel();
  }

  // FIXME: Should be const Query.
  bool sortsAreSupported(Query& q) {
    JFSContext &ctx = q.getContext();
    auto p = std::make_shared<SortConformanceCheckPass>([&ctx](Z3SortHandle s) {
      switch (s.getKind()) {
      case Z3_BOOL_SORT: {
        return true;
      }
      case Z3_BV_SORT: {
        unsigned width = s.getBitVectorWidth();
        if (width <= 64) {
          return true;
        }
        // Too wide
        IF_VERB(ctx,
                ctx.getWarningStream()
                    << "(BitVector width " << width << " not supported)\n");
        return false;
      }
      // TODO: Add support for floating point
      default: {
        // Sort not supported
        IF_VERB(ctx,
                ctx.getWarningStream()
                    << "(Sort \"" << s.toStr() << "\" not supported)\n");
        return false;
      }
      }
    });

    QueryPassManager pm;
    {
      // Make the pass cancellable
      std::lock_guard<std::mutex> lock(cancellablePassesMutex);
      cancellablePasses.insert(p.get());
      pm.add(p);
    }

    pm.run(q);

    {
      // The pass is done remove it from set of cancellable passes
      std::lock_guard<std::mutex> lock(cancellablePassesMutex);
      cancellablePasses.erase(p.get());
    }
    return p->predicateAlwaysHeld();
  }

  std::unique_ptr<jfs::core::SolverResponse>
  fuzz(jfs::core::Query &q, bool produceModel,
       std::shared_ptr<FuzzingAnalysisInfo> info) {
    assert(ctx == q.getContext());
    if (produceModel) {
      ctx.getErrorStream() << "(error model generation not supported)\n";
      return nullptr;
    }
#define CHECK_CANCELLED()                                                      \
  if (cancelled) {                                                             \
    IF_VERB(ctx, ctx.getDebugStream() << "(" << getName() << " cancelled)\n"); \
    return std::unique_ptr<SolverResponse>(                                    \
        new CXXFuzzingSolverResponse(SolverResponse::UNKNOWN));                \
  }

    // Check types are supported
    if (!sortsAreSupported(q)) {
      IF_VERB(ctx, ctx.getDebugStream() << "(unsupported sorts)\n");
      return std::unique_ptr<SolverResponse>(
          new CXXFuzzingSolverResponse(SolverResponse::UNKNOWN));
    }

    // Cancellation point
    CHECK_CANCELLED();

    // Generate program
    QueryPassManager pm;
    auto pbp = std::make_shared<CXXProgramBuilderPass>(info, ctx);

    {
      // Make the pass cancellable
      std::lock_guard<std::mutex> lock(cancellablePassesMutex);
      cancellablePasses.insert(pbp.get());
      pm.add(pbp);
    }
    pm.run(q);
    {
      // Pass is done. Remove from the set of cancellable passes
      std::lock_guard<std::mutex> lock(cancellablePassesMutex);
      cancellablePasses.insert(pbp.get());
    }

    // Cancellation point
    CHECK_CANCELLED();

    // Build program
    // FIXME: We should teach ClangInvocationManager to pipe the program
    // directly
    // to Clang so we don't need to write it disk and then immediatly read it
    // back.
    std::string sourceFilePath = wdm->getPathToFileInDirectory("program.cpp");
    std::string outputFilePath = wdm->getPathToFileInDirectory("fuzzer");
    std::string clangStdOutFile;
    std::string clangStdErrFile;
    if (ctx.getVerbosity() == 0) {
      // When being quiet redirect to files
      clangStdOutFile = wdm->getPathToFileInDirectory("clang.stdout.txt");
      clangStdErrFile = wdm->getPathToFileInDirectory("clang.stderr.txt");
    }
    bool compileSuccess = cim.compile(
        /*program=*/pbp->getProgram().get(), /*sourceFile=*/sourceFilePath,
        /*outputFile=*/outputFilePath,
        /*clangOptions=*/options->getClangOptions(),
        /*stdOutFile=*/clangStdOutFile,
        /*stdErrFile=*/clangStdErrFile);
    if (!compileSuccess) {
      return std::unique_ptr<SolverResponse>(
          new CXXFuzzingSolverResponse(SolverResponse::UNKNOWN));
    }
    // Cancellation point
    CHECK_CANCELLED();

    // Set LibFuzzer options
    LibFuzzerOptions* lfo = options->getLibFuzzerOptions();
    // FIXME: We've already computed this earlier so we should cache it
    // somewhere.
    lfo->maxLength =
        (info->freeVariableAssignment->bufferAssignment->computeWidth() + 7) /
        8;
    lfo->targetBinary = outputFilePath;
    std::string corpusDir = wdm->makeNewDirectoryInDirectory("corpus");
    lfo->corpusDir = corpusDir;
    std::string artifactDir = wdm->makeNewDirectoryInDirectory("artifacts");
    lfo->artifactDir = artifactDir;
    std::string libFuzzerStdOutFile;
    std::string libFuzzerStdErrFile;
    lfo->useCmp = false;
    // FIXME: This is O(N). We should probably change sanitizerCoverageOptions
    // to be a set.
    if (std::find(options->getClangOptions()->sanitizerCoverageOptions.begin(),
                  options->getClangOptions()->sanitizerCoverageOptions.end(),
                  ClangOptions::SanitizerCoverageTy::TRACE_CMP) !=
        options->getClangOptions()->sanitizerCoverageOptions.end()) {
      lfo->useCmp = true;
    }
    if (ctx.getVerbosity() == 0) {
      // When being quiet redirect to files
      libFuzzerStdOutFile =
          wdm->getPathToFileInDirectory("libfuzzer.stdout.txt");
      libFuzzerStdErrFile =
          wdm->getPathToFileInDirectory("libfuzzer.stderr.txt");
    }
    // Fuzz
    auto fuzzingResponse =
        lim.fuzz(lfo, libFuzzerStdOutFile, libFuzzerStdErrFile);
    if (fuzzingResponse->outcome == LibFuzzerResponse::ResponseTy::UNKNOWN ||
        fuzzingResponse->outcome == LibFuzzerResponse::ResponseTy::CANCELLED) {
      return std::unique_ptr<SolverResponse>(
          new CXXFuzzingSolverResponse(SolverResponse::UNKNOWN));
    }
    assert(fuzzingResponse->outcome ==
           LibFuzzerResponse::ResponseTy::TARGET_FOUND);
    // Solution found
    // TODO: Handle setting up model if its needed.
    return std::unique_ptr<SolverResponse>(
        new CXXFuzzingSolverResponse(SolverResponse::SAT));
  }
};

CXXFuzzingSolver::CXXFuzzingSolver(
    std::unique_ptr<CXXFuzzingSolverOptions> options,
    std::unique_ptr<WorkingDirectoryManager> wdm, JFSContext& ctx)
    : jfs::fuzzingCommon::FuzzingSolver(std::move(options), std::move(wdm),
                                        ctx),
      impl(new CXXFuzzingSolverImpl(
          ctx, static_cast<CXXFuzzingSolverOptions*>(this->options.get()),
          this->wdm.get())) {}

CXXFuzzingSolver::~CXXFuzzingSolver() {}

std::unique_ptr<jfs::core::SolverResponse>
CXXFuzzingSolver::fuzz(jfs::core::Query &q, bool produceModel,
                       std::shared_ptr<FuzzingAnalysisInfo> info) {
  return impl->fuzz(q, produceModel, info);
}

llvm::StringRef CXXFuzzingSolver::getName() const { return "CXXFuzzingSolver"; }

void CXXFuzzingSolver::cancel() {
  // Call parent
  FuzzingSolver::cancel();
  // Notify implementation
  impl->cancel();
}
}
}
