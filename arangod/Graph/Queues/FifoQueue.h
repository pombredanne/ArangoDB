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
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGOD_GRAPH_QUEUE_H
#define ARANGOD_GRAPH_QUEUE_H 1

#include "Basics/debugging.h"

#include <queue>

namespace arangodb {
namespace graph {

template <class Step>
class FifoQueue {
 public:
  // TODOS: Add Sorting (Performance)
  // -> loose ends to the end

  FifoQueue() {}
  ~FifoQueue() {}

  /// @brief queue datastore
  std::deque<Step> _queue;

  void clear() { _queue.clear(); };

  void append(Step step) { _queue.push_back(step); };

  bool hasProcessableElement() const {
    if (!isEmpty()) {
      auto& first = _queue.front();
      if (first.isProcessable()) {
        return true;
      }
    }

    return false;
  };

  size_t size() const { return _queue.size(); };

  bool isEmpty() const { return _queue.size() == 0; };

  std::vector<Step*> getLooseEnds(){
    TRI_ASSERT(!hasProcessableElement());

    std::vector<Step*> steps;
    for (auto& step: _queue) {
      if (!step.isProcessable()) {
        steps.emplace_back(&step);
      }
    }

    return std::move(steps);
  };

  Step pop() {
    TRI_ASSERT(!isEmpty());
    Step first = std::move(_queue.front());
    _queue.pop_front();
    return std::move(first);
  };
};

}  // namespace graph
}  // namespace arangodb

#endif  // ARANGOD_GRAPH_QUEUE_H