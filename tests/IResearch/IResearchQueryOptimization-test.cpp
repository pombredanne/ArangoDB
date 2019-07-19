////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2017 ArangoDB GmbH, Cologne, Germany
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
/// @author Andrey Abramov
/// @author Vasiliy Nabatchikov
////////////////////////////////////////////////////////////////////////////////

#include "common.h"
#include "gtest/gtest.h"

#include "../Mocks/Servers.h"
#include "../Mocks/StorageEngineMock.h"

#include "3rdParty/iresearch/tests/tests_config.hpp"
#include "Aql/AqlFunctionFeature.h"
#include "Aql/Ast.h"
#include "Aql/ExecutionNode.h"
#include "Aql/ExecutionPlan.h"
#include "Aql/ExpressionContext.h"
#include "Aql/OptimizerRules.h"
#include "Aql/Query.h"
#include "Aql/QueryRegistry.h"
#include "Basics/SmallVector.h"
#include "Basics/VelocyPackHelper.h"
#include "IResearch/IResearchAnalyzerFeature.h"
#include "IResearch/IResearchCommon.h"
#include "IResearch/IResearchFilterFactory.h"
#include "IResearch/IResearchLink.h"
#include "IResearch/IResearchLinkHelper.h"
#include "IResearch/IResearchView.h"
#include "Logger/LogTopic.h"
#include "Logger/Logger.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "Transaction/Methods.h"
#include "Transaction/StandaloneContext.h"
#include "Utils/OperationOptions.h"
#include "V8/v8-globals.h"
#include "VocBase/LogicalCollection.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ManagedDocumentResult.h"

#include "IResearch/VelocyPackHelper.h"
#include "analysis/analyzers.hpp"
#include "analysis/token_attributes.hpp"
#include "search/boolean_filter.hpp"
#include "search/range_filter.hpp"
#include "search/term_filter.hpp"
#include "utils/utf8_path.hpp"

#include <velocypack/Iterator.h>

extern const char* ARGV0;  // defined in main.cpp

NS_LOCAL

bool findEmptyNodes(TRI_vocbase_t& vocbase, std::string const& queryString,
                    std::shared_ptr<arangodb::velocypack::Builder> bindVars = nullptr) {
  auto options = VPackParser::fromJson(
      //    "{ \"tracing\" : 1 }"
      "{ }");

  arangodb::aql::Query query(false, vocbase, arangodb::aql::QueryString(queryString),
                             bindVars, options, arangodb::aql::PART_MAIN);

  query.prepare(arangodb::QueryRegistryFeature::registry());

  arangodb::SmallVector<arangodb::aql::ExecutionNode*>::allocator_type::arena_type a;
  arangodb::SmallVector<arangodb::aql::ExecutionNode*> nodes{a};

  // try to find `EnumerateViewNode`s and process corresponding filters and sorts
  query.plan()->findNodesOfType(nodes, arangodb::aql::ExecutionNode::NORESULTS, true);
  return !nodes.empty();
}

// -----------------------------------------------------------------------------
// --SECTION--                                                 setup / tear-down
// -----------------------------------------------------------------------------

class IResearchQueryOptimizationTest : public ::testing::Test {
 protected:
  arangodb::tests::mocks::MockAqlServer server;
  TRI_vocbase_t* _vocbase;
  std::deque<arangodb::ManagedDocumentResult> insertedDocs;

