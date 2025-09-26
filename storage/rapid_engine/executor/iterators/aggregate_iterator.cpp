/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
/** The table scan iterator class for IMCS. All specific iterators are all inherited
 * from this.
 * vectorized/parallelized table scan iterator impl for rapid engine. In
 */
#include "storage/rapid_engine/executor/iterators/aggregate_iterator.h"

#include "include/my_base.h"

#include "sql/dd/cache/dictionary_client.h"
#include "sql/sql_class.h"
#include "sql/sql_optimizer.h"

#include "storage/innobase/include/dict0dd.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {
namespace Executor {
VectorizedAggregateIterator::VectorizedAggregateIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source,
                                                         JOIN *join, pack_rows::TableCollection tables, bool rollup,
                                                         double expected_rows)
    : RowIterator(thd),
      m_source(std::move(source)),
      m_join(join),
      m_rollup(rollup),
      m_tables(std::move(tables)),
      m_state(READING_FIRST_ROW),
      m_seen_eof(false),
      m_save_nullinfo(0),
      m_last_unchanged_group_item_idx(0),
      m_current_rollup_position(-1),
      m_output_slice(-1) {
  // Reserve buffers for row save/restore (identical to original)
  const size_t upper_data_length = ComputeRowSizeUpperBound(m_tables);
  m_first_row_this_group.reserve(upper_data_length);
  m_first_row_next_group.reserve(upper_data_length);

  // Calculate optimal batch size based on expected data
  if (expected_rows > 0) {
    // Conservative sizing for aggregation workloads
    size_t est_batch_size = static_cast<size_t>(std::min(expected_rows / 10.0, 2048.0));
    m_vectorizer.optimal_batch_size = std::clamp(est_batch_size, m_min_batch_size, m_max_batch_size);
  }

  InitializeVectorization();
}

bool VectorizedAggregateIterator::Init() {
  // Identical initialization to original AggregateIterator
  assert(!m_join->tmp_table_param.precomputed_group_by);

  m_current_rollup_position = -1;
  SetRollupLevel(INT_MAX);

  // Restore table buffers if re-executing
  if (!m_first_row_next_group.is_empty()) {
    LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_next_group.ptr()));
    m_first_row_next_group.length(0);
  }

  if (m_source->Init()) {
    return true;
  }

  // Set output slice for HAVING evaluation
  if (!(m_join->implicit_grouping || m_join->group_optimized_away) && !thd()->lex->using_hypergraph_optimizer()) {
    m_output_slice = m_join->get_ref_item_slice();
  }

  // Initialize state
  m_seen_eof = false;
  m_save_nullinfo = 0;
  m_last_unchanged_group_item_idx = 0;
  m_state = READING_FIRST_ROW;

  // Reset vectorization state
  m_vectorizer.reset();
  m_vectorizer.analysis_complete = false;

  // Clear stats
  m_stats = VectorizationStats{};

  return false;
}

