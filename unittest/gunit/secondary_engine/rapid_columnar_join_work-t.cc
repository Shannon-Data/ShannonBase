// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>
#include <limits>
#include "storage/rapid_engine/cost/cost.h"

using ColumnarJoinCostEstimator = ShannonBase::Optimizer::RpdCostEstimator::ColumnarJoinCostEstimator;

TEST(RapidColumnarJoinWork, WiderBuildCanSpillDespiteFewerRows) {
  const auto narrow = ColumnarJoinCostEstimator::estimate(1000, 900, 8, 9);
  const auto wide = ColumnarJoinCostEstimator::estimate(900, 1000, 2048, 9);
  EXPECT_LT(narrow.retained_bytes, 1024 * 1024);
  EXPECT_GT(wide.retained_bytes, 1024 * 1024);
  EXPECT_LT(narrow.build_copy_bytes, wide.build_copy_bytes);
  EXPECT_EQ(narrow.key_bytes, wide.key_bytes);
}

TEST(RapidColumnarJoinWork, UsesFilteredRowsAndProjectedWidth) {
  const auto full = ColumnarJoinCostEstimator::estimate(10000, 20000, 1024, 9);
  const auto filtered = ColumnarJoinCostEstimator::estimate(100, 20000, 1024, 9);
  const auto projected = ColumnarJoinCostEstimator::estimate(10000, 20000, 8, 9);
  EXPECT_DOUBLE_EQ(full.build_copy_bytes / 100, filtered.build_copy_bytes);
  EXPECT_DOUBLE_EQ(full.retained_bytes / 100, filtered.retained_bytes);
  EXPECT_LT(projected.retained_bytes, full.retained_bytes);
}

TEST(RapidColumnarJoinWork, CompositeKeysChargeBothInputsAndRetainedArena) {
  const auto single = ColumnarJoinCostEstimator::estimate(128, 2048, 8, 9);
  const auto composite = ColumnarJoinCostEstimator::estimate(128, 2048, 8, 27);
  EXPECT_DOUBLE_EQ(single.key_bytes * 3, composite.key_bytes);
  EXPECT_DOUBLE_EQ(single.retained_bytes + 128 * 18, composite.retained_bytes);
  EXPECT_EQ(single.build_copy_bytes, composite.build_copy_bytes);
}

