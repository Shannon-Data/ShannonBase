// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>
#include "scope_guard.h"
#include "sql/iterators/timing_iterator.h"
#include "sql/item_cmpfunc.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/partition_info.h"
#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/executor/iterators/nested_loop_iterator.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"
#include "storage/rapid_engine/utils/memory_pool.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/include/rapid_table_info.h"
#include "unittest/gunit/fake_table.h"
#include "unittest/gunit/test_utils.h"
#include "unittest/gunit/temptable/mock_field_varstring.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"

namespace {
using ShannonBase::Executor::ColumnChunk;
using ShannonBase::Executor::VectorizedNestedLoopIterator;
using NestedLoopBatchEstimator = ShannonBase::Optimizer::RpdCostEstimator::NestedLoopBatchEstimator;

auto TransportCost(double rows, double strings = 0, double width = 16, double budget = 256 * 1024) {
  return NestedLoopBatchEstimator::estimate(rows, 2, strings, width, 32, budget, .003, .0005, .001, .01, .00001);
}
TEST(RapidNestedLoopCost, SelectionAmortizesSetupRatherThanUsingABoolean) {
  EXPECT_FALSE(TransportCost(1).choose_batches());
  EXPECT_TRUE(TransportCost(1000).choose_batches());
  EXPECT_FALSE(TransportCost(-1).valid);
  EXPECT_FALSE(TransportCost(1000, 0, 1024).valid);
  EXPECT_FALSE(TransportCost(1000, 0, 16, 64).valid);
}
TEST(RapidNestedLoopCost, DictionaryAndProjectionWorkAffectTheComparison) {
  auto numeric = TransportCost(1000);
  auto strings = TransportCost(1000, 2);
  ASSERT_TRUE(numeric.valid);
  ASSERT_TRUE(strings.valid);
  EXPECT_GT(strings.batches, numeric.batches);
  EXPECT_EQ(strings.scalar, numeric.scalar);
  EXPECT_GT(TransportCost(1000, 0, 32).retained_bytes, numeric.retained_bytes);
}

TEST(RapidBatchPolicy, HardwareAlignmentNeverExceedsMemoryAndWidthControlsRows) {
  using ShannonBase::Executor::BatchHardware;
  using ShannonBase::Executor::SelectBatchRows;
  const BatchHardware hardware{64, 512 * 1024, 32};
  const size_t narrow = SelectBatchRows(8, 1024 * 1024, 2, hardware);
  const size_t wide = SelectBatchRows(128, 1024 * 1024, 2, hardware);
  EXPECT_GT(narrow, wide);
  EXPECT_EQ(0U, narrow % 64);
  EXPECT_EQ(0U, wide % 64);
  EXPECT_EQ(0U, SelectBatchRows(8, 7, 1, hardware));
  EXPECT_EQ(3U, SelectBatchRows(8, 24, 1, hardware));
  EXPECT_EQ(0U, SelectBatchRows(SIZE_MAX, SIZE_MAX, 2, hardware));
  for (size_t budget : {size_t{1}, size_t{31}, size_t{1000}, size_t{1048576}}) {
    const size_t rows = SelectBatchRows(17, budget, 3, hardware);
    EXPECT_LE(rows * 17 * 3, budget);
  }
}
TEST(RapidBatchPolicy, CacheHintsAndLiveBuffersChangeTheWorkingSet) {
  using ShannonBase::Executor::BatchHardware;
  using ShannonBase::Executor::SelectBatchRows;
  const BatchHardware small{64, 128 * 1024, 16}, large{64, 1024 * 1024, 32};
  EXPECT_LT(SelectBatchRows(32, 8 * 1024 * 1024, 1, small),
            SelectBatchRows(32, 8 * 1024 * 1024, 1, large));
  EXPECT_LT(SelectBatchRows(32, 8 * 1024 * 1024, 4, large),
            SelectBatchRows(32, 8 * 1024 * 1024, 1, large));
}

class DataRows final : public RowIterator {
 public:
  DataRows(THD *thd, TABLE *table, Field *outer = nullptr, size_t *unlocks = nullptr)
      : RowIterator(thd), table(table), outer(outer), unlocks(unlocks) {}
  bool Init() override {
    index = 0;
    table->reset_null_row();
    return false;
  }
  int Read() override {
    if (!outer) {
      const longlong values[] = {1, 2, 9};
      if (index == 3) return -1;
      table->field[0]->store(values[index++], false);
    } else {
      const longlong key = outer->val_int();
      const size_t count = key == 1 ? 3 : key == 2 ? 1 : 0;
      if (index == count) return -1;
      table->field[0]->store(key * 10 + index++, false);
    }
    return 0;
  }
  void SetNullRowFlag(bool value) override { value ? table->set_null_row() : table->reset_null_row(); }
  void UnlockRow() override {
    if (unlocks) ++*unlocks;
  }

 private:
  TABLE *table;
  Field *outer;
  size_t index{0};
  size_t *unlocks;
};

class RapidNestedLoop : public ::testing::Test {
 protected:
  void SetUp() override {
    server.SetUp();
    left = std::make_unique<Fake_TABLE>(1, false);
    right = std::make_unique<Fake_TABLE>(1, false);
    right->set_nullable();
    bitmap_set_all(left->read_set);
    bitmap_set_all(left->write_set);
    bitmap_set_all(right->read_set);
    bitmap_set_all(right->write_set);
    ShannonBase::Utils::MemoryPool::Config config(128 * 1024 * 1024);
    config.small_pool_ratio = 0;
    config.auto_defragmentation = false;
    pool = std::make_shared<ShannonBase::Utils::MemoryPool>(config);
    ASSERT_TRUE(ShannonBase::ResMgmt::initialize_system_pool(pool));
  }
  void TearDown() override {
    ShannonBase::ResMgmt::shutdown_system_pool();
    pool.reset();
    right.reset();
    left.reset();
    server.TearDown();
  }
  std::unique_ptr<VectorizedNestedLoopIterator> Make(JoinType type) {
    Prealloced_array<TABLE *, 4> tables(PSI_NOT_INSTRUMENTED);
    tables.push_back(left.get());
    tables.push_back(right.get());
    return std::make_unique<VectorizedNestedLoopIterator>(
        server.thd(), NewIterator<DataRows>(server.thd(), server.thd()->mem_root, left.get()),
        NewIterator<DataRows>(server.thd(), server.thd()->mem_root, right.get(), left->field[0]), type, false, tables,
        256 * 1024, 4);
  }
  std::pmr::vector<ColumnChunk> Chunks(size_t capacity) {
    std::pmr::vector<ColumnChunk> result;
    result.emplace_back(left->field[0], capacity);
    result.emplace_back(right->field[0], capacity);
    return result;
  }
  static int32 Value(const ColumnChunk &chunk, size_t row) {
    int32 value;
    std::memcpy(&value, chunk.data_fast(row), sizeof(value));
    return value;
  }
  my_testing::Server_initializer server;
  std::unique_ptr<Fake_TABLE> left, right;
  std::shared_ptr<ShannonBase::Utils::MemoryPool> pool;
};

// A correlated batch source whose scalar entry point is forbidden. This catches
// regressions back to calling MySQL Read() once per join output row.
struct BatchPfsState {
  size_t starts{0}, ends{0}, init_calls{0};
  bool active{false}, fail_reads{false}, supports_batches{true};
};
class BatchDataRows final : public RowIterator, public ShannonBase::Executor::BatchReadable {
 public:
  BatchDataRows(THD *thd, TABLE *table, size_t *calls, Field *outer = nullptr, BatchPfsState *pfs = nullptr)
      : RowIterator(thd), m_table(table), m_calls(calls), m_outer(outer), m_pfs(pfs) {}
  bool Init() override {
    if (m_pfs) ++m_pfs->init_calls;
    m_pos = 0;
    m_key = m_outer ? m_outer->val_int() : 0;
    return false;
  }
  int Read() override {
    ++*m_calls;
    return 1;
  }
  void SetNullRowFlag(bool value) override { value ? m_table->set_null_row() : m_table->reset_null_row(); }
  void UnlockRow() override {}
  void StartPSIBatchMode() override {
    if (!m_pfs) return;
    EXPECT_FALSE(m_pfs->active);
    m_pfs->active = true;
    ++m_pfs->starts;
  }
  void EndPSIBatchModeIfStarted() override {
    if (!m_pfs || !m_pfs->active) return;
    m_pfs->active = false;
    ++m_pfs->ends;
  }
  bool PreservesBatchNullRows() const override { return true; }
  bool SupportsBatchRead() const override { return !m_pfs || m_pfs->supports_batches; }
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &, size_t, size_t) override { return true; }
  int ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) override {
    rows = 0;
    if (m_pfs && m_pfs->fail_reads) return 1;
    const size_t count = !m_outer ? 3 : m_key == 1 ? 1300 : m_key == 2 ? 1 : 0;
    while (rows < capacity && m_pos < count) {
      const int32 value = !m_outer ? (m_pos == 2 ? 9 : m_pos + 1) : m_key * 10000 + m_pos;
      if (!chunks[0].add(reinterpret_cast<const uchar *>(&value), sizeof(value), false)) return 1;
      ++m_pos;
      ++rows;
    }
    return m_pos == count ? HA_ERR_END_OF_FILE : 0;
  }

 private:
  TABLE *m_table;
  size_t *m_calls;
  Field *m_outer;
  size_t m_pos{0};
  longlong m_key{0};
  BatchPfsState *m_pfs;
};

