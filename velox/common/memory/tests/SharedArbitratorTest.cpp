/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <re2/re2.h>
#include <deque>

#include <folly/init/Init.h>
#include <functional>
#include <optional>
#include "folly/experimental/EventCount.h"
#include "velox/common/base/Exceptions.h"
#include "velox/common/base/tests/GTestUtils.h"
#include "velox/common/memory/MallocAllocator.h"
#include "velox/common/memory/SharedArbitrator.h"
#include "velox/common/testutil/TestValue.h"
#include "velox/connectors/hive/HiveConfig.h"
#include "velox/core/PlanNode.h"
#include "velox/exec/Driver.h"
#include "velox/exec/HashAggregation.h"
#include "velox/exec/PlanNodeStats.h"
#include "velox/exec/TableWriter.h"
#include "velox/exec/Values.h"
#include "velox/exec/tests/utils/ArbitratorTestUtil.h"
#include "velox/exec/tests/utils/HiveConnectorTestBase.h"
#include "velox/exec/tests/utils/SumNonPODAggregate.h"

DECLARE_bool(velox_memory_leak_check_enabled);
DECLARE_bool(velox_suppress_memory_capacity_exceeding_error_message);

using namespace ::testing;
using namespace facebook::velox::common::testutil;
using namespace facebook::velox::exec;
using namespace facebook::velox::exec::test;

namespace facebook::velox::memory {
// Custom node for the custom factory.
class FakeMemoryNode : public core::PlanNode {
 public:
  FakeMemoryNode(const core::PlanNodeId& id, core::PlanNodePtr input)
      : PlanNode(id), sources_{input} {}

  const RowTypePtr& outputType() const override {
    return sources_[0]->outputType();
  }

  const std::vector<std::shared_ptr<const PlanNode>>& sources() const override {
    return sources_;
  }

  std::string_view name() const override {
    return "FakeMemoryNode";
  }

 private:
  void addDetails(std::stringstream& /* stream */) const override {}

  std::vector<core::PlanNodePtr> sources_;
};

using AllocationCallback = std::function<TestAllocation(Operator* op)>;
// If return true, the caller will terminate execution and return early.
using ReclaimInjectionCallback = std::function<bool(
    memory::MemoryPool* pool,
    uint64_t targetByte,
    MemoryReclaimer::Stats& stats)>;

// Custom operator for the custom factory.
class FakeMemoryOperator : public Operator {
 public:
  FakeMemoryOperator(
      DriverCtx* ctx,
      int32_t id,
      core::PlanNodePtr node,
      bool canReclaim,
      AllocationCallback allocationCb,
      ReclaimInjectionCallback reclaimCb)
      : Operator(ctx, node->outputType(), id, node->id(), "FakeMemoryNode"),
        canReclaim_(canReclaim),
        allocationCb_(std::move(allocationCb)),
        reclaimCb_(std::move(reclaimCb)) {}

  ~FakeMemoryOperator() override {
    clear();
  }

  bool needsInput() const override {
    return !noMoreInput_;
  }

  void addInput(RowVectorPtr input) override {
    input_ = std::move(input);
    if (allocationCb_ != nullptr) {
      TestAllocation allocation = allocationCb_(this);
      if (allocation.buffer != nullptr) {
        allocations_.push_back(allocation);
      }
      totalBytes_ += allocation.size;
    }
  }

  void noMoreInput() override {
    clear();
    Operator::noMoreInput();
  }

  RowVectorPtr getOutput() override {
    return std::move(input_);
  }

  BlockingReason isBlocked(ContinueFuture* /*future*/) override {
    return BlockingReason::kNotBlocked;
  }

  bool isFinished() override {
    return noMoreInput_ && input_ == nullptr && allocations_.empty();
  }

  void close() override {
    clear();
    Operator::close();
  }

  bool canReclaim() const override {
    return canReclaim_;
  }

  void reclaim(uint64_t targetBytes, memory::MemoryReclaimer::Stats& stats)
      override {
    VELOX_CHECK(canReclaim());
    auto* driver = operatorCtx_->driver();
    VELOX_CHECK(!driver->state().isOnThread() || driver->state().suspended());
    VELOX_CHECK(driver->task()->pauseRequested());
    VELOX_CHECK_GT(targetBytes, 0);

    if (reclaimCb_ != nullptr && reclaimCb_(pool(), targetBytes, stats)) {
      return;
    }

    uint64_t bytesReclaimed{0};
    auto allocIt = allocations_.begin();
    while (allocIt != allocations_.end() &&
           ((targetBytes != 0) && (bytesReclaimed < targetBytes))) {
      bytesReclaimed += allocIt->size;
      totalBytes_ -= allocIt->size;
      pool()->free(allocIt->buffer, allocIt->size);
      allocIt = allocations_.erase(allocIt);
    }
  }

 private:
  void clear() {
    for (auto& allocation : allocations_) {
      totalBytes_ -= allocation.free();
    }
    allocations_.clear();
    VELOX_CHECK_EQ(totalBytes_.load(), 0);
  }

  const bool canReclaim_;
  const AllocationCallback allocationCb_;
  const ReclaimInjectionCallback reclaimCb_{nullptr};

  std::atomic<size_t> totalBytes_{0};
  std::vector<TestAllocation> allocations_;
};

// Custom factory that creates fake memory operator.
class FakeMemoryOperatorFactory : public Operator::PlanNodeTranslator {
 public:
  FakeMemoryOperatorFactory() = default;

  std::unique_ptr<Operator> toOperator(
      DriverCtx* ctx,
      int32_t id,
      const core::PlanNodePtr& node) override {
    if (std::dynamic_pointer_cast<const FakeMemoryNode>(node)) {
      return std::make_unique<FakeMemoryOperator>(
          ctx, id, node, canReclaim_, allocationCallback_, reclaimCallback_);
    }
    return nullptr;
  }

  std::optional<uint32_t> maxDrivers(const core::PlanNodePtr& node) override {
    if (std::dynamic_pointer_cast<const FakeMemoryNode>(node)) {
      return maxDrivers_;
    }
    return std::nullopt;
  }

  void setMaxDrivers(uint32_t maxDrivers) {
    maxDrivers_ = maxDrivers;
  }

  void setCanReclaim(bool canReclaim) {
    canReclaim_ = canReclaim;
  }

  void setAllocationCallback(AllocationCallback allocCb) {
    allocationCallback_ = std::move(allocCb);
  }

  void setReclaimCallback(ReclaimInjectionCallback reclaimCb) {
    reclaimCallback_ = std::move(reclaimCb);
  }