int VectorizedAggregateIterator::Read() {
  switch (m_state) {
    case READING_FIRST_ROW: {
      // Read first row - identical to original logic
      int err = m_source->Read();
      if (err == -1) {
        m_seen_eof = true;
        m_state = DONE_OUTPUTTING_ROWS;

        if (m_join->grouped || m_join->group_optimized_away) {
          SetRollupLevel(m_join->send_group_parts);
          return -1;
        } else {
          // Handle no GROUP BY case - output single row with aggregate results
          for (Item *item : *m_join->get_current_fields()) {
            if (!item->hidden || (item->type() == Item::SUM_FUNC_ITEM &&
                                  down_cast<Item_sum *>(item)->aggr_query_block == m_join->query_block)) {
              item->no_rows_in_result();
            }
          }

          if (m_join->clear_fields(&m_save_nullinfo)) {
            return 1;
          }

          for (Item_sum **item = m_join->sum_funcs; *item != nullptr; ++item) {
            (*item)->clear();
          }

          if (m_output_slice != -1) {
            m_join->set_ref_item_slice(m_output_slice);
          }
          return 0;
        }
      }

      if (err != 0) return err;

      // Set initial group values
      (void)update_item_cache_if_changed(m_join->group_fields);
      StoreFromTableBuffers(m_tables, &m_first_row_next_group);
      m_last_unchanged_group_item_idx = 0;

      // Analyze aggregates for vectorization if not done yet
      if (!m_vectorizer.analysis_complete) {
        m_vectorizer.can_vectorize_current_group = AnalyzeAggregatesForVectorization();
        m_vectorizer.analysis_complete = true;
      }

      m_state = PROCESSING_CURRENT_GROUP;
    }
      [[fallthrough]];

    case PROCESSING_CURRENT_GROUP:
    case LAST_ROW_STARTED_NEW_GROUP: {
      SetRollupLevel(m_join->send_group_parts);

      // Swap and restore group row (identical to original)
      swap(m_first_row_this_group, m_first_row_next_group);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_group.ptr()));

      // Reset and initialize aggregates
      for (Item_sum **item = m_join->sum_funcs; *item != nullptr; ++item) {
        if (m_rollup) {
          if (down_cast<Item_rollup_sum_switcher *>(*item)->reset_and_add_for_rollup(m_last_unchanged_group_item_idx))
            return 1;
        } else {
          if ((*item)->reset_and_add()) return 1;
        }
      }

      // Choose processing method based on analysis
      int result;
      if (m_vectorization_enabled && m_vectorizer.can_vectorize_current_group &&
          !m_rollup) {  // Don't vectorize rollup initially
        result = ProcessCurrentGroupVectorized();
        if (result != 0) {
          // Fall back to traditional on error
          m_stats.traditional_fallbacks++;
          result = ProcessCurrentGroupTraditional();
        }
      } else {
        result = ProcessCurrentGroupTraditional();
        m_stats.traditional_fallbacks++;
      }

      if (result != 0) return result;

      // Handle group completion - identical to original logic
      if (m_seen_eof) {
        // End of input - finalize current group
        StoreFromTableBuffers(m_tables, &m_first_row_next_group);
        LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_group.ptr()));

        if (m_rollup && m_join->send_group_parts > 0) {
          SetRollupLevel(m_join->send_group_parts);
          m_last_unchanged_group_item_idx = 0;
          m_state = OUTPUTTING_ROLLUP_ROWS;
        } else {
          SetRollupLevel(m_join->send_group_parts);
          m_state = DONE_OUTPUTTING_ROWS;
        }
      } else {
        // More groups to process
        m_state = LAST_ROW_STARTED_NEW_GROUP;
      }

      if (m_output_slice != -1) {
        m_join->set_ref_item_slice(m_output_slice);
      }
      return 0;
    }

    case OUTPUTTING_ROLLUP_ROWS: {
      // Identical rollup logic to original
      SetRollupLevel(m_current_rollup_position - 1);

      if (m_current_rollup_position <= m_last_unchanged_group_item_idx) {
        if (m_seen_eof) {
          m_state = DONE_OUTPUTTING_ROWS;
        } else {
          m_state = LAST_ROW_STARTED_NEW_GROUP;
        }
      }

      if (m_output_slice != -1) {
        m_join->set_ref_item_slice(m_output_slice);
      }
      return 0;
    }

    case DONE_OUTPUTTING_ROWS: {
      // Cleanup - identical to original
      if (m_save_nullinfo != 0) {
        m_join->restore_fields(m_save_nullinfo);
        m_save_nullinfo = 0;
      }
      SetRollupLevel(INT_MAX);

      // Log performance metrics periodically
      if (m_stats.total_batches_processed > 0 && m_stats.total_batches_processed % 100 == 0) {
        LogPerformanceMetrics();
      }

      return -1;
    }
  }

  assert(false);
  return 1;
}

