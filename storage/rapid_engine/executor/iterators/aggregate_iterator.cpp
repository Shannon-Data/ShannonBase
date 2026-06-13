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
      m_last_unchanged_grp_item_idx(0),
      m_current_rollup_pos(-1),
      m_output_slice(-1) {
  // Reserve buffers for row save/restore (identical to original)
  const size_t upper_data_length = ComputeRowSizeUpperBound(m_tables);
  m_first_row_this_grp.reserve(upper_data_length);
  m_first_row_next_grp.reserve(upper_data_length);

  // Calculate optimal batch size based on expected data
  if (expected_rows > 0) {
    size_t est_batch_size = static_cast<size_t>(std::min(expected_rows / 10.0, 2048.0));
    m_vectorizer.opt_batch_size = std::clamp(est_batch_size, m_min_batch_size, m_max_batch_size);
  }

  InitializeVectorization();
}

bool VectorizedAggregateIterator::Init() {
  // Identical initialization to original AggregateIterator
  assert(!m_join->tmp_table_param.precomputed_group_by);

  m_current_rollup_pos = -1;
  SetRollupLevel(INT_MAX);

  // Restore table buffers if re-executing (e.g. correlated subquery)
  if (!m_first_row_next_grp.is_empty()) {
    LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_next_grp.ptr()));
    m_first_row_next_grp.length(0);
  }

  if (m_source->Init()) return true;

  // Probe : does the source implement BatchReadable?
  m_batch_source = dynamic_cast<BatchReadable *>(m_source.get());
  m_source_supports_batch = (m_batch_source != nullptr);

  // Set output slice for HAVING evaluation
  if (!(m_join->implicit_grouping || m_join->group_optimized_away) && !thd()->lex->using_hypergraph_optimizer()) {
    m_output_slice = m_join->get_ref_item_slice();
  }

  // Initialize state
  m_seen_eof = false;
  m_save_nullinfo = 0;
  m_last_unchanged_grp_item_idx = 0;
  m_state = READING_FIRST_ROW;

  // Reset vectorization state
  m_vectorizer.reset();
  m_vectorizer.analysis_complete = false;
  m_batch_chunks_initialized = false;

  // Clear stats
  m_stats = VectorizationStats{};

  return false;
}

int VectorizedAggregateIterator::Read() {
  switch (m_state) {
    case READING_FIRST_ROW: {
      int err = m_source->Read();
      if (err == -1) {
        m_seen_eof = true;
        m_state = DONE_OUTPUTTING_ROWS;

        if (m_join->grouped || m_join->group_optimized_away) {
          SetRollupLevel(m_join->send_group_parts);
          return -1;
        } else {
          // No GROUP BY — output a single row with aggregate results for zero input rows
          for (Item *item : *m_join->get_current_fields()) {
            if (!item->hidden || (item->type() == Item::SUM_FUNC_ITEM &&
                                  down_cast<Item_sum *>(item)->aggr_query_block == m_join->query_block)) {
              item->no_rows_in_result();
            }
          }
          if (m_join->clear_fields(&m_save_nullinfo)) return 1;
          for (Item_sum **item = m_join->sum_funcs; *item != nullptr; ++item) (*item)->clear();
          if (m_output_slice != -1) m_join->set_ref_item_slice(m_output_slice);
          return 0;
        }
      }

      if (err != 0) return err;

      // Set initial group values
      (void)update_item_cache_if_changed(m_join->group_fields);
      StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
      m_last_unchanged_grp_item_idx = 0;

      if (!m_vectorizer.analysis_complete) {
        m_vectorizer.can_vectorize_curr_grp = AnalyzeAggregatesForVectorization();
        m_vectorizer.analysis_complete = true;
      }

      m_state = PROCESSING_CURRENT_GROUP;
    }
      [[fallthrough]];

    case PROCESSING_CURRENT_GROUP:
    case LAST_ROW_STARTED_NEW_GROUP: {
      SetRollupLevel(m_join->send_group_parts);
      swap(m_first_row_this_grp, m_first_row_next_grp);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));

      for (Item_sum **item = m_join->sum_funcs; *item != nullptr; ++item) {
        if (m_rollup) {
          if (down_cast<Item_rollup_sum_switcher *>(*item)->reset_and_add_for_rollup(m_last_unchanged_grp_item_idx))
            return 1;
        } else {
          if ((*item)->reset_and_add()) return 1;
        }
      }

      int result = ProcessCurrentGroupTraditional();
      if (result != 0) return result;
      if (m_output_slice != -1) m_join->set_ref_item_slice(m_output_slice);
      return 0;
    } break;
    case OUTPUTTING_ROLLUP_ROWS: {
      SetRollupLevel(m_current_rollup_pos - 1);
      if (m_current_rollup_pos <= m_last_unchanged_grp_item_idx) {
        m_state = m_seen_eof ? DONE_OUTPUTTING_ROWS : LAST_ROW_STARTED_NEW_GROUP;
      }
      if (m_output_slice != -1) m_join->set_ref_item_slice(m_output_slice);
      return 0;
    } break;
    case DONE_OUTPUTTING_ROWS: {
      if (m_save_nullinfo != 0) {
        m_join->restore_fields(m_save_nullinfo);
        m_save_nullinfo = 0;
      }
      SetRollupLevel(INT_MAX);
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
  if (m_source_supports_batch && m_vectorization_enabled && m_vectorizer.can_vectorize_curr_grp && !m_rollup)
    return ProcessGroupBatchVectorized();

  if (m_vectorization_enabled && m_vectorizer.can_vectorize_curr_grp && !m_rollup) return ProcessGroupRowVectorized();

  return ProcessGroupScalar();
}