 private:
  bool canReclaim_{true};
  AllocationCallback allocationCallback_{nullptr};
  ReclaimInjectionCallback reclaimCallback_{nullptr};
  uint32_t maxDrivers_{1};
};

namespace {
std::unique_ptr<folly::Executor> newParallelExecutor() {
  return std::make_unique<folly::CPUThreadPoolExecutor>(32);
}

struct TestParam {
  bool isSerialExecutionMode{false};
};
} // namespace

class SharedArbitrationTest : public testing::WithParamInterface<TestParam>,
                              public exec::test::HiveConnectorTestBase {
 public:
 protected:
  static void SetUpTestCase() {
    exec::test::HiveConnectorTestBase::SetUpTestCase();
    auto fakeOperatorFactory = std::make_unique<FakeMemoryOperatorFactory>();
    fakeOperatorFactory_ = fakeOperatorFactory.get();
    Operator::registerOperator(std::move(fakeOperatorFactory));
  }

  void SetUp() override {
    HiveConnectorTestBase::SetUp();
    fakeOperatorFactory_->setCanReclaim(true);

    setupMemory();

    rowType_ = ROW(
        {{"c0", INTEGER()},
         {"c1", INTEGER()},
         {"c2", VARCHAR()},
         {"c3", VARCHAR()}});
    fuzzerOpts_.vectorSize = 1024;
    fuzzerOpts_.nullRatio = 0;
    fuzzerOpts_.stringVariableLength = false;
    fuzzerOpts_.stringLength = 1024;
    fuzzerOpts_.allowLazyVector = false;
    vector_ = makeRowVector(rowType_, fuzzerOpts_);
    isSerialExecutionMode_ = GetParam().isSerialExecutionMode;
    if (isSerialExecutionMode_) {
      executor_ = nullptr;
    } else {
      executor_ = newParallelExecutor();
    }
  }

  void TearDown() override {
    vector_.reset();
    HiveConnectorTestBase::TearDown();
  }

  void setupMemory(
      int64_t memoryCapacity = 0,
      uint64_t memoryPoolInitCapacity = kMemoryPoolInitCapacity) {
    memoryCapacity = (memoryCapacity != 0) ? memoryCapacity : kMemoryCapacity;
    memoryManager_ =
        createMemoryManager(memoryCapacity, memoryPoolInitCapacity);
    ASSERT_EQ(memoryManager_->arbitrator()->kind(), "SHARED");
    arbitrator_ = static_cast<SharedArbitrator*>(memoryManager_->arbitrator());
  }

  void checkOperatorStatsForArbitration(
      PlanNodeStats& stats,
      bool expectGlobalArbitration) {
    if (expectGlobalArbitration) {
      VELOX_CHECK_EQ(
          stats.customStats.count(
              SharedArbitrator::kGlobalArbitrationWaitCount),
          1);
      VELOX_CHECK_GE(
          stats.customStats.at(SharedArbitrator::kGlobalArbitrationWaitCount)
              .sum,
          1);
      VELOX_CHECK_EQ(
          stats.customStats.count(SharedArbitrator::kLocalArbitrationCount), 0);
    } else {
      VELOX_CHECK_EQ(
          stats.customStats.count(SharedArbitrator::kLocalArbitrationCount), 1);
      VELOX_CHECK_EQ(
          stats.customStats.at(SharedArbitrator::kLocalArbitrationCount).sum,
          1);
      VELOX_CHECK_EQ(
          stats.customStats.count(
              SharedArbitrator::kGlobalArbitrationWaitCount),
          0);
    }
  }

  AssertQueryBuilder newQueryBuilder() {
    AssertQueryBuilder builder = AssertQueryBuilder(duckDbQueryRunner_);
    builder.serialExecution(isSerialExecutionMode_);
    return builder;
  }

  AssertQueryBuilder newQueryBuilder(const core::PlanNodePtr& plan) {
    AssertQueryBuilder builder = AssertQueryBuilder(plan);
    builder.serialExecution(isSerialExecutionMode_);
    return builder;
  }

  static inline FakeMemoryOperatorFactory* fakeOperatorFactory_;
  std::unique_ptr<memory::MemoryManager> memoryManager_;
  SharedArbitrator* arbitrator_{nullptr};
  RowTypePtr rowType_;
  VectorFuzzer::Options fuzzerOpts_;
  RowVectorPtr vector_;
  bool isSerialExecutionMode_{false};
};

/// A test fixture that runs cases within parallel execution mode.
class SharedArbitrationTestWithParallelExecutionModeOnly
    : public SharedArbitrationTest {};
/// A test fixture that runs cases within both serial and
/// parallel execution modes.
class SharedArbitrationTestWithThreadingModes : public SharedArbitrationTest {};

DEBUG_ONLY_TEST_P(
    SharedArbitrationTestWithThreadingModes,
    queryArbitrationStateCheck) {
  const std::vector<RowVectorPtr> vectors =
      createVectors(rowType_, 32, 32 << 20);
  createDuckDbTable(vectors);
  std::shared_ptr<core::QueryCtx> queryCtx =
      newQueryCtx(memory::memoryManager(), executor_.get(), kMemoryCapacity);

  std::atomic_bool queryCtxStateChecked{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Task::requestPauseLocked",
      std::function<void(Task*)>(([&](Task* /*unused*/) {
        ASSERT_TRUE(queryCtx->testingUnderArbitration());
        queryCtxStateChecked = true;
      })));

  const auto spillDirectory = exec::test::TempDirectoryPath::create();
  TestScopedSpillInjection scopedSpillInjection(100);
  core::PlanNodeId aggregationNodeId;
  newQueryBuilder()
      .queryCtx(queryCtx)
      .spillDirectory(spillDirectory->getPath())
      .config(core::QueryConfig::kSpillEnabled, "true")
      .plan(PlanBuilder()
                .values(vectors)
                .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                .capturePlanNodeId(aggregationNodeId)
                .planNode())
      .assertResults("SELECT c0, c1, array_agg(c2) FROM tmp GROUP BY c0, c1");
  ASSERT_TRUE(queryCtxStateChecked);
  ASSERT_FALSE(queryCtx->testingUnderArbitration());
  waitForAllTasksToBeDeleted();
  ASSERT_FALSE(queryCtx->testingUnderArbitration());
}

DEBUG_ONLY_TEST_P(
    SharedArbitrationTestWithThreadingModes,
    raceBetweenAbortAndArbitrationLeave) {
  const std::vector<RowVectorPtr> vectors =
      createVectors(rowType_, 32, 32 << 20);
  setupMemory(kMemoryCapacity, /*memoryPoolInitCapacity=*/0);
  std::shared_ptr<core::QueryCtx> queryCtx =
      newQueryCtx(memoryManager_.get(), executor_.get(), 32 << 20);

  folly::EventCount abortWait;
  std::atomic_bool abortWaitFlag{true};
  std::atomic<Task*> task{nullptr};
  const std::string errorMsg{"injected abort error"};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Task::leaveSuspended",
      std::function<void(exec::Task*)>(([&](exec::Task* _task) {
        if (task.exchange(_task) != nullptr) {
          return;
        }
        abortWaitFlag = false;
        abortWait.notifyAll();
        // Let memory pool abort thread to run first. We inject a randomized
        // delay here to trigger all the potential timing race conditions but
        // the test result should be the same.
        std::this_thread::sleep_for(
            std::chrono::milliseconds(folly::Random::rand32() % 1'000));
      })));

  std::thread queryThread([&] {
    const auto spillDirectory = exec::test::TempDirectoryPath::create();
    core::PlanNodeId aggregationNodeId;
    auto plan = PlanBuilder()
                    .values(vectors)
                    .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                    .capturePlanNodeId(aggregationNodeId)
                    .planNode();
    VELOX_ASSERT_THROW(
        newQueryBuilder(plan)
            .queryCtx(queryCtx)
            .spillDirectory(spillDirectory->getPath())
            .config(core::QueryConfig::kSpillEnabled, "true")
            .copyResults(pool()),
        errorMsg);
  });

  abortWait.await([&] { return !abortWaitFlag.load(); });

  try {
    VELOX_FAIL(errorMsg);
  } catch (...) {
    task.load()->pool()->abort(std::current_exception());
  }
  queryThread.join();
  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_P(
    SharedArbitrationTestWithThreadingModes,
    skipNonReclaimableTaskTest) {
  const std::vector<RowVectorPtr> vectors =
      createVectors(rowType_, 32, 32 << 20);
  std::shared_ptr<core::QueryCtx> queryCtx =
      newQueryCtx(memory::memoryManager(), executor_.get(), kMemoryCapacity);
  std::unordered_map<std::string, std::string> configs;
  configs.emplace(core::QueryConfig::kSpillEnabled, "true");
  queryCtx->testingOverrideConfigUnsafe(std::move(configs));

  std::atomic_bool blockedAggregation{false};
  std::atomic_bool blockedPartialAggregation{false};
  folly::EventCount arbitrationWait;
  std::atomic<bool> arbitrationWaitFlag{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Driver::runInternal::addInput",
      std::function<void(exec::Operator*)>(([&](exec::Operator* op) {
        if (op->operatorCtx()->operatorType() != "Aggregation" &&
            op->operatorCtx()->operatorType() != "PartialAggregation") {
          return;
        }
        if (op->pool()->usedBytes() == 0) {
          return;
        }
        if (op->operatorCtx()->operatorType() == "PartialAggregation") {
          if (blockedPartialAggregation.exchange(true)) {
            return;
          }
        } else {
          if (blockedAggregation.exchange(true)) {
            return;
          }
        }
        auto* driver = op->operatorCtx()->driver();
        TestSuspendedSection suspendedSection(driver);
        arbitrationWait.await([&]() { return !arbitrationWaitFlag.load(); });
      })));

  std::atomic_int taskPausedCount{0};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Task::requestPauseLocked",
      std::function<void(Task*)>(([&](Task* /*unused*/) {
        ASSERT_TRUE(queryCtx->testingUnderArbitration());
        ++taskPausedCount;
      })));

  const auto spillPlan = PlanBuilder()
                             .values(vectors)
                             .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                             .planNode();
  std::thread spillableThread([&]() {
    const auto spillDirectory = exec::test::TempDirectoryPath::create();
    newQueryBuilder(spillPlan)
        .queryCtx(queryCtx)
        .spillDirectory(spillDirectory->getPath())
        .copyResults(pool());
  });

  const auto nonSpillPlan = PlanBuilder()
                                .values(vectors)
                                .aggregation(
                                    {"c0", "c1"},
                                    {"array_agg(c2)"},
                                    {},
                                    core::AggregationNode::Step::kPartial,
                                    false)
                                .planNode();
  std::thread nonSpillableThread([&]() {
    newQueryBuilder(nonSpillPlan).queryCtx(queryCtx).copyResults(pool());
  });

  while (!blockedPartialAggregation || !blockedAggregation) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // NOLINT
  }

  testingRunArbitration();

  arbitrationWaitFlag = false;
  arbitrationWait.notifyAll();

  spillableThread.join();
  nonSpillableThread.join();

  // We shall only reclaim from the reclaimable task but not non-reclaimable
  // task.
  ASSERT_EQ(taskPausedCount, 1);
  ASSERT_FALSE(queryCtx->testingUnderArbitration());
  waitForAllTasksToBeDeleted();
  ASSERT_FALSE(queryCtx->testingUnderArbitration());
  ASSERT_EQ(taskPausedCount, 1);
}

DEBUG_ONLY_TEST_P(SharedArbitrationTestWithThreadingModes, reclaimToOrderBy) {
  const int numVectors = 32;
  std::vector<RowVectorPtr> vectors;
  for (int i = 0; i < numVectors; ++i) {
    vectors.push_back(makeRowVector(rowType_, fuzzerOpts_));
  }
  createDuckDbTable(vectors);
  std::vector<bool> sameQueries = {false, true};
  for (bool sameQuery : sameQueries) {
    SCOPED_TRACE(fmt::format("sameQuery {}", sameQuery));
    const auto oldStats = arbitrator_->stats();
    std::shared_ptr<core::QueryCtx> fakeMemoryQueryCtx =
        newQueryCtx(memoryManager_.get(), executor_.get(), kMemoryCapacity);
    std::shared_ptr<core::QueryCtx> orderByQueryCtx;
    if (sameQuery) {
      orderByQueryCtx = fakeMemoryQueryCtx;
    } else {
      orderByQueryCtx =
          newQueryCtx(memoryManager_.get(), executor_.get(), kMemoryCapacity);
    }

    folly::EventCount orderByWait;
    auto orderByWaitKey = orderByWait.prepareWait();
    folly::EventCount taskPauseWait;
    auto taskPauseWaitKey = taskPauseWait.prepareWait();

    const auto fakeAllocationSize = kMemoryCapacity - (32L << 20);

    std::atomic<bool> injectAllocationOnce{true};
    fakeOperatorFactory_->setAllocationCallback([&](Operator* op) {
      if (!injectAllocationOnce.exchange(false)) {
        return TestAllocation{};
      }
      auto buffer = op->pool()->allocate(fakeAllocationSize);
      orderByWait.notify();
      // Wait for pause to be triggered.
      taskPauseWait.wait(taskPauseWaitKey);
      return TestAllocation{op->pool(), buffer, fakeAllocationSize};
    });

    std::atomic<bool> injectOrderByOnce{true};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::addInput",
        std::function<void(Operator*)>(([&](Operator* op) {
          if (op->operatorType() != "OrderBy") {
            return;
          }
          if (!injectOrderByOnce.exchange(false)) {
            return;
          }
          orderByWait.wait(orderByWaitKey);
        })));

    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Task::requestPauseLocked",
        std::function<void(Task*)>(
            ([&](Task* /*unused*/) { taskPauseWait.notify(); })));

