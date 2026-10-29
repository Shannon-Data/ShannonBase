// Copyright (c) 2026, Shannon Data AI and/or its affiliates.
// SPDX-License-Identifier: GPL-2.0-only
#include <gtest/gtest.h>
#include "sql/iterators/timing_iterator.h"
#include "sql/item_cmpfunc.h"
#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/executor/iterators/nested_loop_iterator.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"
#include "storage/rapid_engine/utils/memory_pool.h"
#include "unittest/gunit/fake_table.h"
#include "unittest/gunit/test_utils.h"

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

class DataRows final : public RowIterator {
 public:
  DataRows(THD *thd, TABLE *table, Field *outer = nullptr) : RowIterator(thd), table(table), outer(outer) {}
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
  void UnlockRow() override {}

 private:
  TABLE *table;
  Field *outer;
  size_t index{0};
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
TEST_F(RapidBatchPipeline, LimitSkipsChunksAndRetainsOnlyItsOwnOutputRange) {
  size_t calls=0;
  VectorizedLimitIterator limit(server.thd(),Source(40,&calls),12,5,nullptr,Tables(),256*1024);
  ASSERT_FALSE(limit.Init()); ASSERT_TRUE(limit.SupportsBatchRead());
  auto chunks=Chunks(16); size_t rows=0;
  EXPECT_EQ(HA_ERR_END_OF_FILE,limit.ReadBatch(chunks,16,rows)); ASSERT_EQ(7U,rows);
  EXPECT_EQ(6,Value(chunks[0],0)); EXPECT_EQ(12,Value(chunks[0],6));
  EXPECT_EQ(-1,limit.Read()); EXPECT_EQ(0U,calls);
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

}  // namespace
