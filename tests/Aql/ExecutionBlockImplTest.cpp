////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2018 ArangoDB GmbH, Cologne, Germany
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
/// @author Tobias Goedderz
/// @author Michael Hackstein
/// @author Heiko Kernbach
/// @author Jan Christoph Uhde
////////////////////////////////////////////////////////////////////////////////

#include "gtest/gtest.h"

#include "AqlItemBlockHelper.h"
#include "Mocks/Servers.h"
#include "TestEmptyExecutorHelper.h"
#include "TestExecutorHelper.h"
#include "TestLambdaExecutor.h"
#include "WaitingExecutionBlockMock.h"
#include "fakeit.hpp"

#include "Aql/AqlCallStack.h"
#include "Aql/AqlItemBlock.h"
#include "Aql/AqlItemBlockSerializationFormat.h"
#include "Aql/ConstFetcher.h"
#include "Aql/ExecutionBlockImpl.h"
#include "Aql/ExecutionEngine.h"
#include "Aql/IdExecutor.h"
#include "Aql/Query.h"
#include "Aql/SingleRowFetcher.h"
#include "Transaction/Context.h"
#include "Transaction/Methods.h"

using namespace arangodb;
using namespace arangodb::aql;

namespace arangodb {
namespace tests {
namespace aql {

using LambdaExePassThrough = TestLambdaExecutor<BlockPassthrough::Enable>;
using LambdaExe = TestLambdaExecutor<BlockPassthrough::Disable>;

// This test is supposed to only test getSome return values,
// it is not supposed to test the fetch logic!

class ExecutionBlockImplTest : public ::testing::Test {
 protected:
  // ExecutionState state
  SharedAqlItemBlockPtr result;

  // Mock of the ExecutionEngine
  fakeit::Mock<ExecutionEngine> mockEngine;
  ExecutionEngine& engine;

  // Mock of the AqlItemBlockManager
  fakeit::Mock<AqlItemBlockManager> mockBlockManager;
  AqlItemBlockManager& itemBlockManager;

  // Mock of the transaction
  fakeit::Mock<transaction::Methods> mockTrx;
  transaction::Methods& trx;

  // Mock of the transaction context
  fakeit::Mock<transaction::Context> mockContext;
  transaction::Context& context;

  // Mock of the Query
  fakeit::Mock<Query> mockQuery;
  Query& query;

  ExecutionState state;
  ResourceMonitor monitor;

  // Mock of the QueryOptions
  fakeit::Mock<QueryOptions> mockQueryOptions;
  QueryOptions& lqueryOptions;
  ProfileLevel profile;

  // This is not used thus far in Base-Clase
  ExecutionNode const* node = nullptr;

  // Executor Infos
  TestExecutorHelperInfos infos;
  TestEmptyExecutorHelperInfos emptyInfos;

  SharedAqlItemBlockPtr block;

  ExecutionBlockImplTest()
      : engine(mockEngine.get()),
        itemBlockManager(mockBlockManager.get()),
        trx(mockTrx.get()),
        context(mockContext.get()),
        query(mockQuery.get()),
        lqueryOptions(mockQueryOptions.get()),
        profile(ProfileLevel(PROFILE_LEVEL_NONE)),
        node(nullptr),
        infos(0, 1, 1, {}, {0}),
        emptyInfos(0, 1, 1, {}, {0}),
        block(nullptr) {
    fakeit::When(Method(mockBlockManager, requestBlock)).AlwaysDo([&](size_t nrItems, RegisterId nrRegs) -> SharedAqlItemBlockPtr {
      return SharedAqlItemBlockPtr{new AqlItemBlock(itemBlockManager, nrItems, nrRegs)};
    });

    fakeit::When(Method(mockEngine, itemBlockManager)).AlwaysReturn(itemBlockManager);
    fakeit::When(Method(mockEngine, getQuery)).AlwaysReturn(&query);
    fakeit::When(OverloadedMethod(mockBlockManager, returnBlock, void(AqlItemBlock*&)))
        .AlwaysDo([&](AqlItemBlock*& block) -> void {
          AqlItemBlockManager::deleteBlock(block);
          block = nullptr;
        });
    fakeit::When(Method(mockBlockManager, resourceMonitor)).AlwaysReturn(&monitor);
    fakeit::When(ConstOverloadedMethod(mockQuery, queryOptions, QueryOptions const&()))
        .AlwaysDo([&]() -> QueryOptions const& { return lqueryOptions; });
    fakeit::When(OverloadedMethod(mockQuery, queryOptions, QueryOptions & ()))
        .AlwaysDo([&]() -> QueryOptions& { return lqueryOptions; });
    fakeit::When(Method(mockQuery, trx)).AlwaysReturn(&trx);

    fakeit::When(Method(mockQueryOptions, getProfileLevel)).AlwaysReturn(profile);

    fakeit::When(Method(mockTrx, transactionContextPtr)).AlwaysReturn(&context);
    fakeit::When(Method(mockContext, getVPackOptions)).AlwaysReturn(&velocypack::Options::Defaults);
  }
};

TEST_F(ExecutionBlockImplTest,
       there_is_a_block_in_the_upstream_with_no_rows_inside_the_executor_waits_using_getsome) {
  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr block = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(block));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestExecutorHelper> testee(&engine, node, std::move(infos));
  testee.addDependency(&dependency);

