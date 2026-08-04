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

   The fundmental code for imcs. It's based on mysql executor iterators.
*/
#include "storage/rapid_engine/executor/iterators/aggregate_iterator.h"

#include <cmath>
#include <limits>

#include "include/my_base.h"

#include "sql/dd/cache/dictionary_client.h"
#include "sql/sql_class.h"
#include "sql/sql_executor.h"
#include "sql/sql_optimizer.h"

#include "storage/innobase/include/dict0dd.h"
#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/optimizer/optimizer.h"

namespace ShannonBase {
namespace Executor {
VectorizedAggregateIterator::VectorizedAggregateIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source,
                                                         JOIN *join, pack_rows::TableCollection tables, bool rollup,
                                                         AggregateStrategy strategy, double expected_rows)
    : RowIterator(thd),
      m_source(std::move(source)),
      m_join(join),
      m_rollup(rollup),
      m_tables(std::move(tables)),
      m_strategy(strategy),
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
  ut_a(!m_join->tmp_table_param.precomputed_group_by);

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
  if (m_batch_source == nullptr) m_batch_source = dynamic_cast<BatchReadable *>(m_source->real_iterator());
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
  m_field_to_batch_chunk_idx.clear();

  m_hash_groups_built = false;
  m_hash_group_output_idx = 0;
  m_hash_group_index.clear();
  m_hash_groups.clear();

  if (m_strategy == AggregateStrategy::HASH) {
    if (!m_join->grouped || m_rollup || m_batch_source == nullptr) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash aggregate received an incompatible physical input plan");
      return true;
    }
    m_vectorizer.can_vectorize_curr_grp = AnalyzeAggregatesForVectorization();
    m_vectorizer.analysis_complete = true;
    if (!ValidateHashAggregatePlan()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "Rapid hash aggregate received unsupported GROUP BY or aggregate expressions");
      return true;
    }
    SetupBatchChunks();
  }

  // Clear stats
  m_stats = VectorizationStats{};

  return false;
}

int VectorizedAggregateIterator::Read() {
  if (m_strategy == AggregateStrategy::HASH) return ReadHashAggregate();

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

  ut_a(false);
  return 1;
}

bool VectorizedAggregateIterator::ValidateHashAggregatePlan() const {
  if (!m_vectorizer.can_vectorize_curr_grp || m_join->group_fields.is_empty()) return false;

  for (const Cached_item &cached : m_join->group_fields) {
    Item *item = cached.get_item();
    if (item == nullptr || item->type() != Item::FIELD_ITEM) return false;
    Field *field = down_cast<Item_field *>(item)->field;
    if (field == nullptr) return false;
    switch (field->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_YEAR:
        break;
      default:
        return false;
    }
  }

  for (const auto &info : m_vectorizer.aggregate_infos) {
    if (!info.vectorizable) return false;
    switch (info.type) {
      case Item_sum::COUNT_FUNC:
      case Item_sum::SUM_FUNC:
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
        break;
      default:
        return false;
    }
  }
  return true;
}

int VectorizedAggregateIterator::ReadHashAggregate() {
  if (!m_hash_groups_built) {
    if (BuildHashGroups() != 0) return 1;
    m_hash_groups_built = true;
  }

  if (m_hash_group_output_idx >= m_hash_groups.size()) {
    m_seen_eof = true;
    m_state = DONE_OUTPUTTING_ROWS;
    return -1;
  }

  if (MaterializeHashGroup(m_hash_groups[m_hash_group_output_idx++]) != 0) return 1;
  if (m_output_slice != -1) m_join->set_ref_item_slice(m_output_slice);
  return 0;
}

