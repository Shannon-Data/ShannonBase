// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>

#include "sql/item_cmpfunc.h"
#include "sql/parse_tree_helpers.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/optimizer/query_plan.h"
#include "storage/rapid_engine/optimizer/rules/condition_pushdown.h"
#include "unittest/gunit/fake_table.h"
#include "unittest/gunit/test_utils.h"

namespace {
using namespace ShannonBase::Optimizer;

class RapidPredicatePushdown : public ::testing::Test {
 protected:
  void SetUp() override {
    server.SetUp();
    left = std::make_unique<Fake_TABLE>(1, false);
    right = std::make_unique<Fake_TABLE>(1, false);
    right->set_nullable();
    // These logical-plan tests have no loaded Rapid tables or handler stats.
    left->file = right->file = nullptr;
    left->s->db = right->s->db = {"test", 4};
    left->s->table_name = {"left", 4};
    right->s->table_name = {"right", 5};
    left->alias = "left";
    right->alias = "right";
  }
  void TearDown() override {
    right.reset();
    left.reset();
    server.TearDown();
  }
  Item *Field(TABLE *table) { return new Item_field(table->field[0]); }
  Item *Equal(TABLE *table) { return new Item_func_eq(Field(table), new (server.thd()->mem_root) Item_int(1)); }
  bool Rejects(Item *item) { return PredicatePushDown{}.is_null_rejecting(item, {right.get()}); }
  Plan Scan(TABLE *table) {
    auto scan = std::make_unique<ScanTable>();
    scan->source_table = table;
    scan->vectorized = false;  // Observe the pushed residual without an IMCS.
    scan->estimated_rows = 10;
    return scan;
  }
  Plan MakeJoin(bool hash, RelationalExpression::Type type, Item *condition) {
    auto *expr = new (server.thd()->mem_root) RelationalExpression(server.thd());
    expr->type = type;
    expr->equijoin_conditions.push_back(new Item_func_eq(Field(left.get()), Field(right.get())));
    expr->join_conditions.push_back(new Item_func_gt(Field(left.get()), new (server.thd()->mem_root) Item_int(0)));
    auto *predicate = new (server.thd()->mem_root) JoinPredicate{};
    predicate->expr = expr;
    auto *path = new (server.thd()->mem_root) AccessPath();
    Plan join;
    if (hash) {
      path->type = AccessPath::HASH_JOIN;
      path->hash_join().join_predicate = predicate;
      auto node = std::make_unique<HashJoin>();
      node->join_type = type == RelationalExpression::LEFT_JOIN ? JoinType::OUTER : JoinType::ANTI;
      join = std::move(node);
    } else {
      path->type = AccessPath::NESTED_LOOP_JOIN;
      path->nested_loop_join().join_predicate = predicate;
      path->nested_loop_join().join_type = type == RelationalExpression::LEFT_JOIN ? JoinType::OUTER : JoinType::ANTI;
      auto node = std::make_unique<NestLoopJoin>();
      node->source_join_predicate = predicate;
      join = std::move(node);
    }
    join->original_path = path;
    join->children.push_back(Scan(left.get()));
    join->children.push_back(Scan(right.get()));
    condition->update_used_tables();
    auto filter = std::make_unique<Filter>();
    filter->condition = condition;
    filter->children.push_back(std::move(join));
    return filter;
  }
  my_testing::Server_initializer server;
  std::unique_ptr<Fake_TABLE> left, right;
};

TEST_F(RapidPredicatePushdown, NullPropagationAndBooleanLogic) {
  EXPECT_TRUE(Rejects(Equal(right.get())));
  EXPECT_FALSE(Rejects(Equal(left.get())));
  EXPECT_TRUE(Rejects(new Item_func_not(Equal(right.get()))));
  auto *pattern = new Item_string("%special%requests%", 18, &my_charset_latin1);
  EXPECT_TRUE(Rejects(new Item_func_not(new Item_func_like(Field(right.get()), pattern))));
  EXPECT_FALSE(Rejects(new Item_func_isnull(Field(right.get()))));
  EXPECT_FALSE(Rejects(new Item_func_equal(Field(right.get()), new Item_null())));
  auto *coalesce = new Item_func_coalesce(Field(right.get()), new (server.thd()->mem_root) Item_int(1));
  EXPECT_FALSE(Rejects(new Item_func_eq(coalesce, new (server.thd()->mem_root) Item_int(1))));
  EXPECT_TRUE(Rejects(new Item_cond_and(Equal(right.get()), Equal(left.get()))));
  EXPECT_FALSE(Rejects(new Item_cond_or(Equal(right.get()), Equal(left.get()))));
  EXPECT_TRUE(Rejects(new Item_cond_or(Equal(right.get()), Equal(right.get()))));
  // FALSE AND NULL can be FALSE; negating it can accept an unmatched row.
  EXPECT_FALSE(Rejects(new Item_func_not(new Item_cond_and(Equal(right.get()), Equal(left.get())))));
  EXPECT_TRUE(Rejects(new Item_func_not(new Item_cond_or(Equal(right.get()), Equal(right.get())))));
  EXPECT_FALSE(Rejects(new Item_func_not(new Item_func_isnotnull(Field(right.get())))));
  EXPECT_TRUE(Rejects(new Item_func_between(POS(), Field(right.get()), new (server.thd()->mem_root) Item_int(0),
                                            new (server.thd()->mem_root) Item_int(2), false)));
  EXPECT_FALSE(Rejects(
      new Item_func_not(new Item_func_between(POS(), new (server.thd()->mem_root) Item_int(1), Field(right.get()),
                                              new (server.thd()->mem_root) Item_int(0), false))));
  auto *list = new (server.thd()->mem_root) PT_item_list(POS());
  list->push_back(new (server.thd()->mem_root) Item_int(1));
  list->push_back(Field(right.get()));
  list->push_back(new (server.thd()->mem_root) Item_int(1));
  EXPECT_FALSE(Rejects(new Item_func_in(POS(), list, false)));
  // FIELD has null_on_null=true in this source tree, but NULL is a search
  // argument, not a guarantee that the function result itself becomes NULL.
  EXPECT_FALSE(Rejects(new Item_func_field(POS(), list)));
}

TEST_F(RapidPredicatePushdown, SimplificationIsLocalAndPushesOnlyAfterChangingJoinType) {
  for (bool hash : {false, true}) {
    Plan plan = MakeJoin(hash, RelationalExpression::LEFT_JOIN, new Item_func_not(Equal(right.get())));
    const AccessPath *source = plan->children[0]->original_path;
    PredicatePushDown{}.apply(plan);
    ASSERT_NE(PlanNode::Type::FILTER, plan->type());
    EXPECT_TRUE(Utils::is_provably_inner_join(plan.get()));
    ASSERT_EQ(PlanNode::Type::FILTER, plan->children[1]->type());
    EXPECT_NE(source, plan->original_path);
    const auto *original = hash ? source->hash_join().join_predicate : source->nested_loop_join().join_predicate;
    EXPECT_EQ(RelationalExpression::LEFT_JOIN, original->expr->type);
    const auto *rewritten =
        hash ? plan->original_path->hash_join().join_predicate : plan->original_path->nested_loop_join().join_predicate;
    ASSERT_EQ(1U, rewritten->expr->equijoin_conditions.size());
    ASSERT_EQ(1U, rewritten->expr->join_conditions.size());
    EXPECT_EQ(original->expr->equijoin_conditions[0], rewritten->expr->equijoin_conditions[0]);
    EXPECT_EQ(original->expr->join_conditions[0], rewritten->expr->join_conditions[0]);
    if (hash)
      EXPECT_EQ(JoinType::INNER, static_cast<HashJoin *>(plan.get())->join_type);
    else
      EXPECT_EQ(JoinType::INNER, plan->original_path->nested_loop_join().join_type);
  }
}

TEST_F(RapidPredicatePushdown, NullAcceptingAndAntiJoinsRetainTheirFilter) {
  for (bool hash : {false, true}) {
    for (auto type : {RelationalExpression::LEFT_JOIN, RelationalExpression::ANTIJOIN}) {
      Plan plan = MakeJoin(hash, type, new Item_func_isnull(Field(right.get())));
      PredicatePushDown{}.apply(plan);
      ASSERT_EQ(PlanNode::Type::FILTER, plan->type());
      EXPECT_FALSE(Utils::is_provably_inner_join(plan->children[0].get()));
      EXPECT_EQ(PlanNode::Type::SCAN, plan->children[0]->children[1]->type());
    }
    Plan anti = MakeJoin(hash, RelationalExpression::ANTIJOIN, Equal(right.get()));
    PredicatePushDown{}.apply(anti);
    ASSERT_EQ(PlanNode::Type::FILTER, anti->type());
    EXPECT_EQ(PlanNode::Type::SCAN, anti->children[0]->children[1]->type());
    Plan mixed = MakeJoin(hash, RelationalExpression::LEFT_JOIN,
                          new Item_cond_and(Equal(right.get()), new Item_func_isnull(Field(right.get()))));
    PredicatePushDown{}.apply(mixed);
    ASSERT_EQ(PlanNode::Type::FILTER, mixed->type());
    EXPECT_FALSE(Utils::is_provably_inner_join(mixed->children[0].get()));
  }
}
}  // namespace
