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
/// @author Markus Pfeiffer
////////////////////////////////////////////////////////////////////////////////

#include "UpdateReplaceModifier.h"

#include "Aql/AqlValue.h"
#include "Aql/Collection.h"
#include "Aql/ModificationExecutor.h"
#include "Aql/ModificationExecutorAccumulator.h"
#include "Aql/ModificationExecutorHelpers.h"
#include "Aql/OutputAqlItemRow.h"
#include "Basics/Common.h"
#include "Transaction/Methods.h"
#include "VocBase/LogicalCollection.h"

#include <velocypack/Collection.h>
#include <velocypack/velocypack-aliases.h>

#include "Logger/LogMacros.h"

class CollectionNameResolver;

using namespace arangodb;
using namespace arangodb::aql;
using namespace arangodb::aql::ModificationExecutorHelpers;

ModifierOperationType UpdateReplaceModifierCompletion::accumulate(
    ModificationExecutorAccumulator& accu, InputAqlItemRow& row) try {
  RegisterId const inDocReg = _infos._input1RegisterId;
  RegisterId const keyReg = _infos._input2RegisterId;
  bool const hasKeyVariable = keyReg != RegisterPlan::MaxRegisterId;

  // The document to be REPLACE/UPDATEd
  AqlValue const& inDoc = row.getValue(inDocReg);

  if (!inDoc.isObject()) {
    if (!_infos._ignoreErrors) {
      THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_ARANGO_DOCUMENT_TYPE_INVALID,
                                     std::string("expecting 'Object', got: ") +
                                         inDoc.slice().typeName() + std::string(" while handling: UPDATE or REPLACE"));
    }
    return ModifierOperationType::SkipRow;
  }

  // WARNING
  //
  // We must never take _rev from the document if there is a key
  // expression.
  TRI_ASSERT(_infos._trx->resolver() != nullptr);
  CollectionNameResolver const& collectionNameResolver{*_infos._trx->resolver()};

  Result result;
  auto key = std::string{};
  auto rev = std::string{};

  AqlValue const& keyDoc = hasKeyVariable ? row.getValue(keyReg) : inDoc;
  result = getKeyAndRevision(collectionNameResolver, keyDoc, key, rev);

  if (!result.ok()) {
    // error happened extracting key, record in operations map
    if (!_infos._ignoreErrors) {
      THROW_ARANGO_EXCEPTION_MESSAGE(result.errorNumber(), result.errorMessage());
    }
    return ModifierOperationType::SkipRow;
  }

  if (writeRequired(_infos, inDoc.slice(), key)) {
    if (hasKeyVariable) {
      VPackBuilder keyDocBuilder;

      if (_infos._options.ignoreRevs) {
        rev.clear();
      }

      buildKeyAndRevDocument(keyDocBuilder, key, rev);

      // This deletes _rev if rev is empty or ignoreRevs is set in
      // options.
      auto merger =
          VPackCollection::merge(inDoc.slice(), keyDocBuilder.slice(), false, true);
      accu.add(merger.slice());
    } else {
      accu.add(inDoc.slice());
    }
    return ModifierOperationType::ReturnIfAvailable;
  } else {
    return ModifierOperationType::CopyRow;
  }
} catch (std::exception const& ex) {
  LOG_TOPIC("def12", WARN, Logger::DEVEL)
      << "UpdateReplaceModifierCompletion::accumulate failed: " << ex.what();
  throw;
}

OperationResult UpdateReplaceModifierCompletion::transact(VPackSlice const& data) try {
  if (_infos._isReplace) {
    return _infos._trx->replace(_infos._aqlCollection->name(), data, _infos._options);
  } else {
    return _infos._trx->update(_infos._aqlCollection->name(), data, _infos._options);
  }
} catch (std::exception const& ex) {
  LOG_TOPIC("def11", WARN, Logger::DEVEL)
      << "UpdateReplaceModifierCompletion::transact failed: " << ex.what();
  throw;
}