TEST_F(RapidNestedLoop, BatchReinitAndErrorsEndTheActiveInnerPfsBatch) {
  for (bool fail : {false, true}) {
    SCOPED_TRACE(fail);
    size_t row_calls = 0;
    BatchPfsState pfs;
    Prealloced_array<TABLE *, 4> tables(PSI_NOT_INSTRUMENTED), outer(PSI_NOT_INSTRUMENTED), inner(PSI_NOT_INSTRUMENTED);
    tables.push_back(left.get()); tables.push_back(right.get());
    outer.push_back(left.get()); inner.push_back(right.get());
    ShannonBase::Executor::detail::BatchNestedLoop loop(
        server.thd(), NewIterator<BatchDataRows>(server.thd(), server.thd()->mem_root, left.get(), &row_calls),
        NewIterator<BatchDataRows>(server.thd(), server.thd()->mem_root, right.get(), &row_calls,
                                   left->field[0], &pfs),
        JoinType::INNER, true, tables, outer, inner, 256 * 1024);
    ASSERT_FALSE(loop.Init());
    auto chunks = Chunks(1);
    size_t rows = 0;
    ASSERT_EQ(0, loop.ReadBatch(chunks, 1, rows));
    ASSERT_EQ(1U, rows);
    EXPECT_TRUE(pfs.active);
    EXPECT_EQ(1U, pfs.starts);
    // Reinitialization after an early LIMIT must end the previous probe batch.
    ASSERT_FALSE(loop.Init());
    EXPECT_FALSE(pfs.active);
    EXPECT_EQ(1U, pfs.ends);
    for (auto &chunk : chunks) chunk.clear();
    pfs.fail_reads = fail;
    EXPECT_EQ(fail ? 1 : 0, loop.ReadBatch(chunks, 1, rows));
    EXPECT_EQ(2U, pfs.starts);
    if (fail) {
      EXPECT_FALSE(pfs.active);
      EXPECT_EQ(2U, pfs.ends);
    }
    loop.EndPSIBatchModeIfStarted();
    EXPECT_FALSE(pfs.active);
    EXPECT_EQ(2U, pfs.ends);
    EXPECT_EQ(0U, row_calls);
  }
}

TEST_F(RapidNestedLoop, NativeFallbackPreservesOuterJoinUnlockState) {
  size_t unlocks = 0;
  Prealloced_array<TABLE *, 4> tables(PSI_NOT_INSTRUMENTED), outer(PSI_NOT_INSTRUMENTED), inner(PSI_NOT_INSTRUMENTED);
  tables.push_back(left.get()); tables.push_back(right.get());
  outer.push_back(left.get()); inner.push_back(right.get());
  ShannonBase::Executor::detail::BatchNestedLoop loop(
      server.thd(), NewIterator<DataRows>(server.thd(), server.thd()->mem_root, left.get()),
      NewIterator<DataRows>(server.thd(), server.thd()->mem_root, right.get(), left->field[0], &unlocks),
      JoinType::OUTER, false, tables, outer, inner, 256 * 1024);
  ASSERT_FALSE(loop.Init());
  ASSERT_FALSE(loop.SupportsBatchRead());
  for (size_t row = 0; row < 4; ++row) {
    ASSERT_EQ(0, loop.Read());
    loop.UnlockRow();
  }
  ASSERT_EQ(4U, unlocks);
  ASSERT_EQ(0, loop.Read());
  EXPECT_EQ(9, left->field[0]->val_int());
  EXPECT_TRUE(right->has_null_row());
  loop.UnlockRow();
  EXPECT_EQ(4U, unlocks);
  EXPECT_EQ(-1, loop.Read());
  loop.UnlockRow();
  EXPECT_EQ(4U, unlocks);
}

TEST_F(RapidNestedLoop, RuntimeFallbackDoesNotInitializeTheOuterInputTwice) {
  size_t row_calls = 0;
  BatchPfsState state;
  state.supports_batches = false;
  Prealloced_array<TABLE *, 4> tables(PSI_NOT_INSTRUMENTED), outer(PSI_NOT_INSTRUMENTED), inner(PSI_NOT_INSTRUMENTED);
  tables.push_back(left.get()); tables.push_back(right.get());
  outer.push_back(left.get()); inner.push_back(right.get());
  ShannonBase::Executor::detail::BatchNestedLoop loop(
      server.thd(), NewIterator<BatchDataRows>(server.thd(), server.thd()->mem_root, left.get(), &row_calls,
                                              nullptr, &state),
      NewIterator<BatchDataRows>(server.thd(), server.thd()->mem_root, right.get(), &row_calls, left->field[0]),
      JoinType::INNER, false, tables, outer, inner, 256 * 1024);
  for (size_t execution = 1; execution <= 2; ++execution) {
    ASSERT_FALSE(loop.Init());
    EXPECT_FALSE(loop.SupportsBatchRead());
    EXPECT_EQ(execution, state.init_calls);
  }
  EXPECT_EQ(0U, row_calls);
}

TEST_F(RapidNestedLoop, BatchKernelNeverCallsScalarChildrenAndPreservesJoinTypes) {
  for (JoinType type : {JoinType::INNER, JoinType::OUTER, JoinType::SEMI, JoinType::ANTI}) {
    SCOPED_TRACE(static_cast<int>(type));
    size_t calls = 0;
    Prealloced_array<TABLE *, 4> tables(PSI_NOT_INSTRUMENTED), outer(PSI_NOT_INSTRUMENTED), inner(PSI_NOT_INSTRUMENTED);
    outer.push_back(left.get());
    inner.push_back(right.get());
    tables.push_back(left.get());
    tables.push_back(right.get());
    VectorizedNestedLoopIterator loop(
        server.thd(), NewIterator<BatchDataRows>(server.thd(), server.thd()->mem_root, left.get(), &calls),
        NewIterator<BatchDataRows>(server.thd(), server.thd()->mem_root, right.get(), &calls, left->field[0]), type,
        false, tables, outer, inner, 256 * 1024, 1302);
    for (int execution = 0; execution < 2; ++execution) {
      ASSERT_FALSE(loop.Init());
      auto chunks = Chunks(31);
      size_t total = 0;
      for (;;) {
        for (auto &chunk : chunks) chunk.clear();
        size_t rows = 0;
        const int result = loop.ReadBatch(chunks, 31, rows);
        ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
        for (size_t row = 0; row < rows; ++row) {
          const size_t ordinal = total + row;
          if (type == JoinType::ANTI || (type == JoinType::OUTER && ordinal == 1301)) {
            EXPECT_EQ(9, Value(chunks[0], row));
            EXPECT_TRUE(chunks[1].nullable_fast(row));
          } else if (type == JoinType::SEMI) {
            EXPECT_EQ(ordinal + 1, Value(chunks[0], row));
          } else {
            EXPECT_EQ(ordinal < 1300 ? 1 : 2, Value(chunks[0], row));
            EXPECT_EQ(ordinal < 1300 ? 10000 + ordinal : 20000, Value(chunks[1], row));
          }
        }
        total += rows;
        // Every consumer is allowed to overwrite the parameter buffers.
        left->field[0]->store(777, false);
        right->field[0]->store(888, false);
        if (result == HA_ERR_END_OF_FILE) break;
      }
      EXPECT_EQ(type == JoinType::INNER   ? 1301U
                : type == JoinType::OUTER ? 1302U
                : type == JoinType::SEMI  ? 2U
                                          : 1U,
                total);
    }
    EXPECT_EQ(0U, calls);
  }
}
TEST_F(RapidNestedLoop, BatchKernelRebindsReorderedAndProjectedOutputAcrossCalls) {
  size_t calls = 0;
  Prealloced_array<TABLE *, 4> tables(PSI_NOT_INSTRUMENTED), outer(PSI_NOT_INSTRUMENTED), inner(PSI_NOT_INSTRUMENTED);
  tables.push_back(left.get()); tables.push_back(right.get());
  outer.push_back(left.get()); inner.push_back(right.get());
  ShannonBase::Executor::detail::BatchNestedLoop loop(
      server.thd(), NewIterator<BatchDataRows>(server.thd(), server.thd()->mem_root, left.get(), &calls),
      NewIterator<BatchDataRows>(server.thd(), server.thd()->mem_root, right.get(), &calls, left->field[0]),
      JoinType::OUTER, false, tables, outer, inner, 256 * 1024);
  for (size_t execution = 0; execution < 2; ++execution) {
    ASSERT_FALSE(loop.Init());
    ASSERT_TRUE(loop.SupportsBatchRead());
    size_t total = 0;
    for (size_t batch = 0;; ++batch) {
      // Both fields have field_index() == 0. Output bindings must use Field
      // identity and must not retain the previous consumer's column order.
      std::pmr::vector<ColumnChunk> chunks;
      chunks.emplace_back(right->field[0], 257);
      if (batch % 2 == 0) {
        chunks.emplace_back(nullptr, 0);
        chunks.emplace_back(left->field[0], 257);
      }
      size_t rows = 0;
      const int result = loop.ReadBatch(chunks, 257, rows);
      ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
      for (size_t row = 0; row < rows; ++row) {
        const size_t ordinal = total + row;
        ASSERT_LT(ordinal, 1302U);
        if (ordinal == 1301) {
          EXPECT_TRUE(chunks[0].nullable_fast(row));
          EXPECT_TRUE(loop.BatchNullRow(right.get(), row));
        } else {
          EXPECT_FALSE(chunks[0].nullable_fast(row));
          EXPECT_EQ(ordinal < 1300 ? 10000 + ordinal : 20000, Value(chunks[0], row));
        }
        if (chunks.size() == 3) {
          EXPECT_EQ(ordinal < 1300 ? 1 : ordinal == 1300 ? 2 : 9, Value(chunks[2], row));
        }
      }
      total += rows;
      if (result == HA_ERR_END_OF_FILE) break;
    }
    EXPECT_EQ(1302U, total);
  }
  EXPECT_EQ(0U, calls);
}

