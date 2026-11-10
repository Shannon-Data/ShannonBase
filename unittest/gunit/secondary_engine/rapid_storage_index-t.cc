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

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/utils/utils.h"
#include "unittest/gunit/fake_table.h"
#include "unittest/gunit/temptable/mock_field_varstring.h"
#include "unittest/gunit/test_utils.h"

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
    preds.push_back(std::make_unique<Simple_Predicate>(kCol, PredicateValue(lo), PredicateValue(hi), kColType));
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
    preds[0]->m_oper = PredicateOperator::NOT_BETWEEN;
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

// ------------------------------------------------------- serialize round-trip

/**
 * serialize()/deserialize() have no caller today -- the snapshot path rebuilds
 * the zone map instead. Covered anyway: the first caller makes every length in
 * the stream reachable from a corrupt file.
 */
TEST(StorageIndexSerializeTest, RoundTripsNumericAndStringStatistics) {
  auto index = MakeIndex();
  index->update(kCol, 10.0);
  index->update(kCol, 90.0);
  index->update_null(kCol);
  index->update_string_stats(kCol, "alpha");
  index->update_string_stats(kCol, "omega");

  std::stringstream buf;
  ASSERT_TRUE(index->serialize(buf));

  auto restored = MakeIndex();
  ASSERT_TRUE(restored->deserialize(buf));

  EXPECT_EQ(index->get_min_value(kCol), restored->get_min_value(kCol));
  EXPECT_EQ(index->get_max_value(kCol), restored->get_max_value(kCol));
  EXPECT_EQ(index->get_has_null(kCol), restored->get_has_null(kCol));
  EXPECT_EQ("alpha", restored->get_min_string(kCol));
  EXPECT_EQ("omega", restored->get_max_string(kCol));

  // The flag must come back set, or the first update_string_stats() after a
  // restore collapses both bounds onto one value.
  ASSERT_TRUE(restored->has_string_stats(kCol));
  restored->update_string_stats(kCol, "middle");
  EXPECT_EQ("alpha", restored->get_min_string(kCol)) << "min string was reset by the first post-restore update";
  EXPECT_EQ("omega", restored->get_max_string(kCol)) << "max string was reset by the first post-restore update";
}

// A stream that ends early must be refused, at every length in the format,
// rather than sized from whatever bytes were read.
TEST(StorageIndexSerializeTest, RejectsATruncatedStream) {
  auto index = MakeIndex();
  index->update(kCol, 10.0);
  index->update_string_stats(kCol, "alpha");
  std::stringstream buf;
  ASSERT_TRUE(index->serialize(buf));
  const std::string full = buf.str();
  ASSERT_GT(full.size(), sizeof(size_t));

  // Every proper prefix is a truncated file; none may be accepted.
  for (size_t len = 0; len < full.size(); ++len) {
    std::stringstream partial(full.substr(0, len));
    auto restored = MakeIndex();
    EXPECT_FALSE(restored->deserialize(partial)) << "accepted a stream truncated to " << len << " bytes";
  }

  // The whole stream still loads, so the loop above is not rejecting blindly.
  std::stringstream whole(full);
  auto restored = MakeIndex();
  EXPECT_TRUE(restored->deserialize(whole));
}

// A corrupt column count must not be handed to resize().
TEST(StorageIndexSerializeTest, RejectsAnImplausibleColumnCount) {
  const size_t absurd = std::numeric_limits<size_t>::max() / 2;
  std::stringstream buf;
  buf.write(reinterpret_cast<const char *>(&absurd), sizeof(absurd));

  auto restored = MakeIndex();
  EXPECT_FALSE(restored->deserialize(buf));
}