  size_t atMost = 1000;
  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_NE(block, nullptr);
  ASSERT_EQ(block->size(), 1);
  ASSERT_EQ(state, ExecutionState::DONE);

  // done should stay done!
  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(block, nullptr);
  ASSERT_EQ(state, ExecutionState::DONE);
}

TEST_F(ExecutionBlockImplTest,
       there_is_a_block_in_the_upstream_with_no_rows_inside_the_executor_waits_using_skipsome) {
  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr block = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(block));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestExecutorHelper> testee(&engine, node, std::move(infos));
  testee.addDependency(&dependency);

  size_t atMost = 1;
  size_t skipped = 0;

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(skipped, 1);

  // done should stay done!
  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(skipped, 0);
}

TEST_F(ExecutionBlockImplTest,
       there_are_multiple_blocks_in_the_upstream_with_no_rows_inside_the_executor_waits_using_getsome_one_block) {
  // we are checking multiple input blocks
  // we are only fetching 1 row each (atMost = 1)
  // after a DONE is returned, it must stay done!

  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr blocka = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockb = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockc = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockd = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blocke = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(blocka));
  blockDeque.push_back(std::move(blockb));
  blockDeque.push_back(std::move(blockc));
  blockDeque.push_back(std::move(blockd));
  blockDeque.push_back(std::move(blocke));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestExecutorHelper> testee(&engine, node, std::move(infos));
  testee.addDependency(&dependency);
  size_t atMost = 1;
  size_t total = 0;

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);

  ASSERT_EQ(total, 5);
}

TEST_F(ExecutionBlockImplTest,
       there_are_multiple_blocks_in_the_upstream_with_no_rows_inside_the_executor_waits_using_getsome_multiple_blocks) {
  // as test above, BUT with a higher atMost value.

  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr blocka = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockb = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockc = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockd = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blocke = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(blocka));
  blockDeque.push_back(std::move(blockb));
  blockDeque.push_back(std::move(blockc));
  blockDeque.push_back(std::move(blockd));
  blockDeque.push_back(std::move(blocke));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestExecutorHelper> testee(&engine, node, std::move(infos));
  testee.addDependency(&dependency);
  size_t atMost = 2;
  size_t total = 0;

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  total = total + block->size();

  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(block, nullptr);

  ASSERT_EQ(total, 5);
}

TEST_F(ExecutionBlockImplTest,
       there_are_multiple_blocks_in_the_upstream_with_no_rows_inside_the_executor_waits_using_skipsome) {
  // we are checking multiple input blocks
  // we are only fetching 1 row each (atMost = 1)
  // after a DONE is returned, it must stay done!

  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr blocka = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockb = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockc = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blockd = buildBlock<1>(itemBlockManager, {{42}});
  SharedAqlItemBlockPtr blocke = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(blocka));
  blockDeque.push_back(std::move(blockb));
  blockDeque.push_back(std::move(blockc));
  blockDeque.push_back(std::move(blockd));
  blockDeque.push_back(std::move(blocke));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestExecutorHelper> testee(&engine, node, std::move(infos));
  testee.addDependency(&dependency);
  size_t atMost = 1;
  size_t skipped = 0;

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  ASSERT_EQ(skipped, 1);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  ASSERT_EQ(skipped, 1);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  ASSERT_EQ(skipped, 1);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::HASMORE);
  ASSERT_EQ(skipped, 1);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::WAITING);
  ASSERT_EQ(skipped, 0);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(skipped, 1);

  std::tie(state, skipped) = testee.skipSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(skipped, 0);
}