TEST_F(RapidNestedLoop, BatchKernelPushbackAndScalarBoundaryRestoreNullFlags) {
  size_t calls = 0;
  Prealloced_array<TABLE *, 4> tables(PSI_NOT_INSTRUMENTED), outer(PSI_NOT_INSTRUMENTED), inner(PSI_NOT_INSTRUMENTED);
  outer.push_back(left.get());
  inner.push_back(right.get());
  tables.push_back(left.get());
  tables.push_back(right.get());
  VectorizedNestedLoopIterator loop(
      server.thd(), NewIterator<BatchDataRows>(server.thd(), server.thd()->mem_root, left.get(), &calls),
      NewIterator<BatchDataRows>(server.thd(), server.thd()->mem_root, right.get(), &calls, left->field[0]),
      JoinType::OUTER, false, tables, outer, inner, 256 * 1024, 1302);
  ASSERT_FALSE(loop.Init());
  ASSERT_EQ(0, loop.Read());
  EXPECT_EQ(10000, right->field[0]->val_int());
  auto chunks = Chunks(1024);
  size_t total = 1;
  for (;;) {
    for (auto &chunk : chunks) chunk.clear();
    size_t rows = 0;
    const int result = loop.ReadBatch(chunks, 1024, rows);
    ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
    total += rows;
    if (result == HA_ERR_END_OF_FILE) {
      ASSERT_GT(rows, 0U);
      ASSERT_FALSE(loop.PushbackBatchTail(chunks, rows - 1, rows));
      ASSERT_EQ(0, loop.Read());
      EXPECT_EQ(9, left->field[0]->val_int());
      EXPECT_TRUE(right->has_null_row());
      EXPECT_EQ(-1, loop.Read());
      break;
    }
  }
  EXPECT_EQ(1302U, total);
  EXPECT_EQ(0U, calls);
}

TEST_F(RapidNestedLoop, BatchBoundaryRestoresTheSuspendedOuterRecord) {
  auto loop = Make(JoinType::INNER);
  ASSERT_FALSE(loop->Init());
  auto chunks = Chunks(2);
  size_t rows = 0;
  ASSERT_EQ(0, loop->ReadBatch(chunks, 2, rows));
  ASSERT_EQ(2, rows);
  EXPECT_EQ(10, Value(chunks[1], 0));
  EXPECT_EQ(11, Value(chunks[1], 1));
  left->field[0]->store(777, false);
  right->field[0]->store(888, false);
  for (auto &chunk : chunks) chunk.clear();
  ASSERT_EQ(0, loop->ReadBatch(chunks, 2, rows));
  ASSERT_EQ(2, rows);
  EXPECT_EQ(1, Value(chunks[0], 0));
  EXPECT_EQ(12, Value(chunks[1], 0));
  EXPECT_EQ(2, Value(chunks[0], 1));
  EXPECT_EQ(20, Value(chunks[1], 1));
  EXPECT_EQ(-1, loop->Read());
}
TEST_F(RapidNestedLoop, RepeatedPushbackPrependsBeforeUnreadTail) {
  auto loop = Make(JoinType::INNER);
  ASSERT_FALSE(loop->Init());
  auto chunks = Chunks(4);
  size_t rows = 0;
  ASSERT_EQ(0, loop->ReadBatch(chunks, 4, rows));
  ASSERT_FALSE(loop->PushbackBatchTail(chunks, 1, rows));
  ASSERT_EQ(0, loop->Read());
  EXPECT_EQ(11, right->field[0]->val_int());
  for (auto &chunk : chunks) chunk.clear();
  ASSERT_EQ(0, loop->ReadBatch(chunks, 1, rows));
  EXPECT_EQ(12, Value(chunks[1], 0));
  ASSERT_FALSE(loop->PushbackBatchTail(chunks, 0, rows));
  ASSERT_EQ(0, loop->Read());
  EXPECT_EQ(12, right->field[0]->val_int());
  ASSERT_EQ(0, loop->Read());
  EXPECT_EQ(20, right->field[0]->val_int());
  EXPECT_EQ(-1, loop->Read());
}
TEST_F(RapidNestedLoop, OuterNullComplementSurvivesPushbackAndReexecution) {
  auto loop = Make(JoinType::OUTER);
  for (int execution = 0; execution < 2; ++execution) {
    ASSERT_FALSE(loop->Init());
    auto chunks = Chunks(8);
    size_t rows = 0;
    EXPECT_EQ(HA_ERR_END_OF_FILE, loop->ReadBatch(chunks, 8, rows));
    ASSERT_EQ(5, rows);
    EXPECT_TRUE(chunks[1].nullable_fast(4));
    ASSERT_FALSE(loop->PushbackBatchTail(chunks, 4, rows));
    right->reset_null_row();
    ASSERT_EQ(0, loop->Read());
    EXPECT_EQ(9, left->field[0]->val_int());
    EXPECT_TRUE(right->has_null_row());
    EXPECT_EQ(-1, loop->Read());
  }
}
TEST_F(RapidNestedLoop, SemiAndAntiDoNotFanOut) {
  for (JoinType type : {JoinType::SEMI, JoinType::ANTI}) {
    auto loop = Make(type);
    ASSERT_FALSE(loop->Init());
    auto chunks = Chunks(8);
    size_t rows = 0;
    EXPECT_EQ(HA_ERR_END_OF_FILE, loop->ReadBatch(chunks, 8, rows));
    ASSERT_EQ(type == JoinType::SEMI ? 2 : 1, rows);
    EXPECT_EQ(type == JoinType::SEMI ? 1 : 9, Value(chunks[0], 0));
    if (type == JoinType::ANTI) {
      EXPECT_TRUE(chunks[1].nullable_fast(0));
    }
  }
}

TEST_F(RapidNestedLoop, FinalPartialBatchIsCountedOnce) {
  auto loop = Make(JoinType::INNER);
  ASSERT_FALSE(loop->Init());
  auto chunks = Chunks(8);
  size_t rows = 0;
  EXPECT_EQ(HA_ERR_END_OF_FILE, loop->ReadBatch(chunks, 8, rows));
  EXPECT_EQ(4U, rows);
  EXPECT_EQ(1U, loop->GetOperatorStats().batches);
  for (auto &chunk : chunks) chunk.clear();
  EXPECT_EQ(HA_ERR_END_OF_FILE, loop->ReadBatch(chunks, 8, rows));
  EXPECT_EQ(0U, rows);
  EXPECT_EQ(1U, loop->GetOperatorStats().batches);
}
}  // namespace

#include "sql/filesort.h"
#include "sql/sql_optimizer.h"
#include "sql/temp_table_param.h"
#include "storage/rapid_engine/executor/iterators/sort_iterator.h"
#include "storage/rapid_engine/executor/iterators/hash_join_iterator.h"
#include "storage/rapid_engine/executor/iterators/stream_iterator.h"
#include "storage/rapid_engine/executor/iterators/materialize_iterator.h"
#include "storage/rapid_engine/executor/iterators/limit_iterator.h"
#include "unittest/gunit/parsertest.h"

namespace {
using namespace ShannonBase::Executor;
class PipelineRows final : public RowIterator, public BatchReadable {
 public:
  PipelineRows(THD *thd, TABLE *left, TABLE *right, size_t count, size_t *row_calls, size_t *init_calls = nullptr)
      : RowIterator(thd), m_left(left), m_right(right), m_count(count), m_row_calls(row_calls), m_init_calls(init_calls) {}
  bool Init() override { m_position = 0; if (m_init_calls) ++*m_init_calls; return false; }
  int Read() override {
    ++*m_row_calls;
    if (m_position == m_count) return -1;
    m_left->field[0]->store(static_cast<longlong>(m_position+1), false);
    if (m_position % 17 == 0) m_left->field[0]->set_null(); else m_left->field[0]->set_notnull();
    m_right->field[0]->store(static_cast<longlong>(m_count-m_position), false);
    m_right->field[0]->set_notnull();
    ++m_position; return 0;
  }
  int ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) override {
    rows = 0;
    while (rows < capacity && m_position < m_count) {
      for (auto &chunk : chunks) {
        if (!chunk.valid()) continue;
        const bool left = chunk.source_field() == m_left->field[0];
        const int32_t value = left ? m_position+1 : m_count-m_position;
        const bool is_null = left && m_position % 17 == 0;
        if (!chunk.add(reinterpret_cast<const uchar *>(&value), sizeof(value), is_null)) return 1;
      }
      ++rows; ++m_position;
    }
    return m_position == m_count ? HA_ERR_END_OF_FILE : 0;
  }
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &, size_t, size_t) override { return true; }
  void SetNullRowFlag(bool) override {}
  void UnlockRow() override {}
 private:
  TABLE *m_left, *m_right;
  size_t m_count, m_position{0}, *m_row_calls, *m_init_calls;
};
class RapidBatchPipeline : public ::testing::Test {
 protected:
  void SetUp() override {
    server.SetUp();
    left = std::make_unique<Fake_TABLE>(1, true); right = std::make_unique<Fake_TABLE>(1, true);
    for (TABLE *table : {left.get(), right.get()}) {
      bitmap_set_all(table->read_set); bitmap_set_all(table->write_set);
    }
    ShannonBase::Utils::MemoryPool::Config config(128 * 1024 * 1024);
    config.small_pool_ratio = 0; config.auto_defragmentation = false;
    pool = std::make_shared<ShannonBase::Utils::MemoryPool>(config);
    ASSERT_TRUE(ShannonBase::ResMgmt::initialize_system_pool(pool));
  }
  void TearDown() override {
    ShannonBase::ResMgmt::shutdown_system_pool(); pool.reset(); right.reset(); left.reset(); server.TearDown();
  }
  Prealloced_array<TABLE *,4> Tables() {
    Prealloced_array<TABLE *,4> tables(PSI_NOT_INSTRUMENTED); tables.push_back(left.get()); tables.push_back(right.get());
    return tables;
  }
  unique_ptr_destroy_only<RowIterator> Source(size_t count, size_t *calls, size_t *inits = nullptr) {
    return NewIterator<PipelineRows>(server.thd(), server.thd()->mem_root, left.get(), right.get(), count, calls, inits);
  }
  std::pmr::vector<ColumnChunk> Chunks(size_t capacity) {
    std::pmr::vector<ColumnChunk> result; result.emplace_back(left->field[0],capacity); result.emplace_back(right->field[0],capacity);
    return result;
  }
  static int32_t Value(const ColumnChunk &chunk, size_t row) { int32_t value; memcpy(&value, chunk.data_fast(row),sizeof(value)); return value; }
  my_testing::Server_initializer server;
  std::unique_ptr<Fake_TABLE> left,right;
  std::shared_ptr<ShannonBase::Utils::MemoryPool> pool;
};

