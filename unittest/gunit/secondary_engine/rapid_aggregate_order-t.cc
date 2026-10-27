// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>
#include <limits>

#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/cost_model.h"
#include "sql/join_optimizer/make_join_hypergraph.h"
#include "sql/join_optimizer/materialize_path_parameters.h"
#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/cost/operator_cost.h"
#include "storage/rapid_engine/optimizer/optimizer.h"
#include "storage/rapid_engine/optimizer/rules/cost_refinement.h"
using AggregateOrderEstimator = ShannonBase::Optimizer::RpdCostEstimator::AggregateOrderEstimator;

TEST(RapidAggregateOrder, ScaleAndBudgetChangeTheChoice) {
  // Identical shape, projection and cost rates. Scaling the data crosses the
  // memory boundary without an SF-specific row-count cutoff.
  const auto small = AggregateOrderEstimator::estimate(4000, 1000, 41, 320, 1024 * 1024);
  const auto large = AggregateOrderEstimator::estimate(40000, 10000, 41, 320, 1024 * 1024);
  const auto granted = AggregateOrderEstimator::estimate(40000, 10000, 41, 320, 4 * 1024 * 1024);
  EXPECT_FALSE(small.prefer_ordered(.003, .003, 2.0 / 16384));
  EXPECT_TRUE(large.prefer_ordered(.003, .003, 2.0 / 16384));
  EXPECT_FALSE(granted.prefer_ordered(.003, .003, 2.0 / 16384));
}

TEST(RapidAggregateOrder, FewGroupsStayInMemoryAtLargeInputCardinality) {
  const auto work = AggregateOrderEstimator::estimate(12000000, 4, 41, 320, 1024 * 1024);
  EXPECT_EQ(0, work.spill_bytes);
  EXPECT_FALSE(work.prefer_ordered(.003, .003, 2.0 / 16384));
}

TEST(RapidAggregateOrder, InvalidStatisticsCannotSelectStreaming) {
  for (double rows : {-1.0, 0.0, std::numeric_limits<double>::infinity(),
                      std::numeric_limits<double>::quiet_NaN()})
    EXPECT_FALSE(AggregateOrderEstimator::estimate(rows, 1, 41, 320, 1024).prefer_ordered(.003, .003, 1));
  EXPECT_FALSE(AggregateOrderEstimator::estimate(10, 11, 41, 320, 1024).valid);
}

namespace {
using namespace ShannonBase::Optimizer;

AccessPath CostedInput(double rows, double total, double startup, double once = 0) {
  AccessPath child;
  child.type = AccessPath::TABLE_SCAN;
  child.table_scan().table = nullptr;
  child.set_num_output_rows(rows);
  child.set_cost(total);
  child.set_init_cost(startup);
  child.set_init_once_cost(once);
  return child;
}

class RapidOperatorCost : public ::testing::Test {
 protected:
  MEM_ROOT root{PSI_NOT_INSTRUMENTED, 8192};
  JoinHypergraph graph{&root, nullptr};

