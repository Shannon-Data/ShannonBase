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
 * Unit test for the Rapid ART index: node layout, key/value storage, the
 * insert/search/delete decision paths and the bidirectional cursor.
 *
 * The ART is the only structure that maps a Rapid key to its row ids, so a
 * wrong answer here is a wrong answer to the query: a lost leaf drops rows from
 * an index lookup with no error anywhere, and MTR can only see that indirectly,
 * as a row count that happens to disagree with InnoDB.  Every case below
 * compares the tree against a std::map oracle over one of several key shapes.
 *
 * Key shapes matter because the tree's behaviour is entirely driven by byte
 * patterns: a dense sequence keeps one compressed path, random keys fan out at
 * the root, a long shared prefix runs past MAX_PREFIX_LEN and forces the
 * fallback that re-reads the prefix from a leaf, and 256 distinct first bytes
 * walk a node up through all four fan-out sizes and back down again.
 *
 * ART keys arrive from RapidKeyCodec, which frames every key part as
 * [NULL=00|NONNULL=01] [01,b0] [01,b1] ... [00].  That framing is what makes
 * Rapid's keys prefix-free, and this ART -- like the libart it is derived from
 * -- relies on it: it has no terminator handling, so a key that is a proper
 * prefix of another key cannot be stored.  The variable-length shapes below
 * reproduce the framing rather than raw strings for that reason.
 */

#include "storage/rapid_engine/imcs/index/art/art.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "storage/rapid_engine/imcs/index/art/iterator.h"
#include "storage/rapid_engine/imcs/index/encoder.h"
#include "storage/rapid_engine/imcs/index/index.h"