int VectorizedAggregateIterator::ProcessGroupBatchVectorized() {
  assert(m_batch_source != nullptr);
  if (!m_batch_chunks_initialized) SetupBatchChunks();

  for (;;) {
    for (auto &c : m_batch_col_chunks) c.clear();

    size_t rows_read = 0;
    int err = m_batch_source->ReadBatch(m_batch_col_chunks, m_vectorizer.opt_batch_size, rows_read);
    const bool is_eof = (err == HA_ERR_END_OF_FILE);
    if (err != 0 && !is_eof) return 1;

    if (rows_read == 0 && is_eof) {
      // Table is empty or the very last call returned nothing.
      m_seen_eof = true;
      StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
      SetRollupLevel(m_join->send_group_parts);
      m_state = DONE_OUTPUTTING_ROWS;
      break;
    }

    // For global aggregates (group_fields empty) there is no boundary —
    // all rows belong to one group.  Skip the loop entirely.
    // For grouped aggregates, walk rows one-by-one restoring only the
    // key fields to table->field so update_item_cache_if_changed() can
    // compare them.  This is cheap: boundaries are rare (one per group).
    size_t boundary = rows_read;  // default: all rows in this batch → current group
    if (!m_join->group_fields.is_empty()) {
      for (size_t row = 0; row < rows_read; ++row) {
        RestoreGroupKeyField(row);
        if (update_item_cache_if_changed(m_join->group_fields) >= 0) {
          boundary = row;
          break;
        }
      }
    }

    // SIMD-accumulate rows [0, boundary).
    if (boundary > 0) {
      if (ProcessVectorizedAggregates(m_batch_col_chunks, boundary) != 0) return 1;
      m_stats.total_batches_processed++;
      m_stats.total_rows_vectorized += boundary;
    }

    if (boundary < rows_read) {
      // GROUP BY boundary found mid-batch.
      // Push rows [boundary, rows_read) back into source so they are
      // re-delivered as the first rows of the next group's ReadBatch().
      m_batch_source->PushbackBatchTail(m_batch_col_chunks, boundary, rows_read);

      // Reconstruct table->field for the boundary row so StoreFromTableBuffers
      // captures it as the next group's first row.
      // Key fields were already written by RestoreGroupKeyField(boundary).
      // Now also restore the aggregate-field values.
      RestoreGroupKeyField(boundary);
      for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
        const auto &agg_info = m_vectorizer.aggregate_infos[i];
        if (!agg_info.source_field) continue;
        uint fld_idx = agg_info.source_field->field_index();
        if (fld_idx < m_batch_col_chunks.size() && m_batch_col_chunks[fld_idx].valid() &&
            boundary < m_batch_col_chunks[fld_idx].size()) {
          bool is_null = m_batch_col_chunks[fld_idx].nullable(boundary);
          if (is_null) {
            agg_info.source_field->set_null();
          } else {
            agg_info.source_field->set_notnull();
            memcpy(agg_info.source_field->field_ptr(), m_batch_col_chunks[fld_idx].data(boundary),
                   m_batch_col_chunks[fld_idx].width());
          }
        }
      }
      StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
      m_last_unchanged_grp_item_idx = 0;
      m_state = LAST_ROW_STARTED_NEW_GROUP;
      return 0;
    }

    if (is_eof) {
      m_seen_eof = true;
      StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
      SetRollupLevel(m_join->send_group_parts);
      m_state = DONE_OUTPUTTING_ROWS;
      break;
    }
    // Not EOF, no boundary: continue reading the next batch for the same group.
  }
  return 0;
}