int VectorizedAggregateIterator::BuildHashGroups() {
  const size_t packed_row_capacity = ComputeRowSizeUpperBound(m_tables);
  for (;;) {
    for (ColumnChunk &chunk : m_batch_col_chunks) chunk.clear();

    size_t rows = 0;
    int result = m_batch_source->ReadBatch(m_batch_col_chunks, m_vectorizer.opt_batch_size, rows);
    if (result != 0 && result != HA_ERR_END_OF_FILE) return 1;

    for (size_t row = 0; row < rows; ++row) {
      if (RestoreHashBatchRow(row)) return 1;

      std::string key;
      if (BuildHashGroupKey(&key)) return 1;
      auto [it, inserted] = m_hash_group_index.emplace(std::move(key), m_hash_groups.size());
      if (inserted) {
        HashGroupState group;
        String representative;
        representative.reserve(packed_row_capacity);
        if (StoreFromTableBuffers(m_tables, &representative)) return 1;
        const auto *begin = pointer_cast<const uchar *>(representative.ptr());
        group.representative_row.assign(begin, begin + representative.length());
        group.aggregates.resize(m_vectorizer.aggregate_infos.size());
        m_hash_groups.push_back(std::move(group));
      }
      if (UpdateHashGroup(&m_hash_groups[it->second])) return 1;
    }

    m_stats.total_batches_processed++;
    m_stats.total_rows_vectorized += rows;
    if (result == HA_ERR_END_OF_FILE || rows == 0) break;
  }
  return 0;
}

bool VectorizedAggregateIterator::RestoreHashBatchRow(size_t row_idx) {
  auto restore_field = [&](Field *field) {
    if (field == nullptr) return false;
    auto it = m_field_to_batch_chunk_idx.find(field);
    if (it == m_field_to_batch_chunk_idx.end()) return true;
    const ColumnChunk &chunk = m_batch_col_chunks[it->second];
    if (!chunk.valid() || row_idx >= chunk.size()) return true;
    if (chunk.nullable(row_idx)) {
      field->set_null();
    } else {
      field->set_notnull();
      memcpy(field->field_ptr(), chunk.data(row_idx), chunk.width());
    }
    return false;
  };

  for (const Cached_item &cached : m_join->group_fields) {
    Item *item = cached.get_item();
    if (restore_field(down_cast<Item_field *>(item)->field)) return true;
  }
  for (const auto &info : m_vectorizer.aggregate_infos) {
    if (info.source_field != nullptr && restore_field(info.source_field)) return true;
  }
  return false;
}

bool VectorizedAggregateIterator::BuildHashGroupKey(std::string *key) const {
  key->clear();
  for (const Cached_item &cached : m_join->group_fields) {
    Field *field = down_cast<Item_field *>(cached.get_item())->field;
    const char null_marker = field->is_null() ? 1 : 0;
    key->append(&null_marker, sizeof(null_marker));
    if (null_marker != 0) continue;

    const auto type = static_cast<uint8_t>(field->type());
    key->append(pointer_cast<const char *>(&type), sizeof(type));
    switch (field->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_YEAR: {
        const longlong value = field->val_int();
        key->append(pointer_cast<const char *>(&value), sizeof(value));
      } break;
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE: {
        double value = field->val_real();
        if (value == 0.0) value = 0.0;
        if (std::isnan(value)) value = std::numeric_limits<double>::quiet_NaN();
        key->append(pointer_cast<const char *>(&value), sizeof(value));
      } break;
      case MYSQL_TYPE_NEWDECIMAL: {
        my_decimal decimal;
        String value;
        field->val_decimal(&decimal);
        if (my_decimal2string(E_DEC_FATAL_ERROR, &decimal, &value) != 0) return true;
        const uint32_t length = value.length();
        key->append(pointer_cast<const char *>(&length), sizeof(length));
        key->append(value.ptr(), value.length());
      } break;
      default:
        return true;
    }
  }
  return false;
}