// Likewise for a corrupt string length: the count is plausible, the string is
// not.
TEST(StorageIndexSerializeTest, RejectsAnImplausibleStringLength) {
  auto index = MakeIndex();
  index->update(kCol, 10.0);
  index->update_string_stats(kCol, "alpha");
  std::stringstream buf;
  ASSERT_TRUE(index->serialize(buf));
  std::string raw = buf.str();

  // The numeric block ahead of the string is fixed width, so the offset is known.
  const size_t numeric = sizeof(double) * 4 + sizeof(size_t) + sizeof(bool) + sizeof(size_t);
  const size_t min_len_at = sizeof(size_t) + numeric;
  ASSERT_LE(min_len_at + sizeof(size_t), raw.size());
  size_t stored = 0;
  std::memcpy(&stored, raw.data() + min_len_at, sizeof(stored));
  ASSERT_EQ(std::string("alpha").size(), stored) << "serialized layout moved; fix the offset above";

  const size_t absurd = std::numeric_limits<size_t>::max() / 2;
  std::memcpy(raw.data() + min_len_at, &absurd, sizeof(absurd));

  std::stringstream corrupt(raw);
  auto restored = MakeIndex();
  EXPECT_FALSE(restored->deserialize(corrupt));
}

}  // namespace shannon_storage_index_unittest

namespace shannon_like_predicate_unittest {
using namespace ShannonBase::Imcs;

TEST(LikePredicateTest, SqlEscapesNullAndNegation) {
  Simple_Predicate like(0, PredicateOperator::LIKE, PredicateValue(std::string("a!_%")), MYSQL_TYPE_VARCHAR,
                        &my_charset_utf8mb4_0900_ai_ci, '!');
  Simple_Predicate unlike(0, PredicateOperator::NOT_LIKE, PredicateValue(std::string("a!_%")), MYSQL_TYPE_VARCHAR,
                          &my_charset_utf8mb4_0900_ai_ci, '!');
  const std::string matching = "A_tail";
  const std::string other = "abc";
  EXPECT_EQ(TruthValue::TRUE_VALUE, like.evaluate(reinterpret_cast<const uchar *>(matching.data()), matching.size()));
  EXPECT_EQ(TruthValue::FALSE_VALUE,
            unlike.evaluate(reinterpret_cast<const uchar *>(matching.data()), matching.size()));
  EXPECT_EQ(TruthValue::FALSE_VALUE, like.evaluate(reinterpret_cast<const uchar *>(other.data()), other.size()));
  EXPECT_EQ(TruthValue::TRUE_VALUE, unlike.evaluate(reinterpret_cast<const uchar *>(other.data()), other.size()));
  const uchar *null_value = nullptr;
  EXPECT_EQ(TruthValue::UNKNOWN, unlike.evaluate(null_value));
  auto clone = like.clone();
  EXPECT_TRUE(clone->equals(like));
  EXPECT_EQ(TruthValue::TRUE_VALUE, static_cast<Simple_Predicate *>(clone.get())
                                        ->evaluate(reinterpret_cast<const uchar *>(matching.data()), matching.size()));
}

TEST(LikePredicateTest, DictionaryMaskRefreshesAndKeepsPriorSnapshotValid) {
  auto dictionary = std::make_shared<ShannonBase::Compress::Dictionary>();
  auto store = [&](const std::string &text) {
    return dictionary->store(reinterpret_cast<const uchar *>(text.data()), text.size());
  };
  const auto first = store("alpha");
  const auto second = store("beta");
  Simple_Predicate like(0, PredicateOperator::LIKE, PredicateValue(std::string("a%")), MYSQL_TYPE_VARCHAR,
                        &my_charset_utf8mb4_0900_ai_ci, '\\');
  auto mask = like.dictionary_mask(dictionary);
  ASSERT_TRUE(mask);
  EXPECT_EQ(1, mask->at(first));
  EXPECT_EQ(0, mask->at(second));
  EXPECT_EQ(mask, like.dictionary_mask(dictionary));
  const auto added = store("ALPINE");
  auto refreshed = like.dictionary_mask(dictionary);
  ASSERT_TRUE(refreshed);
  EXPECT_NE(mask, refreshed);
  EXPECT_EQ(1, refreshed->at(added));
  EXPECT_EQ(1, mask->at(first));
  auto replacement = std::make_shared<ShannonBase::Compress::Dictionary>();
  const std::string other = "beta";
  const auto id = replacement->store(reinterpret_cast<const uchar *>(other.data()), other.size());
  EXPECT_EQ(0, like.dictionary_mask(replacement)->at(id));
}

TEST(LikePredicateTest, CollationMultibyteAndEmbeddedNull) {
  auto matches = [](const CHARSET_INFO *charset, const std::string &text, const std::string &pattern, int escape) {
    Simple_Predicate pred(0, PredicateOperator::LIKE, PredicateValue(pattern), MYSQL_TYPE_VARCHAR, charset, escape);
    return pred.evaluate(reinterpret_cast<const uchar *>(text.data()), text.size());
  };
  EXPECT_EQ(TruthValue::TRUE_VALUE, matches(&my_charset_utf8mb4_0900_ai_ci, "ALPHA", "a%", '\\'));
  EXPECT_EQ(TruthValue::FALSE_VALUE, matches(&my_charset_utf8mb4_bin, "ALPHA", "a%", '\\'));
  EXPECT_EQ(TruthValue::TRUE_VALUE, matches(&my_charset_utf8mb4_0900_ai_ci, "猫咪", "_咪", '\\'));
  EXPECT_EQ(TruthValue::TRUE_VALUE, matches(&my_charset_utf8mb4_0900_ai_ci, "a_tail", "a__%", '_'));
  EXPECT_EQ(TruthValue::TRUE_VALUE, matches(&my_charset_utf8mb4_0900_ai_ci, "a%tail", "a%%tail", '%'));
  EXPECT_EQ(TruthValue::TRUE_VALUE,
            matches(&my_charset_utf8mb4_bin, std::string("a\0b", 3), std::string("a\0_", 3), '\\'));
  EXPECT_EQ(TruthValue::FALSE_VALUE, matches(&my_charset_utf8mb4_bin, "alpha  ", "alpha", '\\'));
  EXPECT_EQ(TruthValue::TRUE_VALUE, matches(&my_charset_utf8mb4_bin, "", "", '\\'));
}

TEST(LikePredicateTest, CharDictionaryTrimsOnlyTextPadding) {
  auto dictionary = std::make_shared<ShannonBase::Compress::Dictionary>();
  const std::string padded = "alpha       ";
  const auto id = dictionary->store(reinterpret_cast<const uchar *>(padded.data()), padded.size());
  Simple_Predicate text(0, PredicateOperator::LIKE, PredicateValue(std::string("alpha")), MYSQL_TYPE_STRING,
                        &my_charset_utf8mb4_0900_ai_ci, '\\');
  EXPECT_EQ(TruthValue::TRUE_VALUE, text.evaluate(reinterpret_cast<const uchar *>(padded.data()), padded.size()));
  ASSERT_TRUE(text.dictionary_mask(dictionary));
  EXPECT_EQ(1, text.dictionary_mask(dictionary)->at(id));
  Simple_Predicate binary(0, PredicateOperator::LIKE, PredicateValue(std::string("alpha")), MYSQL_TYPE_STRING,
                          &my_charset_bin, '\\');
  EXPECT_EQ(TruthValue::FALSE_VALUE, binary.evaluate(reinterpret_cast<const uchar *>(padded.data()), padded.size()));
}
}  // namespace shannon_like_predicate_unittest