TEST_F(ExecutionBlockImplTest,
       there_is_an_invalid_empty_block_in_the_upstream_the_executor_waits_using_getsome) {
  std::deque<SharedAqlItemBlockPtr> blockDeque;
  SharedAqlItemBlockPtr block = buildBlock<1>(itemBlockManager, {{42}});
  blockDeque.push_back(std::move(block));

  WaitingExecutionBlockMock dependency{&engine, node, std::move(blockDeque)};

  ExecutionBlockImpl<TestEmptyExecutorHelper> testee(&engine, node, std::move(emptyInfos));
  testee.addDependency(&dependency);

  size_t atMost = 1000;
  std::tie(state, block) = testee.getSome(atMost);
  ASSERT_EQ(state, ExecutionState::DONE);
  ASSERT_EQ(block, nullptr);
}

// Test of the execute() Logic stack
class ExecutionBlockImplExecuteTest : public ::testing::Test {
 protected:
  mocks::MockAqlServer server{};
  ResourceMonitor monitor{};
  std::unique_ptr<arangodb::aql::Query> fakedQuery{server.createFakeQuery()};

  ExecutionBlockImplExecuteTest() {
    auto engine =
        std::make_unique<ExecutionEngine>(*fakedQuery, SerializationFormat::SHADOWROWS);
    fakedQuery->setEngine(engine.release());
  }

  LambdaExecutorInfos makeInfos(ProduceCall&& call, RegisterId read = 0,
                                RegisterId write = 0) {
    EXPECT_LE(read, write);
    auto readAble = make_shared_unordered_set();
    auto writeAble = make_shared_unordered_set();
    if (read > 0) {
      for (RegisterId i = 0; i <= read; ++i) {
        readAble->emplace(i);
      }
      for (RegisterId i = read + 1; i <= write; ++i) {
        writeAble->emplace(i);
      }
    }
    return LambdaExecutorInfos(readAble, writeAble, read, write, {}, {}, std::move(call));
  }

  std::unique_ptr<ExecutionBlock> createSingleton() {
    return std::make_unique<ExecutionBlockImpl<IdExecutor<ConstFetcher>>>(
        fakedQuery->engine(), nullptr, IdExecutorInfos{0, {}, {}});
  }
};

TEST_F(ExecutionBlockImplExecuteTest, test_toplevel_unlimited_call) {
  AqlCall fullCall{};
  AqlCallStack stack{std::move(fullCall)};
  ExecutionNode const* node = nullptr;
  size_t nrCalls = 0;

  ProduceCall execImpl = [&nrCalls](AqlItemBlockInputRange& input, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
    auto const& clientCall = output.getClientCall();
    if (nrCalls > 10) {
      EXPECT_TRUE(false);
      // This is emergency bailout, we ask way to often here
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
    }
    nrCalls++;
    EXPECT_EQ(clientCall.getOffset(), 0);
    EXPECT_TRUE(std::holds_alternative<AqlCall::Infinity>(clientCall.softLimit));
    EXPECT_TRUE(std::holds_alternative<AqlCall::Infinity>(clientCall.hardLimit));
    EXPECT_FALSE(clientCall.needsFullCount());
    EXPECT_EQ(output.numRowsLeft(), ExecutionBlock::DefaultBatchSize());
    NoStats stats{};
    AqlCall call{};
    return {input.upstreamState(), stats, call};
  };

  ExecutionBlockImpl<LambdaExe> testee{fakedQuery->engine(), node,
                                       makeInfos(std::move(execImpl))};
  auto singleton = createSingleton();
  testee.addDependency(singleton.get());
  auto [state, skipped, block] = testee.execute(stack);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 0);
  EXPECT_EQ(block, nullptr);
  // Once with empty, once with the line by Singleton
  EXPECT_EQ(nrCalls, 2);
}