int VectorizedAggregateIterator::ProcessGroupRowVectorized() {
  if (!m_vectorizer.current_batch.initialized) SetupColumnChunks();

  for (;;) {
    int err = m_source->Read();
    if (err == 1) return 1;

    if (err == -1) {
      // EOF: flush remaining rows, then mark end of output.
      m_seen_eof = true;
      if (m_vectorizer.current_batch.row_count > 0) {
        if (ProcessVectorizedAggregates() != 0) return 1;
        m_stats.total_batches_processed++;
        m_stats.total_rows_vectorized += m_vectorizer.current_batch.row_count;
        m_vectorizer.current_batch.clear();
      }
      StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
      SetRollupLevel(m_join->send_group_parts);
      m_state = DONE_OUTPUTTING_ROWS;
      break;
    }

    int first_changed_idx = update_item_cache_if_changed(m_join->group_fields);
    if (first_changed_idx >= 0) {
      // GROUP BY boundary: flush accumulated rows first.
      if (m_vectorizer.current_batch.row_count > 0) {
        if (ProcessVectorizedAggregates() != 0) return 1;
        m_stats.total_batches_processed++;
        m_stats.total_rows_vectorized += m_vectorizer.current_batch.row_count;
        m_vectorizer.current_batch.clear();
      }
      StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
      // rollup is not active here (guarded by caller)
      m_last_unchanged_grp_item_idx = 0;
      m_state = LAST_ROW_STARTED_NEW_GROUP;
      return 0;
    }

    // Row belongs to the current group — append to batch.
    AppendCurrentRowToChunks();

    // Flush when the batch is full.
    if (m_vectorizer.current_batch.full()) {
      if (ProcessVectorizedAggregates() != 0) return 1;
      m_stats.total_batches_processed++;
      m_stats.total_rows_vectorized += m_vectorizer.current_batch.row_count;
      m_vectorizer.current_batch.clear();
    }
  }
  return 0;
}

int VectorizedAggregateIterator::ProcessGroupScalar() {
  for (;;) {
    int err = m_source->Read();
    if (err == 1) return 1;

    if (err == -1) {
      m_seen_eof = true;
      StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
      if (m_rollup && m_join->send_group_parts > 0) {
        SetRollupLevel(m_join->send_group_parts);
        m_last_unchanged_grp_item_idx = 0;
        m_state = OUTPUTTING_ROLLUP_ROWS;
      } else {
        SetRollupLevel(m_join->send_group_parts);
        m_state = DONE_OUTPUTTING_ROWS;
      }
      break;
    }

    int first_changed_idx = update_item_cache_if_changed(m_join->group_fields);
    if (first_changed_idx >= 0) {
      StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
      if (m_rollup) {
        m_last_unchanged_grp_item_idx = first_changed_idx + 1;
        m_state = (static_cast<unsigned>(first_changed_idx) < m_join->send_group_parts - 1)
                      ? OUTPUTTING_ROLLUP_ROWS
                      : LAST_ROW_STARTED_NEW_GROUP;
        SetRollupLevel(m_join->send_group_parts);
      } else {
        m_last_unchanged_grp_item_idx = 0;
        m_state = LAST_ROW_STARTED_NEW_GROUP;
      }
      break;
    }

    for (Item_sum **item = m_join->sum_funcs; *item != nullptr; ++item) {
      if (m_rollup) {
        if (down_cast<Item_rollup_sum_switcher *>(*item)->aggregator_add_all()) return 1;
      } else {
        if ((*item)->aggregator_add()) return 1;
      }
    }
  }
  return 0;
}