// Request cooperative eviction after a selected input batch has advanced the
// child, but before the join can retain it. That batch must survive the spill.
class EvictAfterSelectedRead final : public RowIterator, public BatchReadable {
 public:
  EvictAfterSelectedRead(THD *thd, unique_ptr_destroy_only<RowIterator> source, VectorizedHashJoinIterator **owner)
      : RowIterator(thd), m_source(std::move(source)), m_owner(owner) {}
  bool Init() override { m_requested = false; return m_source->Init(); }
  int Read() override { ADD_FAILURE() << "Unexpected scalar read"; return 1; }
  int ReadBatch(std::pmr::vector<ColumnChunk> &, size_t, size_t &) override {
    ADD_FAILURE() << "Unexpected dense read";
    return 1;
  }
  int ReadSelectedBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows,
                        std::pmr::vector<uint32_t> &selection) override {
    auto *batch = dynamic_cast<BatchReadable *>(m_source->real_iterator());
    const int result = batch->ReadSelectedBatch(chunks, capacity, rows, selection);
    if (rows && *m_owner && !m_requested) {
      (*m_owner)->Evict(1);
      m_requested = true;
    }
    return result;
  }
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &, size_t, size_t) override { return true; }
  void SetNullRowFlag(bool value) override { m_source->SetNullRowFlag(value); }
  void UnlockRow() override { m_source->UnlockRow(); }
  std::shared_ptr<ShannonBase::Compress::Dictionary> Dictionary(const Field *field) const override {
    return dynamic_cast<BatchReadable *>(m_source->real_iterator())->Dictionary(field);
  }
 private:
  unique_ptr_destroy_only<RowIterator> m_source;
  VectorizedHashJoinIterator **m_owner;
  bool m_requested{false};
};

TEST_F(RapidBatchPipeline, PartitionedInputsKeepNativeLookupAndJoinSelection) {
  AccessPath outer, inner, join, lookup;
  outer.type = AccessPath::TABLE_SCAN;
  outer.table_scan().table = left.get();
  inner.type = AccessPath::TABLE_SCAN;
  inner.table_scan().table = right.get();
  join.type = AccessPath::NESTED_LOOP_JOIN;
  join.nested_loop_join().outer = &outer;
  join.nested_loop_join().inner = &inner;
  join.nested_loop_join().join_type = JoinType::INNER;
  lookup.type = AccessPath::EQ_REF;
  lookup.eq_ref().table = right.get();
  lookup.set_num_output_rows(1);
  ASSERT_TRUE(VectorizedNestedLoopIterator::CanVectorize(&join));
  ASSERT_TRUE(SelectVectorizedLookup(server.thd(), &lookup, true));

  partition_info partitions;
  for (TABLE *table : {left.get(), right.get()}) {
    table->part_info = &partitions;
    auto restore = create_scope_guard([&] { table->part_info = nullptr; });
    EXPECT_FALSE(VectorizedNestedLoopIterator::CanVectorize(&join));
    // Numeric records remain encodable at SQL boundaries. That does not
    // authorize direct access to ha_rapid's non-partitioned lookup cursor.
    EXPECT_TRUE(CanBatchRecords(table));
    lookup.eq_ref().table = table;
    EXPECT_FALSE(SelectVectorizedLookup(server.thd(), &lookup, true));
    EXPECT_FALSE(SelectVectorizedLookup(server.thd(), &lookup, false));
  }
}

TEST_F(RapidBatchPipeline, SortGathersJoinedColumnsWithCollidingFieldIndices) {
  ORDER order{}; order.item_initial = new Item_field(right->field[0]); order.direction = ORDER_ASC;
  Mem_root_array<TABLE *> tables(server.thd()->mem_root); tables.push_back(left.get()); tables.push_back(right.get());
  Filesort filesort(server.thd(), std::move(tables), false, &order, HA_POS_ERROR, false, false, false);
  size_t row_calls = 0;
  VectorizedSortIterator sort(server.thd(), &filesort, Source(40,&row_calls),
      pack_rows::TableCollection(Tables(),false,0,0),2*1024*1024,nullptr);
  ASSERT_FALSE(sort.Init()); ASSERT_TRUE(sort.SupportsBatchRead());
  auto chunks = Chunks(8); size_t rows = 0;
  ASSERT_EQ(0,sort.ReadBatch(chunks,8,rows)); ASSERT_EQ(8U,rows);
  for(size_t row=0;row<rows;++row) EXPECT_EQ(row+1,Value(chunks[1],row));
  ASSERT_FALSE(sort.PushbackBatchTail(chunks,2,rows));
  ASSERT_EQ(0,sort.Read()); EXPECT_EQ(3,right->field[0]->val_int());
  left->field[0]->store(999,false); right->field[0]->store(999,false);
  for(auto &chunk:chunks) chunk.clear();
  ASSERT_EQ(0,sort.ReadBatch(chunks,6,rows)); ASSERT_EQ(6U,rows);
  for(size_t row=0;row<rows;++row) EXPECT_EQ(row+4,Value(chunks[1],row));
  EXPECT_EQ(0U,row_calls);
}
TEST_F(RapidBatchPipeline, SpilledSortHonorsLimitInBatchOutput) {
  ORDER order{}; order.item_initial = new Item_field(right->field[0]); order.direction = ORDER_ASC;
  Mem_root_array<TABLE *> tables(server.thd()->mem_root); tables.push_back(left.get()); tables.push_back(right.get());
  Filesort filesort(server.thd(), std::move(tables), false, &order, 1000, false, false, false);
  size_t row_calls = 0;
  VectorizedSortIterator sort(server.thd(), &filesort, Source(4000, &row_calls),
      pack_rows::TableCollection(Tables(), false, 0, 0), 64 * 1024, nullptr);
  ASSERT_FALSE(sort.Init()); ASSERT_TRUE(sort.SupportsBatchRead());
  auto chunks = Chunks(16); size_t total = 0;
  for (;;) {
    for (auto &chunk : chunks) chunk.clear();
    size_t rows = 0;
    const int result = sort.ReadBatch(chunks, 16, rows);
    ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
    for (size_t row = 0; row < rows; ++row) EXPECT_EQ(total + row + 1, Value(chunks[1], row));
    total += rows;
    if (result == HA_ERR_END_OF_FILE) break;
  }
  EXPECT_EQ(1000U, total); EXPECT_EQ(-1, sort.Read()); EXPECT_EQ(0U, row_calls);
}

TEST_F(RapidBatchPipeline, SortConsumesSelectionWithoutCompactingFilterColumns) {
  for (size_t budget : {size_t{64 * 1024}, size_t{2 * 1024 * 1024}}) {
    ORDER order{};
    order.item_initial = new Item_field(right->field[0]);
    order.direction = ORDER_ASC;
    Mem_root_array<TABLE *> tables(server.thd()->mem_root);
    tables.push_back(left.get());
    tables.push_back(right.get());
    Filesort filesort(server.thd(), std::move(tables), false, &order, HA_POS_ERROR, false, false, false);
    Item *condition = new Item_func_lt(new Item_field(right->field[0]), new Item_int(3000));
    ASSERT_FALSE(condition->fix_fields(server.thd(), &condition));
    size_t row_calls = 0;
    auto filter = NewIterator<VectorizedFilterIterator>(server.thd(), server.thd()->mem_root,
                                                        Source(4000, &row_calls), condition);
    auto *filter_stats = down_cast<VectorizedFilterIterator *>(filter->real_iterator());
    VectorizedSortIterator sort(server.thd(), &filesort, std::move(filter),
                                pack_rows::TableCollection(Tables(), false, 0, 0), budget, nullptr);
    for (int execution = 0; execution < 2; ++execution) {
      ASSERT_FALSE(sort.Init());
      ASSERT_TRUE(sort.SupportsBatchRead());
      auto chunks = Chunks(31);
      size_t total = 0;
      for (;;) {
        for (auto &chunk : chunks) chunk.clear();
        size_t rows = 0;
        const int result = sort.ReadBatch(chunks, 31, rows);
        ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
        for (size_t row = 0; row < rows; ++row) {
          const size_t right_value = total + row + 1;
          const size_t physical = 4000 - right_value;
          EXPECT_EQ(right_value, Value(chunks[1], row));
          EXPECT_EQ(physical % 17 == 0, chunks[0].nullable_fast(row));
          if (!chunks[0].nullable_fast(row)) {
            EXPECT_EQ(physical + 1, Value(chunks[0], row));
          }
        }
        total += rows;
        if (result == HA_ERR_END_OF_FILE) break;
      }
      EXPECT_EQ(2999U, total);
      EXPECT_EQ(0U, filter_stats->stats().bytes_copied);
      EXPECT_EQ(0U, filter_stats->row_materializations());
    }
    EXPECT_EQ(0U, row_calls);
  }
}

TEST_F(RapidBatchPipeline, MaterializationConsumesSelectionWithoutCompactionAndRewinds) {
  size_t row_calls = 0, init_calls = 0;
  Item *condition = new Item_func_gt(new Item_field(left->field[0]), new Item_int(1000));
  ASSERT_FALSE(condition->fix_fields(server.thd(), &condition));
  auto filter = NewIterator<VectorizedFilterIterator>(server.thd(), server.thd()->mem_root,
                                                      Source(4000, &row_calls, &init_calls), condition);
  auto *filter_stats = down_cast<VectorizedFilterIterator *>(filter->real_iterator());
  VectorizedMaterializeIterator materialize(server.thd(), std::move(filter), left.get(), 16 * 1024);
  std::vector<int32_t> expected;
  for (int32_t value = 1001; value <= 4000; ++value)
    if ((value - 1) % 17 != 0) expected.push_back(value);
  for (int execution = 0; execution < 2; ++execution) {
    ASSERT_FALSE(materialize.Init());
    std::pmr::vector<ColumnChunk> chunks;
    chunks.emplace_back(left->field[0], 31);
    size_t total = 0;
    for (;;) {
      chunks[0].clear();
      size_t rows = 0;
      const int result = materialize.ReadBatch(chunks, 31, rows);
      ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
      ASSERT_LE(total + rows, expected.size());
      for (size_t row = 0; row < rows; ++row) {
        EXPECT_FALSE(chunks[0].nullable_fast(row));
        EXPECT_EQ(expected[total + row], Value(chunks[0], row));
      }
      total += rows;
      if (result == HA_ERR_END_OF_FILE) break;
    }
    EXPECT_EQ(expected.size(), total);
    EXPECT_EQ(0U, filter_stats->stats().bytes_copied);
    EXPECT_EQ(0U, filter_stats->row_materializations());
  }
  EXPECT_EQ(0U, row_calls);
  EXPECT_EQ(1U, init_calls);
}