namespace shannon_rapid_art_unittest {

using ShannonBase::Imcs::Index::ART;
using ShannonBase::Imcs::Index::ARTIterator;
using ShannonBase::Imcs::Index::Encoder;
using ShannonBase::Imcs::Index::Index;

// Raw key bytes; embedded NULs are ordinary content.
using Key = std::string;
using RowId = uint64_t;
// key -> row ids under it, in insertion order, which is also the order the tree
// must return them in.
using Oracle = std::map<Key, std::vector<RowId>>;

const unsigned char *Bytes(const Key &k) { return reinterpret_cast<const unsigned char *>(k.data()); }

int KeyLen(const Key &k) { return static_cast<int>(k.size()); }

void Insert(ART *t, Oracle *o, const Key &k, RowId v) {
  t->ART_insert(Bytes(k), KeyLen(k), &v, sizeof(v));
  (*o)[k].push_back(v);
}

// ART_search hands back the last value stored under the key.
bool SearchLast(ART *t, const Key &k, RowId *out) {
  void *p = t->ART_search(Bytes(k), KeyLen(k));
  if (p == nullptr) return false;
  std::memcpy(out, p, sizeof(RowId));
  return true;
}

std::vector<RowId> SearchAll(ART *t, const Key &k) {
  std::vector<RowId> out;
  for (const auto &bytes : t->ART_search_all(Bytes(k), KeyLen(k))) {
    RowId v = 0;
    EXPECT_EQ(sizeof(RowId), bytes.size());
    if (bytes.size() == sizeof(RowId)) std::memcpy(&v, bytes.data(), sizeof(v));
    out.push_back(v);
  }
  return out;
}

// Every key/value pair the tree yields in its own order.
std::vector<std::pair<Key, RowId>> WalkTree(ART *t) {
  std::vector<std::pair<Key, RowId>> seen;
  t->ART_iter(
      [](void *data, const void *key, uint32_t key_len, const void *value, uint32_t value_len) -> int {
        auto *out = static_cast<std::vector<std::pair<Key, RowId>> *>(data);
        RowId v = 0;
        if (value_len == sizeof(RowId)) std::memcpy(&v, value, sizeof(v));
        out->emplace_back(Key(static_cast<const char *>(key), key_len), v);
        return 0;
      },
      &seen);
  return seen;
}

std::vector<std::pair<Key, RowId>> Flatten(const Oracle &o) {
  std::vector<std::pair<Key, RowId>> out;
  for (const auto &[k, values] : o)
    for (RowId v : values) out.emplace_back(k, v);
  return out;
}

// Assert the tree agrees with the oracle on point lookups, duplicate lists and
// full ordered traversal.
void ExpectMatchesOracle(ART *t, const Oracle &o) {
  for (const auto &[k, values] : o) {
    RowId last = 0;
    ASSERT_TRUE(SearchLast(t, k, &last)) << "key of " << k.size() << " bytes vanished";
    EXPECT_EQ(values.back(), last);
    EXPECT_EQ(values, SearchAll(t, k));
  }
  EXPECT_EQ(Flatten(o), WalkTree(t));
}

// ---------------------------------------------------------------- key shapes

enum class Shape {
  SEQ_BE64,       // dense big-endian integers: one long compressed path
  RANDOM_BE64,    // random integers: wide fan-out just below the root
  COMPOSITE,      // (BIGINT, INT): the TPC-H lineitem primary key shape
  LONG_PREFIX,    // 64 shared bytes, past MAX_PREFIX_LEN
  FRAMED_VARLEN,  // RapidKeyCodec framing at every length from 1 upward
  EMBEDDED_NUL,   // NUL bytes inside the key, not just around it
  WIDE_FANOUT,    // all 256 first bytes: Node4 -> 16 -> 48 -> 256 and back
};

void AppendBigEndian(Key *k, uint64_t value, int width) {
  const size_t base = k->size();
  k->append(width, '\0');
  for (int b = 0; b < width; ++b) (*k)[base + width - 1 - b] = static_cast<char>((value >> (8 * b)) & 0xff);
}

// One RapidKeyCodec-framed part: marker, escaped body, terminator.
Key FramedPart(const std::string &body) {
  Key k;
  k.push_back('\x01');  // NON-NULL marker
  for (char c : body) {
    k.push_back('\x01');
    k.push_back(c);
  }
  k.push_back('\x00');  // terminator: makes keys of differing length prefix-free
  return k;
}

std::vector<Key> MakeKeys(Shape shape, size_t n, std::mt19937_64 *rng) {
  std::vector<Key> keys;
  keys.reserve(n);
  switch (shape) {
    case Shape::SEQ_BE64:
      for (size_t i = 0; i < n; ++i) {
        Key k;
        AppendBigEndian(&k, i, 8);
        keys.push_back(std::move(k));
      }
      break;
    case Shape::RANDOM_BE64: {
      std::set<Key> unique;
      while (unique.size() < n) {
        Key k;
        AppendBigEndian(&k, (*rng)(), 8);
        unique.insert(std::move(k));
      }
      keys.assign(unique.begin(), unique.end());
      break;
    }
    case Shape::COMPOSITE:
      for (size_t i = 0; i < n; ++i) {
        Key k;
        AppendBigEndian(&k, i / 7, 8);
        AppendBigEndian(&k, i % 7, 4);
        keys.push_back(std::move(k));
      }
      break;
    case Shape::LONG_PREFIX:
      for (size_t i = 0; i < n; ++i) {
        Key k(56, 'P');  // well past MAX_PREFIX_LEN
        AppendBigEndian(&k, i, 8);
        keys.push_back(std::move(k));
      }
      break;
    case Shape::FRAMED_VARLEN:
      for (size_t i = 1; i <= n; ++i) keys.push_back(FramedPart(std::string(i, 'a')));
      break;
    case Shape::EMBEDDED_NUL:
      for (size_t i = 0; i < n; ++i) {
        Key k("k\0", 2);
        AppendBigEndian(&k, i, 4);
        k.append("\0z", 2);
        keys.push_back(std::move(k));
      }
      break;
    case Shape::WIDE_FANOUT:
      for (size_t i = 0; i < n; ++i) {
        Key k;
        k.push_back(static_cast<char>(i % 256));
        k.push_back(static_cast<char>((i / 256) % 256));
        k.push_back(static_cast<char>((i / 65536) % 256));
        keys.push_back(std::move(k));
      }
      break;
  }
  return keys;
}

const char *ShapeName(Shape s) {
  switch (s) {
    case Shape::SEQ_BE64:
      return "SEQ_BE64";
    case Shape::RANDOM_BE64:
      return "RANDOM_BE64";
    case Shape::COMPOSITE:
      return "COMPOSITE";
    case Shape::LONG_PREFIX:
      return "LONG_PREFIX";
    case Shape::FRAMED_VARLEN:
      return "FRAMED_VARLEN";
    case Shape::EMBEDDED_NUL:
      return "EMBEDDED_NUL";
    case Shape::WIDE_FANOUT:
      return "WIDE_FANOUT";
  }
  return "?";
}

// FRAMED_VARLEN grows a byte per key, so it stays short.
size_t KeyCountFor(Shape s) { return s == Shape::FRAMED_VARLEN ? 200 : 2000; }

// ------------------------------------------------------------- leaf internals

TEST(RapidArtLeaf, LayoutIsCompact) {
  // These are the numbers the index's memory footprint rests on: one leaf is
  // paid per indexed row, and at 40 bytes make_shared turns it plus its
  // 16-byte control block into a single small heap chunk. A leaf that grows a
  // vtable, a prefix buffer or a lock word again costs that on every row of
  // every loaded table, so pin it here rather than discovering it in an OOM.
  EXPECT_EQ(1u, sizeof(ART::Art_node));
  EXPECT_EQ(24u, sizeof(ART::Art_inner_node));
  EXPECT_EQ(40u, sizeof(ART::Art_leaf));
  EXPECT_EQ(ART::LEAF, ART::Art_leaf().type());
}

TEST(RapidArtLeaf, StoresKeyAndValuesAcrossSpills) {
  const unsigned char key[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  RowId first = 111;
  ART::Art_leaf leaf(key, sizeof(key), &first, sizeof(first));

  EXPECT_EQ(12u, leaf.key_length());
  EXPECT_EQ(0, std::memcmp(leaf.key(), key, sizeof(key)));
  EXPECT_EQ(1u, leaf.value_count());
  EXPECT_EQ(sizeof(RowId), leaf.value_length());

  // Append well past the inline buffer. The key and every earlier value must
  // survive each move to a wider heap buffer.
  std::vector<RowId> expected{first};
  for (RowId v = 200; v < 280; ++v) {
    ASSERT_TRUE(leaf.add_value(&v, sizeof(v)));
    expected.push_back(v);
    ASSERT_EQ(12u, leaf.key_length());
    ASSERT_EQ(0, std::memcmp(leaf.key(), key, sizeof(key)));
    ASSERT_EQ(expected.size(), leaf.value_count());
    for (size_t i = 0; i < expected.size(); ++i) {
      RowId got = 0;
      std::memcpy(&got, leaf.value_at(static_cast<uint32_t>(i)), sizeof(got));
      ASSERT_EQ(expected[i], got) << "value " << i << " lost at count " << expected.size();
    }
  }
  EXPECT_EQ(nullptr, leaf.value_at(leaf.value_count()));
}

TEST(RapidArtLeaf, RefusesAValueOfADifferentWidth) {
  // Values in one leaf are packed at a single stride. Accepting a narrower one
  // would shift every value after it, so it is refused instead.
  const unsigned char key[4] = {9, 9, 9, 9};
  RowId wide = 1;
  ART::Art_leaf leaf(key, sizeof(key), &wide, sizeof(wide));
  uint32_t narrow = 2;
  EXPECT_FALSE(leaf.add_value(&narrow, sizeof(narrow)));
  EXPECT_EQ(1u, leaf.value_count());
  EXPECT_FALSE(leaf.add_value(nullptr, sizeof(wide)));
  EXPECT_EQ(1u, leaf.value_count());
}

TEST(RapidArtLeaf, RemovesFromEitherEndAndTheMiddle) {
  const unsigned char key[4] = {7, 7, 7, 7};
  RowId v = 0;
  ART::Art_leaf leaf(key, sizeof(key), &v, sizeof(v));
  std::vector<RowId> expected{0};
  for (RowId i = 1; i < 40; ++i) {
    ASSERT_TRUE(leaf.add_value(&i, sizeof(i)));
    expected.push_back(i);
  }

  for (RowId victim : {RowId{20}, RowId{0}, RowId{39}}) {
    ASSERT_TRUE(leaf.remove_value(&victim, sizeof(victim)));
    expected.erase(std::find(expected.begin(), expected.end(), victim));
    ASSERT_EQ(expected.size(), leaf.value_count());
    for (size_t i = 0; i < expected.size(); ++i) {
      RowId got = 0;
      std::memcpy(&got, leaf.value_at(static_cast<uint32_t>(i)), sizeof(got));
      ASSERT_EQ(expected[i], got);
    }
  }
  RowId absent = 9999;
  EXPECT_FALSE(leaf.remove_value(&absent, sizeof(absent)));
}

TEST(RapidArtLeaf, ClearReleasesTheSpillAndStaysUsable) {
  // A leaf is cleared while an in-flight cursor may still hold a reference to
  // it, so it has to read as empty rather than as freed memory -- and it must
  // fall back to its inline buffer, not keep pointing at the released one.
  const unsigned char key[20] = {};
  RowId v = 5;
  ART::Art_leaf leaf(key, sizeof(key), &v, sizeof(v));
  for (RowId extra = 1; extra < 50; ++extra) ASSERT_TRUE(leaf.add_value(&extra, sizeof(extra)));

  leaf.clear();
  EXPECT_EQ(0u, leaf.key_length());
  EXPECT_EQ(nullptr, leaf.key());
  EXPECT_EQ(0u, leaf.value_count());
  EXPECT_EQ(nullptr, leaf.value_at(0));

  RowId again = 42;
  ASSERT_TRUE(leaf.add_value(&again, sizeof(again)));
  EXPECT_EQ(1u, leaf.value_count());
  RowId got = 0;
  std::memcpy(&got, leaf.value_at(0), sizeof(got));
  EXPECT_EQ(again, got);
}

TEST(RapidArtLeaf, HoldsAKeyWiderThanTheInlineBuffer) {
  std::vector<unsigned char> key(300, 0xab);
  RowId v = 77;
  ART::Art_leaf leaf(key.data(), static_cast<int>(key.size()), &v, sizeof(v));
  EXPECT_EQ(key.size(), leaf.key_length());
  EXPECT_EQ(0, std::memcmp(leaf.key(), key.data(), key.size()));
  RowId got = 0;
  std::memcpy(&got, leaf.value_at(0), sizeof(got));
  EXPECT_EQ(v, got);
}

// ------------------------------------------------------- tree over key shapes

class RapidArtShape : public ::testing::TestWithParam<Shape> {
 protected:
  void SetUp() override {
    m_rng.seed(20240910);
    m_tree.ART_tree_init();
    m_keys = MakeKeys(GetParam(), KeyCountFor(GetParam()), &m_rng);
  }
  void TearDown() override { m_tree.ART_tree_destroy(); }

  // Load every key, in an order unrelated to their sort order, so the tree is
  // not built left to right.
  void LoadShuffled(int duplicates_per_key) {
    std::vector<size_t> order(m_keys.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), m_rng);
    RowId next = 1;
    for (size_t idx : order)
      for (int d = 0; d < duplicates_per_key; ++d) Insert(&m_tree, &m_oracle, m_keys[idx], next++);
  }

  std::vector<Key> ShuffledOracleKeys() {
    std::vector<Key> keys;
    for (const auto &entry : m_oracle) keys.push_back(entry.first);
    std::shuffle(keys.begin(), keys.end(), m_rng);
    return keys;
  }

  std::mt19937_64 m_rng;
  ART m_tree;
  Oracle m_oracle;
  std::vector<Key> m_keys;
};

INSTANTIATE_TEST_SUITE_P(AllShapes, RapidArtShape,
                         ::testing::Values(Shape::SEQ_BE64, Shape::RANDOM_BE64, Shape::COMPOSITE, Shape::LONG_PREFIX,
                                           Shape::FRAMED_VARLEN, Shape::EMBEDDED_NUL, Shape::WIDE_FANOUT),
                         [](const ::testing::TestParamInfo<Shape> &shape_info) { return ShapeName(shape_info.param); });

TEST_P(RapidArtShape, StoresAndFindsEveryKey) {
  LoadShuffled(1);
  ExpectMatchesOracle(&m_tree, m_oracle);
}

TEST_P(RapidArtShape, KeepsDuplicateRowIdsInInsertionOrder) {
  LoadShuffled(5);
  ExpectMatchesOracle(&m_tree, m_oracle);
}

TEST_P(RapidArtShape, MissesKeysThatWereNeverInserted) {
  LoadShuffled(1);
  const size_t step = std::max<size_t>(1, m_keys.size() / 32);
  for (size_t i = 0; i < m_keys.size(); i += step) {
    RowId ignored = 0;
    // One byte flipped: same length, same path down to the last node.
    Key mutated = m_keys[i];
    mutated.back() = static_cast<char>(mutated.back() ^ 0x5a);
    if (m_oracle.count(mutated) == 0) {
      EXPECT_FALSE(SearchLast(&m_tree, mutated, &ignored)) << "flipped byte matched";
    }

    // One byte longer and one byte shorter: a descent that runs off the end of
    // the key, and one that stops before the leaf.
    Key longer = m_keys[i] + '\x01';
    if (m_oracle.count(longer) == 0) {
      EXPECT_FALSE(SearchLast(&m_tree, longer, &ignored)) << "extended key matched";
    }
    if (m_keys[i].size() > 1) {
      Key shorter = m_keys[i].substr(0, m_keys[i].size() - 1);
      if (m_oracle.count(shorter) == 0) {
        EXPECT_FALSE(SearchLast(&m_tree, shorter, &ignored)) << "prefix matched";
      }
    }
  }
}

TEST_P(RapidArtShape, DeleteValueDropsOneRowIdAndKeepsItsSiblings) {
  LoadShuffled(3);
  for (const Key &k : ShuffledOracleKeys()) {
    auto &values = m_oracle[k];
    const RowId victim = values.front();
    EXPECT_NE(nullptr, m_tree.ART_delete_value(Bytes(k), KeyLen(k), &victim, sizeof(victim)));
    values.erase(values.begin());
    // The same value a second time is gone, and must report so.
    EXPECT_EQ(nullptr, m_tree.ART_delete_value(Bytes(k), KeyLen(k), &victim, sizeof(victim)));
    if (values.empty()) m_oracle.erase(k);
  }
  ExpectMatchesOracle(&m_tree, m_oracle);
}

TEST_P(RapidArtShape, DeleteRemovesOnlyTheKeyItWasGiven) {
  // The regression this pins: Recursive_delete used to unhook a child at every
  // frame as it unwound, tearing a whole subtree out of every ancestor on the
  // path, and the Node48->Node16 and Node16->Node4 shrinks left the
  // replacement node reporting zero children. Both showed up only as keys that
  // silently stopped being findable after unrelated keys were deleted.
  LoadShuffled(1);
  std::vector<Key> keys = ShuffledOracleKeys();
  for (size_t i = 0; i < keys.size(); ++i) {
    EXPECT_NE(nullptr, m_tree.ART_delete(Bytes(keys[i]), KeyLen(keys[i]))) << "delete " << i << " of " << keys.size();
    m_oracle.erase(keys[i]);
    EXPECT_EQ(nullptr, m_tree.ART_delete(Bytes(keys[i]), KeyLen(keys[i]))) << "second delete " << i;
    // Every survivor is still reachable after each individual deletion.
    if (i % 64 == 0 || i + 1 == keys.size()) {
      for (const auto &[k, values] : m_oracle) {
        RowId last = 0;
        ASSERT_TRUE(SearchLast(&m_tree, k, &last)) << "survivor lost after deleting " << (i + 1) << " keys";
        ASSERT_EQ(values.back(), last);
      }
    }
  }
  EXPECT_EQ(nullptr, m_tree.root());
}

TEST_P(RapidArtShape, IsReusableAfterBeingEmptied) {
  LoadShuffled(1);
  std::vector<Key> keys = ShuffledOracleKeys();
  for (const Key &k : keys) m_tree.ART_delete(Bytes(k), KeyLen(k));
  m_oracle.clear();
  ASSERT_EQ(nullptr, m_tree.root());

  RowId next = 500000;
  for (const Key &k : keys) Insert(&m_tree, &m_oracle, k, next++);
  ExpectMatchesOracle(&m_tree, m_oracle);
}

TEST_P(RapidArtShape, ReportsTheSmallestAndLargestKey) {
  LoadShuffled(1);
  ART::Art_leaf *lo = m_tree.ART_minimum();
  ART::Art_leaf *hi = m_tree.ART_maximum();
  ASSERT_NE(nullptr, lo);
  ASSERT_NE(nullptr, hi);
  EXPECT_EQ(m_oracle.begin()->first, Key(reinterpret_cast<const char *>(lo->key()), lo->key_length()));
  EXPECT_EQ(m_oracle.rbegin()->first, Key(reinterpret_cast<const char *>(hi->key()), hi->key_length()));
}

/**
 * ART_minimum()/ART_maximum() drop the tree lock as they return, so a writer
 * can free the leaf before the caller reads it -- Index::minimum()/maximum()
 * used to do exactly that. The *_copy() overloads copy under the lock.
 */
TEST_P(RapidArtShape, CopiesTheBoundaryRowIdsUnderTheTreeLock) {
  LoadShuffled(2);

  const Key &lo_key = m_oracle.begin()->first;
  const Key &hi_key = m_oracle.rbegin()->first;

  // Value 0 of the boundary leaves, which is the first row id inserted there.
  RowId lo = 0;
  RowId hi = 0;
  ASSERT_TRUE(m_tree.ART_minimum_copy(&lo, sizeof(lo), 0));
  ASSERT_TRUE(m_tree.ART_maximum_copy(&hi, sizeof(hi), 0));
  EXPECT_EQ(m_oracle.at(lo_key).front(), lo);
  EXPECT_EQ(m_oracle.at(hi_key).front(), hi);

  // Duplicates are reachable by index, and one past the end is not.
  RowId second = 0;
  ASSERT_TRUE(m_tree.ART_minimum_copy(&second, sizeof(second), 1));
  EXPECT_EQ(m_oracle.at(lo_key).at(1), second);
  EXPECT_FALSE(m_tree.ART_minimum_copy(&second, sizeof(second), 2));

  // A width that disagrees with the stored value is refused rather than
  // copying a partial value into the caller's buffer.
  uint32_t narrow = 0xdeadbeef;
  EXPECT_FALSE(m_tree.ART_minimum_copy(&narrow, sizeof(narrow), 0));
  EXPECT_EQ(0xdeadbeefu, narrow) << "a refused copy must not write to the output";
  EXPECT_FALSE(m_tree.ART_maximum_copy(&narrow, sizeof(narrow), 0));
}

/**
 * An index swap replays as remove(old_key) + insert(new_key). Once the remove
 * half has taken effect, replaying appended the row id twice under one key and
 * an index scan returned the same physical row twice.
 */
TEST_P(RapidArtShape, IgnoresARepeatedKeyValuePair) {
  LoadShuffled(1);

  // Replay every pair the tree already holds.
  for (const auto &[key, values] : m_oracle) {
    for (RowId v : values) {
      RowId dup = v;
      m_tree.ART_insert(Bytes(key), KeyLen(key), &dup, sizeof(dup));
    }
  }

  // The oracle is deliberately not updated: the replay must change nothing.
  ExpectMatchesOracle(&m_tree, m_oracle);
  for (const auto &[key, values] : m_oracle) {
    EXPECT_EQ(values.size(), SearchAll(&m_tree, key).size()) << "key grew a duplicate row id";
  }
}

/** The duplicate check must key off the value: a non-unique index legitimately
 *  stores several row ids under one key. */
TEST_P(RapidArtShape, StillAppendsADistinctRowIdUnderAnExistingKey) {
  LoadShuffled(1);

  RowId next = 1000000;
  for (const auto &key : ShuffledOracleKeys()) Insert(&m_tree, &m_oracle, key, next++);

  ExpectMatchesOracle(&m_tree, m_oracle);
  for (const auto &[key, values] : m_oracle) {
    ASSERT_EQ(2u, values.size());
    EXPECT_EQ(values, SearchAll(&m_tree, key)) << "a distinct row id was swallowed by the duplicate check";
  }
}

// -------------------------------------------------------------- range cursor

/**
 * ARTIterator takes no locks of its own: the tree lock belongs to its caller
 * (Art_Iterator in imcs/index/iterator.h holds it around every call).  This
 * mirrors that discipline so the test exercises the same locking the engine
 * uses rather than a looser one.
 */
class LockedCursor {
 public:
  explicit LockedCursor(ART *t) : m_tree(t), m_iter(t) {}