    std::thread orderByThread([&]() {
      core::PlanNodeId orderByNodeId;
      auto task =
          newQueryBuilder()
              .queryCtx(orderByQueryCtx)
              .serialExecution(isSerialExecutionMode_)
              .plan(PlanBuilder()
                        .values(vectors)
                        .orderBy({"c0 ASC NULLS LAST"}, false)
                        .capturePlanNodeId(orderByNodeId)
                        .planNode())
              .assertResults("SELECT * FROM tmp ORDER BY c0 ASC NULLS LAST");
      auto taskStats = exec::toPlanStats(task->taskStats());
      auto& stats = taskStats.at(orderByNodeId);
      checkOperatorStatsForArbitration(
          stats, !sameQuery /*expectGlobalArbitration*/);
    });

    std::thread memThread([&]() {
      auto task =
          newQueryBuilder()
              .queryCtx(fakeMemoryQueryCtx)
              .serialExecution(isSerialExecutionMode_)
              .plan(PlanBuilder()
                        .values(vectors)
                        .addNode([&](std::string id, core::PlanNodePtr input) {
                          return std::make_shared<FakeMemoryNode>(id, input);
                        })
                        .planNode())
              .assertResults("SELECT * FROM tmp");
    });

    orderByThread.join();
    memThread.join();
    waitForAllTasksToBeDeleted();
    const auto newStats = arbitrator_->stats();
    ASSERT_GT(newStats.reclaimedUsedBytes, oldStats.reclaimedUsedBytes);
    ASSERT_GT(orderByQueryCtx->pool()->stats().numCapacityGrowths, 0);
  }
}