TEST_F(ExecutionBlockImplExecuteTest, test_toplevel_softlimit_call) {
  AqlCall fullCall{};
  fullCall.softLimit = 20;
  AqlCallStack stack{std::move(fullCall)};
  ExecutionNode const* node = nullptr;
  size_t nrCalls = 0;

  ProduceCall execImpl = [&nrCalls](AqlItemBlockInputRange& input, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
    auto const& clientCall = output.getClientCall();
    if (nrCalls > 10) {
      EXPECT_TRUE(false);
      // This is emergency bailout, we ask way to often here
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
    }
    nrCalls++;
    EXPECT_EQ(clientCall.getOffset(), 0);
    EXPECT_FALSE(std::holds_alternative<AqlCall::Infinity>(clientCall.softLimit));
    EXPECT_TRUE(std::holds_alternative<AqlCall::Infinity>(clientCall.hardLimit));
    EXPECT_EQ(std::get<std::size_t>(clientCall.softLimit), 20);
    EXPECT_FALSE(clientCall.needsFullCount());
    EXPECT_EQ(output.numRowsLeft(), 20);
    NoStats stats{};
    AqlCall call{};
    return {input.upstreamState(), stats, call};
  };

  ExecutionBlockImpl<LambdaExe> testee{fakedQuery->engine(), node,
                                       makeInfos(std::move(execImpl))};
  auto singleton = createSingleton();
  testee.addDependency(singleton.get());
  auto [state, skipped, block] = testee.execute(stack);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 0);
  EXPECT_EQ(block, nullptr);
  // Once with empty, once with the line by Singleton
  EXPECT_EQ(nrCalls, 2);
}

TEST_F(ExecutionBlockImplExecuteTest, test_toplevel_hardlimit_call) {
  AqlCall fullCall{};
  fullCall.hardLimit = 20;
  AqlCallStack stack{std::move(fullCall)};
  ExecutionNode const* node = nullptr;
  size_t nrCalls = 0;

  ProduceCall execImpl = [&nrCalls](AqlItemBlockInputRange& input, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
    auto const& clientCall = output.getClientCall();
    if (nrCalls > 10) {
      EXPECT_TRUE(false);
      // This is emergency bailout, we ask way to often here
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
    }
    nrCalls++;
    EXPECT_EQ(clientCall.getOffset(), 0);
    EXPECT_TRUE(std::holds_alternative<AqlCall::Infinity>(clientCall.softLimit));
    EXPECT_FALSE(std::holds_alternative<AqlCall::Infinity>(clientCall.hardLimit));
    EXPECT_EQ(std::get<std::size_t>(clientCall.hardLimit), 20);
    EXPECT_EQ(output.numRowsLeft(), 20);
    NoStats stats{};
    AqlCall call{};
    return {input.upstreamState(), stats, call};
  };

  ExecutionBlockImpl<LambdaExe> testee{fakedQuery->engine(), node,
                                       makeInfos(std::move(execImpl))};
  auto singleton = createSingleton();
  testee.addDependency(singleton.get());
  auto [state, skipped, block] = testee.execute(stack);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 0);
  EXPECT_EQ(block, nullptr);
  // Once with empty, once with the line by Singleton
  EXPECT_EQ(nrCalls, 2);
}

TEST_F(ExecutionBlockImplExecuteTest, DISABLED_test_toplevel_offset_call_passthrough) {
  AqlCall fullCall{};
  fullCall.offset = 20;
  AqlCallStack stack{std::move(fullCall)};
  ExecutionNode const* node = nullptr;
  size_t nrCalls = 0;

  ProduceCall execImpl = [&nrCalls](AqlItemBlockInputRange& input, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, LambdaExePassThrough::Stats, AqlCall> {
    if (nrCalls > 10) {
      EXPECT_TRUE(false);
      // This is emergency bailout, we ask way to often here
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
    }
    // We are not allowed to get called at all.
    EXPECT_FALSE(true);
    nrCalls++;
    NoStats stats{};
    AqlCall call{};
    return {input.upstreamState(), stats, call};
  };

  ExecutionBlockImpl<LambdaExePassThrough> testee{fakedQuery->engine(), node,
                                                  makeInfos(std::move(execImpl))};
  auto singleton = createSingleton();
  testee.addDependency(singleton.get());
  auto [state, skipped, block] = testee.execute(stack);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 1);
  EXPECT_EQ(block, nullptr);
  // Once with empty, once with the line by Singleton
  EXPECT_EQ(nrCalls, 0);
}

