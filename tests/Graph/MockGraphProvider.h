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
#include <unordered_map>
#include <vector>

#include "./MockGraph.h"

namespace arangodb {

namespace futures {
template <typename T>
class Future;
}

namespace velocypack {
class Builder;
}

namespace tests {
namespace graph {

class MockGraphProvider {
  using VertexType = size_t;
  using EdgeType = MockGraph::EdgeDef;

 public:
  struct Step {
    class Vertex {
     public:
      Vertex(VertexType v) : _vertex(v){}; // TODO: private?
      void addToBuilder(arangodb::velocypack::Builder& builder);

     private:
      VertexType _vertex;
    };

    class Edge {
      Edge(EdgeType e) : _edge(e){}; // TODO: private?
      void addToBuilder(arangodb::velocypack::Builder& builder);

     private:
      EdgeType _edge;
    };

    explicit Step(VertexType v);
    Step(size_t prev, VertexType v, EdgeType e);
    ~Step();

    Vertex vertex;
    std::optional<Edge> edge;
    size_t previous;

    size_t getPrevious() { return previous; }
  };

  MockGraphProvider(MockGraph const& data);
  ~MockGraphProvider();

  auto startVertex(VertexType vertex) -> Step;
  auto fetch(std::vector<Step> const& looseEnds) -> futures::Future<std::vector<Step>>;
  auto expand(Step const& from, size_t previous) -> std::vector<Step>;

 private:
  std::unordered_map<Step::Vertex, std::vector<MockGraph::EdgeDef>> _fromIndex;
  std::unordered_map<Step::Vertex, std::vector<MockGraph::EdgeDef>> _toIndex;
};
}  // namespace graph
}  // namespace tests
}  // namespace arangodb

#endif