DEBUG_ONLY_TEST_P(
    SharedArbitrationTestWithThreadingModes,
    reclaimToAggregation) {
  const int numVectors = 32;
  std::vector<RowVectorPtr> vectors;
  for (int i = 0; i < numVectors; ++i) {
    vectors.push_back(makeRowVector(rowType_, fuzzerOpts_));
  }
  createDuckDbTable(vectors);
  std::vector<bool> sameQueries = {false, true};
  for (bool sameQuery : sameQueries) {
    SCOPED_TRACE(fmt::format("sameQuery {}", sameQuery));
    const auto oldStats = arbitrator_->stats();
    std::shared_ptr<core::QueryCtx> fakeMemoryQueryCtx =
        newQueryCtx(memoryManager_.get(), executor_.get(), kMemoryCapacity);
    std::shared_ptr<core::QueryCtx> aggregationQueryCtx;
    if (sameQuery) {
      aggregationQueryCtx = fakeMemoryQueryCtx;
    } else {
      aggregationQueryCtx =
          newQueryCtx(memoryManager_.get(), executor_.get(), kMemoryCapacity);
    }

    folly::EventCount aggregationWait;
    auto aggregationWaitKey = aggregationWait.prepareWait();
    folly::EventCount taskPauseWait;
    auto taskPauseWaitKey = taskPauseWait.prepareWait();

    const auto fakeAllocationSize = kMemoryCapacity - (32L << 20);

    std::atomic<bool> injectAllocationOnce{true};
    fakeOperatorFactory_->setAllocationCallback([&](Operator* op) {
      if (!injectAllocationOnce.exchange(false)) {
        return TestAllocation{};
      }
      auto buffer = op->pool()->allocate(fakeAllocationSize);
      aggregationWait.notify();
      // Wait for pause to be triggered.
      taskPauseWait.wait(taskPauseWaitKey);
      return TestAllocation{op->pool(), buffer, fakeAllocationSize};
    });

    std::atomic<bool> injectAggregationOnce{true};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::addInput",
        std::function<void(Operator*)>(([&](Operator* op) {
          if (op->operatorType() != "Aggregation") {
            return;
          }
          if (!injectAggregationOnce.exchange(false)) {
            return;
          }
          aggregationWait.wait(aggregationWaitKey);
        })));

    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Task::requestPauseLocked",
        std::function<void(Task*)>(
            ([&](Task* /*unused*/) { taskPauseWait.notify(); })));

    std::thread aggregationThread([&]() {
      core::PlanNodeId aggregationNodeId;
      auto task =
          newQueryBuilder()
              .queryCtx(aggregationQueryCtx)
              .serialExecution(isSerialExecutionMode_)
              .plan(PlanBuilder()
                        .values(vectors)
                        .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                        .capturePlanNodeId(aggregationNodeId)
                        .planNode())
              .assertResults(
                  "SELECT c0, c1, array_agg(c2) FROM tmp GROUP BY c0, c1");
      auto taskStats = exec::toPlanStats(task->taskStats());
      auto& stats = taskStats.at(aggregationNodeId);
      checkOperatorStatsForArbitration(
          stats, !sameQuery /*expectGlobalArbitration*/);
    });

    std::thread memThread([&]() {
      auto task =
          newQueryBuilder()
              .queryCtx(fakeMemoryQueryCtx)
              .serialExecution(isSerialExecutionMode_)
              .plan(PlanBuilder()
                        .values(vectors)
                        .addNode([&](std::string id, core::PlanNodePtr input) {
                          return std::make_shared<FakeMemoryNode>(id, input);
                        })
                        .planNode())
              .assertResults("SELECT * FROM tmp");
    });

    aggregationThread.join();
    memThread.join();
    waitForAllTasksToBeDeleted();

    const auto newStats = arbitrator_->stats();
    ASSERT_GT(newStats.reclaimedUsedBytes, oldStats.reclaimedUsedBytes);
  }
}