int VectorizedAggregateIterator::ProcessCurrentGroupTraditional() {
  // Identical to original AggregateIterator group processing
  for (;;) {
    int err = m_source->Read();
    if (err == 1) return 1;  // Error

    if (err == -1) {
      // EOF
      m_seen_eof = true;
      break;
    }

    // Check for group change using original logic
    int first_changed_idx = update_item_cache_if_changed(m_join->group_fields);
    if (first_changed_idx >= 0) {
      // Group changed - save new row and break
      StoreFromTableBuffers(m_tables, &m_first_row_next_group);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_group.ptr()));

      // Handle rollup state
      if (m_rollup) {
        m_last_unchanged_group_item_idx = first_changed_idx + 1;
        if (static_cast<unsigned>(first_changed_idx) < m_join->send_group_parts - 1) {
          SetRollupLevel(m_join->send_group_parts);
          m_state = OUTPUTTING_ROLLUP_ROWS;
        } else {
          SetRollupLevel(m_join->send_group_parts);
          m_state = LAST_ROW_STARTED_NEW_GROUP;
        }
      } else {
        m_last_unchanged_group_item_idx = 0;
        m_state = LAST_ROW_STARTED_NEW_GROUP;
      }
      break;
    }

    // Add current row to aggregates
    for (Item_sum **item = m_join->sum_funcs; *item != nullptr; ++item) {
      if (m_rollup) {
        if (down_cast<Item_rollup_sum_switcher *>(*item)->aggregator_add_all()) {
          return 1;
        }
      } else {
        if ((*item)->aggregator_add()) {
          return 1;
        }
      }
    }
  }

  return 0;
}

int VectorizedAggregateIterator::ProcessCurrentGroupVectorized() {
  auto start_time = std::chrono::high_resolution_clock::now();

  // Setup column chunks if needed
  if (!m_vectorizer.current_batch.initialized) {
    SetupColumnChunks();
  }

  // Read rows into batch
  int batch_result = ReadRowsIntoCurrentBatch();
  if (batch_result != 0) {
    return batch_result;
  }

  // Process vectorized aggregates
  int agg_result = ProcessVectorizedAggregates();
  if (agg_result != 0) {
    return agg_result;
  }

  // Update performance metrics
  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
  double processing_time = duration.count() / 1000.0;  // Convert to ms

  UpdateBatchSizeFromPerformance(processing_time);

  m_stats.total_batches_processed++;
  m_stats.total_rows_vectorized += m_vectorizer.current_batch.row_count;
  m_stats.total_vectorized_time_ms += processing_time;
  m_stats.avg_batch_processing_time_ms =
      (m_stats.avg_batch_processing_time_ms * (m_stats.total_batches_processed - 1) + processing_time) /
      m_stats.total_batches_processed;

  return 0;
}

int VectorizedAggregateIterator::ReadRowsIntoCurrentBatch() {
  m_vectorizer.current_batch.clear();
  size_t rows_read = 0;
  size_t batch_capacity = m_vectorizer.optimal_batch_size;

  for (;;) {
    // Don't exceed batch capacity
    if (rows_read >= batch_capacity) {
      break;
    }

    int err = m_source->Read();
    if (err == 1) return 1;  // Error

    if (err == -1) {
      // EOF
      m_seen_eof = true;
      break;
    }

    // Check for group change
    int first_changed_idx = update_item_cache_if_changed(m_join->group_fields);
    if (first_changed_idx >= 0) {
      // Group changed - save new row and break
      StoreFromTableBuffers(m_tables, &m_first_row_next_group);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_group.ptr()));

      // Set rollup state
      if (m_rollup) {
        m_last_unchanged_group_item_idx = first_changed_idx + 1;
        if (static_cast<unsigned>(first_changed_idx) < m_join->send_group_parts - 1) {
          SetRollupLevel(m_join->send_group_parts);
          m_state = OUTPUTTING_ROLLUP_ROWS;
        } else {
          SetRollupLevel(m_join->send_group_parts);
          m_state = LAST_ROW_STARTED_NEW_GROUP;
        }
      } else {
        m_last_unchanged_group_item_idx = 0;
        m_state = LAST_ROW_STARTED_NEW_GROUP;
      }
      break;
    }

    // Store row data in column chunks
    for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
      const auto &agg_info = m_vectorizer.aggregate_infos[i];
      if (agg_info.vectorizable && agg_info.source_field) {
        auto &chunk = m_vectorizer.current_batch.column_chunks[i];

        Field *field = agg_info.source_field;
        if (field->is_null()) {
          if (!chunk.add(nullptr, 0, true)) {
            // Chunk full - break and process what we have
            break;
          }
        } else {
          const uchar *data = field->data_ptr();
          size_t data_len = field->pack_length();
          if (!chunk.add(const_cast<uchar *>(data), data_len, false)) {
            // Chunk full - break and process what we have
            break;
          }
        }
      }
    }

    rows_read++;
  }

  m_vectorizer.current_batch.row_count = rows_read;
  return 0;
}

