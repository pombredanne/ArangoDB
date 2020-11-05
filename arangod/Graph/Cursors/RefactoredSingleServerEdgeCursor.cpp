////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2020 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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
/// @author Michael Hackstein
////////////////////////////////////////////////////////////////////////////////

#include "./RefactoredSingleServerEdgeCursor.h"
#include <Aql/AstNode.h>

#include "Graph/EdgeCursor.h"
#include "Graph/EdgeDocumentToken.h"
#include "Transaction/Helpers.h"
#include "Transaction/Methods.h"
#include "VocBase/LogicalCollection.h"

using namespace arangodb;
using namespace arangodb::graph;

namespace {
IndexIteratorOptions defaultIndexIteratorOptions;
}

RefactoredSingleServerEdgeCursor::RefactoredSingleServerEdgeCursor(
    arangodb::transaction::Methods* trx, arangodb::aql::QueryContext* queryContext)
    : _currentCursor(0), _currentSubCursor(0), _cachePos(0), _trx(trx), _queryContext(queryContext) {
  // set tmpVar
  _cache.reserve(1000);
}

RefactoredSingleServerEdgeCursor::~RefactoredSingleServerEdgeCursor() {}

#ifdef USE_ENTERPRISE
static bool CheckInaccessible(transaction::Methods* trx, VPackSlice const& edge) {
  // for skipInaccessibleCollections we need to check the edge
  // document, in that case nextWithExtra has no benefit
  TRI_ASSERT(edge.isString());
  arangodb::velocypack::StringRef str(edge);
  size_t pos = str.find('/');
  TRI_ASSERT(pos != std::string::npos);
  return trx->isInaccessibleCollection(str.substr(0, pos).toString());
}
#endif

void RefactoredSingleServerEdgeCursor::rearm(Step vertex, uint64_t /*depth*/) {
  _currentCursor = 0;
  _currentSubCursor = 0;
  _cache.clear();
  _cachePos = 0;

  if (_cursors.empty()) {
    buildLookupInfo(vertex);
    return;
  }

  size_t i = 0;
  for (auto& info : _lookupInfo) {
    auto& node = info.indexCondition;
    TRI_ASSERT(node->numMembers() > 0);
    if (info.conditionNeedUpdate) {
      // We have to inject _from/_to iff the condition needs it
      auto dirCmp = node->getMemberUnchecked(info.conditionMemberToUpdate);
      TRI_ASSERT(dirCmp->type == aql::NODE_TYPE_OPERATOR_BINARY_EQ);
      TRI_ASSERT(dirCmp->numMembers() == 2);

      auto idNode = dirCmp->getMemberUnchecked(1);
      TRI_ASSERT(idNode->type == aql::NODE_TYPE_VALUE);
      TRI_ASSERT(idNode->isValueType(aql::VALUE_TYPE_STRING));
      // must edit node in place; TODO replace node?
      TEMPORARILY_UNLOCK_NODE(idNode);
      idNode->setStringValue(vertex.data(), vertex.length());
    }

    auto& csrs = _cursors[i++];
    size_t j = 0;
    for (auto const& it : info.idxHandles) {
      auto& cursor = csrs[j];
      // check if the underlying index iterator supports rearming
      if (cursor->canRearm()) {
        // rearming supported
        if (!cursor->rearm(node, _tmpVar, ::defaultIndexIteratorOptions)) {
          cursor = std::make_unique<EmptyIndexIterator>(cursor->collection(), _trx);
        }
      } else {
        // rearming not supported - we need to throw away the index iterator
        // and create a new one
        cursor = _trx->indexScanForCondition(it, node, _tmpVar, ::defaultIndexIteratorOptions);
      }
      ++j;
    }
  }
}

void RefactoredSingleServerEdgeCursor::buildLookupInfo(Step vertex) {
  TRI_ASSERT(_cursors.empty());
  _cursors.reserve(_lookupInfo.size());

  if (_internalCursorMapping == nullptr) {
    for (auto& info : _lookupInfo) {
      addCursor(info, vertex);
    }
  } else {
    for (auto& index : *_internalCursorMapping) {
      TRI_ASSERT(index < _lookupInfo.size());
      auto& info = _lookupInfo[index];
      addCursor(info, vertex);
    }
  }
  TRI_ASSERT(_internalCursorMapping == nullptr ||
             _internalCursorMapping->size() == _cursors.size());
}

void RefactoredSingleServerEdgeCursor::getDocAndRunCallback(IndexIterator* cursor,
                                                            Callback const& callback) {
  auto collection = cursor->collection();
  EdgeDocumentToken etkn(collection->id(), _cache[_cachePos++]);
  collection->readDocumentWithCallback(
      _trx, etkn.localDocumentId(), [&](LocalDocumentId const&, VPackSlice edgeDoc) {
#ifdef USE_ENTERPRISE
        if (_trx->skipInaccessible()) {
          // TODO: we only need to check one of these
          VPackSlice from = transaction::helpers::extractFromFromDocument(edgeDoc);
          VPackSlice to = transaction::helpers::extractToFromDocument(edgeDoc);
          if (CheckInaccessible(_trx, from) || CheckInaccessible(_trx, to)) {
            return false;
          }
        }
#endif
        //_opts->cache()->increaseCounter(); TODO: Heiko check - Why?
        if (_internalCursorMapping != nullptr) {
          TRI_ASSERT(_currentCursor < _internalCursorMapping->size());
          callback(std::move(etkn), edgeDoc, _internalCursorMapping->at(_currentCursor));
        } else {
          callback(std::move(etkn), edgeDoc, _currentCursor);
        }
        return true;
      });
}