DEBUG_ONLY_TEST_P(
    SharedArbitrationTestWithThreadingModes,
    reclaimToJoinBuilder) {
  const int numVectors = 32;
  std::vector<RowVectorPtr> vectors;
  for (int i = 0; i < numVectors; ++i) {
    vectors.push_back(makeRowVector(rowType_, fuzzerOpts_));
  }
  createDuckDbTable(vectors);
  std::vector<bool> sameQueries = {false, true};
  for (bool sameQuery : sameQueries) {
    SCOPED_TRACE(fmt::format("sameQuery {}", sameQuery));
    const auto oldStats = arbitrator_->stats();
    std::shared_ptr<core::QueryCtx> fakeMemoryQueryCtx =
        newQueryCtx(memoryManager_.get(), executor_.get(), kMemoryCapacity);
    std::shared_ptr<core::QueryCtx> joinQueryCtx;
    if (sameQuery) {
      joinQueryCtx = fakeMemoryQueryCtx;
    } else {
      joinQueryCtx =
          newQueryCtx(memoryManager_.get(), executor_.get(), kMemoryCapacity);
    }

    folly::EventCount joinWait;
    auto joinWaitKey = joinWait.prepareWait();
    folly::EventCount taskPauseWait;
    auto taskPauseWaitKey = taskPauseWait.prepareWait();

    const auto fakeAllocationSize = kMemoryCapacity - (32L << 20);

    std::atomic<bool> injectAllocationOnce{true};
    fakeOperatorFactory_->setAllocationCallback([&](Operator* op) {
      if (!injectAllocationOnce.exchange(false)) {
        return TestAllocation{};
      }
      auto buffer = op->pool()->allocate(fakeAllocationSize);
      joinWait.notify();
      // Wait for pause to be triggered.
      taskPauseWait.wait(taskPauseWaitKey);
      return TestAllocation{op->pool(), buffer, fakeAllocationSize};
    });

    std::atomic<bool> injectJoinOnce{true};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Driver::runInternal::addInput",
        std::function<void(Operator*)>(([&](Operator* op) {
          if (op->operatorType() != "HashBuild") {
            return;
          }
          if (!injectJoinOnce.exchange(false)) {
            return;
          }
          joinWait.wait(joinWaitKey);
        })));

    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Task::requestPauseLocked",
        std::function<void(Task*)>(
            ([&](Task* /*unused*/) { taskPauseWait.notify(); })));

    std::thread joinThread([&]() {
      auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
      core::PlanNodeId joinNodeId;
      auto task =
          newQueryBuilder()
              .queryCtx(joinQueryCtx)
              .serialExecution(isSerialExecutionMode_)
              .plan(PlanBuilder(planNodeIdGenerator)
                        .values(vectors)
                        .project({"c0 AS t0", "c1 AS t1", "c2 AS t2"})
                        .hashJoin(
                            {"t0"},
                            {"u0"},
                            PlanBuilder(planNodeIdGenerator)
                                .values(vectors)
                                .project({"c0 AS u0", "c1 AS u1", "c2 AS u2"})
                                .planNode(),
                            "",
                            {"t1"},
                            core::JoinType::kAnti)
                        .capturePlanNodeId(joinNodeId)
                        .planNode())
              .assertResults(
                  "SELECT c1 FROM tmp WHERE c0 NOT IN (SELECT c0 FROM tmp)");
      auto taskStats = exec::toPlanStats(task->taskStats());
      auto& stats = taskStats.at(joinNodeId);
      checkOperatorStatsForArbitration(
          stats, !sameQuery /*expectGlobalArbitration*/);
    });

    std::thread memThread([&]() {
      auto task =
          newQueryBuilder()
              .queryCtx(fakeMemoryQueryCtx)
              .serialExecution(isSerialExecutionMode_)
              .plan(PlanBuilder()
                        .values(vectors)
                        .addNode([&](std::string id, core::PlanNodePtr input) {
                          return std::make_shared<FakeMemoryNode>(id, input);
                        })
                        .planNode())
              .assertResults("SELECT * FROM tmp");
    });

    joinThread.join();
    memThread.join();
    waitForAllTasksToBeDeleted();

    const auto newStats = arbitrator_->stats();
    ASSERT_GT(newStats.reclaimedUsedBytes, oldStats.reclaimedUsedBytes);
  }
}