  IResearchQueryOptimizationTest() : server() {
    arangodb::tests::init(true);

    auto* functions =
        arangodb::application_features::ApplicationServer::getFeature<arangodb::aql::AqlFunctionFeature>(
            "AQLFunctions");
    TRI_ASSERT(functions != nullptr);

    // register fake non-deterministic function in order to suppress optimizations
    functions->add(arangodb::aql::Function{
        "_NONDETERM_", ".",
        arangodb::aql::Function::makeFlags(
            // fake non-deterministic
            arangodb::aql::Function::Flags::CanRunOnDBServer),
        [](arangodb::aql::ExpressionContext*, arangodb::transaction::Methods*,
           arangodb::aql::VPackFunctionParameters const& params) {
          TRI_ASSERT(!params.empty());
          return params[0];
        }});

    // register fake non-deterministic function in order to suppress optimizations
    functions->add(arangodb::aql::Function{
        "_FORWARD_", ".",
        arangodb::aql::Function::makeFlags(
            // fake deterministic
            arangodb::aql::Function::Flags::Deterministic, arangodb::aql::Function::Flags::Cacheable,
            arangodb::aql::Function::Flags::CanRunOnDBServer),
        [](arangodb::aql::ExpressionContext*, arangodb::transaction::Methods*,
           arangodb::aql::VPackFunctionParameters const& params) {
          TRI_ASSERT(!params.empty());
          return params[0];
        }});

    auto* analyzers =
        arangodb::application_features::ApplicationServer::getFeature<arangodb::iresearch::IResearchAnalyzerFeature>();
    arangodb::iresearch::IResearchAnalyzerFeature::EmplaceResult result;
    TRI_ASSERT(analyzers != nullptr);

    auto* dbFeature =
        arangodb::application_features::ApplicationServer::getFeature<arangodb::DatabaseFeature>(
            "DatabaseFeature");

    TRI_ASSERT(dbFeature != nullptr);
    dbFeature->createDatabase(1, "testVocbase", _vocbase);  // required for IResearchAnalyzerFeature::emplace(...)
    analyzers->emplace(result, "testVocbase::test_analyzer", "TestAnalyzer",
                       VPackParser::fromJson("\"abc\"")->slice());  // cache analyzer
    analyzers->emplace(result, "testVocbase::test_csv_analyzer",
                       "TestDelimAnalyzer",
                       VPackParser::fromJson("\",\"")->slice());  // cache analyzer
  }

  ~IResearchQueryOptimizationTest() {}

  void addLinkToCollection(std::shared_ptr<arangodb::iresearch::IResearchView>& view) {
    auto updateJson = VPackParser::fromJson(
        "{ \"links\" : {"
        "\"collection_1\" : { \"includeAllFields\" : true }"
        "}}");
    EXPECT_TRUE((view->properties(updateJson->slice(), true).ok()));

    arangodb::velocypack::Builder builder;

    builder.openObject();
    view->properties(builder, arangodb::LogicalDataSource::makeFlags(
                                  arangodb::LogicalDataSource::Serialize::Detailed));
    builder.close();

    auto slice = builder.slice();
    EXPECT_TRUE(slice.isObject());
    EXPECT_TRUE(slice.get("name").copyString() == "testView");
    EXPECT_TRUE(slice.get("type").copyString() ==
                arangodb::iresearch::DATA_SOURCE_TYPE.name());
    EXPECT_TRUE(slice.get("deleted").isNone());  // no system properties
    auto tmpSlice = slice.get("links");
    EXPECT_TRUE((true == tmpSlice.isObject() && 1 == tmpSlice.length()));
  }

  void SetUp() override {
    auto const createCollectionString = R"({"name": "collection_1"})";
    auto collectionJson = VPackParser::fromJson(createCollectionString);
    auto logicalCollection1 = _vocbase->createCollection(collectionJson->slice());
    ASSERT_NE(logicalCollection1.get(), nullptr);

    // add view
    auto createViewString = R"({"name": "testView", "type": "arangosearch"})";
    auto createJson = VPackParser::fromJson(createViewString);
    auto view = std::dynamic_pointer_cast<arangodb::iresearch::IResearchView>(
        _vocbase->createView(createJson->slice()));
    ASSERT_NE(view.get(), nullptr);

    // add link to collection
    addLinkToCollection(view);

    // populate view with the data
    {
      arangodb::OperationOptions opt;
      static std::vector<std::string> const EMPTY;

      arangodb::transaction::Methods trx(
          arangodb::transaction::StandaloneContext::Create(vocbase()), EMPTY,
          EMPTY, EMPTY, arangodb::transaction::Options());
      EXPECT_TRUE(trx.begin().ok());

      // insert into collection
      auto builder =
          VPackParser::fromJson("[{ \"values\" : [ \"A\", \"C\", \"B\" ] }]");

      auto root = builder->slice();
      ASSERT_TRUE(root.isArray());

      for (auto doc : arangodb::velocypack::ArrayIterator(root)) {
        insertedDocs.emplace_back();
        auto const res =
            logicalCollection1->insert(&trx, doc, insertedDocs.back(), opt, false);
        EXPECT_TRUE(res.ok());
      }

      EXPECT_TRUE(trx.commit().ok());
      EXPECT_TRUE(arangodb::iresearch::IResearchLinkHelper::find(*logicalCollection1, *view)
                      ->commit()
                      .ok());
    }
  }