  void CompareLimit(double rows, double startup, ha_rows end, ha_rows offset) {
    auto child = CostedInput(rows, 1000, startup, startup / 2);
    AccessPath path;
    path.type = AccessPath::LIMIT_OFFSET;
    path.limit_offset().child = &child;
    path.limit_offset().limit = end;
    path.limit_offset().offset = offset;
    path.limit_offset().count_all_rows = false;
    AccessPath reference = path;
    EstimateLimitOffsetCost(&reference);
    EXPECT_FALSE(ModifyLimitCost(nullptr, graph, &path, nullptr));
    EXPECT_DOUBLE_EQ(reference.cost(), path.cost());
    EXPECT_DOUBLE_EQ(reference.init_cost(), path.init_cost());
    EXPECT_LE(path.init_once_cost(), path.init_cost());
    EXPECT_GE(path.rescan_cost(), 0);
  }
};

TEST_F(RapidOperatorCost, LimitPreservesBlockingWorkAndOffsetContract) {
  CompareLimit(100000, 900, 10, 0);
  CompareLimit(1000, 0, 110, 100);
  CompareLimit(1000, 0, HA_POS_ERROR - 5, 10);
  CompareLimit(1000, 900, HA_POS_ERROR, 0);
  CompareLimit(0, 900, 10, 0);
}

TEST_F(RapidOperatorCost, CountAllRowsDoesNotDiscountTheChild) {
  auto child = CostedInput(1000, 1000, 200, 100);
  AccessPath path;
  path.type = AccessPath::LIMIT_OFFSET;
  path.limit_offset().child = &child;
  path.limit_offset().limit = 110;
  path.limit_offset().offset = 100;
  path.limit_offset().count_all_rows = true;
  EXPECT_FALSE(ModifyLimitCost(nullptr, graph, &path, nullptr));
  EXPECT_DOUBLE_EQ(1000, path.cost());
  EXPECT_DOUBLE_EQ(280, path.init_cost());
  EXPECT_DOUBLE_EQ(100, path.init_once_cost());
}

TEST_F(RapidOperatorCost, UnknownStatisticsKeepTheExistingEstimate) {
  auto child = CostedInput(kUnknownRowCount, 1000, 200);
  AccessPath path;
  path.type = AccessPath::SORT;
  path.sort().child = &child;
  path.sort().limit = HA_POS_ERROR;
  path.set_cost(1234);
  path.set_init_cost(1234);
  EXPECT_FALSE(ModifySortCost(nullptr, graph, &path, nullptr));
  EXPECT_DOUBLE_EQ(1234, path.cost());
}

TEST_F(RapidOperatorCost, SpecializedSemijoinDoesNotReadTheNestedLoopUnionMember) {
  AccessPath path;
  path.type = AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL;
  path.set_cost(1234);
  EXPECT_FALSE(ModifyNestedLoopJoinCost(nullptr, graph, &path, nullptr));
  EXPECT_DOUBLE_EQ(1234, path.cost());
}

TEST_F(RapidOperatorCost, TopOneMustReadEveryInputRowAndLargeLimitIsBounded) {
  auto child = CostedInput(1000, 100, 10);
  AccessPath path;
  path.type = AccessPath::SORT;
  path.sort().child = &child;
  path.sort().limit = 1;
  EXPECT_FALSE(ModifySortCost(nullptr, graph, &path, nullptr));
  EXPECT_GT(path.cost(), child.cost());
  EXPECT_DOUBLE_EQ(path.cost(), path.init_cost());
  path.sort().limit = HA_POS_ERROR;
  EXPECT_FALSE(ModifySortCost(nullptr, graph, &path, nullptr));
  const double full = path.cost();
  path.sort().limit = 1000000;
  EXPECT_FALSE(ModifySortCost(nullptr, graph, &path, nullptr));
  EXPECT_DOUBLE_EQ(full, path.cost());
}

TEST_F(RapidOperatorCost, ExplicitRematerializeCannotReuseTheBuild) {
  auto child = CostedInput(1000, 100, 10);
  MaterializePathParameters param{};
  param.m_operands = Mem_root_array<MaterializePathParameters::Operand>(&root);
  MaterializePathParameters::Operand operand{};
  operand.subquery_path = &child;
  ASSERT_FALSE(param.m_operands.push_back(operand));
  param.rematerialize = true;
  AccessPath path;
  path.type = AccessPath::MATERIALIZE;
  path.materialize().param = &param;
  path.parameter_tables = 0;
  EXPECT_FALSE(ModifyMaterializeCost(nullptr, graph, &path, nullptr));
  EXPECT_DOUBLE_EQ(0, path.init_once_cost());
  EXPECT_DOUBLE_EQ(path.cost(), path.rescan_cost());
  param.rematerialize = false;
  EXPECT_FALSE(ModifyMaterializeCost(nullptr, graph, &path, nullptr));
  EXPECT_GT(path.init_once_cost(), 100);
  EXPECT_LT(path.rescan_cost(), 100);
}

TEST(RapidCostProfile, NestedLoopPaysForMaterializedBuildOnlyOnce) {
  OperatorCost outer{0, 10, 0};
  OperatorCost inner{100, 2, 100};
  auto cost = NestedLoopCost(outer, inner, 10);
  ASSERT_TRUE(cost);
  EXPECT_DOUBLE_EQ(130, cost->total());
  EXPECT_DOUBLE_EQ(100, cost->startup);
  EXPECT_DOUBLE_EQ(30, cost->rescan());
  inner.reusable = 0;
  cost = NestedLoopCost(outer, inner, 10);
  ASSERT_TRUE(cost);
  EXPECT_DOUBLE_EQ(1030, cost->total());
}

TEST(RapidCostProfile, InvalidDimensionsAreNotSilentlyNormalized) {
  EXPECT_FALSE(OperatorCost::from(10, 11, 0));
  EXPECT_FALSE(OperatorCost::from(10, 5, 6));
  EXPECT_FALSE(OperatorCost::from(10, -1, 0));
  EXPECT_FALSE(OperatorCost::from(std::numeric_limits<double>::infinity(), 0, 0));
  EXPECT_FALSE(LimitCost({0, 10, 0}, -1, 10, 0, false));
  EXPECT_FALSE(MaterializeCost(10, 1, -1, false));
}
}  // namespace

