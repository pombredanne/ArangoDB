////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2020-2020 ArangoDB GmbH, Cologne, Germany
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
/// @author Michael Hackstein
/// @author Heiko Kernbach
////////////////////////////////////////////////////////////////////////////////

#ifndef ARANGODB_GRAPH_ENUMERATORS_TWO_SIDED_ENUMERATOR_H
#define ARANGODB_GRAPH_ENUMERATORS_TWO_SIDED_ENUMERATOR_H 1

#include "Containers/HashSet.h"

#include "Graph/ShortestPathFinder.h"  // TODO: Change / remove / minimize

#include <set>

namespace arangodb {

namespace velocypack {
class Builder;
class HashedStringRef;
}  // namespace velocypack

namespace graph {

template <class Step>
class PathResult;

template <class QueueType, class PathStoreType, class ProviderType>
class TwoSidedEnumerator {
 private:
  enum Direction { FORWARD, BACKWARD };

  using VertexRef = arangodb::velocypack::HashedStringRef;
  using Step = typename ProviderType::Step;
  using Shell = std::multiset<Step>;
  using ResultList = std::vector<std::pair<Step, Step>>;

  class Ball {
   public:
    Ball(Direction dir, ProviderType&& provider);
    ~Ball();
    auto clear() -> void;
    auto reset(Step center) -> void;
    auto startNextDepth() -> void;
    auto noPathLeft() const -> bool;
    auto getDepth() const -> size_t;
    auto shellSize() const -> size_t;
    auto doneWithDepth() const -> bool;

    auto buildPath(Step const& vertexInShell, PathResult<Step>& path) -> void;

    auto matchResultsInShell(Step const& match, ResultList& results) const -> void;
    auto computeNeighbourhoodOfNextVertex(Ball const& other, ResultList& results) -> void;

   private:
    // Fast path, to test if we find a connecting vertex between left and right.
    Shell _shell{};
    // This stores all paths processed by this ball
    PathStoreType _interior{};

    // The next elements to process
    QueueType _queue{};

    // Next steps to be processed.
    QueueType _nextDepth{};

    ProviderType _provider;

    size_t _depth{0};
    size_t _searchIndex{std::numeric_limits<size_t>::max()};
    Direction _direction;
  };

 public:
  TwoSidedEnumerator(ProviderType&& forwardProvider, ProviderType&& backwardProvider);

  ~TwoSidedEnumerator();

  void clear();

  /**
   * @brief Quick test if the finder can prove there is no more data available.
   *        It can respond with false, even though there is no path left.
   * @return true There will be no further path.
   * @return false There is a chance that there is more data available.
   */
  bool isDone() const;

  /**
   * @brief Reset to new source and target vertices.
   * This API uses string references, this class will not take responsibility
   * for the referenced data. It is caller's responsibility to retain the
   * underlying data and make sure the StringRefs stay valid until next
   * call of reset.
   *
   * @param source The source vertex to start the paths
   * @param target The target vertex to end the paths
   */
  void reset(VertexRef source, VertexRef target);

  /**
   * @brief Get the next path, if available written into the result build.
   * The given builder will be not be cleared, this function requires a
   * prepared builder to write into.
   *
   * @param result Input and output, this needs to be an open builder,
   * where the path can be placed into.
   * Can be empty, or an openArray, or the value of an object.
   * Guarantee: Every returned path matches the conditions handed in via
   * options. No path is returned twice, it is intended that paths overlap.
   * @return true Found and written a path, result is modified.
   * @return false No path found, result has not been changed.
   */
  bool getNextPath(arangodb::velocypack::Builder& result);

  /**
   * @brief Skip the next Path, like getNextPath, but does not return the path.
   *
   * @return true Found and skipped a path.
   * @return false No path found.
   */

  bool skipPath();

 private:
  auto searchDone() const -> bool;
  auto startNextDepth() -> void;

 private:

  Ball _left;
  Ball _right;
  bool _searchLeft{true};
  ResultList _results{};

  PathResult<Step> _resultPath;
};
}  // namespace graph
}  // namespace arangodb

#endif