TEST_F(ExecutionBlockImplExecuteTest, test_toplevel_offset_only_call_passthrough) {
  AqlCall fullCall{};
  fullCall.offset = 20;
  // This test simulates a simple "skipSome" call on the old API.
  // It is releveant in any intermediate state.
  fullCall.softLimit = 0;
  AqlCallStack stack{std::move(fullCall)};
  ExecutionNode const* node = nullptr;
  size_t nrCalls = 0;

  ProduceCall execImpl = [&nrCalls](AqlItemBlockInputRange& input, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, LambdaExePassThrough::Stats, AqlCall> {
    if (nrCalls > 10) {
      EXPECT_TRUE(false);
      // This is emergency bailout, we ask way to often here
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
    }
    // We are not allowed to get called at all.
    EXPECT_FALSE(true);
    nrCalls++;
    NoStats stats{};
    AqlCall call{};
    return {input.upstreamState(), stats, call};
  };

  ExecutionBlockImpl<LambdaExePassThrough> testee{fakedQuery->engine(), node,
                                                  makeInfos(std::move(execImpl))};
  auto singleton = createSingleton();
  testee.addDependency(singleton.get());
  auto [state, skipped, block] = testee.execute(stack);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 1);
  EXPECT_EQ(block, nullptr);
  // Once with empty, once with the line by Singleton
  EXPECT_EQ(nrCalls, 0);
}

TEST_F(ExecutionBlockImplExecuteTest, DISABLED_test_toplevel_offset_call_execute) {
  AqlCall fullCall{};
  fullCall.offset = 20;
  AqlCallStack stack{std::move(fullCall)};
  ExecutionNode const* node = nullptr;
  size_t nrCalls = 0;

  ProduceCall execImpl = [&nrCalls](AqlItemBlockInputRange& inputRange, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, LambdaExePassThrough::Stats, AqlCall> {
    auto const& clientCall = output.getClientCall();
    if (nrCalls > 10) {
      EXPECT_TRUE(false);
      // This is emergency bailout, we ask way to often here
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
    }
    nrCalls++;
    EXPECT_EQ(clientCall.getOffset(), 0);
    EXPECT_FALSE(std::holds_alternative<AqlCall::Infinity>(clientCall.softLimit));
    EXPECT_EQ(std::get<std::size_t>(clientCall.softLimit), 20);
    EXPECT_TRUE(std::holds_alternative<AqlCall::Infinity>(clientCall.hardLimit));
    EXPECT_EQ(output.numRowsLeft(), 20);
    while (inputRange.hasDataRow() && !output.isFull()) {
      auto const& [state, input] = inputRange.nextDataRow();
      EXPECT_TRUE(input.isInitialized());
      // We do not write anything, so just advance!
      output.copyRow(input);
      output.advanceRow();
    }

    NoStats stats{};
    // Forward the client call.
    return {inputRange.upstreamState(), stats, output.getClientCall()};
  };

  ExecutionBlockImpl<LambdaExePassThrough> testee{fakedQuery->engine(), node,
                                                  makeInfos(std::move(execImpl))};
  auto singleton = createSingleton();
  testee.addDependency(singleton.get());
  auto [state, skipped, block] = testee.execute(stack);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 1);
  EXPECT_EQ(block, nullptr);
  // Once with empty, once with the line by Singleton
  EXPECT_EQ(nrCalls, 0);
}

TEST_F(ExecutionBlockImplExecuteTest, test_toplevel_offset_only_call_execute) {
  AqlCall fullCall{};
  fullCall.offset = 20;
  fullCall.softLimit = 0;
  AqlCallStack stack{std::move(fullCall)};
  ExecutionNode const* node = nullptr;
  size_t nrCalls = 0;

  ProduceCall execImpl = [&nrCalls](AqlItemBlockInputRange& inputRange, OutputAqlItemRow& output)
      -> std::tuple<ExecutorState, LambdaExePassThrough::Stats, AqlCall> {
    auto const& clientCall = output.getClientCall();
    if (nrCalls > 10) {
      EXPECT_TRUE(false);
      // This is emergency bailout, we ask way to often here
      THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
    }
    nrCalls++;
    EXPECT_EQ(clientCall.getOffset(), 0);
    EXPECT_FALSE(std::holds_alternative<AqlCall::Infinity>(clientCall.softLimit));
    EXPECT_EQ(std::get<std::size_t>(clientCall.softLimit), 20);
    EXPECT_TRUE(std::holds_alternative<AqlCall::Infinity>(clientCall.hardLimit));
    EXPECT_EQ(output.numRowsLeft(), 20);

    while (inputRange.hasDataRow() && !output.isFull()) {
      auto const& [state, input] = inputRange.nextDataRow();
      EXPECT_TRUE(input.isInitialized());
      // We do not write anything, so just advance!
      output.copyRow(input);
      output.advanceRow();
    }

    NoStats stats{};
    return {inputRange.upstreamState(), stats, output.getClientCall()};
  };

  ExecutionBlockImpl<LambdaExePassThrough> testee{fakedQuery->engine(), node,
                                                  makeInfos(std::move(execImpl))};
  auto singleton = createSingleton();
  testee.addDependency(singleton.get());
  auto [state, skipped, block] = testee.execute(stack);

  EXPECT_EQ(state, ExecutionState::DONE);
  EXPECT_EQ(skipped, 1);
  EXPECT_EQ(block, nullptr);
  // Once with empty, once with the line by Singleton
  EXPECT_EQ(nrCalls, 0);
}