namespace shannon_dictionary_mask_unittest {
using namespace ShannonBase::Imcs;

class DictionaryMaskTest : public ::testing::Test {
 protected:
  void SetUp() override { server.SetUp(); }
  void TearDown() override { server.TearDown(); }
  my_testing::Server_initializer server;
};

TEST_F(DictionaryMaskTest, AdmitsEqualityButRejectsStringRangesWithBoundMetadata) {
  Fake_TABLE_SHARE share(1);
  Mock_field_varstring field(&share, "value", 16, true);
  auto dictionary = std::make_shared<ShannonBase::Compress::Dictionary>();
  const auto alpha = dictionary->store(reinterpret_cast<const uchar *>("alpha"), 5);
  const auto beta = dictionary->store(reinterpret_cast<const uchar *>("beta"), 4);
  for (auto op : {PredicateOperator::EQUAL, PredicateOperator::NOT_EQUAL}) {
    Simple_Predicate predicate(0, op, PredicateValue(std::string("ALPHA")), MYSQL_TYPE_VARCHAR);
    predicate.set_column_name_from_field(&field);
    auto mask = predicate.dictionary_mask(dictionary);
    ASSERT_TRUE(mask);
    EXPECT_EQ(op == PredicateOperator::EQUAL, mask->at(alpha));
    EXPECT_EQ(op == PredicateOperator::NOT_EQUAL, mask->at(beta));
    EXPECT_EQ(0, mask->at(0));  // Reserved NULL ID never matches, including NE.
  }
  for (auto op : {PredicateOperator::LESS_THAN, PredicateOperator::LESS_EQUAL,
                  PredicateOperator::GREATER_THAN, PredicateOperator::GREATER_EQUAL}) {
    Simple_Predicate predicate(0, op, PredicateValue(std::string("alpha")), MYSQL_TYPE_VARCHAR);
    predicate.set_column_name_from_field(&field);
    EXPECT_FALSE(predicate.dictionary_mask(dictionary));
  }
}
}  // namespace shannon_dictionary_mask_unittest

