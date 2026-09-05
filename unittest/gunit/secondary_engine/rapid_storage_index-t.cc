/* Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
 * Unit test for the Storage Index (zone map) IMCU-pruning decision.
 *
 * can_skip_imcu() is a correctness-critical predicate: skipping an IMCU that
 * holds a matching row silently drops that row from the answer, and no error is
 * raised anywhere. The MTR suite can only observe this indirectly, through row
 * counts that happen to disagree with InnoDB, so the decision table is pinned
 * here directly.
 *
 * The invariant every case below defends: pruning may be *conservative*
 * (refusing to skip a skippable IMCU only costs time), but it must never skip
 * an IMCU that could contain a matching row.
 */

#include "storage/rapid_engine/imcs/storage0index.h"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "storage/rapid_engine/imcs/predicate.h"

namespace shannon_storage_index_unittest {

using ShannonBase::Imcs::PredicateOperator;
using ShannonBase::Imcs::PredicateValue;
using ShannonBase::Imcs::Simple_Predicate;
using ShannonBase::Imcs::StorageIndex;

constexpr uint32 kCol = 0;
constexpr size_t kNumColumns = 2;

// MYSQL_TYPE_LONG: can_skip_simple_predicate() deliberately refuses to prune
// DECIMAL / LONGLONG / FLOAT / DOUBLE and string columns, whose zone maps pass
// through double and may lose ordering precision. A plain INT is the type the
// pruning path actually acts on.
constexpr enum_field_types kColType = MYSQL_TYPE_LONG;

// A StorageIndex with no owning IMCU: the pruning decision reads only the
// per-column min/max/null statistics fed in here.
std::unique_ptr<StorageIndex> MakeIndex() { return std::make_unique<StorageIndex>(kNumColumns, nullptr); }

std::vector<std::unique_ptr<ShannonBase::Imcs::Predicate>> OnePredicate(PredicateOperator op, int64 value) {
  std::vector<std::unique_ptr<ShannonBase::Imcs::Predicate>> preds;
  preds.push_back(std::make_unique<Simple_Predicate>(kCol, op, PredicateValue(value), kColType));
  return preds;
}

// No predicate carries no information, so nothing may be pruned.
TEST(StorageIndexPruningTest, EmptyPredicateListNeverSkips) {
  auto index = MakeIndex();
  index->update(kCol, 10.0);
  std::vector<std::unique_ptr<ShannonBase::Imcs::Predicate>> none;
  EXPECT_FALSE(index->can_skip_imcu(none));
}

// col = v is skippable exactly when v falls outside [min, max].
TEST(StorageIndexPruningTest, EqualitySkipsOnlyOutsideMinMax) {
  auto index = MakeIndex();
  index->update(kCol, 10.0);
  index->update(kCol, 20.0);

  EXPECT_TRUE(index->can_skip_imcu(OnePredicate(PredicateOperator::EQUAL, 9)));
  EXPECT_TRUE(index->can_skip_imcu(OnePredicate(PredicateOperator::EQUAL, 21)));

  EXPECT_FALSE(index->can_skip_imcu(OnePredicate(PredicateOperator::EQUAL, 10)));
  EXPECT_FALSE(index->can_skip_imcu(OnePredicate(PredicateOperator::EQUAL, 15)));
  EXPECT_FALSE(index->can_skip_imcu(OnePredicate(PredicateOperator::EQUAL, 20)));
}

// col <> v may be skipped only when every row is provably v: no NULLs, and
// min == max == v. This is the narrow rule the NOT_EQUAL MTR tests exercise
// end to end; the three negative cases below are where a wrong rule would
// silently drop rows.
TEST(StorageIndexPruningTest, NotEqualSkipsOnlyOnConstantNullFreeBlock) {
  auto constant_block = MakeIndex();
  constant_block->update(kCol, 5.0);
  constant_block->update(kCol, 5.0);
  EXPECT_TRUE(constant_block->can_skip_imcu(OnePredicate(PredicateOperator::NOT_EQUAL, 5)));

  // A different target: rows do qualify.
  EXPECT_FALSE(constant_block->can_skip_imcu(OnePredicate(PredicateOperator::NOT_EQUAL, 6)));

  // One NULL is enough to make min/max unable to prove the block is uniform:
  // min/max cover only the non-NULL values.
  auto with_null = MakeIndex();
  with_null->update(kCol, 5.0);
  with_null->update_null(kCol);
  EXPECT_FALSE(with_null->can_skip_imcu(OnePredicate(PredicateOperator::NOT_EQUAL, 5)));

  // A varying block cannot be pruned either.
  auto varying = MakeIndex();
  varying->update(kCol, 5.0);
  varying->update(kCol, 7.0);
  EXPECT_FALSE(varying->can_skip_imcu(OnePredicate(PredicateOperator::NOT_EQUAL, 5)));
}

// Range operators prune on the far side of the block only, and the boundary
// value itself must not be skipped for the inclusive forms.
TEST(StorageIndexPruningTest, RangeOperatorBoundaries) {
  auto index = MakeIndex();
  index->update(kCol, 10.0);
  index->update(kCol, 20.0);

  // col > v: skippable once max <= v.
  EXPECT_TRUE(index->can_skip_imcu(OnePredicate(PredicateOperator::GREATER_THAN, 20)));
  EXPECT_FALSE(index->can_skip_imcu(OnePredicate(PredicateOperator::GREATER_THAN, 19)));

  // col >= v: max == v still has a match, so only max < v prunes.
  EXPECT_TRUE(index->can_skip_imcu(OnePredicate(PredicateOperator::GREATER_EQUAL, 21)));
  EXPECT_FALSE(index->can_skip_imcu(OnePredicate(PredicateOperator::GREATER_EQUAL, 20)));

  // col < v: skippable once min >= v.
  EXPECT_TRUE(index->can_skip_imcu(OnePredicate(PredicateOperator::LESS_THAN, 10)));
  EXPECT_FALSE(index->can_skip_imcu(OnePredicate(PredicateOperator::LESS_THAN, 11)));

  // col <= v: min == v still has a match, so only min > v prunes.
  EXPECT_TRUE(index->can_skip_imcu(OnePredicate(PredicateOperator::LESS_EQUAL, 9)));
  EXPECT_FALSE(index->can_skip_imcu(OnePredicate(PredicateOperator::LESS_EQUAL, 10)));
}

// BETWEEN prunes on a disjoint range; NOT BETWEEN prunes when the block is
// wholly contained in the excluded range.
TEST(StorageIndexPruningTest, BetweenAndNotBetween) {
  auto index = MakeIndex();
  index->update(kCol, 10.0);
  index->update(kCol, 20.0);

  const auto between = [](int64 lo, int64 hi) {
    std::vector<std::unique_ptr<ShannonBase::Imcs::Predicate>> preds;
    preds.push_back(
        std::make_unique<Simple_Predicate>(kCol, PredicateValue(lo), PredicateValue(hi), kColType));
    return preds;
  };

  EXPECT_TRUE(index->can_skip_imcu(between(0, 9)));     // entirely below
  EXPECT_TRUE(index->can_skip_imcu(between(21, 30)));   // entirely above
  EXPECT_FALSE(index->can_skip_imcu(between(15, 25)));  // overlaps
  EXPECT_FALSE(index->can_skip_imcu(between(0, 10)));   // touches min

  // NOT BETWEEN has no two-bound constructor of its own: build the BETWEEN
  // shape and retarget the operator, which is what the planner's builder does.
  const auto not_between = [&between](int64 lo, int64 hi) {
    auto preds = between(lo, hi);
    preds[0]->op = PredicateOperator::NOT_BETWEEN;
    return preds;
  };

  // The whole block sits inside the excluded range, so nothing qualifies.
  EXPECT_TRUE(index->can_skip_imcu(not_between(5, 25)));
  // Exactly the block's own bounds still excludes every row.
  EXPECT_TRUE(index->can_skip_imcu(not_between(10, 20)));
  // Partial overlap leaves rows outside the excluded range.
  EXPECT_FALSE(index->can_skip_imcu(not_between(15, 25)));
  EXPECT_FALSE(index->can_skip_imcu(not_between(0, 9)));
}

// IS NULL is skippable exactly when the block recorded no NULL.
TEST(StorageIndexPruningTest, IsNullFollowsNullCount) {
  auto no_nulls = MakeIndex();
  no_nulls->update(kCol, 1.0);
  std::vector<std::unique_ptr<ShannonBase::Imcs::Predicate>> is_null;
  is_null.push_back(
      std::make_unique<Simple_Predicate>(kCol, PredicateOperator::IS_NULL, PredicateValue::null_value(), kColType));
  EXPECT_TRUE(no_nulls->can_skip_imcu(is_null));

  auto with_null = MakeIndex();
  with_null->update(kCol, 1.0);
  with_null->update_null(kCol);
  std::vector<std::unique_ptr<ShannonBase::Imcs::Predicate>> is_null2;
  is_null2.push_back(
      std::make_unique<Simple_Predicate>(kCol, PredicateOperator::IS_NULL, PredicateValue::null_value(), kColType));
  EXPECT_FALSE(with_null->can_skip_imcu(is_null2));
}

// UPDATE / DELETE / rollback can leave min/max/null counters no longer
// conservative. invalidate_pruning() marks that, and while it is set nothing
// may be pruned however skippable the stale statistics look -- this is the
// guard that keeps a stale zone map from silently dropping rows.
TEST(StorageIndexPruningTest, InvalidatedPruningNeverSkips) {
  auto index = MakeIndex();
  index->update(kCol, 5.0);
  index->update(kCol, 5.0);

  // Skippable while the statistics are trusted.
  ASSERT_TRUE(index->can_skip_imcu(OnePredicate(PredicateOperator::EQUAL, 9)));
  ASSERT_TRUE(index->can_skip_imcu(OnePredicate(PredicateOperator::NOT_EQUAL, 5)));

  index->invalidate_pruning();
  EXPECT_TRUE(index->pruning_invalid());
  EXPECT_FALSE(index->can_skip_imcu(OnePredicate(PredicateOperator::EQUAL, 9)));
  EXPECT_FALSE(index->can_skip_imcu(OnePredicate(PredicateOperator::NOT_EQUAL, 5)));

  // clear_dirty() is what a full rebuild calls once the statistics are exact
  // again; pruning has to come back with it.
  index->clear_dirty();
  EXPECT_FALSE(index->pruning_invalid());
  EXPECT_TRUE(index->can_skip_imcu(OnePredicate(PredicateOperator::EQUAL, 9)));
}

// A predicate naming a column the index does not describe carries no usable
// statistics, so it must not prune.
TEST(StorageIndexPruningTest, OutOfRangeColumnNeverSkips) {
  auto index = MakeIndex();
  index->update(kCol, 10.0);

  std::vector<std::unique_ptr<ShannonBase::Imcs::Predicate>> preds;
  preds.push_back(std::make_unique<Simple_Predicate>(static_cast<uint32>(kNumColumns + 5), PredicateOperator::EQUAL,
                                                     PredicateValue(int64{9999}), kColType));
  EXPECT_FALSE(index->can_skip_imcu(preds));
}

}  // namespace shannon_storage_index_unittest