int VectorizedAggregateIterator::ProcessVectorizedAggregates() {
  // Group aggregates by type for efficient vectorized processing
  std::vector<size_t> count_indices, sum_indices, minmax_indices, avg_indices;

  for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
    const auto &info = m_vectorizer.aggregate_infos[i];
    if (!info.vectorizable) continue;

    switch (info.type) {
      case Item_sum::COUNT_FUNC:
      case Item_sum::COUNT_DISTINCT_FUNC:
        count_indices.push_back(i);
        break;
      case Item_sum::SUM_FUNC:
        sum_indices.push_back(i);
        break;
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
        minmax_indices.push_back(i);
        break;
      case Item_sum::AVG_FUNC:
        avg_indices.push_back(i);
        break;
      default:
        // Fall back to row-by-row for this aggregate
        for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
          // Restore row and process traditionally
          RestoreRowFromBatch(row, i);
          if (info.item->aggregator_add()) {
            return 1;
          }
        }
        break;
    }
  }

  // Process each aggregate type in vectorized batches
  if (!count_indices.empty()) {
    int result = ProcessCountAggregates(count_indices);
    if (result != 0) return result;
  }

  if (!sum_indices.empty()) {
    int result = ProcessSumAggregates(sum_indices);
    if (result != 0) return result;
  }

  if (!minmax_indices.empty()) {
    int result = ProcessMinMaxAggregates(minmax_indices);
    if (result != 0) return result;
  }

  if (!avg_indices.empty()) {
    int result = ProcessAvgAggregates(avg_indices);
    if (result != 0) return result;
  }

  return 0;
}

int VectorizedAggregateIterator::ProcessCountAggregates(const std::vector<size_t> &count_indices) {
  for (size_t idx : count_indices) {
    const auto &info = m_vectorizer.aggregate_infos[idx];
    auto &chunk = m_vectorizer.current_batch.column_chunks[idx];

    // Count non-null values vectorized
    size_t non_null_count = ColumnChunkOper::CountNonNull(chunk, m_vectorizer.current_batch.row_count);

    // Add the count to the aggregate
    Item_sum_count *count_item = down_cast<Item_sum_count *>(info.item);
    count_item->add_value(non_null_count);
  }

  return 0;
}

int VectorizedAggregateIterator::ProcessSumAggregates(const std::vector<size_t> &sum_indices) {
  for (size_t idx : sum_indices) {
    const auto &info = m_vectorizer.aggregate_infos[idx];
    auto &chunk = m_vectorizer.current_batch.column_chunks[idx];

    // Vectorized summation based on field type
    Field *field = info.source_field;

    switch (field->type()) {
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG: {
        int64_t sum = 0;
        for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
          if (!chunk.nullable(row)) {
            const uchar *data = chunk.data(row);
            int64_t val = Utils::Util::get_field_numeric<int64_t>(field, data, nullptr);
            sum += val;
          }
        }

        // Add vectorized sum to aggregate
        Item_sum_sum *sum_item = down_cast<Item_sum_sum *>(info.item);
        sum_item->add_value(sum);
        break;
      }

      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_FLOAT: {
        double sum = 0.0;
        for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
          if (!chunk.nullable(row)) {
            const uchar *data = chunk.data(row);
            double val = Utils::Util::get_field_numeric<double>(field, data, nullptr);
            sum += val;
          }
        }

        Item_sum_sum *sum_item = down_cast<Item_sum_sum *>(info.item);
        sum_item->add_value(sum);
        break;
      }

      default:
        // Fall back to row-by-row for complex types
        for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
          RestoreRowFromBatch(row, idx);
          if (info.item->aggregator_add()) {
            return 1;
          }
        }
        break;
    }
  }

  return 0;
}