namespace shannon_dictionary_order_unittest {
using ShannonBase::Compress::Dictionary;
TEST(DictionaryOrder, CollationRanksGrowthRecoveryAndStableSnapshots) {
  Dictionary dictionary;
  auto put = [&](std::string_view text) { return dictionary.store(reinterpret_cast<const uchar *>(text.data()), text.size()); };
  const auto z = put("z"), a = put("a"), upper = put("A"), accent = put("á"), padded = put("a ");
  const auto empty = put("");
  auto order = dictionary.order_ranks(&my_charset_utf8mb4_0900_ai_ci, false);
  ASSERT_TRUE(order);
  EXPECT_LT(order->ranks[a], order->ranks[z]);  // IDs have the opposite order.
  EXPECT_EQ(order->ranks[a], order->ranks[upper]);
  EXPECT_EQ(order->ranks[a], order->ranks[accent]);
  EXPECT_LT(order->ranks[a], order->ranks[padded]);  // NO PAD
  EXPECT_LT(order->ranks[empty], order->ranks[a]);
  EXPECT_EQ(order, dictionary.order_ranks(&my_charset_utf8mb4_0900_ai_ci, false));
  auto trimmed = dictionary.order_ranks(&my_charset_utf8mb4_0900_ai_ci, true);
  ASSERT_TRUE(trimmed);
  EXPECT_EQ(trimmed->ranks[a], trimmed->ranks[padded]);
  auto binary = dictionary.order_ranks(&my_charset_bin, false);
  ASSERT_TRUE(binary);
  EXPECT_LT(binary->ranks[upper], binary->ranks[a]);
  const auto new_id = put("0");
  auto grown = dictionary.order_ranks(&my_charset_utf8mb4_0900_ai_ci, false);
  ASSERT_TRUE(grown);
  EXPECT_LT(grown->ranks[new_id], grown->ranks[a]);
  EXPECT_EQ(UINT32_MAX, grown->ranks[0]);
  dictionary.restore_entry(z, reinterpret_cast<const uchar *>("!"), 1);
  auto restored = dictionary.order_ranks(&my_charset_utf8mb4_0900_ai_ci, false);
  ASSERT_TRUE(restored);
  EXPECT_LT(restored->ranks[z], restored->ranks[a]);
  EXPECT_LT(order->ranks[a], order->ranks[z]);  // Previous snapshots remain immutable.
  dictionary.restore_entry(new_id + 3, reinterpret_cast<const uchar *>("x"), 1);
  auto sparse = dictionary.order_ranks(&my_charset_bin, false);
  ASSERT_TRUE(sparse);
  EXPECT_EQ(UINT32_MAX, sparse->ranks[new_id + 1]);
}

TEST(DictionaryOrder, CompressedEntriesUseDecodedCollationValues) {
  using ShannonBase::Compress::ENCODING_TYPE;
  auto dictionary = std::make_shared<Dictionary>(ENCODING_TYPE::SORTED);
  const std::string z(256, 'z'), a(256, 'a');
  const auto z_id = dictionary->store(reinterpret_cast<const uchar *>(z.data()), z.size(), ENCODING_TYPE::SORTED);
  const auto a_id = dictionary->store(reinterpret_cast<const uchar *>(a.data()), a.size(), ENCODING_TYPE::SORTED);
  ASSERT_EQ(nullptr, dictionary->get_view(z_id).data());
  auto order = dictionary->order_ranks(&my_charset_bin, false);
  ASSERT_TRUE(order);
  EXPECT_LT(order->ranks[a_id], order->ranks[z_id]);
  using namespace ShannonBase::Imcs;
  Simple_Predicate pred(0, PredicateOperator::LIKE, PredicateValue(std::string("a%")), MYSQL_TYPE_VARCHAR,
                        &my_charset_bin, 92);
  auto mask = pred.dictionary_mask(dictionary);
  ASSERT_TRUE(mask);
  EXPECT_EQ(1, mask->at(a_id));
  EXPECT_EQ(0, mask->at(z_id));
}

TEST(DictionaryOrder, LikeMaskInvalidatesOnReplacementWithoutSizeChange) {
  using namespace ShannonBase::Imcs;
  auto dictionary = std::make_shared<Dictionary>();
  auto id = dictionary->store(reinterpret_cast<const uchar *>("alpha"), 5);
  Simple_Predicate pred(0, PredicateOperator::LIKE, PredicateValue(std::string("a%")), MYSQL_TYPE_VARCHAR,
                        &my_charset_utf8mb4_0900_ai_ci, '\\');
  auto before = pred.dictionary_mask(dictionary);
  ASSERT_TRUE(before);
  EXPECT_EQ(1, before->at(id));
  dictionary->restore_entry(id, reinterpret_cast<const uchar *>("beta"), 4);
  auto after = pred.dictionary_mask(dictionary);
  ASSERT_TRUE(after);
  EXPECT_EQ(0, after->at(id));
  EXPECT_EQ(1, before->at(id));
}

TEST(DictionaryOrder, HighCardinalityUsesScalarFallback) {
  using namespace ShannonBase::Imcs;
  auto dictionary = std::make_shared<Dictionary>();
  dictionary->restore_entry(1u << 16, reinterpret_cast<const uchar *>("alpha"), 5);
  EXPECT_FALSE(dictionary->order_ranks(&my_charset_bin, false));
  Simple_Predicate pred(0, PredicateOperator::NOT_LIKE, PredicateValue(std::string("b%")), MYSQL_TYPE_VARCHAR,
                        &my_charset_bin, '\\');
  EXPECT_FALSE(pred.dictionary_mask(dictionary));
  EXPECT_EQ(TruthValue::TRUE_VALUE, pred.evaluate(reinterpret_cast<const uchar *>("alpha"), 5));
}
}  // namespace shannon_dictionary_order_unittest