TEST_F(RapidBatchPipeline, ArithmeticResidualConsumesSelectionWithoutRestoringFields) {
  for (bool decimal : {false, true}) {
    size_t row_calls = 0;
    Item *first = new Item_func_gt(new Item_field(right->field[0]), new Item_int(100));
    ASSERT_FALSE(first->fix_fields(server.thd(), &first));
    auto source = NewIterator<VectorizedFilterIterator>(server.thd(), server.thd()->mem_root,
                                                        Source(1000, &row_calls), first);
    Item *condition = decimal
        ? static_cast<Item *>(new Item_func_gt(new Item_field(left->field[0]), new Item_decimal(20.5)))
        : new Item_func_lt(new Item_func_mul(new Item_field(left->field[0]), new Item_int(2)),
                           new Item_field(right->field[0]));
    ASSERT_FALSE(condition->fix_fields(server.thd(), &condition));
    VectorizedFilterIterator filter(server.thd(), std::move(source), condition);
    for (size_t execution = 0; execution < 2; ++execution) {
      ASSERT_FALSE(filter.Init());
      auto chunks = Chunks(71);
      std::pmr::vector<uint32_t> selection;
      std::vector<int32_t> actual, expected;
      for (int32_t value = 1; value <= 900; ++value)
        if ((value - 1) % 17 != 0 && (decimal ? value > 20.5 : value * 2 < 1001 - value))
          expected.push_back(value);
      for (;;) {
        for (auto &chunk : chunks) chunk.clear();
        size_t rows = 0;
        const int result = filter.ReadSelectedBatch(chunks, 71, rows, selection);
        ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
        for (size_t row = 0; row < rows; ++row)
          actual.push_back(Value(chunks[0], selection.empty() ? row : selection[row]));
        if (result == HA_ERR_END_OF_FILE) break;
      }
      EXPECT_EQ(expected, actual);
      EXPECT_EQ(0U, filter.row_materializations());
      EXPECT_EQ(0U, filter.stats().bytes_copied);
      EXPECT_EQ(0U, filter.stats().scalar_fallback_rows);
      EXPECT_GT(filter.stats().expression_batch_rows, 0U);
    }
    EXPECT_EQ(0U, row_calls);
  }
}

TEST_F(RapidBatchPipeline, DecimalExpressionMatchesMysqlWithoutWritingSourceFields) {
  Field_new_decimal price(my_decimal_precision_to_length(15, 2, false), true, "price", 2, false);
  Field_new_decimal discount(my_decimal_precision_to_length(15, 2, false), true, "discount", 2, false);
  Fake_TABLE table(&price, &discount);
  bitmap_set_all(table.write_set);
  Item *expression = new Item_func_mul(new Item_field(&price),
      new Item_func_minus(new Item_int(1), new Item_field(&discount)));
  ASSERT_FALSE(expression->fix_fields(server.thd(), &expression));
  Field_new_decimal output(my_decimal_precision_to_length(expression->decimal_precision(), expression->decimals, false),
                           true, "output", expression->decimals, false);
  Fake_TABLE output_table(&output);
  bitmap_set_all(output_table.write_set);
  VectorizedExpression program(std::pmr::get_default_resource());
  ASSERT_TRUE(program.Compile(expression));
  std::pmr::vector<ColumnChunk> chunks;
  chunks.emplace_back(&price, 8);
  chunks.emplace_back(&discount, 8);
  ColumnChunk target(&output, 8);
  std::vector<std::vector<uchar>> expected;
  const char *prices[] = {"21168.23", "-45983.16", "0.00", "9999999999999.99", "-0.01", "7.13", "1.10", "0.01"};
  const char *discounts[] = {"0.04", "1.09", "0.10", "-0.09", "0.10", "1.00", "0.07", "0.01"};
  for (size_t row = 0; row < 8; ++row) {
    price.set_notnull();
    discount.set_notnull();
    ASSERT_EQ(TYPE_OK, price.store(prices[row], strlen(prices[row]), &my_charset_latin1));
    ASSERT_EQ(TYPE_OK, discount.store(discounts[row], strlen(discounts[row]), &my_charset_latin1));
    if (row == 5) price.set_null();
    if (row == 6) discount.set_null();
    ASSERT_TRUE(chunks[0].add(price.field_ptr(), price.pack_length(), price.is_null()));
    ASSERT_TRUE(chunks[1].add(discount.field_ptr(), discount.pack_length(), discount.is_null()));
    my_decimal buffer;
    my_decimal *value = expression->val_decimal(&buffer);
    expected.emplace_back();
    if (!expression->null_value) {
      ASSERT_NE(nullptr, value);
      ASSERT_EQ(TYPE_OK, output.store_decimal(value));
      expected.back().assign(output.field_ptr(), output.field_ptr() + output.pack_length());
    }
  }
  ASSERT_EQ(TYPE_OK, price.store("123.45", 6, &my_charset_latin1));
  ASSERT_TRUE(program.Evaluate(chunks, 8));
  ASSERT_TRUE(program.Store(&target, 8));
  for (size_t row = 0; row < 8; ++row) {
    EXPECT_EQ(expected[row].empty(), target.nullable_fast(row));
    if (!expected[row].empty()) {
      EXPECT_EQ(0, memcmp(expected[row].data(), target.data_fast(row), output.pack_length()));
    }
  }
  EXPECT_DOUBLE_EQ(123.45, price.val_real());
}

TEST_F(RapidBatchPipeline, IntegerExpressionOverflowFallsBackWithoutDiagnosticsOrMutation) {
  Item *expression = new Item_func_mul(new Item_field(left->field[0]),
                                      new Item_int(std::numeric_limits<longlong>::max()));
  ASSERT_FALSE(expression->fix_fields(server.thd(), &expression));
  VectorizedExpression program(std::pmr::get_default_resource());
  ASSERT_TRUE(program.Compile(expression));
  auto chunks = Chunks(2);
  for (int32_t value : {1, 2}) ASSERT_TRUE(chunks[0].add(reinterpret_cast<const uchar *>(&value), sizeof(value), false));
  left->field[0]->store(123, false);
  EXPECT_FALSE(program.Evaluate(chunks, 2));
  EXPECT_FALSE(server.thd()->is_error());
  EXPECT_EQ(123, left->field[0]->val_int());
}

TEST_F(RapidBatchPipeline, PackedExpressionOutputMatchesMysqlAtDecimalGroupBoundaries) {
  for (unsigned precision : {9U, 18U, 19U, 38U, 39U, 65U}) {
    for (unsigned scale : {0U, 1U, 9U, 18U, 30U}) {
      if (scale > precision) continue;
      Field_new_decimal field(my_decimal_precision_to_length(precision, scale, false), true, "decimal", scale, false);
      Fake_TABLE table(&field);
      bitmap_set_all(table.write_set);
      VectorizedExpression program(std::pmr::get_default_resource());
      ASSERT_TRUE(program.Compile(new Item_field(&field)));
      std::pmr::vector<ColumnChunk> chunks;
      chunks.emplace_back(&field, 3);
      std::vector<std::vector<uchar>> expected;
      for (int sign : {-1, 0, 1}) {
        std::string value(sign == 0 ? 1 : std::min(precision, 37U), sign == 0 ? '0' : '9');
        if (value.size() <= scale) value.insert(0, scale + 1 - value.size(), '0');
        if (scale) value.insert(value.size() - scale, 1, '.');
        if (sign == -1) value.insert(0, 1, '-');
        ASSERT_EQ(TYPE_OK, field.store(value.data(), value.size(), &my_charset_latin1));
        expected.emplace_back(field.field_ptr(), field.field_ptr() + field.pack_length());
        ASSERT_TRUE(chunks[0].add(field.field_ptr(), field.pack_length(), false));
      }
      ASSERT_TRUE(program.Evaluate(chunks, 3));
      ColumnChunk output(&field, 3);
      ASSERT_TRUE(program.Store(&output, 3));
      for (size_t row = 0; row < 3; ++row)
        EXPECT_EQ(0, memcmp(expected[row].data(), output.data_fast(row), field.pack_length()))
            << "DECIMAL(" << precision << ',' << scale << ')';
    }
  }
}

TEST_F(RapidBatchPipeline, HashJoinConsumesSelectionsOnBothInputsWithoutCompaction) {
  for (bool spill : {false, true}) {
    left->pos_in_table_list->set_tableno(0);
    right->pos_in_table_list->set_tableno(1);
    size_t row_calls = 0;
    Item *build_condition = new Item_func_gt(new Item_field(left->field[0]), new Item_int(20));
    Item *probe_condition = new Item_func_gt(new Item_field(right->field[0]), new Item_int(10));
    ASSERT_FALSE(build_condition->fix_fields(server.thd(), &build_condition));
    ASSERT_FALSE(probe_condition->fix_fields(server.thd(), &probe_condition));
    auto build = NewIterator<VectorizedFilterIterator>(server.thd(), server.thd()->mem_root,
                                                       Source(40, &row_calls), build_condition);
    auto probe = NewIterator<VectorizedFilterIterator>(server.thd(), server.thd()->mem_root,
                                                       Source(40, &row_calls), probe_condition);
    auto *build_stats = down_cast<VectorizedFilterIterator *>(build->real_iterator());
    auto *probe_stats = down_cast<VectorizedFilterIterator *>(probe->real_iterator());
    VectorizedHashJoinIterator *owner = nullptr;
    auto input = NewIterator<EvictAfterSelectedRead>(server.thd(), server.thd()->mem_root, std::move(build), &owner);
    auto *eq = new Item_func_eq(new Item_field(left->field[0]), new Item_field(right->field[0]));
    eq->set_cmp_func();
    std::vector<HashJoinCondition> conditions{HashJoinCondition(eq, server.thd()->mem_root)};
    Mem_root_array<Item *> extra(server.thd()->mem_root);
    Prealloced_array<TABLE *, 4> build_tables(PSI_NOT_INSTRUMENTED), probe_tables(PSI_NOT_INSTRUMENTED);
    build_tables.push_back(left.get());
    probe_tables.push_back(right.get());
    VectorizedHashJoinIterator join(server.thd(), std::move(input), build_tables, 40, std::move(probe), probe_tables,
                                    false, 0, 1024 * 1024, conditions, false, JoinType::INNER, extra,
                                    HashJoinInput::kBuild, false, nullptr, 40);
    if (spill) owner = &join;
    ASSERT_FALSE(join.Init());
    ASSERT_TRUE(join.SupportsBatchRead());
    auto chunks = Chunks(7);
    size_t total = 0;
    for (;;) {
      for (auto &chunk : chunks) chunk.clear();
      size_t rows = 0;
      const int result = join.ReadBatch(chunks, 7, rows);
      ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
      for (size_t row = 0; row < rows; ++row) {
        EXPECT_EQ(Value(chunks[0], row), Value(chunks[1], row));
        EXPECT_GT(Value(chunks[0], row), 20);
        EXPECT_NE(35, Value(chunks[0], row));
      }
      total += rows;
      if (result == HA_ERR_END_OF_FILE) break;
    }
    EXPECT_EQ(19U, total);
    EXPECT_EQ(0U, build_stats->stats().bytes_copied);
    EXPECT_EQ(0U, probe_stats->stats().bytes_copied);
    EXPECT_EQ(0U, row_calls);
  }
}