int VectorizedAggregateIterator::ProcessMinMaxAggregates(const std::vector<size_t> &minmax_indices) {
  for (size_t idx : minmax_indices) {
    const auto &info = m_vectorizer.aggregate_infos[idx];
    auto &chunk = m_vectorizer.current_batch.column_chunks[idx];

    // Process min/max vectorized
    for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
      if (!chunk.nullable(row)) {
        RestoreRowFromBatch(row, idx);
        if (info.item->aggregator_add()) {
          return 1;
        }
      }
    }
  }

  return 0;
}

int VectorizedAggregateIterator::ProcessAvgAggregates(const std::vector<size_t> &avg_indices) {
  // AVG is more complex - needs both sum and count
  // For now, fall back to traditional processing
  for (size_t idx : avg_indices) {
    const auto &info = m_vectorizer.aggregate_infos[idx];

    for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
      RestoreRowFromBatch(row, idx);
      if (info.item->aggregator_add()) {
        return 1;
      }
    }
  }

  return 0;
}

void VectorizedAggregateIterator::RestoreRowFromBatch(size_t row_idx, size_t agg_idx) {
  if (agg_idx >= m_vectorizer.aggregate_infos.size() || row_idx >= m_vectorizer.current_batch.row_count) {
    return;
  }

  const auto &info = m_vectorizer.aggregate_infos[agg_idx];
  auto &chunk = m_vectorizer.current_batch.column_chunks[agg_idx];

  if (chunk.nullable(row_idx)) {
    info.source_field->set_null();
  } else {
    info.source_field->set_notnull();
    const uchar *data = chunk.data(row_idx);
    size_t data_len = chunk.width();

    // Copy data back to field
    memcpy((void *)info.source_field->data_ptr(), data, data_len);
  }
}

bool VectorizedAggregateIterator::AnalyzeAggregatesForVectorization() {
  m_vectorizer.aggregate_infos.clear();

  bool any_vectorizable = false;

  for (Item_sum **item = m_join->sum_funcs; *item != nullptr; ++item) {
    VectorizedGroupProcessor::AggregateInfo info;
    info.item = *item;
    info.type = (*item)->sum_func();
    info.vectorizable = IsSimpleAggregate(*item);
    info.source_field = GetPrimaryFieldForAggregate(*item);
    info.field_index = m_vectorizer.aggregate_infos.size();

    m_vectorizer.aggregate_infos.push_back(info);

    if (info.vectorizable) {
      any_vectorizable = true;
    }
  }

  return any_vectorizable;
}

bool VectorizedAggregateIterator::IsSimpleAggregate(Item_sum *item) const {
  // Only vectorize simple, common aggregate functions
  switch (item->sum_func()) {
    case Item_sum::COUNT_FUNC:
    case Item_sum::COUNT_DISTINCT_FUNC:
    case Item_sum::SUM_FUNC:
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC:
      return true;
    case Item_sum::AVG_FUNC:
      // AVG can be vectorized but is more complex
      return true;
    default:
      return false;
  }
}

Field *VectorizedAggregateIterator::GetPrimaryFieldForAggregate(Item_sum *item) const {
  // Extract the primary field used by this aggregate function
  if (item->arg_count > 0) {
    Item *arg = item->get_arg(0);
    if (arg->type() == Item::FIELD_ITEM) {
      return down_cast<Item_field *>(arg)->field;
    }
  }
  return nullptr;
}