bool VectorizedAggregateIterator::UpdateHashGroup(HashGroupState *group) {
  for (size_t index = 0; index < m_vectorizer.aggregate_infos.size(); ++index) {
    const auto &info = m_vectorizer.aggregate_infos[index];
    HashAggregateState &state = group->aggregates[index];
    Field *field = info.source_field;
    const bool is_null = field != nullptr && field->is_null();

    if (info.type == Item_sum::COUNT_FUNC) {
      if (field == nullptr || !is_null) ++state.count;
      continue;
    }
    if (is_null) continue;

    if (info.type == Item_sum::MIN_FUNC || info.type == Item_sum::MAX_FUNC) {
      if (!state.has_value) {
        state.extremum.assign(field->field_ptr(), field->field_ptr() + field->pack_length());
        state.has_value = true;
      } else {
        const int cmp = field->cmp(field->field_ptr(), state.extremum.data());
        if ((info.type == Item_sum::MIN_FUNC && cmp < 0) || (info.type == Item_sum::MAX_FUNC && cmp > 0))
          state.extremum.assign(field->field_ptr(), field->field_ptr() + field->pack_length());
      }
      continue;
    }

    state.has_value = true;
    ++state.count;
    switch (field->type()) {
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_NEWDECIMAL: {
        my_decimal value;
        field->val_decimal(&value);
        if (!state.decimal_value) {
          my_decimal2decimal(&value, &state.decimal_sum);
          state.decimal_value = true;
        } else {
          my_decimal result;
          if (my_decimal_add(E_DEC_FATAL_ERROR, &result, &state.decimal_sum, &value) > 1) return true;
          state.decimal_sum = result;
        }
      } break;
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
        state.real_sum += field->val_real();
        break;
      default:
        return true;
    }
  }
  return false;
}

int VectorizedAggregateIterator::MaterializeHashGroup(const HashGroupState &group) {
  if (group.representative_row.empty()) return 1;
  LoadIntoTableBuffers(m_tables, group.representative_row.data());
  (void)update_item_cache_if_changed(m_join->group_fields);
  SetRollupLevel(m_join->send_group_parts);

  for (size_t index = 0; index < m_vectorizer.aggregate_infos.size(); ++index) {
    const auto &info = m_vectorizer.aggregate_infos[index];
    const HashAggregateState &state = group.aggregates[index];
    Item_sum *item = info.item;
    item->clear();

    switch (info.type) {
      case Item_sum::COUNT_FUNC:
        down_cast<Item_sum_count *>(item)->add_value(state.count);
        break;
      case Item_sum::SUM_FUNC: {
        if (!state.has_value) break;
        info.source_field->set_notnull();
        Item_sum_sum *sum = down_cast<Item_sum_sum *>(item);
        if (state.decimal_value) {
          if (sum->add_value(state.decimal_sum)) return 1;
        } else if (sum->add_value(state.real_sum)) {
          return 1;
        }
      } break;
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
        if (state.has_value) {
          info.source_field->set_notnull();
          memcpy(info.source_field->field_ptr(), state.extremum.data(), state.extremum.size());
          if (item->aggregator_add()) return 1;
        }
        break;
      default:
        return 1;
    }
  }
  return 0;
}

int VectorizedAggregateIterator::ProcessCurrentGroupTraditional() {
  if (m_vectorization_enabled && m_vectorizer.can_vectorize_curr_grp && !m_rollup) return ProcessGroupVectorized();
  return ProcessGroupScalar();
}