TEST_F(RapidBatchPipeline, HashJoinRowReadReplaysPushedBatchTail) {
  for (bool spill : {false, true}) {
    left->pos_in_table_list->set_tableno(0);
    right->pos_in_table_list->set_tableno(1);
    size_t row_calls = 0;
    VectorizedHashJoinIterator *owner = nullptr;
    auto input = NewIterator<EvictAfterSelectedRead>(server.thd(), server.thd()->mem_root, Source(40, &row_calls), &owner);
    auto *eq = new Item_func_eq(new Item_field(left->field[0]), new Item_field(right->field[0]));
    eq->set_cmp_func();
    std::vector<HashJoinCondition> conditions{HashJoinCondition(eq, server.thd()->mem_root)};
    Mem_root_array<Item *> extra(server.thd()->mem_root);
    Prealloced_array<TABLE *, 4> build_tables(PSI_NOT_INSTRUMENTED), probe_tables(PSI_NOT_INSTRUMENTED);
    build_tables.push_back(left.get());
    probe_tables.push_back(right.get());
    VectorizedHashJoinIterator join(server.thd(), std::move(input), build_tables, 40, Source(40, &row_calls),
                                    probe_tables, false, 0, 1024 * 1024, conditions, false, JoinType::INNER, extra,
                                    HashJoinInput::kBuild, false, nullptr, 40);
    if (spill) owner = &join;
    ASSERT_FALSE(join.Init());
    auto chunks = Chunks(7);
    size_t rows = 0;
    ASSERT_EQ(0, join.ReadBatch(chunks, 7, rows));
    ASSERT_EQ(7U, rows);
    std::vector<int32_t> expected;
    for (size_t row = 3; row < rows; ++row) expected.push_back(Value(chunks[0], row));
    ASSERT_FALSE(join.PushbackBatchTail(chunks, 3, rows));
    for (int32_t value : expected) {
      ASSERT_EQ(0, join.Read());
      EXPECT_EQ(value, left->field[0]->val_int());
      EXPECT_EQ(value, right->field[0]->val_int());
    }
    size_t total = 7;
    for (;;) {
      for (auto &chunk : chunks) chunk.clear();
      const int result = join.ReadBatch(chunks, 7, rows);
      ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
      total += rows;
      if (result == HA_ERR_END_OF_FILE) break;
    }
    EXPECT_EQ(37U, total);
    EXPECT_EQ(0U, row_calls);
  }
}

TEST_F(RapidBatchPipeline, MaterializationSpillsColumnsAndRewindsWithoutReadingSqlRows) {
  const auto before = ShannonBase::RapidMonitor::rapid_counters.query_vectorized_materialize_batch_rows_total.load();
  size_t row_calls=0, init_calls=0;
  VectorizedMaterializeIterator materialize(server.thd(),Source(4000,&row_calls,&init_calls),left.get(),16*1024);
  for(int execution=0;execution<2;++execution) {
    ASSERT_FALSE(materialize.Init());
    std::pmr::vector<ColumnChunk> chunks; chunks.emplace_back(left->field[0],31);
    size_t total=0;
    for(;;) {
      chunks[0].clear(); size_t rows=0;
      const int result=materialize.ReadBatch(chunks,31,rows);
      ASSERT_TRUE(result==0 || result==HA_ERR_END_OF_FILE);
      for(size_t row=0;row<rows;++row) {
        EXPECT_EQ((total+row)%17==0,chunks[0].nullable_fast(row));
        if(!chunks[0].nullable_fast(row)) {
          EXPECT_EQ(total+row+1,Value(chunks[0],row));
        }
      }
      total+=rows; if(result==HA_ERR_END_OF_FILE) break;
    }
    EXPECT_EQ(4000U,total);
  }
  EXPECT_EQ(0U,row_calls); EXPECT_EQ(1U,init_calls);
  EXPECT_EQ(8000U, ShannonBase::RapidMonitor::rapid_counters.query_vectorized_materialize_batch_rows_total.load() - before);
}
TEST_F(RapidBatchPipeline, PureProjectionRemapsChunksAndPreservesPushback) {
  Query_block *block=parse(&server,"SELECT 1",0); JOIN join(server.thd(),block);
  block->join = &join;
  auto target=std::make_unique<Fake_TABLE>(1,true);
  bitmap_set_all(target->read_set); bitmap_set_all(target->write_set);
  Temp_table_param param(server.thd()->mem_root);
  Func_ptr_array copies(server.thd()->mem_root); copies.push_back(Func_ptr(new Item_field(left->field[0]),target->field[0]));
  param.items_to_copy=&copies;
  ASSERT_TRUE(VectorizedStreamIterator::CanVectorize(&param,target.get(),false));
  size_t calls=0;
  VectorizedStreamIterator stream(server.thd(),Source(40,&calls),&param,target.get(),false,&join,-1,Tables(),256*1024);
  ASSERT_FALSE(stream.Init()); ASSERT_TRUE(stream.SupportsBatchRead());
  std::pmr::vector<ColumnChunk> chunks; chunks.emplace_back(target->field[0],8); size_t rows=0;
  ASSERT_EQ(0,stream.ReadBatch(chunks,8,rows)); ASSERT_EQ(8U,rows);
  EXPECT_TRUE(chunks[0].nullable_fast(0)); EXPECT_EQ(3,Value(chunks[0],2));
  ASSERT_FALSE(stream.PushbackBatchTail(chunks,3,rows));
  ASSERT_EQ(0,stream.Read()); EXPECT_EQ(4,target->field[0]->val_int());
  chunks[0].clear(); ASSERT_EQ(0,stream.ReadBatch(chunks,8,rows)); ASSERT_EQ(4U,rows);
  EXPECT_EQ(5,Value(chunks[0],0)); EXPECT_EQ(8,Value(chunks[0],3)); EXPECT_EQ(0U,calls);
}
TEST_F(RapidBatchPipeline, ProjectionRowReadsUseTheBatchCoreAndReinitialize) {
  Query_block *block = parse(&server, "SELECT 1", 0);
  JOIN join(server.thd(), block);
  block->join = &join;
  auto target = std::make_unique<Fake_TABLE>(1, true);
  bitmap_set_all(target->read_set);
  bitmap_set_all(target->write_set);
  Temp_table_param param(server.thd()->mem_root);
  Func_ptr_array copies(server.thd()->mem_root);
  copies.push_back(Func_ptr(new Item_field(left->field[0]), target->field[0]));
  param.items_to_copy = &copies;
  size_t calls = 0, inits = 0;
  VectorizedStreamIterator stream(server.thd(), Source(40, &calls, &inits), &param, target.get(), false, &join, -1,
                                  Tables(), 256 * 1024);
  for (size_t execution = 1; execution <= 2; ++execution) {
    ASSERT_FALSE(stream.Init());
    ASSERT_EQ(0, stream.Read());
    EXPECT_TRUE(target->field[0]->is_null());
    std::pmr::vector<ColumnChunk> chunks;
    chunks.emplace_back(target->field[0], 8);
    size_t rows = 0;
    ASSERT_EQ(0, stream.ReadBatch(chunks, 8, rows));
    ASSERT_EQ(8U, rows);
    EXPECT_EQ(2, Value(chunks[0], 0));
    EXPECT_EQ(9, Value(chunks[0], 7));
    ASSERT_EQ(0, stream.Read());
    EXPECT_EQ(10, target->field[0]->val_int());
    EXPECT_EQ(execution, inits);
    EXPECT_EQ(0U, calls);
  }
}