DEBUG_ONLY_TEST_P(
    SharedArbitrationTestWithThreadingModes,
    driverInitTriggeredArbitration) {
  const int numVectors = 2;
  std::vector<RowVectorPtr> vectors;
  const int vectorSize = 100;
  fuzzerOpts_.vectorSize = vectorSize;
  for (int i = 0; i < numVectors; ++i) {
    vectors.push_back(makeRowVector(rowType_, fuzzerOpts_));
  }
  const int expectedResultVectorSize = numVectors * vectorSize;
  const auto expectedVector = makeRowVector(
      {"c0", "c1"},
      {makeFlatVector<int64_t>(
           expectedResultVectorSize, [&](auto /*unused*/) { return 6; }),
       makeFlatVector<int64_t>(
           expectedResultVectorSize, [&](auto /*unused*/) { return 7; })});

  createDuckDbTable(vectors);
  setupMemory(kMemoryCapacity, 0);
  std::shared_ptr<core::QueryCtx> queryCtx =
      newQueryCtx(memoryManager_.get(), executor_.get(), kMemoryCapacity);
  ASSERT_EQ(queryCtx->pool()->capacity(), 0);
  ASSERT_EQ(queryCtx->pool()->maxCapacity(), kMemoryCapacity);

  auto planNodeIdGenerator = std::make_shared<core::PlanNodeIdGenerator>();
  newQueryBuilder()
      .config(core::QueryConfig::kSpillEnabled, "false")
      .queryCtx(queryCtx)
      .plan(PlanBuilder(planNodeIdGenerator, pool())
                .values(vectors)
                // Set filter projection to trigger memory allocation on
                // driver init.
                .project({"1+1+4 as t0", "1+3+3 as t1"})
                .planNode())
      .assertResults(expectedVector);
  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_P(
    SharedArbitrationTestWithThreadingModes,
    DISABLED_raceBetweenTaskTerminateAndReclaim) {
  setupMemory(kMemoryCapacity, 0);
  const int numVectors = 10;
  std::vector<RowVectorPtr> vectors;
  for (int i = 0; i < numVectors; ++i) {
    vectors.push_back(makeRowVector(rowType_, fuzzerOpts_));
  }
  createDuckDbTable(vectors);

  std::shared_ptr<core::QueryCtx> queryCtx =
      newQueryCtx(memoryManager_.get(), executor_.get(), kMemoryCapacity);
  ASSERT_EQ(queryCtx->pool()->capacity(), 0);

  // Allocate a large chunk of memory to trigger memory reclaim during the
  // query execution.
  auto fakeLeafPool = queryCtx->pool()->addLeafChild("fakeLeaf");
  const size_t fakeAllocationSize = kMemoryCapacity / 2;
  TestAllocation fakeAllocation{
      fakeLeafPool.get(),
      fakeLeafPool->allocate(fakeAllocationSize),
      fakeAllocationSize};

  // Set test injection to enforce memory arbitration based on the fake
  // allocation size and the total available memory.
  std::shared_ptr<Task> task;
  std::atomic<bool> injectAllocationOnce{true};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Values::getOutput",
      std::function<void(const exec::Values*)>([&](const exec::Values* values) {
        if (!injectAllocationOnce.exchange(false)) {
          return;
        }
        task = values->operatorCtx()->task();
        memory::MemoryPool* pool = values->pool();
        VELOX_ASSERT_THROW(
            pool->allocate(kMemoryCapacity * 2 / 3),
            "Exceeded memory pool cap");
      }));

  // Set test injection to wait until the reclaim on hash aggregation operator
  // triggers.
  folly::EventCount opReclaimStartWait;
  std::atomic<bool> opReclaimStarted{false};
  folly::EventCount taskAbortWait;
  std::atomic<bool> taskAborted{false};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Operator::MemoryReclaimer::reclaim",
      std::function<void(memory::MemoryPool*)>(([&](memory::MemoryPool* pool) {
        const std::string re(".*Aggregation");
        if (!RE2::FullMatch(pool->name(), re)) {
          return;
        }
        opReclaimStarted = true;
        opReclaimStartWait.notifyAll();
        // Wait for task abort to happen before the actual memory reclaim.
        taskAbortWait.await([&]() { return taskAborted.load(); });
      })));

  const int numDrivers = 1;
  const auto spillDirectory = exec::test::TempDirectoryPath::create();
  std::thread queryThread([&]() {
    VELOX_ASSERT_THROW(
        newQueryBuilder()
            .queryCtx(queryCtx)
            .spillDirectory(spillDirectory->getPath())
            .config(core::QueryConfig::kSpillEnabled, "true")
            .config(core::QueryConfig::kJoinSpillEnabled, "true")
            .config(core::QueryConfig::kSpillNumPartitionBits, "2")
            .maxDrivers(numDrivers)
            .plan(PlanBuilder()
                      .values(vectors)
                      .localPartition({"c0", "c1"})
                      .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                      .localPartition(std::vector<std::string>{})
                      .planNode())
            .assertResults(
                "SELECT c0, c1, array_agg(c2) FROM tmp GROUP BY c0, c1"),
        "Aborted for external error");
  });

  // Wait for the reclaim on aggregation to be started before the task abort.
  opReclaimStartWait.await([&]() { return opReclaimStarted.load(); });
  ASSERT_TRUE(task != nullptr);
  task->requestAbort().wait();

  // Resume aggregation reclaim to execute.
  taskAborted = true;
  taskAbortWait.notifyAll();

  queryThread.join();
  fakeAllocation.free();
  task.reset();
  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_P(
    SharedArbitrationTestWithParallelExecutionModeOnly,
    asyncArbitratonFromNonDriverContext) {
  setupMemory(kMemoryCapacity, 0);
  const int numVectors = 10;
  std::vector<RowVectorPtr> vectors;
  for (int i = 0; i < numVectors; ++i) {
    vectors.push_back(makeRowVector(rowType_, fuzzerOpts_));
  }
  createDuckDbTable(vectors);
  std::shared_ptr<core::QueryCtx> queryCtx =
      newQueryCtx(memoryManager_.get(), executor_.get(), kMemoryCapacity);
  ASSERT_EQ(queryCtx->pool()->capacity(), 0);

  folly::EventCount aggregationAllocationWait;
  std::atomic<bool> aggregationAllocationOnce{true};
  folly::EventCount aggregationAllocationUnblockWait;
  std::atomic<bool> aggregationAllocationUnblocked{false};
  std::atomic<memory::MemoryPool*> injectPool{nullptr};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::memory::MemoryPoolImpl::reserveThreadSafe",
      std::function<void(memory::MemoryPool*)>(([&](memory::MemoryPool* pool) {
        const std::string re(".*Aggregation");
        if (!RE2::FullMatch(pool->name(), re)) {
          return;
        }

        if (!aggregationAllocationOnce.exchange(false)) {
          return;
        }
        injectPool = pool;
        aggregationAllocationWait.notifyAll();

        aggregationAllocationUnblockWait.await(
            [&]() { return aggregationAllocationUnblocked.load(); });
      })));

  const auto spillDirectory = exec::test::TempDirectoryPath::create();
  std::shared_ptr<Task> task;
  std::thread queryThread([&]() {
    task = newQueryBuilder()
               .queryCtx(queryCtx)
               .spillDirectory(spillDirectory->getPath())
               .config(core::QueryConfig::kSpillEnabled, "true")
               .config(core::QueryConfig::kJoinSpillEnabled, "true")
               .config(core::QueryConfig::kSpillNumPartitionBits, "2")
               .plan(PlanBuilder()
                         .values(vectors)
                         .localPartition({"c0", "c1"})
                         .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                         .localPartition(std::vector<std::string>{})
                         .planNode())
               .assertResults(
                   "SELECT c0, c1, array_agg(c2) FROM tmp GROUP BY c0, c1");
  });

  aggregationAllocationWait.await(
      [&]() { return !aggregationAllocationOnce.load(); });
  ASSERT_TRUE(injectPool != nullptr);

  // Trigger the memory arbitration with memory pool whose associated driver
  // is running on driver thread.
  const size_t fakeAllocationSize = arbitrator_->stats().freeCapacityBytes / 2;
  TestAllocation fakeAllocation = {
      injectPool.load(),
      injectPool.load()->allocate(fakeAllocationSize),
      fakeAllocationSize};

  aggregationAllocationUnblocked = true;
  aggregationAllocationUnblockWait.notifyAll();

  queryThread.join();
  fakeAllocation.free();

  task.reset();
  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_P(SharedArbitrationTestWithThreadingModes, runtimeStats) {
  const uint64_t memoryCapacity = 128 * MB;
  setupMemory(memoryCapacity);
  fuzzerOpts_.vectorSize = 1000;
  fuzzerOpts_.stringLength = 1024;
  fuzzerOpts_.stringVariableLength = false;
  VectorFuzzer fuzzer(fuzzerOpts_, pool());
  std::vector<RowVectorPtr> vectors;
  int numRows{0};
  for (int i = 0; i < 10; ++i) {
    vectors.push_back(fuzzer.fuzzInputRow(rowType_));
    numRows += vectors.back()->size();
  }

  std::atomic<int> outputCount{0};
  SCOPED_TESTVALUE_SET(
      "facebook::velox::exec::Values::getOutput",
      std::function<void(const facebook::velox::exec::Values*)>(
          ([&](const facebook::velox::exec::Values* values) {
            if (outputCount++ != 5) {
              return;
            }
            const auto fakeAllocationSize =
                arbitrator_->stats().maxCapacityBytes -
                values->pool()->capacity() + 1;
            void* buffer = values->pool()->allocate(fakeAllocationSize);
            values->pool()->free(buffer, fakeAllocationSize);
          })));

  const auto spillDirectory = exec::test::TempDirectoryPath::create();
  const auto outputDirectory = TempDirectoryPath::create();
  const auto queryCtx =
      newQueryCtx(memoryManager_.get(), executor_.get(), memoryCapacity);
  auto writerPlan =
      PlanBuilder()
          .values(vectors)
          .tableWrite(outputDirectory->getPath())
          .singleAggregation(
              {},
              {fmt::format("sum({})", TableWriteTraits::rowCountColumnName())})
          .planNode();
  {
    const std::shared_ptr<Task> task =
        newQueryBuilder()
            .queryCtx(queryCtx)
            .maxDrivers(1)
            .spillDirectory(spillDirectory->getPath())
            .config(core::QueryConfig::kSpillEnabled, "true")
            .config(core::QueryConfig::kWriterSpillEnabled, "true")
            // Set 0 file writer flush threshold to always trigger flush in
            // test.
            .config(core::QueryConfig::kWriterFlushThresholdBytes, "0")
            // Set stripe size to extreme large to avoid writer internal
            // triggered flush.
            .connectorSessionProperty(
                kHiveConnectorId,
                dwrf::Config::kOrcWriterMaxStripeSizeSession,
                "1GB")
            .connectorSessionProperty(
                kHiveConnectorId,
                dwrf::Config::kOrcWriterMaxDictionaryMemorySession,
                "1GB")
            .plan(std::move(writerPlan))
            .assertResults(fmt::format("SELECT {}", numRows));

    auto stats = task->taskStats().pipelineStats.front().operatorStats;
    // TableWrite Operator's stripeSize runtime stats would be updated twice:
    // - Values Operator's memory allocation triggers TableWrite's memory
    // reclaim, which triggers data flush.
    // - TableWrite Operator's close would trigger flush.
    ASSERT_EQ(stats[1].runtimeStats["stripeSize"].count, 2);
    // Values Operator won't be set stripeSize in its runtimeStats.
    ASSERT_EQ(stats[0].runtimeStats["stripeSize"].count, 0);
  }
  waitForAllTasksToBeDeleted();
}