namespace shannon_date_predicate_unittest {
using namespace ShannonBase::Imcs;

void CheckBatch(Simple_Predicate &predicate, const std::vector<const uchar *> &cells, size_t count) {
  ShannonBase::bit_array_t result(count + 5);
  std::memset(result.data, 0xff, result.size);
  predicate.evaluate(cells, count, result);
  for (size_t row = 0; row < count; ++row) {
    const uchar *cell = cells[row];
    EXPECT_EQ(predicate.evaluate(cell) == TruthValue::TRUE_VALUE, ShannonBase::Utils::Util::bit_array_get(&result, row))
        << "row=" << row << " predicate=" << predicate.to_string();
  }
  // The legacy constant-NULL guard clears the entire mask. Other cases,
  // including storage kernels, must preserve the following bits.
  if (!predicate.value.is_null()) {
    for (size_t row = count; row < result.rows; ++row) {
      EXPECT_TRUE(ShannonBase::Utils::Util::bit_array_get(&result, row));
    }
  }
}

TEST(DatePredicateBatch, ComparisonsNullsZeroDatesAndPartialBytesMatchScalar) {
  Field_newdate field(true, "d");
  field.set_field_index(0);
  const int64 dates[] = {0,        10100,    10101,    19950000, 19950100, 19950901,
                         19950930, 19951001, 20000229, 20010231, 99991231};
  const PredicateOperator ops[] = {PredicateOperator::EQUAL,        PredicateOperator::NOT_EQUAL,
                                   PredicateOperator::LESS_THAN,    PredicateOperator::LESS_EQUAL,
                                   PredicateOperator::GREATER_THAN, PredicateOperator::GREATER_EQUAL,
                                   PredicateOperator::BETWEEN,      PredicateOperator::NOT_BETWEEN,
                                   PredicateOperator::IS_NULL,      PredicateOperator::IS_NOT_NULL};
  for (size_t count : {size_t{0}, size_t{1}, size_t{7}, size_t{8}, size_t{9}, size_t{127}, size_t{128}, size_t{129}}) {
    for (unsigned null_every : {0u, 2u, 1u}) {
      std::vector<std::array<uchar, 3>> slots(count);
      std::vector<const uchar *> cells(count);
      for (size_t row = 0; row < count; ++row) {
        const auto date = dates[row % std::size(dates)];
        const uint32 packed = static_cast<uint32>(((date / 10000) << 9) | (((date / 100) % 100) << 5) | (date % 100));
        int3store(slots[row].data(), packed);
        cells[row] = null_every != 0 && row % null_every == 0 ? nullptr : slots[row].data();
      }
      for (auto op : ops) {
        for (int64 bound : {int64{0}, int64{10101}, int64{19950901}, int64{99991231}}) {
          SCOPED_TRACE(::testing::Message() << "count=" << count << " null_every=" << null_every);
          Simple_Predicate predicate(0, op, PredicateValue(bound), MYSQL_TYPE_DATE);
          predicate.field_meta = &field;
          predicate.value2 = PredicateValue(int64{19951001});
          CheckBatch(predicate, cells, count);
        }
      }
    }
  }
}

TEST(DatePredicateBatch, UnpackableAndNullBoundsKeepScalarComparisonSemantics) {
  Field_newdate field(true, "d");
  field.set_field_index(0);
  std::array<uchar, 3> slot{};
  int3store(slot.data(), (1995u << 9) | (9u << 5) | 1u);
  const std::vector<const uchar *> cells{slot.data(), nullptr, slot.data()};
  std::vector<PredicateValue> bounds{PredicateValue(int64{-1}),       PredicateValue(int64{19950899}),
                                     PredicateValue(int64{19951301}), PredicateValue(int64{99999999}),
                                     PredicateValue(19950901.5),      PredicateValue(std::string("19950901")),
                                     PredicateValue::null_value()};
  PredicateValue unsigned_bound(int64{-1});
  unsigned_bound.unsigned_int64 = true;
  bounds.push_back(unsigned_bound);
  for (const auto &bound : bounds) {
    for (auto op : {PredicateOperator::EQUAL, PredicateOperator::NOT_EQUAL, PredicateOperator::LESS_THAN,
                    PredicateOperator::BETWEEN, PredicateOperator::NOT_BETWEEN}) {
      Simple_Predicate predicate(0, op, bound, MYSQL_TYPE_DATE);
      predicate.field_meta = &field;
      predicate.value2 = PredicateValue::null_value();
      CheckBatch(predicate, cells, cells.size());
    }
  }
  Simple_Predicate in(0, std::vector<PredicateValue>{PredicateValue(int64{19950901}), PredicateValue::null_value()},
                      false, MYSQL_TYPE_DATE);
  in.field_meta = &field;
  CheckBatch(in, cells, cells.size());
  Simple_Predicate not_in(0, in.value_list, true, MYSQL_TYPE_DATE);
  not_in.field_meta = &field;
  CheckBatch(not_in, cells, cells.size());
}

TEST(FixedWidthPredicateBatch, NarrowIntegersSelectWidthSignednessAndByteOrderOnce) {
  for (bool is_unsigned : {false, true}) {
    Field_tiny tiny(3, true, "t", is_unsigned);
    Field_short small(5, true, "s", is_unsigned);
    Field_medium medium(8, true, "m", is_unsigned);
    for (Field *field : {static_cast<Field *>(&tiny), static_cast<Field *>(&small), static_cast<Field *>(&medium)}) {
      field->set_field_index(0);
      for (bool low_order : {false, true}) {
        std::vector<std::array<uchar, 3>> slots(129);
        std::vector<const uchar *> cells(slots.size());
        const uint32 values[] = {0, 1, 127, 128, 255, 32767, 32768, 65535, 8388607, 8388608, 16777215};
        for (size_t row = 0; row < slots.size(); ++row) {
          const auto value = values[row % std::size(values)];
          if (field->pack_length() == 1)
            slots[row][0] = static_cast<uchar>(value);
          else if (field->pack_length() == 2) {
            if (low_order)
              int2store(slots[row].data(), value);
            else {
              const uint16 native = static_cast<uint16>(value);
              std::memcpy(slots[row].data(), &native, sizeof(native));
            }
          } else
            int3store(slots[row].data(), value);
          cells[row] = row % 7 == 0 ? nullptr : slots[row].data();
        }
        for (auto op : {PredicateOperator::EQUAL, PredicateOperator::NOT_EQUAL, PredicateOperator::LESS_THAN,
                        PredicateOperator::GREATER_EQUAL, PredicateOperator::BETWEEN, PredicateOperator::NOT_BETWEEN,
                        PredicateOperator::IS_NULL, PredicateOperator::IS_NOT_NULL}) {
          for (int64 bound : {std::numeric_limits<int64>::min(), int64{-1}, int64{0}, int64{128}, int64{32768},
                              int64{8388608}, std::numeric_limits<int64>::max()}) {
            SCOPED_TRACE(::testing::Message()
                         << "type=" << field->type() << " unsigned=" << is_unsigned << " low_order=" << low_order);
            Simple_Predicate predicate(0, op, PredicateValue(bound), field->type());
            predicate.field_meta = field;
            predicate.low_order = low_order;
            predicate.value2 = PredicateValue(int64{127});
            CheckBatch(predicate, cells, cells.size());
          }
        }
      }
    }
  }
}

TEST(FixedWidthPredicateBatch, YearZeroAndGapsMatchScalar) {
  Field_year field(true, "y");
  field.set_field_index(0);
  std::array<uchar, 256> slots{};
  std::vector<const uchar *> cells;
  for (size_t row = 0; row < slots.size(); ++row) {
    slots[row] = static_cast<uchar>(row);
    cells.push_back(slots.data() + row);
  }
  cells.push_back(nullptr);
  for (int64 bound : {int64{0}, int64{1}, int64{1900}, int64{1901}, int64{2000}, int64{2155}, int64{2156}}) {
    for (auto op : {PredicateOperator::EQUAL, PredicateOperator::NOT_EQUAL, PredicateOperator::LESS_THAN,
                    PredicateOperator::GREATER_EQUAL, PredicateOperator::BETWEEN, PredicateOperator::NOT_BETWEEN}) {
      Simple_Predicate predicate(0, op, PredicateValue(bound), MYSQL_TYPE_YEAR);
      predicate.field_meta = &field;
      predicate.value2 = PredicateValue(int64{2000});
      CheckBatch(predicate, cells, cells.size());
    }
  }
}

TEST(FixedWidthPredicateBatch, DatetimeBinaryOrderAndFractionalFallbackMatchScalar) {
  const int64 values[] = {
      0, 10101000000, 19950900000000, 19950901000000, 19950930235959, 20000229123045, 20010231123045, 99991231235959};
  for (unsigned precision : {0u, 6u}) {
    Field_datetimef field(true, "dt", precision);
    field.set_field_index(0);
    std::vector<std::array<uchar, 8>> slots(std::size(values));
    std::vector<const uchar *> cells;
    for (size_t row = 0; row < slots.size(); ++row) {
      const auto number = values[row];
      MYSQL_TIME time{};
      time.year = static_cast<uint>(number / 10000000000LL);
      time.month = static_cast<uint>((number / 100000000) % 100);
      time.day = static_cast<uint>((number / 1000000) % 100);
      time.hour = static_cast<uint>((number / 10000) % 100);
      time.minute = static_cast<uint>((number / 100) % 100);
      time.second = static_cast<uint>(number % 100);
      time.second_part = precision == 0 ? 0 : 123456;
      time.time_type = MYSQL_TIMESTAMP_DATETIME;
      my_datetime_packed_to_binary(TIME_to_longlong_datetime_packed(time), slots[row].data(), precision);
      cells.push_back(slots[row].data());
    }
    cells.push_back(nullptr);
    for (int64 bound :
         {int64{0}, int64{19950901000000}, int64{19950930235959}, int64{19950930235999}, int64{99991231235959}}) {
      for (auto op : {PredicateOperator::EQUAL, PredicateOperator::NOT_EQUAL, PredicateOperator::LESS_THAN,
                      PredicateOperator::LESS_EQUAL, PredicateOperator::GREATER_THAN, PredicateOperator::GREATER_EQUAL,
                      PredicateOperator::BETWEEN, PredicateOperator::NOT_BETWEEN}) {
        Simple_Predicate predicate(0, op, PredicateValue(bound), MYSQL_TYPE_DATETIME);
        predicate.field_meta = &field;
        predicate.value2 = PredicateValue(int64{19951001000000});
        CheckBatch(predicate, cells, cells.size());
      }
    }
  }
}
}  // namespace shannon_date_predicate_unittest