class DictionaryBatchRows final : public RowIterator, public ShannonBase::Executor::BatchReadable {
 public:
  DictionaryBatchRows(THD *thd, Field *field, size_t *calls, Field *key = nullptr, size_t count = 5,
                      bool shifted_dictionary = false)
      : RowIterator(thd),
        m_field(field),
        m_calls(calls),
        m_key(key),
        m_count(count),
        m_dictionary(std::make_shared<ShannonBase::Compress::Dictionary>()) {
    if (shifted_dictionary) m_dictionary->store(reinterpret_cast<const uchar *>("padding"), 7);
    for (const char *value : {"alpha", "", "unused", "gamma", "delta"})
      m_ids.push_back(m_dictionary->store(reinterpret_cast<const uchar *>(value), strlen(value)));
  }
  bool Init() override {
    m_position = 0;
    return false;
  }
  int Read() override {
    ++*m_calls;
    return 1;
  }
  bool PushbackBatchTail(const std::pmr::vector<ColumnChunk> &, size_t, size_t) override { return true; }
  void SetNullRowFlag(bool value) override { if (value) m_field->table->set_null_row(); else m_field->table->reset_null_row(); }
  void UnlockRow() override {}
  bool SupportsBatchRead() const override { return true; }
  std::shared_ptr<ShannonBase::Compress::Dictionary> Dictionary(const Field *field) const override {
    return field == m_field ? m_dictionary : nullptr;
  }
  int ReadBatch(std::pmr::vector<ColumnChunk> &chunks, size_t capacity, size_t &rows) override {
    rows = 0;
    while (rows < capacity && m_position < m_count) {
      const size_t index = m_position % m_ids.size();
      const bool is_null = index == 2;
      const int32_t key = m_position + 1;
      for (auto &chunk : chunks) {
        if (!chunk.valid()) continue;
        if (chunk.source_field() == m_key) {
          if (!chunk.add(reinterpret_cast<const uchar *>(&key), sizeof(key), false)) return 1;
        } else if (chunk.source_field() == m_field) {
          if (!chunk.add(is_null ? nullptr : reinterpret_cast<const uchar *>(&m_ids[index]),
                         is_null ? 0 : sizeof(uint32_t), is_null)) return 1;
        } else {
          return 1;
        }
      }
      ++rows;
      ++m_position;
    }
    return m_position == m_count ? HA_ERR_END_OF_FILE : 0;
  }

 private:
  Field *m_field;
  size_t *m_calls;
  Field *m_key;
  size_t m_count;
  std::shared_ptr<ShannonBase::Compress::Dictionary> m_dictionary;
  std::vector<uint32_t> m_ids;
  size_t m_position{0};
};

TEST_F(RapidBatchPipeline, HashJoinKeepsDictionaryPayloadsAcrossSpillPushbackAndRowReads) {
  Fake_TABLE_SHARE share(2);
  Mock_field_long build_key("build_key", false, false), probe_key("probe_key", false, false);
  Mock_field_varstring build_value(&share, "build_value", 16, true), probe_value(&share, "probe_value", 16, true);
  Fake_TABLE build_table(&build_key, &build_value), probe_table(&probe_key, &probe_value);
  build_table.pos_in_table_list->set_tableno(0);
  probe_table.pos_in_table_list->set_tableno(1);
  for (auto *table : {&build_table, &probe_table}) {
    bitmap_set_all(table->read_set);
    bitmap_set_all(table->write_set);
  }
  constexpr size_t count = 4000;
  for (bool spill : {false, true}) {
    size_t row_calls = 0;
    VectorizedHashJoinIterator *owner = nullptr;
    auto build = NewIterator<DictionaryBatchRows>(server.thd(), server.thd()->mem_root, &build_value, &row_calls,
                                                 &build_key, count, false);
    auto input = NewIterator<EvictAfterSelectedRead>(server.thd(), server.thd()->mem_root, std::move(build), &owner);
    auto probe = NewIterator<DictionaryBatchRows>(server.thd(), server.thd()->mem_root, &probe_value, &row_calls,
                                                 &probe_key, count, true);
    auto *eq = new Item_func_eq(new Item_field(&build_key), new Item_field(&probe_key));
    eq->set_cmp_func();
    std::vector<HashJoinCondition> conditions{HashJoinCondition(eq, server.thd()->mem_root)};
    Mem_root_array<Item *> extra(server.thd()->mem_root);
    Prealloced_array<TABLE *, 4> build_tables(PSI_NOT_INSTRUMENTED), probe_tables(PSI_NOT_INSTRUMENTED);
    build_tables.push_back(&build_table);
    probe_tables.push_back(&probe_table);
    VectorizedHashJoinIterator join(server.thd(), std::move(input), build_tables, count, std::move(probe),
                                    probe_tables, false, 0, 1024 * 1024, conditions, false, JoinType::INNER, extra,
                                    HashJoinInput::kBuild, false, nullptr, count);
    if (spill) owner = &join;
    for (size_t execution = 0; execution < 2; ++execution) {
      ASSERT_FALSE(join.Init());
      ASSERT_TRUE(join.SupportsBatchRead());
      ASSERT_NE(nullptr, join.Dictionary(&build_value));
      ASSERT_NE(nullptr, join.Dictionary(&probe_value));
      EXPECT_NE(join.Dictionary(&build_value), join.Dictionary(&probe_value));
      std::pmr::vector<ColumnChunk> chunks;
      for (Field *field : std::array<Field *, 4>{&build_key, &build_value, &probe_key, &probe_value})
        chunks.emplace_back(field, 31);
      const char *values[] = {"alpha", "", "unused", "gamma", "delta"};
      std::vector<bool> seen(count + 1);
      const auto check = [&](size_t row) {
        const int32_t key = Value(chunks[0], row);
        ASSERT_GE(key, 1);
        ASSERT_LE(key, count);
        EXPECT_FALSE(seen[key]);
        seen[key] = true;
        EXPECT_EQ(key, Value(chunks[2], row));
        for (size_t col : {size_t{1}, size_t{3}}) {
          const size_t index = (key - 1) % 5;
          EXPECT_EQ(index == 2, chunks[col].nullable_fast(row));
          if (index == 2) continue;
          uint32_t code;
          memcpy(&code, chunks[col].data_fast(row), sizeof(code));
          char decoded[32];
          auto length = join.Dictionary(chunks[col].source_field())->get(code, decoded, sizeof(decoded));
          ASSERT_TRUE(length.has_value());
          EXPECT_EQ(values[index], std::string(decoded, *length));
        }
      };
      size_t rows = 0;
      ASSERT_EQ(0, join.ReadBatch(chunks, 7, rows));
      ASSERT_EQ(7U, rows);
      check(0);
      ASSERT_FALSE(join.PushbackBatchTail(chunks, 1, rows));
      ASSERT_EQ(0, join.Read());
      check(1);
      const size_t index = (build_key.val_int() - 1) % 5;
      EXPECT_EQ(build_key.val_int(), probe_key.val_int());
      for (Field *field : {static_cast<Field *>(&build_value), static_cast<Field *>(&probe_value)}) {
        EXPECT_EQ(index == 2, field->is_null());
        if (index == 2) continue;
        String buffer;
        String *value = field->val_str(&buffer, &buffer);
        ASSERT_NE(nullptr, value);
        EXPECT_EQ(values[index], std::string(value->ptr(), value->length()));
      }
      for (;;) {
        for (auto &chunk : chunks) chunk.clear();
        const int result = join.ReadBatch(chunks, 31, rows);
        ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
        for (size_t row = 0; row < rows; ++row) check(row);
        if (result == HA_ERR_END_OF_FILE) break;
      }
      EXPECT_EQ(count, std::count(seen.begin(), seen.end(), true));
      EXPECT_EQ(0U, row_calls);
    }
  }
}

TEST_F(RapidBatchPipeline, LimitRestoresTemporaryDictionaryStringsThroughTheSharedAdapter) {
  ShannonBase::LoadedTables registry;
  auto *previous_registry = ShannonBase::shannon_loaded_tables;
  ShannonBase::shannon_loaded_tables = &registry;
  auto restore_registry = create_scope_guard([&] { ShannonBase::shannon_loaded_tables = previous_registry; });
  Fake_TABLE_SHARE share(1);
  auto field = std::make_unique<Mock_field_varstring>(&share, "value", 16, true);
  Fake_TABLE table(field.get());
  table.s->db = {"test", 4};
  table.s->table_name = {"dictionary_output", 17};
  bitmap_set_all(table.read_set);
  bitmap_clear_all(table.write_set);
  Prealloced_array<TABLE *, 4> tables(PSI_NOT_INSTRUMENTED);
  tables.push_back(&table);
  size_t calls = 0;
  auto source = NewIterator<DictionaryBatchRows>(server.thd(), server.thd()->mem_root, field.get(), &calls);
  VectorizedLimitIterator limit(server.thd(), std::move(source), 4, 1, nullptr, tables, 256 * 1024);
  ASSERT_FALSE(limit.Init());
  ASSERT_TRUE(limit.SupportsBatchRead());
  ASSERT_NE(nullptr, limit.Dictionary(field.get()));
  ASSERT_EQ(0, limit.Read());
  String buffer;
  String *value = field->val_str(&buffer, &buffer);
  ASSERT_NE(nullptr, value);
  EXPECT_EQ(0U, value->length());
  std::pmr::vector<ColumnChunk> chunks;
  chunks.emplace_back(field.get(), 2);
  size_t rows = 0;
  ASSERT_EQ(HA_ERR_END_OF_FILE, limit.ReadBatch(chunks, 2, rows));
  ASSERT_EQ(2U, rows);
  EXPECT_TRUE(chunks[0].nullable_fast(0));
  ASSERT_FALSE(limit.PushbackBatchTail(chunks, 1, rows));
  ASSERT_EQ(0, limit.Read());
  value = field->val_str(&buffer, &buffer);
  ASSERT_NE(nullptr, value);
  EXPECT_EQ("gamma", std::string(value->ptr(), value->length()));
  EXPECT_FALSE(bitmap_is_set(table.write_set, 0));
  EXPECT_EQ(-1, limit.Read());
  EXPECT_EQ(0U, calls);
}

TEST_F(RapidBatchPipeline, LimitSkipsChunksAndRetainsOnlyItsOwnOutputRange) {
  size_t calls = 0;
  VectorizedLimitIterator limit(server.thd(), Source(40, &calls), 12, 5, nullptr, Tables(), 256 * 1024);
  ASSERT_FALSE(limit.Init());
  ASSERT_TRUE(limit.SupportsBatchRead());
  auto chunks = Chunks(16);
  size_t rows = 0;
  EXPECT_EQ(HA_ERR_END_OF_FILE, limit.ReadBatch(chunks, 16, rows));
  ASSERT_EQ(7U, rows);
  EXPECT_EQ(6, Value(chunks[0], 0));
  EXPECT_EQ(12, Value(chunks[0], 6));
  EXPECT_EQ(-1, limit.Read());
  EXPECT_EQ(0U, calls);
}