DEBUG_ONLY_TEST_P(
    SharedArbitrationTestWithParallelExecutionModeOnly,
    arbitrateMemoryFromOtherOperator) {
  setupMemory(kMemoryCapacity, 0);
  const int numVectors = 10;
  std::vector<RowVectorPtr> vectors;
  for (int i = 0; i < numVectors; ++i) {
    vectors.push_back(makeRowVector(rowType_, fuzzerOpts_));
  }
  createDuckDbTable(vectors);

  for (bool sameDriver : {false, true}) {
    SCOPED_TRACE(fmt::format("sameDriver {}", sameDriver));
    std::shared_ptr<core::QueryCtx> queryCtx =
        newQueryCtx(memoryManager_.get(), executor_.get(), kMemoryCapacity);
    ASSERT_EQ(queryCtx->pool()->capacity(), 0);

    std::atomic<bool> injectAllocationOnce{true};
    const int initialBufferLen = 1 << 20;
    std::atomic<void*> buffer{nullptr};
    std::atomic<memory::MemoryPool*> bufferPool{nullptr};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::exec::Values::getOutput",
        std::function<void(const exec::Values*)>(
            [&](const exec::Values* values) {
              if (!injectAllocationOnce.exchange(false)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return;
              }
              buffer = values->pool()->allocate(initialBufferLen);
              bufferPool = values->pool();
            }));
    std::atomic<bool> injectReallocateOnce{true};
    SCOPED_TESTVALUE_SET(
        "facebook::velox::common::memory::MemoryPoolImpl::allocateNonContiguous",
        std::function<void(memory::MemoryPoolImpl*)>(
            ([&](memory::MemoryPoolImpl* pool) {
              const std::string re(".*Aggregation");
              if (!RE2::FullMatch(pool->name(), re)) {
                return;
              }
              if (pool->root()->usedBytes() == 0) {
                return;
              }
              if (!injectReallocateOnce.exchange(false)) {
                return;
              }
              ASSERT_TRUE(buffer != nullptr);
              ASSERT_TRUE(bufferPool != nullptr);
              const int newLength =
                  kMemoryCapacity - bufferPool.load()->capacity() + 1;
              VELOX_ASSERT_THROW(
                  bufferPool.load()->reallocate(
                      buffer, initialBufferLen, newLength),
                  "Exceeded memory pool cap");
            })));

    std::shared_ptr<Task> task;
    core::PlanNodeId aggregationNodeId;
    std::thread queryThread([&]() {
      if (sameDriver) {
        task = newQueryBuilder()
                   .queryCtx(queryCtx)
                   .plan(PlanBuilder()
                             .values(vectors)
                             .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                             .capturePlanNodeId(aggregationNodeId)
                             .localPartition(std::vector<std::string>{})
                             .planNode())
                   .assertResults(
                       "SELECT c0, c1, array_agg(c2) FROM tmp GROUP BY c0, c1");
      } else {
        task = newQueryBuilder()
                   .queryCtx(queryCtx)
                   .plan(PlanBuilder()
                             .values(vectors)
                             .localPartition({"c0", "c1"})
                             .singleAggregation({"c0", "c1"}, {"array_agg(c2)"})
                             .capturePlanNodeId(aggregationNodeId)
                             .planNode())
                   .assertResults(
                       "SELECT c0, c1, array_agg(c2) FROM tmp GROUP BY c0, c1");
      }
    });

    queryThread.join();
    auto taskStats = exec::toPlanStats(task->taskStats());
    auto& aggNodeStats = taskStats.at(aggregationNodeId);
    checkOperatorStatsForArbitration(
        aggNodeStats, false /*expectGlobalArbitration*/);
    ASSERT_TRUE(buffer != nullptr);
    ASSERT_TRUE(bufferPool != nullptr);
    bufferPool.load()->free(buffer, initialBufferLen);

    task.reset();
    waitForAllTasksToBeDeleted();
  }
}