int VectorizedAggregateIterator::ProcessGroupVectorized() {
  // Batch path: ReadBatch() fills schema-wide m_batch_col_chunks in one call.
  //             GROUP BY detection is post-hoc (iterate rows from chunks).
  // Row-by-row: m_source->Read() fills table->field, then AppendCurrentRowToChunks()
  //             copies aggregate columns into m_vectorizer.current_batch.column_chunks.
  //             GROUP BY detection is inline (check after each Read).
  const bool do_group_by = !m_join->group_fields.is_empty();
  const bool use_batch = (m_batch_source != nullptr) && !do_group_by;

  if (!m_batch_chunks_initialized) SetupBatchChunks();

  for (;;) {
    size_t rows_read = 0;
    bool is_eof = false;
    // Row-by-row only: true when the current row in table->field belongs
    // to the NEXT group (not appended to the current batch).
    bool next_group_row_in_table = false;

    if (use_batch) {
      for (auto &c : m_batch_col_chunks) c.clear();

      int err = m_batch_source->ReadBatch(m_batch_col_chunks, m_vectorizer.opt_batch_size, rows_read);
      is_eof = (err == HA_ERR_END_OF_FILE);
      if (err != 0 && !is_eof) return 1;
    } else {
      m_vectorizer.current_batch.clear();

      for (;;) {
        int err = m_source->Read();
        if (err == -1) {
          is_eof = true;
          break;
        }
        if (err == 1) return 1;

        // Check GROUP BY on the row just read (still in table->field).
        if (do_group_by && update_item_cache_if_changed(m_join->group_fields) >= 0) {
          next_group_row_in_table = true;
          break;  // current row → next group; do NOT append
        }

        AppendCurrentRowToChunks();
        ++rows_read;

        if (m_vectorizer.current_batch.full()) break;
      }
    }

    if (rows_read == 0) {
      if (is_eof) {
        m_seen_eof = true;
        StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
        LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
        SetRollupLevel(m_join->send_group_parts);
        m_state = DONE_OUTPUTTING_ROWS;
        break;
      }
      if (next_group_row_in_table) {
        StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
        LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
        m_last_unchanged_grp_item_idx = 0;
        m_state = LAST_ROW_STARTED_NEW_GROUP;
        return 0;
      }
      continue;  // shouldn't normally happen; retry
    }

    size_t boundary = rows_read;  // default: all rows → current group
    if (use_batch && do_group_by) {
      for (size_t r = 0; r < rows_read; ++r) {
        RestoreGroupKeyField(r);
        if (update_item_cache_if_changed(m_join->group_fields) >= 0) {
          boundary = r;
          break;
        }
      }
    }

    if (boundary > 0) {
      if (use_batch) {
        if (ProcessVectorizedAggregates(m_batch_col_chunks, boundary) != 0) return 1;
      } else {
        if (ProcessVectorizedAggregates() != 0) return 1;
      }
      m_stats.total_batches_processed++;
      m_stats.total_rows_vectorized += boundary;
    }

    const bool group_changed = use_batch ? (boundary < rows_read) : next_group_row_in_table;
    if (group_changed) {
      if (use_batch) {
        // Push unprocessed rows back into the source buffer.
        m_batch_source->PushbackBatchTail(m_batch_col_chunks, boundary, rows_read);
        // Copy the boundary row from column chunks into table->field
        // (needed so StoreFromTableBuffers saves the right group key).
        RestoreGroupKeyField(boundary);
        RestoreBoundaryRowToTableFields(boundary);
      }
      // else: the next-group row is already in table->field.

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
  }
  return 0;
}

void VectorizedAggregateIterator::RestoreBoundaryRowToTableFields(size_t boundary) {
  for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
    const auto &info = m_vectorizer.aggregate_infos[i];
    if (!info.source_field) continue;
    if (info.batch_chunk_idx == static_cast<size_t>(-1) || info.batch_chunk_idx >= m_batch_col_chunks.size()) continue;

    const ColumnChunk &chunk = m_batch_col_chunks[info.batch_chunk_idx];
    if (!chunk.valid() || boundary >= chunk.size()) continue;

    if (chunk.nullable(boundary)) {
      info.source_field->set_null();
    } else {
      info.source_field->set_notnull();
      memcpy(info.source_field->field_ptr(), chunk.data(boundary), chunk.width());
    }
  }
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

    int first_changed_idx = -1;
    first_changed_idx = update_item_cache_if_changed(m_join->group_fields);

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

  for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
    const auto &info = m_vectorizer.aggregate_infos[i];
    if (!info.vectorizable) continue;
    // COUNT(*)/COUNT(1): source_field == nullptr
    if (!info.source_field) {
      if (info.type == Item_sum::COUNT_FUNC) down_cast<Item_sum_count *>(info.item)->add_value(row_count);
      continue;  // the other source_field cannot vectorized, skip.
    }

    if (info.batch_chunk_idx == static_cast<size_t>(-1) || info.batch_chunk_idx >= col_chunks.size() ||
        !col_chunks[info.batch_chunk_idx].valid())
      continue;
    const ColumnChunk &chunk = col_chunks[info.batch_chunk_idx];
    size_t non_null = ColumnChunkOper::CountNonNull(chunk, row_count);

    switch (info.type) {
      case Item_sum::COUNT_FUNC:
        down_cast<Item_sum_count *>(info.item)->add_value(non_null);
        break;
      case Item_sum::SUM_FUNC: {
        Item_sum_sum *sum_item = down_cast<Item_sum_sum *>(info.item);
        switch (info.source_field->type()) {
          case MYSQL_TYPE_LONGLONG: {
            if (non_null == 0) break;
            int64_t s = ColumnChunkOper::Sum<int64_t>(chunk, row_count);
            my_decimal delta;
            int2my_decimal(E_DEC_FATAL_ERROR, s, false /*unsigned*/, &delta);

            info.source_field->set_notnull();
            sum_item->add_value(delta);
          } break;
          case MYSQL_TYPE_LONG: {
            if (non_null == 0) break;
            int64_t s = ColumnChunkOper::Sum<int32_t>(chunk, row_count);
            my_decimal delta;
            int2my_decimal(E_DEC_FATAL_ERROR, s, false, &delta);
            info.source_field->set_notnull();
            sum_item->add_value(delta);
          } break;
          case MYSQL_TYPE_FLOAT: {
            if (non_null == 0) break;
            double s = static_cast<double>(ColumnChunkOper::Sum<float>(chunk, row_count));
            info.source_field->set_notnull();
            down_cast<Item_sum_sum *>(info.item)->add_value(s);
            break;
          }
          case MYSQL_TYPE_DOUBLE: {
            size_t non_null = ColumnChunkOper::CountNonNull(chunk, row_count);
            if (non_null == 0) break;
            double s = ColumnChunkOper::Sum<double>(chunk, row_count);
            info.source_field->set_notnull();
            down_cast<Item_sum_sum *>(info.item)->add_value(s);
          } break;
          case MYSQL_TYPE_NEWDECIMAL: {
            size_t non_null = ColumnChunkOper::CountNonNull(chunk, row_count);
            if (non_null == 0) break;
            my_decimal s = ColumnChunkOper::Sum<my_decimal>(chunk, row_count);
            info.source_field->set_notnull();
            down_cast<Item_sum_sum *>(info.item)->add_value(s);
          } break;
          default:
            for (size_t row = 0; row < row_count; ++row) {
              if (chunk.nullable(row)) {
                info.source_field->set_null();
              } else {
                info.source_field->set_notnull();
                memcpy(info.source_field->field_ptr(), chunk.data(row), chunk.width());
              }
              if (info.item->aggregator_add()) return 1;
            }
            break;
        }
        break;
      }
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
      case Item_sum::AVG_FUNC:
      default: {
        for (size_t row = 0; row < row_count; ++row) {
          bool is_null = chunk.nullable(row);
          if (is_null) {
            info.source_field->set_null();
          } else {
            info.source_field->set_notnull();
            memcpy(info.source_field->field_ptr(), chunk.data(row), chunk.width());
          }
          if (info.item->aggregator_add()) return 1;
        }
        break;
      }
    }
  }
  return 0;
}