TEST_F(RapidBatchPipeline, LimitUsesTheSameBatchCoreAcrossRowReadsAndPushback) {
  size_t calls = 0, inits = 0;
  ha_rows skipped = 0;
  VectorizedLimitIterator limit(server.thd(), Source(40, &calls, &inits), 22, 5, &skipped, Tables(), 256 * 1024);
  ASSERT_FALSE(limit.Init());
  ASSERT_EQ(1U, inits);
  ASSERT_EQ(0, limit.Read());
  EXPECT_EQ(35, right->field[0]->val_int());
  EXPECT_EQ(5U, skipped);
  auto chunks = Chunks(8);
  size_t rows = 0;
  ASSERT_EQ(0, limit.ReadBatch(chunks, 8, rows));
  ASSERT_EQ(8U, rows);
  for (size_t row = 0; row < 3; ++row) EXPECT_EQ(34 - row, Value(chunks[1], row));
  ASSERT_FALSE(limit.PushbackBatchTail(chunks, 3, rows));
  left->field[0]->store(999, false);
  right->field[0]->store(999, false);
  ASSERT_EQ(0, limit.Read());
  EXPECT_EQ(31, right->field[0]->val_int());
  std::vector<int32_t> remaining;
  for (;;) {
    for (auto &chunk : chunks) chunk.clear();
    const int result = limit.ReadBatch(chunks, 2, rows);
    ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
    for (size_t row = 0; row < rows; ++row) {
      remaining.push_back(Value(chunks[1], row));
      EXPECT_EQ(Value(chunks[1], row) == 23, chunks[0].nullable_fast(row));
    }
    if (result == HA_ERR_END_OF_FILE) break;
  }
  ASSERT_EQ(12U, remaining.size());
  for (size_t row = 0; row < remaining.size(); ++row) EXPECT_EQ(30 - row, remaining[row]);
  EXPECT_EQ(-1, limit.Read());
  EXPECT_EQ(0U, calls);
  EXPECT_EQ(5U, skipped);
  ASSERT_FALSE(limit.Init());
  ASSERT_EQ(0, limit.Read());
  EXPECT_EQ(35, right->field[0]->val_int());
  EXPECT_EQ(10U, skipped);
  EXPECT_EQ(2U, inits);
  EXPECT_EQ(0U, calls);
}

TEST_F(RapidBatchPipeline, LimitOffsetsAcrossBatchBoundariesAndPastEnd) {
  for (size_t offset : {0U, 1U, 1023U, 1024U, 1025U, 3999U, 4000U, 4001U}) {
    SCOPED_TRACE(offset);
    size_t calls = 0;
    ha_rows skipped = 0;
    VectorizedLimitIterator limit(server.thd(), Source(4000, &calls), offset + 17, offset, &skipped,
                                   Tables(), 256 * 1024);
    ASSERT_FALSE(limit.Init());
    auto chunks = Chunks(31);
    size_t total = 0, rows = 0;
    for (;;) {
      for (auto &chunk : chunks) chunk.clear();
      const int result = limit.ReadBatch(chunks, 31, rows);
      ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
      for (size_t row = 0; row < rows; ++row) EXPECT_EQ(4000 - offset - total - row, Value(chunks[1], row));
      total += rows;
      if (result == HA_ERR_END_OF_FILE) break;
    }
    EXPECT_EQ(offset < 4000 ? std::min<size_t>(17, 4000 - offset) : 0, total);
    EXPECT_EQ(std::min<size_t>(offset, 4000), skipped);
    EXPECT_EQ(0U, calls);
  }
}

TEST_F(RapidBatchPipeline, LimitNativeFallbackInitializesAndCountsOffsetOnce) {
  left->set_nullable();  // PipelineRows does not expose table-level NULL flags.
  size_t calls = 0, inits = 0;
  ha_rows skipped = 0;
  VectorizedLimitIterator limit(server.thd(), Source(40, &calls, &inits), 12, 5, &skipped, Tables(), 256 * 1024);
  ASSERT_FALSE(limit.Init());
  EXPECT_EQ(1U, inits);
  ASSERT_EQ(0, limit.Read());
  EXPECT_EQ(6, left->field[0]->val_int());
  auto chunks = Chunks(16);
  size_t rows = 0;
  ASSERT_EQ(HA_ERR_END_OF_FILE, limit.ReadBatch(chunks, 16, rows));
  ASSERT_EQ(6U, rows);
  for (size_t row = 0; row < rows; ++row) EXPECT_EQ(7 + row, Value(chunks[0], row));
  EXPECT_EQ(5U, skipped);
  EXPECT_EQ(12U, calls);
  ASSERT_FALSE(limit.Init());
  EXPECT_EQ(2U, inits);
}

TEST_F(RapidBatchPipeline, FilterRestoresNullComplementedNotNullMarkers) {
  left = std::make_unique<Fake_TABLE>(1, false);
  left->set_nullable();
  bitmap_set_all(left->read_set);
  bitmap_set_all(left->write_set);
  ASSERT_FALSE(left->field[0]->is_nullable());
  Item *condition = new Item_func_isnull(new Item_field(left->field[0]));
  ASSERT_FALSE(condition->fix_fields(server.thd(), &condition));
  size_t row_calls = 0;
  VectorizedFilterIterator filter(server.thd(), Source(40, &row_calls), condition);
  ASSERT_FALSE(filter.Init());
  auto chunks = Chunks(40);
  size_t rows = 0;
  const int result = filter.ReadBatch(chunks, 40, rows);
  ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
  ASSERT_EQ(3u, rows);
  for (size_t i = 0; i < rows; ++i) {
    EXPECT_TRUE(chunks[0].nullable_fast(i));
    EXPECT_EQ(40 - 17 * static_cast<int>(i), Value(chunks[1], i));
  }
  EXPECT_EQ(0u, row_calls);
}

TEST_F(RapidBatchPipeline, SelectedFilterChainsRetainPhysicalColumnsUntilDenseBoundary) {
  size_t calls = 0;
  Item *first = new Item_func_gt(new Item_field(right->field[0]), new Item_int(10));
  Item *second = new Item_func_lt(new Item_field(right->field[0]), new Item_int(20));
  ASSERT_FALSE(first->fix_fields(server.thd(), &first));
  ASSERT_FALSE(second->fix_fields(server.thd(), &second));
  auto child = NewIterator<VectorizedFilterIterator>(server.thd(), server.thd()->mem_root, Source(40, &calls), first);
  VectorizedFilterIterator filter(server.thd(), std::move(child), second);
  ASSERT_FALSE(filter.Init());
  auto chunks = Chunks(40);
  std::pmr::vector<uint32_t> selection;
  size_t rows = 0;
  const int result = filter.ReadSelectedBatch(chunks, 40, rows, selection);
  ASSERT_TRUE(result == 0 || result == HA_ERR_END_OF_FILE);
  ASSERT_EQ(9U, rows);
  ASSERT_EQ(rows, selection.size());
  EXPECT_EQ(40U, chunks[1].size());
  EXPECT_EQ(40, Value(chunks[1], 0));
  for (size_t logical = 0; logical < rows; ++logical) EXPECT_EQ(19 - logical, Value(chunks[1], selection[logical]));
  EXPECT_EQ(0U, filter.stats().bytes_copied);
  EXPECT_EQ(0U, filter.row_materializations());
  EXPECT_EQ(0U, calls);
  ASSERT_FALSE(filter.Init());
  for (auto &chunk : chunks) chunk.clear();
  ASSERT_EQ(HA_ERR_END_OF_FILE, filter.ReadBatch(chunks, 40, rows));
  ASSERT_EQ(9U, rows);
  ASSERT_EQ(9U, chunks[1].size());
  for (size_t row = 0; row < rows; ++row) EXPECT_EQ(19 - row, Value(chunks[1], row));
}

TEST_F(RapidBatchPipeline, CorrelatedFilterBindsOuterFieldsWithoutScalarReads) {
  auto parameter = std::make_unique<Fake_TABLE>(1, false);
  bitmap_set_all(parameter->read_set);
  bitmap_set_all(parameter->write_set);
  parameter->field[0]->store(35, false);
  Item *condition = new Item_func_gt(new Item_field(right->field[0]), new Item_field(parameter->field[0]));
  ASSERT_FALSE(condition->fix_fields(server.thd(), &condition));
  size_t calls = 0;
  VectorizedFilterIterator filter(server.thd(), Source(40, &calls), condition);
  ASSERT_FALSE(filter.Init());
  auto chunks = Chunks(40);
  std::pmr::vector<uint32_t> selection;
  size_t rows = 0;
  EXPECT_EQ(HA_ERR_END_OF_FILE, filter.ReadSelectedBatch(chunks, 40, rows, selection));
  ASSERT_EQ(5U, rows);
  for (size_t row = 0; row < rows; ++row) EXPECT_EQ(40 - row, Value(chunks[1], selection[row]));
  EXPECT_EQ(0U, calls);
  EXPECT_EQ(35, parameter->field[0]->val_int());
}

TEST_F(RapidBatchPipeline, FilterPushbackSurvivesScalarAndSmallBatchInterleaving) {
  size_t calls = 0;
  VectorizedFilterIterator filter(server.thd(), Source(40, &calls), nullptr);
  ASSERT_FALSE(filter.Init());
  auto chunks = Chunks(8);
  size_t rows = 0;
  ASSERT_EQ(0, filter.ReadBatch(chunks, 8, rows));
  ASSERT_EQ(8U, rows);
  ASSERT_FALSE(filter.PushbackBatchTail(chunks, 1, rows));
  ASSERT_EQ(0, filter.Read());
  EXPECT_EQ(2, left->field[0]->val_int());
  EXPECT_EQ(39, right->field[0]->val_int());
  for (auto &chunk : chunks) chunk.clear();
  ASSERT_EQ(0, filter.ReadBatch(chunks, 2, rows));
  ASSERT_EQ(2U, rows);
  EXPECT_EQ(3, Value(chunks[0], 0));
  EXPECT_EQ(4, Value(chunks[0], 1));
  ASSERT_FALSE(filter.PushbackBatchTail(chunks, 0, rows));
  for (int expected = 3; expected <= 8; ++expected) {
    ASSERT_EQ(0, filter.Read());
    EXPECT_EQ(expected, left->field[0]->val_int());
  }
  EXPECT_EQ(0U, calls);
}

}  // namespace