TEST(RapidCostRefinement, GlobalAggregateUsesOneGroupWithoutChangingOriginalPath) {
  using namespace ShannonBase::Optimizer;
  auto original = CostedInput(251141, 1000, 500);
  auto aggregate = std::make_unique<LocalAgg>();
  aggregate->original_path = &original;
  aggregate->estimated_rows = 251141;
  auto scan = std::make_unique<ScanTable>();
  scan->estimated_rows = 1000000;
  scan->cost = 200;
  aggregate->children.push_back(std::move(scan));
  Plan plan = std::move(aggregate);
  CostRefinement refinement;
  refinement.apply(plan);
  EXPECT_EQ(1U, plan->estimated_rows);
  EXPECT_DOUBLE_EQ(251141, original.num_output_rows());
  EXPECT_DOUBLE_EQ(1000, original.cost());
}

TEST(RapidCostRefinement, UnknownGroupingKeyRetainsTranslatedEstimate) {
  using namespace ShannonBase::Optimizer;
  auto aggregate = std::make_unique<LocalAgg>();
  aggregate->estimated_rows = 123;
  aggregate->group_by.push_back(nullptr);
  auto scan = std::make_unique<ScanTable>();
  scan->estimated_rows = 1000;
  aggregate->children.push_back(std::move(scan));
  Plan plan = std::move(aggregate);
  CostRefinement refinement;
  refinement.apply(plan);
  EXPECT_EQ(123U, plan->estimated_rows);
}

TEST(RapidCostRefinement, RefinedGroupsPropagateThroughStreamAndSort) {
  using namespace ShannonBase::Optimizer;
  auto scan = std::make_unique<ScanTable>();
  scan->estimated_rows = 1000;
  auto aggregate = std::make_unique<LocalAgg>();
  aggregate->children.push_back(std::move(scan));
  auto stream = std::make_unique<Stream>();
  stream->estimated_rows = 1000;
  stream->children.push_back(std::move(aggregate));
  auto sort = std::make_unique<Sort>();
  sort->estimated_rows = 1000;
  sort->children.push_back(std::move(stream));
  Plan plan = std::move(sort);
  CostRefinement refinement;
  refinement.apply(plan);
  EXPECT_EQ(1U, plan->estimated_rows);
  EXPECT_EQ(1U, plan->children[0]->estimated_rows);
  EXPECT_EQ(1U, plan->children[0]->children[0]->estimated_rows);
}