TEST(RapidColumnarJoinWork, EmptyAndUnknownEstimates) {
  const auto empty = ColumnarJoinCostEstimator::estimate(0, 2048, 8, 9);
  EXPECT_EQ(0, empty.retained_bytes);
  EXPECT_EQ(0, empty.build_copy_bytes);
  for (double unknown : {-1.0, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_EQ(0, ColumnarJoinCostEstimator::estimate(unknown, 128, 8, 9).retained_bytes);
    EXPECT_EQ(0, ColumnarJoinCostEstimator::estimate(128, unknown, 8, 9).key_bytes);
  }
}

using AggregatePlacementCost = ShannonBase::Optimizer::RpdCostEstimator::AggregatePlacementCost;

TEST(RapidAggregatePlacement, SelectiveJoinRemovesRowsBeforeGrouping) {
  for (bool build : {false, true}) {
    const auto cost = AggregatePlacementCost::estimate(100000, 1000, 100, build);
    ASSERT_TRUE(cost.valid);
    EXPECT_LT(cost.join_first, cost.aggregate_first);
    EXPECT_FALSE(cost.push_aggregation());
  }
}

TEST(RapidAggregatePlacement, DenseJoinWithFewGroupsStillPreAggregates) {
  for (bool build : {false, true}) {
    const auto cost = AggregatePlacementCost::estimate(100000, 100000, 100, build);
    ASSERT_TRUE(cost.valid);
    EXPECT_TRUE(cost.push_aggregation());
  }
}

TEST(RapidAggregatePlacement, InputNdvReplacesUniformMatchExtrapolation) {
  const auto uniform = AggregatePlacementCost::estimate(100000, 10000, 1000, true);
  const auto snapshot = AggregatePlacementCost::estimate(100000, 10000, 1000, true, 1000);
  ASSERT_TRUE(uniform.valid);
  ASSERT_TRUE(snapshot.valid);
  EXPECT_TRUE(uniform.assumes_uniform_matches);
  EXPECT_FALSE(snapshot.assumes_uniform_matches);
  EXPECT_DOUBLE_EQ(10000, uniform.input_groups);
  EXPECT_DOUBLE_EQ(1000, snapshot.input_groups);
  EXPECT_DOUBLE_EQ(uniform.join_first, snapshot.join_first);
  EXPECT_LT(snapshot.aggregate_first, uniform.aggregate_first);
  EXPECT_FALSE(AggregatePlacementCost::estimate(100000, 10000, 1000, true, 999).valid);
  EXPECT_FALSE(AggregatePlacementCost::estimate(100000, 10000, 1000, true, 100001).valid);
  EXPECT_FALSE(AggregatePlacementCost::estimate(100000, 10000, 1000, true,
                                               std::numeric_limits<double>::quiet_NaN()).valid);
}

TEST(RapidAggregatePlacement, NoReductionAndUncertainEstimatesKeepJoinFirst) {
  EXPECT_FALSE(AggregatePlacementCost::estimate(1000, 1000, 1000, false)
                   .push_aggregation());
  for (double bad : {-1.0, 0.0, std::numeric_limits<double>::infinity(),
                     std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_FALSE(AggregatePlacementCost::estimate(bad, 100, 10, false).valid);
    EXPECT_FALSE(AggregatePlacementCost::estimate(1000, bad, 10, false).valid);
    EXPECT_FALSE(AggregatePlacementCost::estimate(1000, 100, bad, false).valid);
  }
  EXPECT_FALSE(AggregatePlacementCost::estimate(100, 101, 10, false).valid);
  EXPECT_FALSE(AggregatePlacementCost::estimate(100, 10, 11, false).valid);
}

TEST(RapidAggregatePlacement, RequiresMeaningfulSavingsAtExtremeCardinalities) {
  // At full match rate, 50% as many groups saves only 16.7% on the probe side.
  EXPECT_FALSE(AggregatePlacementCost::estimate(1000, 1000, 500, false)
                   .push_aggregation());
  EXPECT_TRUE(AggregatePlacementCost::estimate(1000, 1000, 500, true)
                  .push_aggregation());
  const auto huge = AggregatePlacementCost::estimate(
      std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
      std::numeric_limits<double>::max(), true);
  EXPECT_TRUE(huge.valid);
  EXPECT_FALSE(huge.push_aggregation());
}

// Exercise the actual tree rewrite as well as the arithmetic cost policy.
#include "storage/rapid_engine/imcs/predicate.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_sum.h"
#include "sql/sql_optimizer.h"
#include "storage/rapid_engine/optimizer/rules/condition_pushdown.h"
#include "storage/rapid_engine/optimizer/rules/join_reorder.h"
#include "storage/rapid_engine/optimizer/optimizer.h"
#include "unittest/gunit/fake_table.h"
#include "unittest/gunit/test_utils.h"

namespace {
using namespace ShannonBase::Optimizer;

class RapidAggregatePlacementRule : public ::testing::Test {
 protected:
  void SetUp() override {
    server.SetUp();
    probe = std::make_unique<Fake_TABLE>(2, false);
    build = std::make_unique<Fake_TABLE>(1, false);
    probe->s->db = {"test", 4};
    probe->s->table_name = {"probe", 5};
    build->s->db = {"test", 4};
    build->s->table_name = {"build", 5};
  }
  void TearDown() override {
    build.reset();
    probe.reset();
    server.TearDown();
  }
  Plan make_plan(ha_rows joined, ha_rows groups) {
    auto left = std::make_unique<ScanTable>();
    left->source_table = probe.get();
    left->estimated_rows = 100000;
    auto right = std::make_unique<ScanTable>();
    right->source_table = build.get();
    right->estimated_rows = 1000;
    auto *key = new Item_field(probe->field[0]);
    auto join = std::make_unique<HashJoin>();
    join->join_type = JoinType::INNER;
    join->child_multiplicity[1] = JoinMultiplicity::kAtMostOne;
    join->join_conditions.push_back(
        new Item_func_eq(key, new Item_field(build->field[0])));
    join->estimated_rows = joined;
    join->children.push_back(std::move(left));
    join->children.push_back(std::move(right));
    auto agg = std::make_unique<LocalAgg>();
    agg->strategy = ShannonBase::AggregateStrategy::HASH;
    agg->estimated_rows = groups;
    agg->group_by.push_back(key);
    agg->aggregates.push_back(
        new Item_sum_sum(POS(), new Item_field(probe->field[1]), false, nullptr));
    agg->children.push_back(std::move(join));
    return agg;
  }
  my_testing::Server_initializer server;
  std::unique_ptr<Fake_TABLE> probe, build;
};

TEST_F(RapidAggregatePlacementRule, SelectiveJoinKeepsOriginalTree) {
  Plan plan = make_plan(1000, 100);
  auto *original = plan.get();
  AggregationPushDown().apply(plan);
  EXPECT_EQ(original, plan.get());
  EXPECT_EQ(PlanNode::Type::HASH_JOIN, plan->children[0]->type());
}

TEST_F(RapidAggregatePlacementRule, NativeAggregatePreservesPhysicalGroupingCache) {
  Query_block query_block(server.thd()->mem_root, nullptr, nullptr);
  JOIN join(server.thd(), &query_block);
  Item *input_key = new Item_field(probe->field[0]);
  Cached_item *cached = new_Cached_item(server.thd(), input_key);
  ASSERT_FALSE(join.group_fields.push_front(cached));
  join.group_fields_cache = join.group_fields;
  join.streaming_aggregation = true;

  AccessPath input;
  input.type = AccessPath::TABLE_SCAN;
  input.table_scan().table = probe.get();
  auto child = std::make_unique<MySQLNative>();
  child->original_path = &input;
  child->vectorized = false;
  LocalAgg aggregate;
  aggregate.join = &join;
  // Legacy GROUP BY can have been rebound to an output temporary table.
  aggregate.group_by.push_back(new Item_field(build->field[0]));
  aggregate.children.push_back(std::move(child));
  AccessPath *path = aggregate.ToAccessPath(server.thd());
  ASSERT_NE(nullptr, path);
  EXPECT_FALSE(path->vectorized);
  ASSERT_EQ(1U, join.group_fields.size());
  EXPECT_EQ(cached, join.group_fields.head());
  EXPECT_EQ(input_key, join.group_fields.head()->get_item());
  EXPECT_EQ(cached, join.group_fields_cache.head());
  EXPECT_TRUE(join.streaming_aggregation);
}

TEST_F(RapidAggregatePlacementRule, DenseJoinMovesSameAggregateBelowProbe) {
  Plan plan = make_plan(100000, 100);
  auto *input_join = plan->children[0].get();
  input_join->children[0]->cost = 200;
  input_join->children[1]->cost = 30;
  input_join->cost = 400;
  const auto comparison = AggregatePlacementCost::estimate(100000, 100000, 100, false);
  const double unchanged = std::max(0.0, 400 - 230 -
      100000 * RapidCostConstants::kHashProbePerRow);
  auto *aggregate = plan.get();
  AggregationPushDown().apply(plan);
  ASSERT_EQ(PlanNode::Type::HASH_JOIN, plan->type());
  EXPECT_EQ(aggregate, plan->children[0].get());
  EXPECT_EQ(PlanNode::Type::SCAN, plan->children[0]->children[0]->type());
  EXPECT_EQ(100U, plan->estimated_rows);
  EXPECT_EQ(100U, aggregate->estimated_rows);
  EXPECT_DOUBLE_EQ(200 + 100000 * RapidCostConstants::kVectorCpuPerRow * 2.0, aggregate->cost);
  EXPECT_DOUBLE_EQ(230 + unchanged + comparison.aggregate_first, plan->cost);
}

TEST_F(RapidAggregatePlacementRule, HashPlacementCostCannotSelectStreamingAlgorithm) {
  Plan plan = make_plan(100000, 100);
  auto *aggregate = static_cast<LocalAgg *>(plan.get());
  aggregate->strategy = ShannonBase::AggregateStrategy::STREAMING;
  AggregationPushDown().apply(plan);
  EXPECT_EQ(aggregate, plan.get());
  EXPECT_EQ(PlanNode::Type::HASH_JOIN, plan->children[0]->type());
}

TEST_F(RapidAggregatePlacementRule, CheapPushStillRequiresUniqueInnerJoin) {
  for (bool outer : {false, true}) {
    Plan plan = make_plan(100000, 100);
    auto *original = plan.get();
    auto *join = static_cast<HashJoin *>(plan->children[0].get());
    if (outer)
      join->join_type = JoinType::OUTER;
    else
      join->child_multiplicity[1] = JoinMultiplicity::kUnknown;
    AggregationPushDown().apply(plan);
    EXPECT_EQ(original, plan.get());
  }
}
TEST_F(RapidAggregatePlacementRule, ExpressionDoesNotPreserveJoinKey) {
  Plan plan = make_plan(100000, 100);
  auto *original = plan.get();
  auto *agg = static_cast<LocalAgg *>(plan.get());
  agg->group_by[0] = new Item_func_mod(agg->group_by[0], new Item_int(10));
  AggregationPushDown().apply(plan);
  EXPECT_EQ(original, plan.get());
}

TEST_F(RapidAggregatePlacementRule, DirectKeyAlongsideExpressionIsPreserved) {
  Plan plan = make_plan(100000, 100);
  auto *agg = static_cast<LocalAgg *>(plan.get());
  agg->group_by.push_back(new Item_func_mod(agg->group_by[0], new Item_int(10)));
  AggregationPushDown().apply(plan);
  EXPECT_EQ(PlanNode::Type::HASH_JOIN, plan->type());
}

TEST_F(RapidAggregatePlacementRule, FilterPreservesOnlyProvenUniqueness) {
  Plan plan = make_plan(100000, 100);
  auto *join = static_cast<HashJoin *>(plan->children[0].get());
  auto filter = std::make_unique<Filter>();
  filter->children.push_back(std::move(join->children[1]));
  auto outer = std::make_unique<Filter>();
  outer->children.push_back(std::move(filter));
  EXPECT_EQ(JoinMultiplicity::kUnknown,
            Utils::prove_at_most_one(outer.get(), join->join_conditions));
  build->create_index(build->field[0], HA_NOSAME);
  EXPECT_EQ(JoinMultiplicity::kAtMostOne,
            Utils::prove_at_most_one(outer.get(), join->join_conditions));
  build->key_info[0].flags |= HA_NULL_PART_KEY;
  EXPECT_EQ(JoinMultiplicity::kUnknown,
            Utils::prove_at_most_one(outer.get(), join->join_conditions));
  build->key_info[0].flags &= ~HA_NULL_PART_KEY;
  join->children[1] = std::move(outer);
  join->child_multiplicity[1] = Utils::prove_at_most_one(
      join->children[1].get(), join->join_conditions);
  AggregationPushDown().apply(plan);
  EXPECT_EQ(PlanNode::Type::HASH_JOIN, plan->type());
}

TEST_F(RapidAggregatePlacementRule, EmptyFilterHasNoUniquenessProof) {
  Filter filter;
  EXPECT_EQ(JoinMultiplicity::kUnknown, Utils::prove_at_most_one(&filter, {}));
  filter.children.push_back(nullptr);
  EXPECT_EQ(JoinMultiplicity::kUnknown, Utils::prove_at_most_one(&filter, {}));
}
}  // namespace

#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "sql/opt_trace.h"

namespace {
class RapidCardinalityObservation : public ::testing::Test {
 protected:
  void SetUp() override { server.SetUp(); }
  void TearDown() override { server.TearDown(); }
  template <class F> std::string trace(F body) {
    auto &context = server.thd()->opt_trace;
    EXPECT_FALSE(context.start(true, false, false, false, -1, 1, 1024 * 1024,
                               Opt_trace_context::default_features));
    {
      Opt_trace_object root(&context);
      Opt_trace_array steps(&context, "steps");
      body();
    }
    context.end();
    Opt_trace_iterator it(&context);
    Opt_trace_info info;
    it.get_value(&info);
    return std::string(info.trace_ptr, info.trace_length);
  }
  my_testing::Server_initializer server;
};

using RapidRuleTrace = RapidCardinalityObservation;

TEST_F(RapidRuleTrace, DisabledJoinReorderExplainsWhyPlanIsUnchanged) {
  using namespace ShannonBase::Optimizer;
  Plan plan = std::make_unique<ScanTable>();
  const auto *original = plan.get();
  const auto result = trace([&] { JoinReOrder().apply(plan); });
  EXPECT_EQ(original, plan.get());
  EXPECT_NE(std::string::npos, result.find("\"decision\": \"rapid_join_reorder\""));
  EXPECT_NE(std::string::npos, result.find("\"enabled\": false"));
  EXPECT_NE(std::string::npos, result.find("\"applied\": false"));
  EXPECT_NE(std::string::npos, result.find("reconstruction_semantics_not_proven"));
}

TEST_F(RapidCardinalityObservation, CompletesOnceAndResetsOnReexecution) {
  ShannonBase::Executor::CardinalityObservation observation(10);
  const auto first = trace([&] {
    observation.Finish(server.thd(), "filter", 100);
    observation.Finish(server.thd(), "filter", 999);
  });
  EXPECT_NE(std::string::npos, first.find("\"actual_rows\": 100"));
  EXPECT_NE(std::string::npos, first.find("\"q_error\": 10"));
  EXPECT_EQ(std::string::npos, first.find("999"));
  observation.Reset();
  const auto second = trace([&] { observation.Finish(server.thd(), "filter", 0); });
  EXPECT_NE(std::string::npos, second.find("\"actual_rows\": 0"));
  EXPECT_NE(std::string::npos, second.find("\"q_error\": 10"));
}

TEST_F(RapidCardinalityObservation, EmptyEstimateAndResultHaveFiniteError) {
  ShannonBase::Executor::CardinalityObservation observation(0);
  const auto result = trace([&] { observation.Finish(server.thd(), "scan", 0); });
  EXPECT_NE(std::string::npos, result.find("\"actual_rows\": 0"));
  EXPECT_NE(std::string::npos, result.find("\"q_error\": 1"));
}

TEST_F(RapidCardinalityObservation, UnknownEstimateHasNoErrorRatio) {
  for (double estimate : {-1.0, std::numeric_limits<double>::infinity(),
                          std::numeric_limits<double>::quiet_NaN()}) {
    ShannonBase::Executor::CardinalityObservation observation(estimate);
    const auto result = trace([&] { observation.Finish(server.thd(), "scan", 8); });
    EXPECT_NE(std::string::npos, result.find("\"estimate_known\": false"));
    EXPECT_EQ(std::string::npos, result.find("q_error"));
  }
}

TEST_F(RapidCardinalityObservation, InvalidatedAndCancelledExecutionsAreSkipped) {
  ShannonBase::Executor::CardinalityObservation observation(10);
  observation.Invalidate();
  auto result = trace([&] { observation.Finish(server.thd(), "scan", 8); });
  EXPECT_EQ(std::string::npos, result.find("rapid_cardinality_observation"));
  observation.Reset();
  server.thd()->killed = THD::KILL_QUERY;
  result = trace([&] { observation.Finish(server.thd(), "scan", 8); });
  server.thd()->killed = THD::NOT_KILLED;
  EXPECT_EQ(std::string::npos, result.find("rapid_cardinality_observation"));
  observation.Reset();
  server.thd()->get_stmt_da()->set_error_status(ER_UNKNOWN_ERROR, "test error", "HY000");
  result = trace([&] { observation.Finish(server.thd(), "scan", 8); });
  server.thd()->clear_error();
  EXPECT_EQ(std::string::npos, result.find("rapid_cardinality_observation"));
}
}  // namespace

#include "storage/rapid_engine/executor/iterators/hash_join_iterator.h"

using ShannonBase::Executor::PlanHashJoinGrowth;

TEST(RapidHashJoinMemory, ReusesCapacityAtBudgetBoundary) {
  const auto plan = PlanHashJoinGrowth(1000, 1000, 16, 128, 16, 128, 40);
  ASSERT_TRUE(plan.fits);
  EXPECT_EQ(1000, plan.peak_bytes);
}

TEST(RapidHashJoinMemory, AccountsForSequentialAllocationOverlap) {
  // Slots: 80 old + 120 new. Then keys: 120 retained slots + 60 old + 100 new.
  const auto plan = PlanHashJoinGrowth(140, 280, 2, 60, 3, 100, 40);
  ASSERT_TRUE(plan.fits);
  EXPECT_EQ(3, plan.slots);
  EXPECT_EQ(100, plan.key_bytes);
  EXPECT_EQ(280, plan.peak_bytes);
  EXPECT_FALSE(PlanHashJoinGrowth(140, 279, 2, 60, 3, 100, 40).fits);
}

TEST(RapidHashJoinMemory, SpareSlotCapacityMustLeaveRoomForKeys) {
  // Doubling slots alone fits, but would make the subsequent key reserve fail.
  const auto plan = PlanHashJoinGrowth(3200, 8500, 64, 640, 65, 3000, 40);
  ASSERT_TRUE(plan.fits);
  EXPECT_EQ(65, plan.slots);
  EXPECT_EQ(3000, plan.key_bytes);
  EXPECT_EQ(6240, plan.peak_bytes);
}

TEST(RapidHashJoinMemory, KeepsGeometricGrowthWithHeadroom) {
  const auto plan = PlanHashJoinGrowth(3200, 10000, 64, 640, 65, 641, 40);
  ASSERT_TRUE(plan.fits);
  EXPECT_EQ(128, plan.slots);
  EXPECT_EQ(1280, plan.key_bytes);
  EXPECT_EQ(8320, plan.peak_bytes);
}

TEST(RapidHashJoinMemory, RejectsOverflowAndInconsistentAccounting) {
  const size_t max = std::numeric_limits<size_t>::max();
  EXPECT_FALSE(PlanHashJoinGrowth(0, max, 0, 0, max, 0, 40).fits);
  EXPECT_FALSE(PlanHashJoinGrowth(140, 139, 2, 60, 3, 100, 40).fits);
  EXPECT_FALSE(PlanHashJoinGrowth(140, 1000, 3, 60, 3, 100, 40).fits);
  EXPECT_FALSE(PlanHashJoinGrowth(0, max, 0, 0, 1, 1, 0).fits);
}

TEST(RapidHashJoinMemory, PlannedPeakMatchesBoundedAllocator) {
  using ShannonBase::ResMgmt::BoundedMemoryResource;
  for (size_t limit : {5800, 6400, 8320, 10000}) {
    BoundedMemoryResource memory(limit, std::pmr::new_delete_resource());
    std::pmr::vector<std::array<char, 40>> slots(&memory);
    std::pmr::vector<char> keys(&memory);
    slots.reserve(64);
    keys.reserve(640);
    const auto plan = PlanHashJoinGrowth(memory.used(), limit, slots.capacity(), keys.capacity(), 65, 1280, 40);
    ASSERT_TRUE(plan.fits);
    EXPECT_NO_THROW(slots.reserve(plan.slots));
    EXPECT_NO_THROW(keys.reserve(plan.key_bytes));
    EXPECT_EQ(plan.peak_bytes, memory.peak());
    EXPECT_LE(memory.peak(), limit);
  }
}