class ExecutionBlockImplExecuteIntegrationTest
    : public ExecutionBlockImplExecuteTest,
      public testing::TestWithParam<AqlCall> {
 public:
  ExecutionBlockImplExecuteIntegrationTest()
      : ExecutionBlockImplExecuteTest() {}

 protected:
  std::unique_ptr<ExecutionBlock> _singleton = createSingleton();

  std::unique_ptr<ExecutionBlock> produceBlock(ExecutionBlock* dependency,
                                               std::shared_ptr<VPackBuilder> data,
                                               RegisterId outReg) {
    TRI_ASSERT(dependency != nullptr);
    TRI_ASSERT(outReg > 0);
    TRI_ASSERT(data != nullptr);
    TRI_ASSERT(data->slice().isArray());
    auto writeData = [data, outReg](AqlItemBlockInputRange& inputRange, OutputAqlItemRow& output)
        -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
      while (inputRange.hasDataRow() && !output.isFull()) {
        auto const& [state, input] = inputRange.nextDataRow();
        EXPECT_TRUE(input.isInitialized());
        VPackSlice toWrite = data->slice();
        EXPECT_GE(output.numRowsLeft(), toWrite.length());
        for (auto const& inSlice : VPackArrayIterator(toWrite)) {
          output.cloneValueInto(outReg, input, AqlValue{inSlice});
          output.advanceRow();
        }
      }
      // We always use a default unlimited call here, we only have Singleton above.
      AqlCall call{};
      return {inputRange.upstreamState(), NoStats{}, call};
    };
    auto producer = std::make_unique<ExecutionBlockImpl<LambdaExe>>(
        fakedQuery->engine(), nullptr, makeInfos(std::move(writeData), outReg - 1, outReg));
    producer->addDependency(dependency);
    return producer;
  }

  std::unique_ptr<ExecutionBlock> forwardBlock(ExecutionBlock* dependency, RegisterId maxReg) {
    TRI_ASSERT(dependency != nullptr);
    auto forwardData = [](AqlItemBlockInputRange& inputRange, OutputAqlItemRow& output)
        -> std::tuple<ExecutorState, LambdaExe::Stats, AqlCall> {
      while (inputRange.hasDataRow() && !output.isFull()) {
        auto const& [state, input] = inputRange.nextDataRow();
        EXPECT_TRUE(input.isInitialized());
        output.copyRow(input);
        output.advanceRow();
      }
      return {inputRange.upstreamState(), NoStats{}, output.getClientCall()};
    };
    auto producer = std::make_unique<ExecutionBlockImpl<LambdaExe>>(
        fakedQuery->engine(), nullptr, makeInfos(std::move(forwardData), maxReg, maxReg));
    producer->addDependency(dependency);
    return producer;
  }

  void ValidateResult(std::shared_ptr<VPackBuilder> data, size_t skipped,
                      SharedAqlItemBlockPtr result) {
    auto const& call = GetParam();

    TRI_ASSERT(data != nullptr);
    TRI_ASSERT(data->slice().isArray());

    VPackSlice expected = data->slice();
    VPackArrayIterator expectedIt{expected};
    // Skip Part
    EXPECT_EQ(call.getOffset(), skipped);
    for (size_t i = 0; i < call.getOffset(); ++i) {
      // The first have been skipped
      expectedIt++;
    }

    // GetSome part
    EXPECT_EQ(call.getLimit(), result->numEntries());
    for (size_t i = 0; i < call.getLimit(); ++i) {
      // The next have to match
      auto got = result->getValueReference(i, 1).slice();
      EXPECT_TRUE(basics::VelocyPackHelper::equal(got, *expectedIt, false))
          << "Expected: " << expectedIt.value().toJson()
          << " got: " << got.toJson() << " in row " << i;
      expectedIt++;
    }

    // TODO test fullcount
  }
};

}  // namespace aql
}  // namespace tests
}  // namespace arangodb