TEST_P(
    SharedArbitrationTestWithParallelExecutionModeOnly,
    concurrentArbitration) {
  // Tries to replicate an actual workload by concurrently running multiple
  // query shapes that support spilling (and hence can be forced to abort or
  // spill by the arbitrator). Also adds an element of randomness by randomly
  // keeping completed tasks alive (zombie tasks) hence holding on to some
  // memory. Ensures that arbitration is engaged under memory contention and
  // failed queries only have errors related to memory or arbitration.
  FLAGS_velox_suppress_memory_capacity_exceeding_error_message = true;
  const int numVectors = 8;
  std::vector<RowVectorPtr> vectors;
  fuzzerOpts_.vectorSize = 32;
  fuzzerOpts_.stringVariableLength = false;
  fuzzerOpts_.stringLength = 32;
  vectors.reserve(numVectors);
  for (int i = 0; i < numVectors; ++i) {
    vectors.push_back(makeRowVector(rowType_, fuzzerOpts_));
  }
  const int numDrivers = 4;
  const auto expectedWriteResult = runWriteTask(
                                       vectors,
                                       nullptr,
                                       isSerialExecutionMode_,
                                       numDrivers,
                                       pool(),
                                       kHiveConnectorId,
                                       false)
                                       .data;
  const auto expectedJoinResult =
      runHashJoinTask(
          vectors, nullptr, isSerialExecutionMode_, numDrivers, pool(), false)
          .data;
  const auto expectedOrderResult =
      runOrderByTask(
          vectors, nullptr, isSerialExecutionMode_, numDrivers, pool(), false)
          .data;
  const auto expectedRowNumberResult =
      runRowNumberTask(
          vectors, nullptr, isSerialExecutionMode_, numDrivers, pool(), false)
          .data;
  const auto expectedTopNResult =
      runTopNTask(
          vectors, nullptr, isSerialExecutionMode_, numDrivers, pool(), false)
          .data;

  struct {
    uint64_t totalCapacity;
    uint64_t queryCapacity;

    std::string debugString() const {
      return fmt::format(
          "totalCapacity = {}, queryCapacity = {}.",
          succinctBytes(totalCapacity),
          succinctBytes(queryCapacity));
    }
  } testSettings[] = {
      {16 * MB, 128 * MB}, {128 * MB, 16 * MB}, {128 * MB, 128 * MB}};

  for (const auto& testData : testSettings) {
    SCOPED_TRACE(testData.debugString());
    const auto totalCapacity = testData.totalCapacity;
    const auto queryCapacity = testData.queryCapacity;
    setupMemory(totalCapacity);

    std::mutex mutex;
    std::vector<std::shared_ptr<core::QueryCtx>> queries;
    std::deque<std::shared_ptr<Task>> zombieTasks;

    const int numThreads = 32;
    const int maxNumZombieTasks = 8;
    std::vector<std::thread> queryThreads;
    queryThreads.reserve(numThreads);
    TestScopedAbortInjection testScopedAbortInjection(10, numThreads);
    for (int i = 0; i < numThreads; ++i) {
      queryThreads.emplace_back([&, i]() {
        std::shared_ptr<Task> task;
        try {
          auto queryCtx =
              newQueryCtx(memoryManager_.get(), executor_.get(), queryCapacity);
          if (i == 0) {
            // Write task contains aggregate node, which does not support
            // multithread aggregation type resolver, so make sure it is built
            // in a single thread.
            task = runWriteTask(
                       vectors,
                       queryCtx,
                       isSerialExecutionMode_,
                       numDrivers,
                       pool(),
                       kHiveConnectorId,
                       true,
                       expectedWriteResult)
                       .task;
          } else if ((i % 4) == 0) {
            task = runHashJoinTask(
                       vectors,
                       queryCtx,
                       isSerialExecutionMode_,
                       numDrivers,
                       pool(),
                       true,
                       expectedJoinResult)
                       .task;
          } else if ((i % 4) == 1) {
            task = runOrderByTask(
                       vectors,
                       queryCtx,
                       isSerialExecutionMode_,
                       numDrivers,
                       pool(),
                       true,
                       expectedOrderResult)
                       .task;
          } else if ((i % 4) == 2) {
            task = runRowNumberTask(
                       vectors,
                       queryCtx,
                       isSerialExecutionMode_,
                       numDrivers,
                       pool(),
                       true,
                       expectedRowNumberResult)
                       .task;
          } else {
            task = runTopNTask(
                       vectors,
                       queryCtx,
                       isSerialExecutionMode_,
                       numDrivers,
                       pool(),
                       true,
                       expectedTopNResult)
                       .task;
          }
        } catch (const VeloxException& e) {
          if (e.errorCode() != error_code::kMemCapExceeded.c_str() &&
              e.errorCode() != error_code::kMemAborted.c_str() &&
              e.errorCode() != error_code::kMemAllocError.c_str() &&
              (e.message() != "Aborted for external error")) {
            std::rethrow_exception(std::current_exception());
          }
        }

        std::lock_guard<std::mutex> l(mutex);
        if (folly::Random().oneIn(3)) {
          zombieTasks.emplace_back(std::move(task));
        }
        while (zombieTasks.size() > maxNumZombieTasks) {
          zombieTasks.pop_front();
        }
      });
    }

    for (auto& queryThread : queryThreads) {
      queryThread.join();
    }
    zombieTasks.clear();
    waitForAllTasksToBeDeleted();
    ASSERT_GT(arbitrator_->stats().numRequests, 0);
  }
}

TEST_P(SharedArbitrationTestWithThreadingModes, reserveReleaseCounters) {
  for (int i = 0; i < 37; ++i) {
    folly::Random::DefaultGenerator rng(i);
    auto numRootPools = folly::Random::rand32(rng) % 11 + 3;
    std::vector<std::thread> threads;
    threads.reserve(numRootPools);
    std::mutex mutex;
    setupMemory(kMemoryCapacity, 0);
    {
      std::vector<std::shared_ptr<core::QueryCtx>> queries;
      queries.reserve(numRootPools);
      for (int j = 0; j < numRootPools; ++j) {
        threads.emplace_back([&]() {
          {
            std::lock_guard<std::mutex> l(mutex);
            queries.emplace_back(
                newQueryCtx(memoryManager_.get(), executor_.get()));
          }
        });
      }

      for (auto& queryThread : threads) {
        queryThread.join();
      }
    }
  }
}

VELOX_INSTANTIATE_TEST_SUITE_P(
    SharedArbitrationTest,
    SharedArbitrationTestWithParallelExecutionModeOnly,
    testing::ValuesIn(std::vector<TestParam>{{false}}));

VELOX_INSTANTIATE_TEST_SUITE_P(
    SharedArbitrationTest,
    SharedArbitrationTestWithThreadingModes,
    testing::ValuesIn(std::vector<TestParam>{{false}, {true}}));
} // namespace facebook::velox::memory

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::Init init{&argc, &argv, false};
  return RUN_ALL_TESTS();
}