int VectorizedAggregateIterator::ProcessCountAggregates(const std::vector<size_t> &count_indices) {
  for (size_t idx : count_indices) {
    size_t count{0};
    const auto &info = m_vectorizer.aggregate_infos[idx];
    if (!info.source_field) {
      // COUNT(*) or COUNT(1): every row in the batch counts unconditionally.
      count = m_vectorizer.current_batch.row_count;
    } else {
      auto &chunk = m_vectorizer.current_batch.column_chunks[idx];
      count = ColumnChunkOper::CountNonNull(chunk, m_vectorizer.current_batch.row_count);
    }
    down_cast<Item_sum_count *>(info.item)->add_value(count);
  }
  return 0;
}

int VectorizedAggregateIterator::ProcessSumAggregates(const std::vector<size_t> &sum_indices) {
  for (size_t idx : sum_indices) {
    const auto &info = m_vectorizer.aggregate_infos[idx];
    const auto &chunk = m_vectorizer.current_batch.column_chunks[idx];
    Item_sum_sum *sum_item = down_cast<Item_sum_sum *>(info.item);
    Field *field = info.source_field;
    if (ColumnChunkOper::CountNonNull(chunk, m_vectorizer.current_batch.row_count) == 0) continue;
    field->set_notnull();
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
  ut_a(!m_batch_chunks_initialized);

  m_batch_col_chunks.clear();
  m_field_to_batch_chunk_idx.clear();

  std::vector<Field *> required_fields;
  auto add_required_field = [&required_fields](Field *field) {
    if (field == nullptr || field->is_flag_set(NOT_SECONDARY_FLAG)) return;
    if (std::find(required_fields.begin(), required_fields.end(), field) == required_fields.end())
      required_fields.push_back(field);
  };

  for (const pack_rows::Table &table : m_tables.tables()) {
    for (const pack_rows::Column &column : table.columns) add_required_field(column.field);
  }

  List_iterator<Cached_item> git(m_join->group_fields);
  Cached_item *cached;
  while ((cached = git++)) {
    Item *item = cached->get_item();
    if (!item || item->type() != Item::FIELD_ITEM) continue;
    add_required_field(down_cast<Item_field *>(item)->field);
  }
  for (const auto &info : m_vectorizer.aggregate_infos) add_required_field(info.source_field);

  if (m_tables.tables().size() == 1) {
    TABLE *table = m_tables.tables()[0].table;
    m_batch_col_chunks.reserve(table->s->fields);
    for (uint field_idx = 0; field_idx < table->s->fields; ++field_idx) {
      Field *field = table->field[field_idx];
      const bool required = bitmap_is_set(table->read_set, field_idx) ||
                            std::find(required_fields.begin(), required_fields.end(), field) != required_fields.end();
      if (required && !field->is_flag_set(NOT_SECONDARY_FLAG)) {
        m_field_to_batch_chunk_idx.emplace(field, field_idx);
        m_batch_col_chunks.emplace_back(field, m_vectorizer.opt_batch_size);
      } else {
        m_batch_col_chunks.emplace_back(nullptr, 0);
      }
    }
  } else {
    // field_index() is table-local, so joined rows require a compact layout
    // keyed by (TABLE*, field_index) instead of a single schema-wide vector.
    for (Field *field : required_fields) {
      m_field_to_batch_chunk_idx.emplace(field, m_batch_col_chunks.size());
      m_batch_col_chunks.emplace_back(field, m_vectorizer.opt_batch_size);
    }
  }

  for (auto &info : m_vectorizer.aggregate_infos) {
    if (!info.source_field) continue;
    auto it = m_field_to_batch_chunk_idx.find(info.source_field);
    info.batch_chunk_idx = (it != m_field_to_batch_chunk_idx.end()) ? it->second : static_cast<size_t>(-1);
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

    auto map_it = m_field_to_batch_chunk_idx.find(field);
    if (map_it == m_field_to_batch_chunk_idx.end()) continue;

    const ColumnChunk &chunk = m_batch_col_chunks[map_it->second];
    if (!chunk.valid() || row_idx >= chunk.size()) continue;

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
    info.source_field_table_index = info.source_field ? static_cast<uint16_t>(info.source_field->field_index()) : 0;
    info.field_index = m_vectorizer.aggregate_infos.size();
    m_vectorizer.aggregate_infos.push_back(info);
    if (info.vectorizable) any_vectorizable = true;
  }

  return any_vectorizable;
}

bool VectorizedAggregateIterator::IsSimpleAggregate(Item_sum *item) const {
  Field *field = GetPrimaryFieldForAggregate(item);
  switch (item->sum_func()) {
    case Item_sum::COUNT_FUNC: {
      if (field != nullptr) return true;
      if (item->arg_count == 0) return true;
      Item *arg = item->get_arg(0);
      return arg != nullptr && arg->const_item() && !arg->is_null();
    }
    case Item_sum::SUM_FUNC:
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC:
    case Item_sum::AVG_FUNC: {
      if (field == nullptr || field->is_flag_set(UNSIGNED_FLAG)) return false;
      switch (field->type()) {
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_NEWDECIMAL:
          return true;
        default:
          return false;
      }
    }
    default:
      return false;
  }
}

Field *VectorizedAggregateIterator::GetPrimaryFieldForAggregate(Item_sum *item) const {
  if (item->arg_count > 0) {
    Item *arg = item->get_arg(0);
    if (arg != nullptr) arg = arg->real_item();
    if (arg != nullptr && arg->type() == Item::FIELD_ITEM) return down_cast<Item_field *>(arg)->field;
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
