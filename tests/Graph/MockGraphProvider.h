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
////////////////////////////////////////////////////////////////////////////////
#ifndef TESTS_MOCK_GRAPH_PROVIDER_H
#define TESTS_MOCK_GRAPH_PROVIDER_H

#include <numeric>
#include <ostream>
#include <unordered_map>
#include <vector>

#include "./MockGraph.h"
#include "Basics/Exceptions.h"
#include "Basics/voc-errors.h"

#include "Graph/Providers/BaseStep.h"

#include <velocypack/HashedStringRef.h>

namespace arangodb {

namespace futures {
template <typename T>
class Future;
}

namespace velocypack {
class Builder;
class HashedStringRef;
}  // namespace velocypack

namespace tests {
namespace graph {
class MockGraphProvider {
  using VertexType = arangodb::velocypack::HashedStringRef;
  using EdgeType = MockGraph::EdgeDef;
  using VertexRef = arangodb::velocypack::HashedStringRef;

 public:
  enum class LooseEndBehaviour {
    NEVER,
    ALLWAYS
  };

  class Step : public arangodb::graph::BaseStep<Step> {
    public:
    class Vertex {
     public:
      explicit Vertex(VertexType v) : _vertex(v){};

      void addToBuilder(arangodb::velocypack::Builder& builder) const;
      VertexRef getId() const;

      // This is only internal for the mock.
      // For some reason I did not manage to get this Class as the unordered_map
      // key. Although it is a trivial wrapper around a size_t...
      VertexType data() const { return _vertex; }

      // Make the set work on the VertexRef attribute only
      bool operator<(Vertex const& other) const noexcept {
        return _vertex < other._vertex;
      }

      bool operator>(Vertex const& other) const noexcept {
        return !operator<(other);
      }

     private:
      VertexType _vertex;
    };

    class Edge {
     public:
      Edge(EdgeType e) : _edge(e){};
      void addToBuilder(arangodb::velocypack::Builder& builder) const;

      std::string toString() const {
        return "Edge - _from: " + _edge._from + ", _to: " + _edge._to;
      }

     private:
      EdgeType _edge;
    };

    Step(VertexType v, bool isProcessable);
    Step(size_t prev, VertexType v, EdgeType e, bool isProcessable);
    ~Step();

    bool operator<(Step const& other) const noexcept {
      return _vertex < other._vertex;
    }

    std::string toString() const {
      if (_edge.has_value()) {
        return "<Step><Vertex>: " + _vertex.data().toString() +
               ", <Edge>:" + _edge.value().toString() +
               ", previous: " + basics::StringUtils::itoa(getPrevious());
      } else {
        return "<Step><Vertex>: " + _vertex.data().toString() +
               ", previous: " + basics::StringUtils::itoa(getPrevious());
      }
    }

    Vertex getVertex() const {
      if (!isProcessable()) {
        THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                       "Accessing vertex (" + getVertex().data().toString() +
                                           "), before fetching it");
      }
      return _vertex;
    }
    
    Edge getEdge() const {
      if (_edge.has_value()) {
        if (!isProcessable()) {
          THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                         "Accessing edge (" + _edge.value().toString() +
                                             "), before fetching it");
        }
        return _edge.value();
      } else {
        THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
      }
    }

    bool isProcessable() const { return _isProcessable; }

    private:
     Vertex _vertex;
     std::optional<Edge> _edge;
     bool _isProcessable;
  };

  MockGraphProvider(MockGraph const& data, LooseEndBehaviour looseEnds, bool reverse = false);
  ~MockGraphProvider();

  auto startVertex(VertexType vertex) -> Step;
  auto fetch(std::vector<Step> const& looseEnds) -> futures::Future<std::vector<Step>>;
  auto expand(Step const& from, size_t previous) -> std::vector<Step>;

 private:
  auto decideProcessable() const -> bool;

 private:
  std::unordered_map<std::string, std::vector<MockGraph::EdgeDef>> _fromIndex;
  std::unordered_map<std::string, std::vector<MockGraph::EdgeDef>> _toIndex;
  bool _reverse;
  LooseEndBehaviour _looseEnds;
};
}  // namespace graph
}  // namespace tests
}  // namespace arangodb

#endif