void VectorizedAggregateIterator::InitializeVectorization() { m_vectorization_enabled = true; }

int VectorizedAggregateIterator::ProcessVectorizedAggregates() {
  if (m_vectorizer.current_batch.row_count == 0) return 0;

  std::vector<size_t> count_indices, sum_indices, minmax_indices, avg_indices;

  for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
    const auto &info = m_vectorizer.aggregate_infos[i];
    if (!info.vectorizable) continue;
    switch (info.type) {
      case Item_sum::COUNT_FUNC:
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
        // Unknown aggregate type: fall back row-by-row for this item only.
        for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
          RestoreRowFromBatch(row, i);
          if (info.item->aggregator_add()) return 1;
        }
        break;
    }
  }

  if (!count_indices.empty() && ProcessCountAggregates(count_indices) != 0) return 1;
  if (!sum_indices.empty() && ProcessSumAggregates(sum_indices) != 0) return 1;
  if (!minmax_indices.empty() && ProcessMinMaxAggregates(minmax_indices) != 0) return 1;
  if (!avg_indices.empty() && ProcessAvgAggregates(avg_indices) != 0) return 1;

  return 0;
}

int VectorizedAggregateIterator::ProcessVectorizedAggregates(const std::vector<ColumnChunk> &col_chunks,
                                                             size_t row_count) {
  if (row_count == 0) return 0;

  std::vector<size_t> count_indices, sum_indices, minmax_indices, avg_indices;
  for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
    const auto &info = m_vectorizer.aggregate_infos[i];
    if (!info.vectorizable) continue;
    switch (info.type) {
      case Item_sum::COUNT_FUNC:
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
        // find the chunk for this agg via field_index
        if (info.source_field) {
          uint fld_idx = info.source_field->field_index();
          if (fld_idx < col_chunks.size()) {
            for (size_t row = 0; row < row_count; ++row) {
              // restore field from col_chunks[fld_idx] row
              const ColumnChunk &chunk = col_chunks[fld_idx];
              if (chunk.nullable(row)) {
                info.source_field->set_null();
              } else {
                info.source_field->set_notnull();
                memcpy(info.source_field->field_ptr(), chunk.data(row), chunk.width());
              }
              if (info.item->aggregator_add()) return 1;
            }
          }
        }
        break;
    }
  }

  for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
    const auto &info = m_vectorizer.aggregate_infos[i];
    if (!info.vectorizable || !info.source_field) continue;
    uint fld_idx = info.source_field->field_index();
    if (fld_idx < col_chunks.size() && col_chunks[fld_idx].valid()) {
      m_vectorizer.current_batch.column_chunks[i] = col_chunks[fld_idx];
      // m_vectorizer.current_batch.column_chunks[i].set_row_count(row_count);
    }
  }
  m_vectorizer.current_batch.row_count = row_count;
  return ProcessVectorizedAggregates();
}

int VectorizedAggregateIterator::ProcessCountAggregates(const std::vector<size_t> &count_indices) {
  for (size_t idx : count_indices) {
    const auto &info = m_vectorizer.aggregate_infos[idx];
    auto &chunk = m_vectorizer.current_batch.column_chunks[idx];
    size_t non_null_count = ColumnChunkOper::CountNonNull(chunk, m_vectorizer.current_batch.row_count);
    down_cast<Item_sum_count *>(info.item)->add_value(non_null_count);
  }
  return 0;
}