  TRI_vocbase_t& vocbase() const {
    TRI_ASSERT(_vocbase != nullptr);
    return *_vocbase;
  }

};  // IResearchQuerySetup

NS_END

// -----------------------------------------------------------------------------
// --SECTION--                                                        test suite
// -----------------------------------------------------------------------------

// dedicated to https://github.com/arangodb/arangodb/issues/8294
TEST_F(IResearchQueryOptimizationTest, test_In_AND_EQ_WITH_X_LT_Y) {
  // a IN [ x ] && a == y, x < y

  std::string const query =
      "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values == 'C' "
      "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_FALSE(findEmptyNodes(vocbase(), query));

  // check structure
  {
    irs::Or expected;
    auto& root = expected.add<irs::And>();
    {
      auto& sub = root.add<irs::Or>();
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    }
    root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
  }

  std::vector<arangodb::velocypack::Slice> expectedDocs{
      arangodb::velocypack::Slice(insertedDocs[0].vpack()),
  };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
  ASSERT_TRUE(queryResult.result.ok());

  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());

  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_TRUE(expectedDocs.size() == resultIt.size());

  // Check documents
  auto expectedDoc = expectedDocs.begin();
  for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
    auto const actualDoc = resultIt.value();
    auto const resolved = actualDoc.resolveExternals();

    EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                          arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
  }
  EXPECT_TRUE(expectedDoc == expectedDocs.end());
}

TEST_F(IResearchQueryOptimizationTest, test_In_AND_EQ_WITH_X_EQ_Y) {
  // a IN [ x ] && a == y, x == y
  std::string const query =
      "FOR d IN testView SEARCH d.values IN [ 'C', 'B', 'A' ] AND d.values "
      "== 'A' RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

  // FIXME
  // check structure
  {
    irs::Or expected;
    auto& root = expected.add<irs::And>();
    {
      auto& sub = root.add<irs::Or>();
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    }
    root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
  }
  //{
  //  irs::Or expected;
  //  auto& root = expected.add<irs::And>();
  //  {
  //    auto& sub = root.add<irs::Or>();
  //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
  //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
  //  }
  //  root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
  //}

  std::vector<arangodb::velocypack::Slice> expectedDocs{
      arangodb::velocypack::Slice(insertedDocs[0].vpack()),
  };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
  ASSERT_TRUE(queryResult.result.ok());

  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());

  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_TRUE(expectedDocs.size() == resultIt.size());

  // Check documents
  auto expectedDoc = expectedDocs.begin();
  for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
    auto const actualDoc = resultIt.value();
    auto const resolved = actualDoc.resolveExternals();

    EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                          arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
  }
  EXPECT_TRUE(expectedDoc == expectedDocs.end());
}

TEST_F(IResearchQueryOptimizationTest, test_In_AND_EQ_WITH_X_GT_Y) {
  // a IN [ x ] && a == y, x > y

  std::string const query =
      "FOR d IN testView SEARCH d.values IN [ 'C', 'B' ] AND d.values == 'A' "
      "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

  // check structure
  {
    irs::Or expected;
    auto& root = expected.add<irs::And>();
    {
      auto& sub = root.add<irs::Or>();
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    }
    root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
  }

  std::vector<arangodb::velocypack::Slice> expectedDocs{
      arangodb::velocypack::Slice(insertedDocs[0].vpack()),
  };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
  ASSERT_TRUE(queryResult.result.ok());

  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());

  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_TRUE(expectedDocs.size() == resultIt.size());

  // Check documents
  auto expectedDoc = expectedDocs.begin();
  for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
    auto const actualDoc = resultIt.value();
    auto const resolved = actualDoc.resolveExternals();

    EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                          arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
  }
  EXPECT_TRUE(expectedDoc == expectedDocs.end());
}

TEST_F(IResearchQueryOptimizationTest, test_In_AND_NE_WITH_X_LT_Y) {
  // a IN [ x ] && a != y, x < y
  std::string const query =
      "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values != 'D' "
      "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

  // check structure
  {
    irs::Or expected;
    auto& root = expected.add<irs::And>();
    {
      auto& sub = root.add<irs::Or>();
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("@");
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    }
    root.add<irs::Not>()
        .filter<irs::by_term>()
        .field(mangleStringIdentity("values"))
        .term("B");
  }

  std::vector<arangodb::velocypack::Slice> expectedDocs{
      arangodb::velocypack::Slice(insertedDocs[0].vpack()),
  };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
  ASSERT_TRUE(queryResult.result.ok());

  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());

  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_TRUE(expectedDocs.size() == resultIt.size());

  // Check documents
  auto expectedDoc = expectedDocs.begin();
  for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
    auto const actualDoc = resultIt.value();
    auto const resolved = actualDoc.resolveExternals();

    EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                          arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
  }
  EXPECT_TRUE(expectedDoc == expectedDocs.end());
}