  std::vector<std::pair<Key, RowId>> Forward(const Key *lo, bool lo_incl, const Key *hi, bool hi_incl) {
    std::shared_lock<std::shared_mutex> guard(m_tree->tree()->tree_mutex);
    m_iter.init_scan(lo ? Bytes(*lo) : nullptr, lo ? KeyLen(*lo) : 0, lo_incl, hi ? Bytes(*hi) : nullptr,
                     hi ? KeyLen(*hi) : 0, hi_incl);
    return Drain(/*forward=*/true);
  }

  std::vector<std::pair<Key, RowId>> Backward(const Key *lo, bool lo_incl, const Key *hi, bool hi_incl) {
    std::shared_lock<std::shared_mutex> guard(m_tree->tree()->tree_mutex);
    m_iter.init_reverse_scan(lo ? Bytes(*lo) : nullptr, lo ? KeyLen(*lo) : 0, lo_incl, hi ? Bytes(*hi) : nullptr,
                             hi ? KeyLen(*hi) : 0, hi_incl);
    return Drain(/*forward=*/false);
  }

 private:
  std::vector<std::pair<Key, RowId>> Drain(bool forward) {
    std::vector<std::pair<Key, RowId>> out;
    const unsigned char *k = nullptr;
    uint32_t key_len = 0;
    RowId v = 0;
    while (forward ? m_iter.next(&k, &key_len, &v) : m_iter.prev(&k, &key_len, &v))
      out.emplace_back(Key(reinterpret_cast<const char *>(k), key_len), v);
    return out;
  }

