////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2019 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrei Lobov
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_AQL_MATERIALIZE_EXECUTOR_H
#define ARANGOD_AQL_MATERIALIZE_EXECUTOR_H

#include "Aql/ExecutionBlock.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionState.h"
#include "Aql/ExecutorInfos.h"
#include "Aql/OutputAqlItemRow.h"
#include "Aql/types.h"
#include "Indexes/IndexIterator.h"
#include "VocBase/LocalDocumentId.h"

#include <iosfwd>
#include <memory>

namespace arangodb {
namespace aql {

class InputAqlItemRow;
class ExecutorInfos;
template <BlockPassthrough>
class SingleRowFetcher;
class NoStats;

class MaterializerExecutorInfos : public ExecutorInfos {
 public:
  MaterializerExecutorInfos(RegisterId nrInputRegisters, RegisterId nrOutputRegisters,
                     std::unordered_set<RegisterId> registersToClear,
                     std::unordered_set<RegisterId> registersToKeep,
                     RegisterId inNmColPtr, RegisterId inNmDocId,
                     RegisterId outDocRegId, transaction::Methods* trx);

  MaterializerExecutorInfos() = delete;
  MaterializerExecutorInfos(MaterializerExecutorInfos&&) = default;
  MaterializerExecutorInfos(MaterializerExecutorInfos const&) = delete;
  ~MaterializerExecutorInfos() = default;

  inline RegisterId outputMaterializedDocumentRegId() const {
    return _outMaterializedDocumentRegId;
  }

  inline RegisterId inputNonMaterializedColRegId() const {
    return _inNonMaterializedColRegId;
  }

  inline RegisterId inputNonMaterializedDocRegId() const {
    return _inNonMaterializedDocRegId;
  }

  inline transaction::Methods* trx() const {
    return _trx;
  }

 private:
  /// @brief register to store raw collection pointer
  RegisterId const _inNonMaterializedColRegId;
  /// @brief register to store local document id
  RegisterId const _inNonMaterializedDocRegId;
  /// @brief register to store materialized document
  RegisterId const _outMaterializedDocumentRegId;

  transaction::Methods* _trx;
};

class MaterializeExecutor {
 public:
  struct Properties {
    static constexpr bool preservesOrder = true;
    static constexpr BlockPassthrough allowsBlockPassthrough = BlockPassthrough::Disable;
    static constexpr bool inputSizeRestrictsOutputSize = false;
  };
  using Fetcher = SingleRowFetcher<Properties::allowsBlockPassthrough>;
  using Infos = MaterializerExecutorInfos;
  using Stats = NoStats;

  MaterializeExecutor(MaterializeExecutor&&) = default;
  MaterializeExecutor(MaterializeExecutor const&) = delete;
  MaterializeExecutor(Fetcher& fetcher, Infos& infos) : _readDocumentContext(infos), _infos(infos), _fetcher(fetcher) {}

  std::pair<ExecutionState, Stats> produceRows(OutputAqlItemRow& output);
  std::tuple<ExecutionState, Stats, size_t> skipRows(size_t toSkipRequested);

 protected:
  class ReadContext {
   public:
    explicit ReadContext(Infos& infos)
      : _infos(&infos),
      _inputRow(nullptr),
      _outputRow(nullptr),
      _callback(copyDocumentCallback(*this)) {}

    ReadContext(ReadContext&&) = default;

    const Infos* _infos;
    const arangodb::aql::InputAqlItemRow* _inputRow;
    arangodb::aql::OutputAqlItemRow* _outputRow;
    arangodb::IndexIterator::DocumentCallback const _callback;

   private:
    static arangodb::IndexIterator::DocumentCallback copyDocumentCallback(ReadContext& ctx);
  };
  ReadContext _readDocumentContext;
  Infos const& _infos;
  Fetcher& _fetcher;
};

}  // namespace aql
}  // namespace arangodb

#endif