TEST_F(IResearchQueryOptimizationTest, test_In_AND_NE_B_WITH_X_LT_Y) {
  // a IN [ x ] && a !== 'B', x < y
  std::string const query =
      "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values != 'B' "
      "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

  // check structure
  {
    irs::Or expected;
    auto& root = expected.add<irs::And>();
    {
      auto& sub = root.add<irs::Or>();
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("@");
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    }
    root.add<irs::Not>()
        .filter<irs::by_term>()
        .field(mangleStringIdentity("values"))
        .term("B");
  }

  std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
  ASSERT_TRUE(queryResult.result.ok());

  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());

  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_TRUE(expectedDocs.size() == resultIt.size());

  // Check documents
  auto expectedDoc = expectedDocs.begin();
  for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
    auto const actualDoc = resultIt.value();
    auto const resolved = actualDoc.resolveExternals();

    EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                          arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
  }
  EXPECT_TRUE(expectedDoc == expectedDocs.end());
}

TEST_F(IResearchQueryOptimizationTest, test_In_C_D_AND_NE_D) {
  // a IN [ x ] && a == y, x > y
  std::string const query =
      "FOR d IN testView SEARCH d.values IN [ 'C', 'D' ] AND d.values != 'D' "
      "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

  // FIXME
  // check structure
  {
    irs::Or expected;
    auto& root = expected.add<irs::And>();
    {
      auto& sub = root.add<irs::Or>();
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    }
    root.add<irs::Not>()
        .filter<irs::by_term>()
        .field(mangleStringIdentity("values"))
        .term("A");
  }
  //{
  //  irs::Or expected;
  //  auto& root = expected.add<irs::And>();
  //  {
  //    auto& sub = root.add<irs::Or>();
  //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
  //  }
  //  root.add<irs::Not>().filter<irs::by_term>().field(mangleStringIdentity("values")).term("A");
  //}

  std::vector<arangodb::velocypack::Slice> expectedDocs{
      arangodb::velocypack::Slice(insertedDocs[0].vpack()),
  };

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
  ASSERT_TRUE(queryResult.result.ok());

  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());

  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_TRUE(expectedDocs.size() == resultIt.size());

  // Check documents
  auto expectedDoc = expectedDocs.begin();
  for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
    auto const actualDoc = resultIt.value();
    auto const resolved = actualDoc.resolveExternals();

    EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                          arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
  }
  EXPECT_TRUE(expectedDoc == expectedDocs.end());
}

TEST_F(IResearchQueryOptimizationTest, DISABLED_test_In_A_A_AND_NE_A) {
  // FIXME
  // a IN [ x ] && a == y, x == y
  std::string const query =
      "FOR d IN testView SEARCH d.values IN [ 'A', 'A' ] AND d.values != 'A' "
      "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(findEmptyNodes(vocbase(), query));

  std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
  ASSERT_TRUE(queryResult.result.ok());

  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());

  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_TRUE(expectedDocs.size() == resultIt.size());

  // Check documents
  auto expectedDoc = expectedDocs.begin();
  for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
    auto const actualDoc = resultIt.value();
    auto const resolved = actualDoc.resolveExternals();

    EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                          arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
  }
  EXPECT_TRUE(expectedDoc == expectedDocs.end());
}

TEST_F(IResearchQueryOptimizationTest, DISABLED_test_In_C_B_AND_NE_A) {
  // a IN [ x ] && a != y, x > y
  std::string const query =
      "FOR d IN testView SEARCH d.values IN [ 'C', 'B' ] AND d.values != 'A' "
      "RETURN d";

  EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                           {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

  EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

  // check structure
  {
    irs::Or expected;
    auto& root = expected.add<irs::And>();
    {
      auto& sub = root.add<irs::Or>();
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    }
    root.add<irs::Not>()
        .filter<irs::by_term>()
        .field(mangleStringIdentity("values"))
        .term("A");
  }

  std::vector<arangodb::velocypack::Slice> expectedDocs{};

  auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
  ASSERT_TRUE(queryResult.result.ok());

  auto result = queryResult.data->slice();
  EXPECT_TRUE(result.isArray());

  arangodb::velocypack::ArrayIterator resultIt(result);
  ASSERT_TRUE(expectedDocs.size() == resultIt.size());

  // Check documents
  auto expectedDoc = expectedDocs.begin();
  for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
    auto const actualDoc = resultIt.value();
    auto const resolved = actualDoc.resolveExternals();

    EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                          arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
  }
  EXPECT_TRUE(expectedDoc == expectedDocs.end());
}