bool RefactoredSingleServerEdgeCursor::advanceCursor(
    IndexIterator*& cursor, std::vector<std::unique_ptr<IndexIterator>>*& cursorSet) {
  TRI_ASSERT(!_cursors.empty());
  ++_currentSubCursor;
  if (_currentSubCursor >= cursorSet->size()) {
    ++_currentCursor;
    _currentSubCursor = 0;
    if (_currentCursor == _cursors.size()) {
      // We are done, all cursors exhausted.
      return false;
    }
    cursorSet = &_cursors[_currentCursor];
  }

  cursor = (*cursorSet)[_currentSubCursor].get();
  // If we switch the cursor. We have to clear the cache.
  _cache.clear();
  return true;
}

bool RefactoredSingleServerEdgeCursor::next(Callback const& callback) {
  // fills callback with next EdgeDocumentToken and Slice that contains the
  // ohter side of the edge (we are standing on a node and want to iterate all
  // connected edges
  TRI_ASSERT(!_cursors.empty());

  if (_currentCursor == _cursors.size()) {
    return false;
  }

  // There is still something in the cache
  if (_cachePos < _cache.size()) {
    // get the collection
    getDocAndRunCallback(_cursors[_currentCursor][_currentSubCursor].get(), callback);
    return true;
  }

  // We need to refill the cache.
  _cachePos = 0;
  auto* cursorSet = &_cursors[_currentCursor];

  // get current cursor
  auto cursor = (*cursorSet)[_currentSubCursor].get();

  // NOTE: We cannot clear the cache,
  // because the cursor expect's it to be filled.
  do {
    if (cursorSet->empty() || !cursor->hasMore()) {
      if (!advanceCursor(cursor, cursorSet)) {
        return false;
      }
    } else {
      if (cursor->hasExtra()) {
        bool operationSuccessful = false;
        auto extraCB = [&](LocalDocumentId const& token, VPackSlice edge) {
          if (token.isSet()) {
#ifdef USE_ENTERPRISE
            if (trx()->skipInaccessible() && CheckInaccessible(_trx, edge)) {
              return false;
            }
#endif
            operationSuccessful = true;
            auto etkn = EdgeDocumentToken(cursor->collection()->id(), token);
            if (_internalCursorMapping != nullptr) {
              TRI_ASSERT(_currentCursor < _internalCursorMapping->size());
              callback(std::move(etkn), edge, _internalCursorMapping->at(_currentCursor));
            } else {
              callback(std::move(etkn), edge, _currentCursor);
            }
            return true;
          }
          return false;
        };
        cursor->nextExtra(extraCB, 1);
        if (operationSuccessful) {
          return true;
        }
      } else {
        _cache.clear();
        auto cb = [&](LocalDocumentId const& token) {
          if (token.isSet()) {
            // Document found
            _cache.emplace_back(token);
            return true;
          }
          return false;
        };
        bool tmp = cursor->next(cb, 1000);
        TRI_ASSERT(tmp == cursor->hasMore());
      }
    }
  } while (_cache.empty());
  TRI_ASSERT(!_cache.empty());
  TRI_ASSERT(_cachePos < _cache.size());
  getDocAndRunCallback(cursor, callback);
  return true;
}

void RefactoredSingleServerEdgeCursor::addCursor(LookupInfo const& info, Step vertex) {
  auto& node = info.indexCondition;
  TRI_ASSERT(node->numMembers() > 0);
  if (info.conditionNeedUpdate) {
    // We have to inject _from/_to iff the condition needs it
    auto dirCmp = node->getMemberUnchecked(info.conditionMemberToUpdate);
    TRI_ASSERT(dirCmp->type == aql::NODE_TYPE_OPERATOR_BINARY_EQ);
    TRI_ASSERT(dirCmp->numMembers() == 2);

    auto idNode = dirCmp->getMemberUnchecked(1);
    TRI_ASSERT(idNode->type == aql::NODE_TYPE_VALUE);
    TRI_ASSERT(idNode->isValueType(aql::VALUE_TYPE_STRING));
    // must edit node in place; TODO replace node?
    TEMPORARILY_UNLOCK_NODE(idNode);
    idNode->setStringValue(vertex.data(), vertex.length());
  }

  _cursors.emplace_back();
  auto& csrs = _cursors.back();
  csrs.reserve(info.idxHandles.size());
  for (std::shared_ptr<Index> const& index : info.idxHandles) {
    csrs.emplace_back(_trx->indexScanForCondition(index, node, _tmpVar,
                                                  ::defaultIndexIteratorOptions));
  }
}

arangodb::transaction::Methods* RefactoredSingleServerEdgeCursor::trx() const {
  return _trx;
}