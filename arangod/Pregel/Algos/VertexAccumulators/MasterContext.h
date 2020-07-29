////////////////////////////////////////////////////////////////////////////////
///
/// DISCLAIMER
///
/// Copyright 2020 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Heiko Kernbach
/// @author Lars Maier
/// @author Markus Pfeiffer
///
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_PREGEL_ALGOS_ACCUMULATORS_MASTERCONTEXT_H
#define ARANGODB_PREGEL_ALGOS_ACCUMULATORS_MASTERCONTEXT_H 1

#include <Pregel/MasterContext.h>

#include <Pregel/Algos/VertexAccumulators/Greenspun/Primitives.h>
#include <Pregel/Algos/VertexAccumulators/VertexAccumulators.h>

namespace arangodb {
namespace pregel {
namespace algos {
namespace accumulators {

struct MasterContext : ::arangodb::pregel::MasterContext {

  struct VertexAccumulatorPhaseEvalContext : PrimEvalContext {
    VertexAccumulatorPhaseEvalContext(MasterContext& mc)
        : PrimEvalContext(), masterContext(mc) {}

    EvalResult gotoPhase(std::string_view nextPhase) const override {
      if (masterContext.gotoPhase(nextPhase)) {
        return {};
      }
      return EvalError("Unknown phase `" + std::string{nextPhase} + "`");
    }

    EvalResult finishAlgorithm() const override {
      masterContext.finish();
      return {};
    }

    MasterContext& masterContext;
  };

  MasterContext(VertexAccumulators const* algorithm);

  ContinuationResult userSelectedNext = ContinuationResult::DONT_CARE;

  bool gotoPhase(std::string_view nextPhase);
  void finish();
  ContinuationResult postGlobalSuperstep(bool allVertexesVotedHalt) override;

private:
  VertexAccumulators const* _algo;
};

}  // namespace accumulators
}  // namespace algos
}  // namespace pregel
}  // namespace arangodb

#endif