int VectorizedAggregateIterator::ProcessSumAggregates(const std::vector<size_t> &sum_indices) {
  for (size_t idx : sum_indices) {
    const auto &info = m_vectorizer.aggregate_infos[idx];
    const auto &chunk = m_vectorizer.current_batch.column_chunks[idx];
    Item_sum_sum *sum_item = down_cast<Item_sum_sum *>(info.item);
    Field *field = info.source_field;
    switch (field->type()) {
      case MYSQL_TYPE_LONG: {
        int64_t sum = ColumnChunkOper::Sum<int32_t>(chunk, m_vectorizer.current_batch.row_count);
        sum_item->add_value(sum);
      } break;
      case MYSQL_TYPE_LONGLONG: {
        int64_t sum = ColumnChunkOper::Sum<int64_t>(chunk, m_vectorizer.current_batch.row_count);
        sum_item->add_value(sum);
      } break;
      case MYSQL_TYPE_FLOAT: {
        double sum = ColumnChunkOper::Sum<float>(chunk, m_vectorizer.current_batch.row_count);
        sum_item->add_value(sum);
      } break;
      case MYSQL_TYPE_DOUBLE: {
        double sum = ColumnChunkOper::Sum<double>(chunk, m_vectorizer.current_batch.row_count);
        sum_item->add_value(sum);
      } break;
      case MYSQL_TYPE_NEWDECIMAL: {
        auto sum_decimal = ColumnChunkOper::Sum<my_decimal>(chunk, m_vectorizer.current_batch.row_count);
        sum_item->add_value(sum_decimal);
      } break;
      default:
        for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
          RestoreRowFromBatch(row, idx);
          if (info.item->aggregator_add()) return 1;
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
    for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
      if (!chunk.nullable(row)) {
        RestoreRowFromBatch(row, idx);
        if (info.item->aggregator_add()) return 1;
      }
    }
  }
  return 0;
}

int VectorizedAggregateIterator::ProcessAvgAggregates(const std::vector<size_t> &avg_indices) {
  // AVG requires both sum and count; fall back to per-row for now.
  for (size_t idx : avg_indices) {
    const auto &info = m_vectorizer.aggregate_infos[idx];
    for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
      RestoreRowFromBatch(row, idx);
      if (info.item->aggregator_add()) return 1;
    }
  }
  return 0;
}

void VectorizedAggregateIterator::SetupColumnChunks() {
  if (m_vectorizer.current_batch.initialized) return;

  m_vectorizer.current_batch.column_chunks.clear();
  m_vectorizer.current_batch.capacity = m_vectorizer.opt_batch_size;

  for (const auto &info : m_vectorizer.aggregate_infos) {
    if (info.vectorizable && info.source_field) {
      m_vectorizer.current_batch.column_chunks.emplace_back(info.source_field, m_vectorizer.current_batch.capacity);
    } else {
      // Placeholder for non-vectorizable aggregate slots.
      m_vectorizer.current_batch.column_chunks.emplace_back(nullptr, m_vectorizer.current_batch.capacity);
    }
  }

  m_vectorizer.current_batch.initialized = true;
}

void VectorizedAggregateIterator::SetupBatchChunks() {
  assert(!m_batch_chunks_initialized);

  // Derive the TABLE pointer from the first aggregate's source field.
  TABLE *tbl = nullptr;
  for (const auto &info : m_vectorizer.aggregate_infos) {
    if (info.source_field) {
      tbl = info.source_field->table;
      break;
    }
  }

  if (tbl) {
    m_batch_col_chunks.assign(tbl->s->fields, ColumnChunk(nullptr, 0));
    for (uint i = 0; i < tbl->s->fields; ++i) {
      Field *field = tbl->field[i];
      if (bitmap_is_set(tbl->read_set, i) && !field->is_flag_set(NOT_SECONDARY_FLAG))
        m_batch_col_chunks[i] = ColumnChunk(field, m_vectorizer.opt_batch_size);
    }
  }

  if (!m_vectorizer.current_batch.initialized) SetupColumnChunks();

  m_batch_chunks_initialized = true;
}