  ART *m_tree;
  ARTIterator<unsigned char, RowId> m_iter;
};

// Rapid's boundary rule: a boundary shorter than the stored key names that
// key's whole prefix equivalence class, so inclusivity covers the entire group.
int BoundaryCompare(const Key &key, const Key &boundary) {
  const size_t common = std::min(key.size(), boundary.size());
  const int c = common ? std::memcmp(key.data(), boundary.data(), common) : 0;
  if (c != 0) return c < 0 ? -1 : 1;
  return key.size() < boundary.size() ? -1 : 0;
}

std::vector<std::pair<Key, RowId>> OracleRange(const Oracle &o, const Key *lo, bool lo_incl, const Key *hi,
                                               bool hi_incl) {
  std::vector<std::pair<Key, RowId>> out;
  for (const auto &[k, values] : o) {
    if (lo != nullptr) {
      const int c = BoundaryCompare(k, *lo);
      if (c < 0 || (c == 0 && !lo_incl)) continue;
    }
    if (hi != nullptr) {
      const int c = BoundaryCompare(k, *hi);
      if (c > 0 || (c == 0 && !hi_incl)) continue;
    }
    for (RowId v : values) out.emplace_back(k, v);
  }
  return out;
}

class RapidArtCursor : public RapidArtShape {};

INSTANTIATE_TEST_SUITE_P(AllShapes, RapidArtCursor,
                         ::testing::Values(Shape::SEQ_BE64, Shape::RANDOM_BE64, Shape::COMPOSITE, Shape::LONG_PREFIX,
                                           Shape::FRAMED_VARLEN, Shape::EMBEDDED_NUL, Shape::WIDE_FANOUT),
                         [](const ::testing::TestParamInfo<Shape> &shape_info) { return ShapeName(shape_info.param); });

TEST_P(RapidArtCursor, WalksEveryRowInBothDirections) {
  LoadShuffled(3);
  LockedCursor cursor(&m_tree);

  const std::vector<std::pair<Key, RowId>> expected = Flatten(m_oracle);
  EXPECT_EQ(expected, cursor.Forward(nullptr, false, nullptr, false));

  std::vector<std::pair<Key, RowId>> reversed(expected.rbegin(), expected.rend());
  EXPECT_EQ(reversed, cursor.Backward(nullptr, false, nullptr, false));
}

TEST_P(RapidArtCursor, HonoursBothBoundsAndTheirInclusivity) {
  LoadShuffled(2);
  LockedCursor cursor(&m_tree);

  std::vector<Key> sorted;
  for (const auto &entry : m_oracle) sorted.push_back(entry.first);
  ASSERT_FALSE(sorted.empty());
  std::uniform_int_distribution<size_t> pick(0, sorted.size() - 1);

  for (int trial = 0; trial < 60; ++trial) {
    size_t a = pick(m_rng), b = pick(m_rng);
    if (a > b) std::swap(a, b);
    Key lo = sorted[a];
    Key hi = sorted[b];
    // Rotate through boundaries that are stored keys, truncated keys naming a
    // prefix class, and keys that are not in the tree at all.
    if (trial % 4 == 1 && lo.size() > 1) lo = lo.substr(0, lo.size() - 1);
    if (trial % 4 == 2 && hi.size() > 1) hi = hi.substr(0, hi.size() - 1);
    if (trial % 8 == 3) hi.back() = static_cast<char>(hi.back() ^ 0x33);

    for (int mask = 0; mask < 4; ++mask) {
      const bool lo_incl = (mask & 1) != 0;
      const bool hi_incl = (mask & 2) != 0;
      std::vector<std::pair<Key, RowId>> expected = OracleRange(m_oracle, &lo, lo_incl, &hi, hi_incl);
      ASSERT_EQ(expected, cursor.Forward(&lo, lo_incl, &hi, hi_incl)) << "trial " << trial << " mask " << mask;
      std::vector<std::pair<Key, RowId>> reversed(expected.rbegin(), expected.rend());
      ASSERT_EQ(reversed, cursor.Backward(&lo, lo_incl, &hi, hi_incl)) << "reverse trial " << trial;
    }
  }
}

TEST_P(RapidArtCursor, HandlesHalfOpenAndEmptyRanges) {
  LoadShuffled(1);
  LockedCursor cursor(&m_tree);

  std::vector<Key> sorted;
  for (const auto &entry : m_oracle) sorted.push_back(entry.first);
  std::uniform_int_distribution<size_t> pick(0, sorted.size() - 1);

  for (int trial = 0; trial < 20; ++trial) {
    const Key bound = sorted[pick(m_rng)];
    for (int mask = 0; mask < 2; ++mask) {
      const bool incl = (mask & 1) != 0;
      EXPECT_EQ(OracleRange(m_oracle, &bound, incl, nullptr, false), cursor.Forward(&bound, incl, nullptr, false));
      EXPECT_EQ(OracleRange(m_oracle, nullptr, false, &bound, incl), cursor.Forward(nullptr, false, &bound, incl));
    }
  }

  const Key above(sorted.back().size() + 1, '\xff');
  const Key below(1, '\0');
  EXPECT_TRUE(cursor.Forward(&above, true, nullptr, false).empty());
  EXPECT_TRUE(cursor.Backward(nullptr, false, &below, false).empty());
}

// ----------------------------------------------------------------- threading

TEST(RapidArtConcurrency, ReadersSeeAConsistentTreeWhileWritersChurn) {
  // Removing the per-leaf mutex left Art_tree::tree_mutex as the only thing
  // between a reader walking a leaf's packed values and a writer reallocating
  // them. This drives both sides through the same locking the engine uses --
  // ART's own methods take the tree lock, cursor callers take it themselves --
  // over a half of the key space writers never touch, so readers have an
  // invariant to assert while the other half is inserted and deleted under them.
  std::mt19937_64 rng(20240910);
  ART tree;
  tree.ART_tree_init();
  const std::vector<Key> keys = MakeKeys(Shape::COMPOSITE, 4000, &rng);
  const size_t stable = keys.size() / 2;
  for (size_t i = 0; i < stable; ++i) {
    RowId v = i + 1;
    tree.ART_insert(Bytes(keys[i]), KeyLen(keys[i]), &v, sizeof(v));
  }

  std::atomic<bool> stop{false};
  std::atomic<size_t> reads{0};
  std::atomic<size_t> writes{0};
  std::atomic<size_t> scans{0};
  std::atomic<size_t> errors{0};
  std::vector<std::thread> threads;

  for (int r = 0; r < 4; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937_64 local(9000 + r);
      LockedCursor cursor(&tree);
      while (!stop.load(std::memory_order_relaxed)) {
        const size_t i = local() % stable;
        void *p = tree.ART_search(Bytes(keys[i]), KeyLen(keys[i]));
        if (p == nullptr) {
          errors.fetch_add(1, std::memory_order_relaxed);
        } else {
          RowId got = 0;
          std::memcpy(&got, p, sizeof(got));
          if (got != RowId(i + 1)) errors.fetch_add(1, std::memory_order_relaxed);
        }
        reads.fetch_add(1, std::memory_order_relaxed);

        // Short and infrequent: this ART serialises writers behind one tree
        // mutex, and a steady stream of readers will otherwise starve them.
        if ((local() & 63) == 0) {
          const size_t a = local() % stable;
          const size_t b = std::min(stable - 1, a + 32);
          const auto rows = cursor.Forward(&keys[a], true, &keys[b], true);
          for (size_t j = 0; j < rows.size(); ++j) {
            if (rows[j].first.empty()) errors.fetch_add(1, std::memory_order_relaxed);
            if (j > 0 && rows[j].first < rows[j - 1].first) errors.fetch_add(1, std::memory_order_relaxed);
          }
          scans.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::yield();
      }
    });
  }