void VectorizedAggregateIterator::SetupColumnChunks() {
  if (m_vectorizer.current_batch.initialized) {
    return;
  }

  m_vectorizer.current_batch.column_chunks.clear();
  m_vectorizer.current_batch.capacity = m_vectorizer.optimal_batch_size;

  // Create column chunks for vectorizable aggregates
  for (const auto &info : m_vectorizer.aggregate_infos) {
    if (info.vectorizable && info.source_field) {
      m_vectorizer.current_batch.column_chunks.emplace_back(info.source_field, m_vectorizer.current_batch.capacity);
    } else {
      // Placeholder chunk for non-vectorizable aggregates
      m_vectorizer.current_batch.column_chunks.emplace_back(nullptr, m_vectorizer.current_batch.capacity);
    }
  }

  m_vectorizer.current_batch.initialized = true;
}

void VectorizedAggregateIterator::UpdateBatchSizeFromPerformance(double processing_time_ms) {
  // Store recent timing
  m_vectorizer.recent_processing_times[m_vectorizer.time_index] = processing_time_ms;
  m_vectorizer.time_index = (m_vectorizer.time_index + 1) % 10;

  // Calculate average of recent timings
  double avg_time = 0.0;
  for (int i = 0; i < 10; ++i) {
    avg_time += m_vectorizer.recent_processing_times[i];
  }
  avg_time /= 10.0;

  // Adapt batch size based on performance
  if (m_stats.total_batches_processed % 10 == 0 && m_stats.total_batches_processed > 10) {
    if (avg_time > m_target_batch_time_ms * 2.0) {
      // Too slow, reduce batch size
      m_vectorizer.optimal_batch_size = std::max(m_vectorizer.optimal_batch_size / 2, m_min_batch_size);
    } else if (avg_time < m_target_batch_time_ms && m_vectorizer.optimal_batch_size < m_max_batch_size) {
      // Fast enough, try larger batches
      m_vectorizer.optimal_batch_size = std::min(m_vectorizer.optimal_batch_size * 2, m_max_batch_size);
    }
  }
}

void VectorizedAggregateIterator::LogPerformanceMetrics() {
  sql_print_information(
      "VectorizedAggregateIterator Performance: "
      "Batches=%zu, VectorizedRows=%zu, Fallbacks=%zu, "
      "AvgBatchTime=%.2fms, TotalVectorizedTime=%.2fms",
      m_stats.total_batches_processed, m_stats.total_rows_vectorized, m_stats.traditional_fallbacks,
      m_stats.avg_batch_processing_time_ms, m_stats.total_vectorized_time_ms);
}

void VectorizedAggregateIterator::InitializeVectorization() {
  // Enable vectorization by default, but allow system variable override
  m_vectorization_enabled = true;
}

void VectorizedAggregateIterator::SetNullRowFlag(bool is_null_row) { m_source->SetNullRowFlag(is_null_row); }

void VectorizedAggregateIterator::StartPSIBatchMode() { m_source->StartPSIBatchMode(); }

void VectorizedAggregateIterator::EndPSIBatchModeIfStarted() { m_source->EndPSIBatchModeIfStarted(); }

void VectorizedAggregateIterator::UnlockRow() {
  // Same as original AggregateIterator - can't unlock aggregated rows
  // Most likely, HAVING failed. Ideally, we'd like to backtrack and
  // unlock all rows that went into this aggregate, but we can't do that,
  // and we also can't unlock the _current_ row, since that belongs to a
  // different group. Thus, do nothing.
}

void VectorizedAggregateIterator::SetRollupLevel(int level) {
  // Identical to original implementation
  if (m_rollup && m_current_rollup_position != level) {
    m_current_rollup_position = level;
    for (Item_rollup_group_item *item : m_join->rollup_group_items) {
      item->set_current_rollup_level(level);
    }
    for (Item_rollup_sum_switcher *item : m_join->rollup_sums) {
      item->set_current_rollup_level(level);
    }
  }
}

}  // namespace Executor
}  // namespace ShannonBase