void VectorizedAggregateIterator::RestoreGroupKeyField(size_t row_idx) {
  List_iterator<Cached_item> it(m_join->group_fields);
  Cached_item *cached;
  while ((cached = it++)) {
    Item *item = cached->get_item();
    if (!item || item->type() != Item::FIELD_ITEM) continue;

    Field *field = down_cast<Item_field *>(item)->field;
    if (!field) continue;
    uint fld_idx = field->field_index();
    if (fld_idx >= m_batch_col_chunks.size()) continue;
    const ColumnChunk &chunk = m_batch_col_chunks[fld_idx];
    if (!chunk.valid()) continue;

    if (chunk.nullable(row_idx)) {
      field->set_null();
    } else {
      field->set_notnull();
      memcpy(field->field_ptr(), chunk.data(row_idx), chunk.width());
    }
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
    if (info.vectorizable) any_vectorizable = true;
  }

  return any_vectorizable;
}

bool VectorizedAggregateIterator::IsSimpleAggregate(Item_sum *item) const {
  switch (item->sum_func()) {
    case Item_sum::COUNT_FUNC:
    case Item_sum::COUNT_DISTINCT_FUNC:
    case Item_sum::SUM_FUNC:
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC:
    case Item_sum::AVG_FUNC:
      return true;
    default:
      return false;
  }
}

Field *VectorizedAggregateIterator::GetPrimaryFieldForAggregate(Item_sum *item) const {
  if (item->arg_count > 0) {
    Item *arg = item->get_arg(0);
    if (arg->type() == Item::FIELD_ITEM) return down_cast<Item_field *>(arg)->field;
  }
  return nullptr;
}

// Row-level helpers
void VectorizedAggregateIterator::AppendCurrentRowToChunks() {
  for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
    const auto &agg_info = m_vectorizer.aggregate_infos[i];
    if (!agg_info.vectorizable || !agg_info.source_field) continue;
    auto &chunk = m_vectorizer.current_batch.column_chunks[i];
    Field *field = agg_info.source_field;
    bool is_null = field->is_null();
    const uchar *data = is_null ? nullptr : field->data_ptr();
    size_t data_len = is_null ? 0 : field->pack_length();
    chunk.add(const_cast<uchar *>(data), data_len, is_null);
  }
  m_vectorizer.current_batch.row_count++;
}

void VectorizedAggregateIterator::RestoreRowFromBatch(size_t row_idx, size_t agg_idx) {
  if (agg_idx >= m_vectorizer.aggregate_infos.size() || row_idx >= m_vectorizer.current_batch.row_count) return;
  const auto &info = m_vectorizer.aggregate_infos[agg_idx];
  auto &chunk = m_vectorizer.current_batch.column_chunks[agg_idx];
  if (chunk.nullable(row_idx)) {
    info.source_field->set_null();
  } else {
    info.source_field->set_notnull();
    memcpy((void *)info.source_field->data_ptr(), chunk.data(row_idx), chunk.width());
  }

  m_vectorizer.current_batch.initialized = true;
}

void VectorizedAggregateIterator::UpdateBatchSizeFromPerformance(double processing_time_ms) {
  m_vectorizer.recent_processing_times[m_vectorizer.time_index] = processing_time_ms;
  m_vectorizer.time_index = (m_vectorizer.time_index + 1) % 10;

  double avg_time = 0.0;
  for (int i = 0; i < 10; ++i) avg_time += m_vectorizer.recent_processing_times[i];
  avg_time /= 10.0;

  if (m_stats.total_batches_processed % 10 == 0 && m_stats.total_batches_processed > 10) {
    if (avg_time > m_target_batch_time_ms * 2.0)
      m_vectorizer.opt_batch_size = std::max(m_vectorizer.opt_batch_size / 2, m_min_batch_size);
    else if (avg_time < m_target_batch_time_ms && m_vectorizer.opt_batch_size < m_max_batch_size)
      m_vectorizer.opt_batch_size = std::min(m_vectorizer.opt_batch_size * 2, m_max_batch_size);
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
  if (m_rollup && m_current_rollup_pos != level) {
    m_current_rollup_pos = level;
    for (Item_rollup_group_item *item : m_join->rollup_group_items) item->set_current_rollup_level(level);
    for (Item_rollup_sum_switcher *item : m_join->rollup_sums) item->set_current_rollup_level(level);
  }
}
}  // namespace Executor
}  // namespace ShannonBase