TEST_F(IResearchQueryOptimizationTest, test) {
  // a IN [ x ] && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'B' ] AND d.values != '@' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      }
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'A', 'B' ] AND d.values < 'C' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    }
    //{
    //  irs::Or expected;
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    //  }
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'A', 'C' ] AND d.values < 'C' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'D', 'C' ] AND d.values < 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a <= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B', 'C' ] AND d.values <= 'D' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("D");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
    //  }
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a <= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B', 'C' ] AND d.values <= 'C' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("C");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    //  }
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a <= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B', 'C' ] AND d.values <= 'A' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a >= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values >= 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("@");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a >= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values >= 'A' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("@");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a >= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'D' ] AND d.values >= 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
    //  }
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a > y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ '@', 'A' ] AND d.values > 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("@");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a > y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'B' ] AND d.values > 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a > y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C', 'D' ] AND d.values > 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
      }
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
    }
    //{
    //  irs::Or expected;
    //  auto& root = expected.add<irs::And>();
    //  {
    //    auto& sub = root.add<irs::Or>();
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
    //    sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("D");
    //  }
    //}

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a IN [ y ]
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'A', 'B' ] AND d.values IN [ "
        "'A', 'B', 'C' ] RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // FIXME optimize
    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      }
      {
        auto& sub = root.add<irs::Or>();
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
        sub.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      }
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B' ] AND d.values == 'C' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values == 'C' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a == y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values == 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'A' ] AND d.values != 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values != 'C' "
        "RETURN d";

    // FIXME
    // EXPECT_TRUE(arangodb::tests::assertRules(
    //  vocbase(), query, {
    //    arangodb::aql::OptimizerRule::handleArangoSearchViewsRule
    //  }
    //));

    EXPECT_TRUE(findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN ['B'] AND d.values != 'C' RETURN "
        "d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B' ] AND d.values < 'C' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values < 'C' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a < y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values < 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a <= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B' ] AND d.values <= 'C' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [x] && a <= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B' ] AND d.values <= 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a <= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'C' ] AND d.values <= 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a >= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'A' ] AND d.values >= 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [ x ] && a >= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN [ 'B' ] AND d.values >= 'B' "
        "RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [x] && a >= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN ['C'] AND d.values >= 'B' RETURN "
        "d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [x] && a > y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN ['A'] AND d.values > 'B' RETURN "
        "d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [x] && a > y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN ['B'] AND d.values > 'B' RETURN "
        "d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a IN [x] && a > y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values IN ['C'] AND d.values > 'B' RETURN "
        "d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a == y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values == 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a == y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values == 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a == y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values == 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'A' AND d.values != 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values != 'C' RETURN d";

    // FIXME
    // EXPECT_TRUE(arangodb::tests::assertRules(
    //  vocbase(), query, {
    //    arangodb::aql::OptimizerRule::handleArangoSearchViewsRule
    //  }
    //));

    EXPECT_TRUE(findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values != 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a < y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values < 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a < y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values < 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a < y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a <= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values <= 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a <= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a <= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a >= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'A' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a >= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a >= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a > y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'A' AND d.values > 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a > y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'B' AND d.values > 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a == x && a > y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values == 'C' AND d.values > 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a == y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '@' AND d.values == 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a == y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values == 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a == y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values == 'A' RETURN d";

    // FIXME
    // EXPECT_TRUE(arangodb::tests::assertRules(
    //  vocbase(), query, {
    //    arangodb::aql::OptimizerRule::handleArangoSearchViewsRule
    //  }
    //));

    EXPECT_TRUE(findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a == y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values == 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a == y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'B' AND d.values == 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '@' AND d.values != 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values != 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values != 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values != 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'B' AND d.values != 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values < 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '@' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values < 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("D");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a < y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'C' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values <= 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("D");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'B' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a <= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'C' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values >= '0' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("0");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values >= 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values >= 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a >= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'C' AND d.values >= 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values > 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values > 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != '0' AND d.values > '0' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("0");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'A' AND d.values > 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'D' AND d.values > 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a != x && a > y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values != 'C' AND d.values > 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a == y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values == 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a == y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values == 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a == y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values == 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values != 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values != 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }
}

TEST_F(IResearchQueryOptimizationTest, test_2) {
  // a < x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'D' AND d.values != 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("D");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values != 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values != '0' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("0");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values != 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a < y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values < 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a < y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a < y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a <= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values <= 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a <= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values <= 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a <= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a >= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values >= 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a >= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a >= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a > y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values > 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a > y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'B' AND d.values > 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a < x && a > y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values < 'C' AND d.values > 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a == y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values == 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a == y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values == 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a == y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'B' AND d.values == 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values != 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values != 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'B' AND d.values != 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'D' AND d.values != 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("D");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values != '@' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values != 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a < y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a < y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'B' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a < y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a <= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a <= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'B' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a <= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a >= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a >= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values >= 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a >= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values >= 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a > y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values > 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a > y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'A' AND d.values > 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a <= x && a > y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values <= 'C' AND d.values > 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a == y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values == 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a == y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values == 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a == y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values == 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }
}

TEST_F(IResearchQueryOptimizationTest, test_3) {
  // a >= x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values != 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values != 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= '@' AND d.values != '@' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("@");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values != 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values != 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values != 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a < y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a < y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a < y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'C' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a <= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a <= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a <= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'C' AND d.values <= 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a >= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a >= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a >= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'C' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a > y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'A' AND d.values > 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a > y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values > 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a >= x && a > y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values >= 'B' AND d.values > 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a == y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values == 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a == y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values == 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a == y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values == 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_term>().field(mangleStringIdentity("values")).term("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values != 'D' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("D");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values != 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > '@' AND d.values != '@' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("@");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values != 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values != '@' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("@");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a != y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values != 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::Not>()
          .filter<irs::by_term>()
          .field(mangleStringIdentity("values"))
          .term("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }
}

TEST_F(IResearchQueryOptimizationTest, test_4) {
  // a > x && a < y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values < 'C' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("C");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a < y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values < 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a < y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'C' AND d.values < 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("C");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(false)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{};

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a <= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("A");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a <= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values <= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a <= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values <= 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MAX>(true)
          .term<irs::Bound::MAX>("A");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a >= y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(true)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a >= y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values >= 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a >= y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values >= 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a > y, x < y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'A' AND d.values > 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }

  // a > x && a > y, x == y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values > 'B' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }
}

TEST_F(IResearchQueryOptimizationTest, test_5) {
  // a > x && a > y, x > y
  {
    std::string const query =
        "FOR d IN testView SEARCH d.values > 'B' AND d.values > 'A' RETURN d";

    EXPECT_TRUE(arangodb::tests::assertRules(vocbase(), query,
                                             {arangodb::aql::OptimizerRule::handleArangoSearchViewsRule}));

    EXPECT_TRUE(!findEmptyNodes(vocbase(), query));

    // check structure
    {
      irs::Or expected;
      auto& root = expected.add<irs::And>();
      root.add<irs::by_range>()
          .field(mangleStringIdentity("values"))
          .include<irs::Bound::MIN>(false)
          .term<irs::Bound::MIN>("B");
      assertFilterOptimized(vocbase(), query, expected);
    }

    std::vector<arangodb::velocypack::Slice> expectedDocs{
        arangodb::velocypack::Slice(insertedDocs[0].vpack()),
    };

    auto queryResult = arangodb::tests::executeQuery(vocbase(), query);
    ASSERT_TRUE(queryResult.result.ok());

    auto result = queryResult.data->slice();
    EXPECT_TRUE(result.isArray());

    arangodb::velocypack::ArrayIterator resultIt(result);
    ASSERT_TRUE(expectedDocs.size() == resultIt.size());

    // Check documents
    auto expectedDoc = expectedDocs.begin();
    for (; resultIt.valid(); resultIt.next(), ++expectedDoc) {
      auto const actualDoc = resultIt.value();
      auto const resolved = actualDoc.resolveExternals();

      EXPECT_TRUE((0 == arangodb::basics::VelocyPackHelper::compare(
                            arangodb::velocypack::Slice(*expectedDoc), resolved, true)));
    }
    EXPECT_TRUE(expectedDoc == expectedDocs.end());
  }
}