  for (int w = 0; w < 2; ++w) {
    threads.emplace_back([&, w] {
      std::mt19937_64 local(50000 + w);
      while (!stop.load(std::memory_order_relaxed)) {
        const size_t i = stable + (local() % (keys.size() - stable));
        RowId v = RowId(i + 1);
        switch (local() % 4) {
          case 0:
            tree.ART_insert(Bytes(keys[i]), KeyLen(keys[i]), &v, sizeof(v));
            break;
          case 1: {  // duplicates under one key, so leaves spill and move
            RowId extra = v + 1000000 + (local() % 64);
            tree.ART_insert(Bytes(keys[i]), KeyLen(keys[i]), &extra, sizeof(extra));
            break;
          }
          case 2:
            tree.ART_delete_value(Bytes(keys[i]), KeyLen(keys[i]), &v, sizeof(v));
            break;
          default:
            tree.ART_delete(Bytes(keys[i]), KeyLen(keys[i]));
            break;
        }
        writes.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(2));
  stop.store(true);
  for (auto &t : threads) t.join();

  EXPECT_EQ(0u, errors.load()) << "reads=" << reads.load() << " writes=" << writes.load();
  EXPECT_GT(reads.load(), 1000u);
  EXPECT_GT(scans.load(), 10u);
  // A run that barely wrote proves nothing about the interleaving. The bar is
  // low on purpose: a sanitizer build runs this several times slower.
  EXPECT_GT(writes.load(), 100u) << "writers were starved";

  // The untouched half must be intact.
  for (size_t i = 0; i < stable; ++i) {
    RowId got = 0;
    ASSERT_TRUE(SearchLast(&tree, keys[i], &got)) << "stable key " << i << " lost";
    ASSERT_EQ(RowId(i + 1), got);
  }
  tree.ART_tree_destroy();
}

// ---------------------------------------------------------- insert contract

Key BeKey(uint64_t v) {
  Key k(8, '\0');
  for (int i = 0; i < 8; ++i) k[i] = static_cast<char>((v >> (56 - 8 * i)) & 0xff);
  return k;
}

uint64_t KeyAsU64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

TEST(RapidArtInsert, ReportsInsertedDuplicateAndRejected) {
  ART tree;
  tree.ART_tree_init();
  const Key k = BeKey(1);
  RowId v1 = 1, v2 = 2;
  EXPECT_EQ(ART::InsertResult::INSERTED, tree.ART_try_insert(Bytes(k), KeyLen(k), &v1, sizeof(v1)));
  EXPECT_EQ(ART::InsertResult::DUPLICATE, tree.ART_try_insert(Bytes(k), KeyLen(k), &v1, sizeof(v1)));
  EXPECT_EQ(ART::InsertResult::INSERTED, tree.ART_try_insert(Bytes(k), KeyLen(k), &v2, sizeof(v2)));
  EXPECT_EQ(2u, tree.tree()->size.load());

  EXPECT_NE(nullptr, tree.ART_delete_value(Bytes(k), KeyLen(k), &v1, sizeof(v1)));
  EXPECT_EQ(1u, tree.tree()->size.load());
  EXPECT_NE(nullptr, tree.ART_delete_value(Bytes(k), KeyLen(k), &v2, sizeof(v2)));
  EXPECT_EQ(0u, tree.tree()->size.load());

  // The tree is reusable once it drains.
  EXPECT_EQ(ART::InsertResult::INSERTED, tree.ART_try_insert(Bytes(k), KeyLen(k), &v1, sizeof(v1)));
  EXPECT_EQ(ART::InsertResult::INSERTED, tree.ART_try_insert(Bytes(k), KeyLen(k), &v2, sizeof(v2)));
  EXPECT_NE(nullptr, tree.ART_delete(Bytes(k), KeyLen(k)));
  EXPECT_EQ(0u, tree.tree()->size.load());
}

TEST(RapidArtInsert, RefusesKeysThatAreNotPrefixFree) {
  ART tree;
  tree.ART_tree_init();
  const unsigned char k1[3] = {1, 2, 3};
  const unsigned char k2[2] = {1, 2};
  const unsigned char k3[4] = {1, 2, 3, 4};
  RowId v = 7;
  EXPECT_EQ(ART::InsertResult::INSERTED, tree.ART_try_insert(k1, 3, &v, sizeof(v)));
  EXPECT_EQ(ART::InsertResult::REJECTED, tree.ART_try_insert(k2, 2, &v, sizeof(v)));  // prefix of k1
  EXPECT_EQ(ART::InsertResult::REJECTED, tree.ART_try_insert(k3, 4, &v, sizeof(v)));  // extends k1
  EXPECT_EQ(1u, tree.tree()->size.load());
  EXPECT_NE(nullptr, tree.ART_search(k1, 3));

  // A different value width would shift every sibling, so it is refused.
  uint32_t narrow = 1;
  EXPECT_EQ(ART::InsertResult::REJECTED, tree.ART_try_insert(k1, 3, &narrow, sizeof(narrow)));
  EXPECT_EQ(1u, tree.tree()->size.load());
  std::vector<unsigned char> big(70000);
  EXPECT_EQ(ART::InsertResult::REJECTED, tree.ART_try_insert(k1, 3, big.data(), static_cast<uint32_t>(big.size())));
  EXPECT_EQ(1u, tree.tree()->size.load());

  // The prefix check also sees bytes inside a compressed inner-node path.
  ART deep;
  deep.ART_tree_init();
  Key a1(20, '\x05'), a2(20, '\x05');
  a2[19] = '\x06';
  EXPECT_EQ(ART::InsertResult::INSERTED, deep.ART_try_insert(Bytes(a1), KeyLen(a1), &v, sizeof(v)));
  EXPECT_EQ(ART::InsertResult::INSERTED, deep.ART_try_insert(Bytes(a2), KeyLen(a2), &v, sizeof(v)));
  EXPECT_EQ(ART::InsertResult::REJECTED, deep.ART_try_insert(Bytes(a1), 10, &v, sizeof(v)));
}

TEST(RapidArtIndex, InsertReportsRefusalAndMoveReplays) {
  Index<unsigned char, RowId> idx;
  Key key = BeKey(1);
  Key prefix(key.data(), key.size() - 1);
  RowId v = 5, out = 0;
  EXPECT_EQ(0, idx.insert(reinterpret_cast<unsigned char *>(key.data()), key.size(), &v, sizeof(v)));
  EXPECT_EQ(1, idx.insert(reinterpret_cast<unsigned char *>(prefix.data()), prefix.size(), &v, sizeof(v)));
  EXPECT_EQ(0, idx.insert(reinterpret_cast<unsigned char *>(key.data()), key.size(), &v, sizeof(v)));

  Key to = BeKey(2);
  EXPECT_EQ(0, idx.move(reinterpret_cast<unsigned char *>(key.data()), key.size(),
                        reinterpret_cast<unsigned char *>(to.data()), to.size(), &v, sizeof(v)));
  EXPECT_FALSE(idx.lookup(reinterpret_cast<unsigned char *>(key.data()), key.size(), &out));
  ASSERT_TRUE(idx.lookup(reinterpret_cast<unsigned char *>(to.data()), to.size(), &out));
  EXPECT_EQ(v, out);

  // Replaying the move must not add a second entry.
  EXPECT_EQ(0, idx.move(reinterpret_cast<unsigned char *>(key.data()), key.size(),
                        reinterpret_cast<unsigned char *>(to.data()), to.size(), &v, sizeof(v)));
  ASSERT_TRUE(idx.lookup(reinterpret_cast<unsigned char *>(to.data()), to.size(), &out));
  EXPECT_EQ(v, out);
  EXPECT_EQ(1u, idx.impl()->tree()->size.load());
}

TEST(RapidArtLeaf, WideFanoutNodeCountsEveryChild) {
  ART tree;
  tree.ART_tree_init();
  for (int i = 0; i < 256; ++i) {
    unsigned char k[2] = {0, static_cast<unsigned char>(i)};
    RowId v = static_cast<RowId>(i);
    ASSERT_EQ(ART::InsertResult::INSERTED, tree.ART_try_insert(k, 2, &v, sizeof(v)));
  }
  ART::Art_inner_node *root = static_cast<ART::Art_inner_node *>(tree.root());
  ASSERT_NE(nullptr, root);
  EXPECT_EQ(ART::NODE256, root->type());
  EXPECT_EQ(256u, root->num_children);

  for (int i = 0; i < 256; ++i) {
    unsigned char k[2] = {0, static_cast<unsigned char>(i)};
    EXPECT_NE(nullptr, tree.ART_delete(k, 2));
  }
  EXPECT_EQ(nullptr, tree.root());
  EXPECT_EQ(0u, tree.tree()->size.load());
}

// ------------------------------------------------------------ cursor robustness

TEST(RapidArtCursor, PositionSurvivesNodeGrowthAndNeighbourDelete) {
  // Both operations replace a node on the cursor's cached path.
  for (bool reverse : {false, true}) {
    ART tree;
    tree.ART_tree_init();
    for (uint64_t i = 1; i <= 4; ++i) {
      const Key k = BeKey(i);
      RowId v = i;
      ASSERT_EQ(ART::InsertResult::INSERTED, tree.ART_try_insert(Bytes(k), KeyLen(k), &v, sizeof(v)));
    }

    ARTIterator<unsigned char, RowId> it(&tree);
    const unsigned char *kp = nullptr;
    uint32_t kl = 0;
    RowId out = 0;
    {
      std::shared_lock<std::shared_mutex> guard(tree.tree()->tree_mutex);
      if (reverse) {
        it.init_reverse_scan(nullptr, 0, false, nullptr, 0, false);
        ASSERT_TRUE(it.prev(&kp, &kl, &out));
        EXPECT_EQ(RowId{4}, out);
      } else {
        it.init_scan(nullptr, 0, false, nullptr, 0, false);
        ASSERT_TRUE(it.next(&kp, &kl, &out));
        EXPECT_EQ(RowId{1}, out);
      }
    }

    // A fifth child grows the node the cursor sits in.
    const Key five = BeKey(5);
    RowId v5 = 5;
    ASSERT_EQ(ART::InsertResult::INSERTED, tree.ART_try_insert(Bytes(five), KeyLen(five), &v5, sizeof(v5)));
    // ... and deleting its neighbour shrinks it again.
    const Key victim = BeKey(reverse ? 3 : 2);
    ASSERT_NE(nullptr, tree.ART_delete(Bytes(victim), KeyLen(victim)));

    bool got = false;
    {
      std::shared_lock<std::shared_mutex> guard(tree.tree()->tree_mutex);
      got = reverse ? it.prev(&kp, &kl, &out) : it.next(&kp, &kl, &out);
    }
    ASSERT_TRUE(got) << (reverse ? "reverse" : "forward");
    EXPECT_EQ(reverse ? RowId{2} : RowId{3}, out);
  }
}

TEST(RapidArtCursor, SurvivesTheValueItEmittedBeingDeleted) {
  ART tree;
  tree.ART_tree_init();
  const Key k7 = BeKey(7);
  for (RowId v = 10; v < 14; ++v)
    ASSERT_EQ(ART::InsertResult::INSERTED, tree.ART_try_insert(Bytes(k7), KeyLen(k7), &v, sizeof(v)));
  const Key k8 = BeKey(8);
  RowId z = 99;
  ASSERT_EQ(ART::InsertResult::INSERTED, tree.ART_try_insert(Bytes(k8), KeyLen(k8), &z, sizeof(z)));

  ARTIterator<unsigned char, RowId> it(&tree);
  const unsigned char *kp = nullptr;
  uint32_t kl = 0;
  RowId out = 0;
  {
    std::shared_lock<std::shared_mutex> guard(tree.tree()->tree_mutex);
    it.init_scan(nullptr, 0, false, nullptr, 0, false);
    ASSERT_TRUE(it.next(&kp, &kl, &out));
    EXPECT_EQ(RowId{10}, out);
    ASSERT_TRUE(it.next(&kp, &kl, &out));
    EXPECT_EQ(RowId{11}, out);
  }

  // Delete the value just emitted; the leaf shrinks but must not strand the cursor.
  RowId victim = 11;
  ASSERT_NE(nullptr, tree.ART_delete_value(Bytes(k7), KeyLen(k7), &victim, sizeof(victim)));

  {
    std::shared_lock<std::shared_mutex> guard(tree.tree()->tree_mutex);
    ASSERT_TRUE(it.next(&kp, &kl, &out));
    EXPECT_EQ(RowId{12}, out);
    ASSERT_TRUE(it.next(&kp, &kl, &out));
    EXPECT_EQ(RowId{13}, out);
    ASSERT_TRUE(it.next(&kp, &kl, &out));
    EXPECT_EQ(RowId{99}, out);
    EXPECT_FALSE(it.next(&kp, &kl, &out));
  }
}

// ------------------------------------------------------------- random oracle

TEST(RapidArtModel, MatchesStdMapUnderRandomMutations) {
  ART tree;
  tree.ART_tree_init();
  std::map<uint64_t, std::set<RowId>> oracle;
  std::mt19937_64 rng(1);
  size_t entries = 0;

  for (int i = 0; i < 200000; ++i) {
    const uint64_t key = rng() % 3000;
    const RowId val = rng() % 4;
    const Key k = BeKey(key);
    switch (rng() % 3) {
      case 0:
      case 1: {
        const auto r = tree.ART_try_insert(Bytes(k), KeyLen(k), &val, sizeof(val));
        const bool had = oracle[key].count(val) != 0;
        EXPECT_EQ(had, r == ART::InsertResult::DUPLICATE);
        if (!had) {
          oracle[key].insert(val);
          ++entries;
        }
        break;
      }
      default:
        if (rng() & 1) {
          const bool had = oracle[key].erase(val) != 0;
          const bool removed = tree.ART_delete_value(Bytes(k), KeyLen(k), &val, sizeof(val)) != nullptr;
          EXPECT_EQ(had, removed);
          if (had) --entries;
        } else {
          entries -= oracle[key].size();
          oracle[key].clear();
          tree.ART_delete(Bytes(k), KeyLen(k));
        }
        break;
    }
  }
  EXPECT_EQ(entries, tree.tree()->size.load());

  std::map<uint64_t, std::set<RowId>> expected;
  for (const auto &[k, values] : oracle)
    if (!values.empty()) expected[k] = values;

  for (bool forward : {true, false}) {
    ARTIterator<unsigned char, RowId> it(&tree);
    std::shared_lock<std::shared_mutex> guard(tree.tree()->tree_mutex);
    if (forward)
      it.init_scan(nullptr, 0, false, nullptr, 0, false);
    else
      it.init_reverse_scan(nullptr, 0, false, nullptr, 0, false);

    std::vector<uint64_t> keys;
    std::map<uint64_t, std::set<RowId>> seen;
    size_t n = 0;
    const unsigned char *kp = nullptr;
    uint32_t kl = 0;
    RowId out = 0;
    while (forward ? it.next(&kp, &kl, &out) : it.prev(&kp, &kl, &out)) {
      ASSERT_EQ(8u, kl);
      const uint64_t key = KeyAsU64(kp);
      if (keys.empty() || keys.back() != key) keys.push_back(key);
      ASSERT_TRUE(seen[key].insert(out).second) << "value emitted twice";
      ++n;
    }
    const bool sorted = forward ? std::is_sorted(keys.begin(), keys.end())
                                : std::is_sorted(keys.rbegin(), keys.rend());
    EXPECT_TRUE(sorted) << (forward ? "forward" : "reverse");
    EXPECT_EQ(expected, seen);
    EXPECT_EQ(entries, n);
  }
}

// ------------------------------------------------------------------ encoder

TEST(RapidArtEncoder, OrderAndRoundTripAgreeWithMemcmp) {
  std::mt19937_64 rng(7);
  unsigned char a[8], b[8];
  for (int i = 0; i < 100000; ++i) {
    const int64_t x = static_cast<int64_t>(rng() >> (rng() % 60));
    const int64_t y = static_cast<int64_t>(rng() >> (rng() % 60));
    Encoder<int64_t>::Encode(x, a);
    Encoder<int64_t>::Encode(y, b);
    EXPECT_EQ(x < y, std::memcmp(a, b, 8) < 0);
    EXPECT_EQ(x, Encoder<int64_t>::Decode(a));

    const uint32_t u = static_cast<uint32_t>(rng()), w = static_cast<uint32_t>(rng());
    Encoder<uint32_t>::Encode(u, a);
    Encoder<uint32_t>::Encode(w, b);
    EXPECT_EQ(u < w, std::memcmp(a, b, 4) < 0);
    EXPECT_EQ(u, Encoder<uint32_t>::Decode(a));

    const int16_t s = static_cast<int16_t>(rng()), s2 = static_cast<int16_t>(rng());
    Encoder<int16_t>::Encode(s, a);
    Encoder<int16_t>::Encode(s2, b);
    EXPECT_EQ(s < s2, std::memcmp(a, b, 2) < 0);
    EXPECT_EQ(s, Encoder<int16_t>::Decode(a));

    const int8_t c = static_cast<int8_t>(rng()), c2 = static_cast<int8_t>(rng());
    Encoder<int8_t>::Encode(c, a);
    Encoder<int8_t>::Encode(c2, b);
    EXPECT_EQ(c < c2, std::memcmp(a, b, 1) < 0);
    EXPECT_EQ(c, Encoder<int8_t>::Decode(a));

    const double d = std::ldexp(static_cast<double>(static_cast<int64_t>(rng())), static_cast<int>(rng() % 80) - 40);
    const double d2 = std::ldexp(static_cast<double>(static_cast<int64_t>(rng())), static_cast<int>(rng() % 80) - 40);
    Encoder<double>::Encode(d, a);
    Encoder<double>::Encode(d2, b);
    EXPECT_EQ(d < d2, std::memcmp(a, b, 8) < 0);
    EXPECT_EQ(d, Encoder<double>::Decode(a));

    const float f = static_cast<float>(d), f2 = static_cast<float>(d2);
    Encoder<float>::Encode(f, a);
    Encoder<float>::Encode(f2, b);
    EXPECT_EQ(f < f2, std::memcmp(a, b, 4) < 0);
    EXPECT_EQ(f, Encoder<float>::Decode(a));
  }
}

TEST(RapidArtEncoder, KnownVectors) {
  unsigned char a[8] = {};
  Encoder<int32_t>::Encode(-1, a);
  EXPECT_EQ(0x7f, static_cast<int>(a[0]));
  EXPECT_EQ(0xff, static_cast<int>(a[1]));
  EXPECT_EQ(0xff, static_cast<int>(a[3]));
  Encoder<int32_t>::Encode(0, a);
  EXPECT_EQ(0x80, static_cast<int>(a[0]));
  EXPECT_EQ(0x00, static_cast<int>(a[3]));
  Encoder<uint16_t>::Encode(0x1234, a);
  EXPECT_EQ(0x12, static_cast<int>(a[0]));
  EXPECT_EQ(0x34, static_cast<int>(a[1]));

  // +0 and -0 must share one key; -inf < max finite < +inf < NaN.
  unsigned char za[8], zb[8];
  Encoder<double>::Encode(0.0, za);
  Encoder<double>::Encode(-0.0, zb);
  EXPECT_EQ(0, std::memcmp(za, zb, 8));

  unsigned char nia[8], ma[8], ia[8], na[8];
  Encoder<double>::Encode(-INFINITY, nia);
  Encoder<double>::Encode(1.7e308, ma);
  Encoder<double>::Encode(INFINITY, ia);
  Encoder<double>::Encode(NAN, na);
  EXPECT_LT(std::memcmp(nia, ma, 8), 0);
  EXPECT_LT(std::memcmp(ma, ia, 8), 0);
  EXPECT_LT(std::memcmp(ia, na, 8), 0);
  EXPECT_TRUE(std::isnan(Encoder<double>::Decode(na)));

  // Any integral type links, not just the fixed-width aliases.
  Encoder<long long>::Encode(5, a);
  EXPECT_EQ(0x05, static_cast<int>(a[7]));
  Encoder<unsigned long>::Encode(5, a);
  EXPECT_EQ(0x05, static_cast<int>(a[7]));
}

}  // namespace shannon_rapid_art_unittest
