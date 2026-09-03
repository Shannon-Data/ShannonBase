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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include <xxhash.h>

#include "include/my_base.h"

#include "sql/dd/cache/dictionary_client.h"
#include "sql/iterators/timing_iterator.h"  // NewIterator
#include "sql/sql_class.h"
#include "sql/sql_executor.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_tmp_table.h"  // create_ondisk_from_heap, instantiate_tmp_table
#include "sql/table.h"          // empty_record

#include "storage/innobase/include/dict0dd.h"
#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/optimizer/optimizer.h"

namespace ShannonBase {
namespace Executor {

namespace {
bool IsHashGroupSortKeyType(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2:
      return true;
    default:
      return false;
  }
}

// Upper bound on what Field::make_sort_key() writes for `field`. For a string
// that is the collation's transform length, which may exceed the stored width;
// for everything else the packed width, which is the bound filesort uses.
size_t HashGroupSortKeyLength(const Field *field) {
  const CHARSET_INFO *cs = field->charset();
  if (cs != nullptr && field->result_type() == STRING_RESULT) return cs->coll->strnxfrmlen(cs, field->field_length);
  return field->pack_length();
}
}  // namespace

bool IsHashGroupKeyFieldType(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_YEAR:
      return true;
    default:
      // FLOAT/DOUBLE are encodable but stay out: grouping on a binary float is
      // not a shape worth routing here, and excluding it keeps this list equal
      // to what the optimizer used to allow plus the sort-key types.
      return IsHashGroupSortKeyType(type);
  }
}

VectorizedAggregateIterator::HashSpillFile::HashSpillFile() : file(std::tmpfile()) {}

VectorizedAggregateIterator::HashSpillFile::~HashSpillFile() {
  if (file != nullptr) std::fclose(file);
}

bool VectorizedAggregateIterator::HashSpillFile::RewindForRead() {
  if (file == nullptr) return true;
  // fseek() is also the required synchronization point when switching an
  // update stream from writes to reads; unlike fflush(), it is valid here even
  // when the same spill file has just been consumed by a previous merge pass.
  std::clearerr(file);
  return std::fseek(file, 0, SEEK_SET) != 0;
}

VectorizedAggregateIterator::VectorizedAggregateIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source,
                                                         JOIN *join, pack_rows::TableCollection tables, bool rollup,
                                                         AggregateStrategy strategy, ORDER *hash_output_order,
                                                         double expected_rows, size_t hash_memory_limit)
    : RowIterator(thd),
      m_source(std::move(source)),
      m_join(join),
      m_rollup(rollup),
      m_tables(std::move(tables)),
      m_strategy(strategy),
      m_hash_output_order(hash_output_order),
      m_state(READING_FIRST_ROW),
      m_seen_eof(false),
      m_save_nullinfo(0),
      m_last_unchanged_grp_item_idx(0),
      m_current_rollup_pos(-1),
      m_output_slice(-1) {
  m_hash_memory_limit = hash_memory_limit;

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
  // Implementing BatchReadable is not the same as being able to serve a batch
  // in this execution; see BatchReadable::SupportsBatchRead().
  if (m_batch_source != nullptr && !m_batch_source->SupportsBatchRead()) m_batch_source = nullptr;
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
  m_hash_spilled = false;
  m_hash_spill_output_read = 0;
  for (auto &partition : m_hash_spill_partitions) partition.reset();
  m_hash_spill_output.reset();

  // Destroy the previous execution's arena wholesale. This releases every
  // retained key/group/index allocation and also resets the memory counter.
  m_hash_arena.reset();
  m_hash_key_scratch.clear();

  if (m_strategy == AggregateStrategy::HASH) {
    if (m_join->group_fields.is_empty() || m_rollup) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash aggregate received an incompatible physical input plan");
      return true;
    }

    /*
     * HASH is the physical aggregation algorithm. BatchReadable is only a
     * transport capability of the child iterator edge. Keep HASH semantics in
     * both cases and choose the ingestion path from the concrete child at
     * runtime. Never fall back from HASH to STREAMING merely because the child
     * is row-only.
     */
    m_hash_input_mode = m_source_supports_batch ? HashInputMode::BATCH : HashInputMode::ROW;
  }

  if (m_strategy == AggregateStrategy::HASH) {
    m_vectorizer.can_vectorize_curr_grp = AnalyzeAggregatesForVectorization();
    m_vectorizer.analysis_complete = true;
    if (!ValidateHashAggregatePlan()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "Rapid hash aggregate received unsupported GROUP BY or aggregate expressions");
      return true;
    }

    if (m_hash_memory_limit == 0) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash aggregate memory limit is zero");
      return true;
    }

    /*
     * Construct a fresh arena for every execution. This is deliberately done
     * after source Init()/plan validation but before any group state is built.
     * std::make_unique only allocates the small fixed HashArena object itself;
     * every cardinality-dependent allocation made by index/groups and their
     * nested buffers is routed through HashMemoryResource.
     */
    try {
      m_hash_arena = std::make_unique<HashArena>(m_hash_memory_limit);
    } catch (const std::bad_alloc &) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash aggregate could not initialize bounded memory arena");
      return true;
    }

    // Batch chunks are needed only by the batch ingestion path. A row-only
    // child already materializes the current row in TABLE::record/Field.
    if (m_hash_input_mode == HashInputMode::BATCH) {
      SetupBatchChunks();
      // A BatchReadable edge says nothing about whether its physical column
      // bytes can reconstruct every MySQL representative field. Keep HASH as
      // the algorithm, but fall back to ROW ingestion when the batch layout is
      // not safe (dictionary/varlen fields, or an unsupported batch key type).
      // An expression aggregate has no column in the batch either:
      // UpdateHashGroupFromBatch() addresses its value through
      // info.batch_chunk_idx, which such an aggregate does not have, while the
      // row path derives it with EvaluateExpressionFields().
      if (!CanMaterializeBatchRows() || !CanBuildHashGroupKeyFromBatch() || HasExpressionAggregate())
        m_hash_input_mode = HashInputMode::ROW;
    }
  }

  // Clear stats
  m_stats = VectorizationStats{};
  if (m_strategy == AggregateStrategy::HASH) {
    m_stats.hash_memory_limit_bytes = m_hash_memory_limit;
  }

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
    if (field == nullptr || !IsHashGroupKeyFieldType(field->type())) return false;
  }

  for (const auto &info : m_vectorizer.aggregate_infos) {
    if (!info.vectorizable) return false;
    switch (info.type) {
      case Item_sum::COUNT_FUNC:
      case Item_sum::SUM_FUNC:
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
        break;
      case Item_sum::AVG_FUNC: {
        // The hash-group finalizer hands Item_sum_avg the accumulated sum and
        // count directly, so every numeric type the accumulator supports is
        // exact -- no finalized average is squeezed back through the source
        // Field. Keep this type set aligned with IsSimpleAggregate().
        const Field *f = info.source_field;
        if (f == nullptr || f->is_flag_set(UNSIGNED_FLAG)) return false;
        switch (f->type()) {
          case MYSQL_TYPE_LONG:
          case MYSQL_TYPE_LONGLONG:
          case MYSQL_TYPE_FLOAT:
          case MYSQL_TYPE_DOUBLE:
          case MYSQL_TYPE_NEWDECIMAL:
            break;
          default:
            return false;
        }
      } break;
      default:
        return false;
    }
  }
  return true;
}

int VectorizedAggregateIterator::ReadHashAggregate() {
  if (!m_hash_groups_built) {
    if (m_hash_arena == nullptr) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid hash aggregate has no bounded memory arena");
      return 1;
    }

    if (BuildHashGroups() != 0) return 1;

    if (!m_hash_spilled) {
      SortHashGroupsForOutput();
      if (m_hash_arena != nullptr)
        m_stats.hash_memory_peak_bytes = std::max(m_stats.hash_memory_peak_bytes, m_hash_arena->memory.peak_bytes());
    }
    m_hash_groups_built = true;
  }

  if (m_hash_spilled) {
    if (m_hash_spill_output == nullptr || m_hash_spill_output_read >= m_hash_spill_output->records) {
      m_seen_eof = true;
      m_state = DONE_OUTPUTTING_ROWS;
      return -1;
    }

    SpillGroupState group;
    std::vector<uchar> row;
    HashSpillRecordType type;
    if (ReadHashSpillRecord(m_hash_spill_output.get(), &type, &group, &row) || type != HashSpillRecordType::STATE) {
      return ReportHashSpillError("could not read finalized spill output");
    }
    ++m_hash_spill_output_read;

    if (MaterializeSpillGroup(group) != 0) return 1;
    if (m_output_slice != -1) m_join->set_ref_item_slice(m_output_slice);
    ++m_stats.rows_out;
    return 0;
  }

  if (m_hash_arena == nullptr) return 1;
  auto &groups = m_hash_arena->groups;
  if (m_hash_group_output_idx >= groups.size()) {
    m_seen_eof = true;
    m_state = DONE_OUTPUTTING_ROWS;
    return -1;
  }

  if (MaterializeHashGroup(groups[m_hash_group_output_idx++]) != 0) return 1;
  if (m_output_slice != -1) m_join->set_ref_item_slice(m_output_slice);
  ++m_stats.rows_out;
  return 0;
}

int VectorizedAggregateIterator::HashMemoryLimitExceeded() {
  ++m_stats.hash_memory_limit_hits;
  if (m_hash_arena != nullptr) {
    m_stats.hash_memory_peak_bytes = std::max(m_stats.hash_memory_peak_bytes, m_hash_arena->memory.peak_bytes());
  }
  return kHashNeedSpill;
}

std::unique_ptr<VectorizedAggregateIterator::HashSpillFile> VectorizedAggregateIterator::CreateHashSpillFile() {
  try {
    auto file = std::make_unique<HashSpillFile>();
    if (!file->valid()) return nullptr;
    return file;
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
}

int VectorizedAggregateIterator::ReportHashSpillError(const char *reason) {
  const size_t used_bytes = m_hash_arena != nullptr ? m_hash_arena->memory.used_bytes() : 0;
  const size_t requested_bytes = m_hash_arena != nullptr ? m_hash_arena->memory.failed_allocation_bytes() : 0;
  const std::string message =
      "Rapid hash aggregate spill failed: " + std::string(reason != nullptr ? reason : "unknown error") +
      "; limit=" + std::to_string(m_hash_memory_limit) + " bytes, used=" + std::to_string(used_bytes) +
      " bytes, requested=" + std::to_string(requested_bytes) +
      " bytes, peak=" + std::to_string(m_stats.hash_memory_peak_bytes) + " bytes";
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), message.c_str());
  return 1;
}

bool VectorizedAggregateIterator::WriteHashSpillRaw(HashSpillFile *file, const void *data, size_t length) {
  if (file == nullptr || file->file == nullptr) return true;
  if (length == 0) return false;
  if (data == nullptr || std::fwrite(data, 1, length, file->file) != length) return true;
  file->bytes_written += length;
  m_stats.hash_spill_bytes_written += length;
  return false;
}

bool VectorizedAggregateIterator::ReadHashSpillRaw(HashSpillFile *file, void *data, size_t length) const {
  if (file == nullptr || file->file == nullptr) return true;
  if (length == 0) return false;
  return data == nullptr || std::fread(data, 1, length, file->file) != length;
}

bool VectorizedAggregateIterator::WriteHashSpillBlob(HashSpillFile *file, const uchar *data, size_t length) {
  const uint64_t length64 = static_cast<uint64_t>(length);
  if (WriteHashSpillRaw(file, &length64, sizeof(length64))) return true;
  return length != 0 && WriteHashSpillRaw(file, data, length);
}

bool VectorizedAggregateIterator::ReadHashSpillBlob(HashSpillFile *file, std::vector<uchar> *data) const {
  if (data == nullptr) return true;
  uint64_t length = 0;
  if (ReadHashSpillRaw(file, &length, sizeof(length))) return true;
  if (length > static_cast<uint64_t>(m_hash_memory_limit) ||
      length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    return true;

  try {
    data->resize(static_cast<size_t>(length));
  } catch (const std::bad_alloc &) {
    return true;
  }
  return length != 0 && ReadHashSpillRaw(file, data->data(), static_cast<size_t>(length));
}

bool VectorizedAggregateIterator::ReadHashSpillString(HashSpillFile *file, std::string *data) const {
  if (data == nullptr) return true;
  uint64_t length = 0;
  if (ReadHashSpillRaw(file, &length, sizeof(length))) return true;
  if (length > static_cast<uint64_t>(m_hash_memory_limit) ||
      length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
    return true;

  try {
    data->resize(static_cast<size_t>(length));
  } catch (const std::bad_alloc &) {
    return true;
  }
  return length != 0 && ReadHashSpillRaw(file, data->data(), static_cast<size_t>(length));
}

bool VectorizedAggregateIterator::WriteHashSpillDecimal(HashSpillFile *file, const my_decimal &value) {
  const int32_t intg = value.intg;
  const int32_t frac = value.frac;
  const uint8_t sign = value.sign() ? 1 : 0;
  // DECIMAL_BUFF_LENGTH digit words fully describe the coefficient; `buf` points
  // at those words for a well-formed my_decimal.
  return WriteHashSpillRaw(file, &intg, sizeof(intg)) || WriteHashSpillRaw(file, &frac, sizeof(frac)) ||
         WriteHashSpillRaw(file, &sign, sizeof(sign)) ||
         WriteHashSpillRaw(file, value.buf, DECIMAL_BUFF_LENGTH * sizeof(decimal_digit_t));
}

bool VectorizedAggregateIterator::ReadHashSpillDecimal(HashSpillFile *file, my_decimal *value) const {
  if (value == nullptr) return true;
  int32_t intg = 0;
  int32_t frac = 0;
  uint8_t sign = 0;
  // *value is a live my_decimal: its `buf` already points at its own internal
  // buffer. Fill that buffer in place and never touch `buf` itself.
  if (ReadHashSpillRaw(file, &intg, sizeof(intg)) || ReadHashSpillRaw(file, &frac, sizeof(frac)) ||
      ReadHashSpillRaw(file, &sign, sizeof(sign)) ||
      ReadHashSpillRaw(file, value->buf, DECIMAL_BUFF_LENGTH * sizeof(decimal_digit_t)))
    return true;
  value->intg = intg;
  value->frac = frac;
  value->len = DECIMAL_BUFF_LENGTH;
  value->decimal_t::sign = (sign != 0);
  return false;
}

bool VectorizedAggregateIterator::WriteHashSpillRow(HashSpillFile *file, const std::string &key, const uchar *row,
                                                    size_t row_length) {
  const uint8_t record_type = static_cast<uint8_t>(HashSpillRecordType::ROW);
  const uint64_t key_length = static_cast<uint64_t>(key.size());
  if (WriteHashSpillRaw(file, &record_type, sizeof(record_type)) ||
      WriteHashSpillRaw(file, &key_length, sizeof(key_length)) ||
      (key_length != 0 && WriteHashSpillRaw(file, key.data(), key.size())) || WriteHashSpillBlob(file, row, row_length))
    return true;
  ++file->records;
  return false;
}

bool VectorizedAggregateIterator::WriteHashSpillState(HashSpillFile *file, const HashGroupState &group) {
  const uint8_t record_type = static_cast<uint8_t>(HashSpillRecordType::STATE);
  const uint64_t key_length = static_cast<uint64_t>(group.key.size());
  const uint32_t aggregate_count = static_cast<uint32_t>(group.aggregates.size());
  const uint32_t order_count = static_cast<uint32_t>(group.order_values.size());

  if (WriteHashSpillRaw(file, &record_type, sizeof(record_type)) ||
      WriteHashSpillRaw(file, &key_length, sizeof(key_length)) ||
      (key_length != 0 && WriteHashSpillRaw(file, group.key.data(), group.key.size())) ||
      WriteHashSpillBlob(file, group.representative_row.data(), group.representative_row.size()) ||
      WriteHashSpillRaw(file, &aggregate_count, sizeof(aggregate_count)))
    return true;

  for (const HashAggregateState &state : group.aggregates) {
    const uint8_t has_value = state.has_value ? 1 : 0;
    const uint8_t decimal_value = state.decimal_value ? 1 : 0;
    if (WriteHashSpillRaw(file, &state.count, sizeof(state.count)) ||
        WriteHashSpillRaw(file, &has_value, sizeof(has_value)) ||
        WriteHashSpillRaw(file, &decimal_value, sizeof(decimal_value)) ||
        WriteHashSpillRaw(file, &state.real_sum, sizeof(state.real_sum)) ||
        WriteHashSpillDecimal(file, state.decimal_sum) ||
        WriteHashSpillBlob(file, state.extremum.data(), state.extremum.size()))
      return true;
  }

  if (WriteHashSpillRaw(file, &order_count, sizeof(order_count))) return true;
  for (const HashGroupOrderValue &value : group.order_values) {
    const uint8_t is_null = value.is_null ? 1 : 0;
    if (WriteHashSpillRaw(file, &is_null, sizeof(is_null)) ||
        WriteHashSpillBlob(file, value.data.data(), value.data.size()))
      return true;
  }

  ++file->records;
  return false;
}

bool VectorizedAggregateIterator::WriteHashSpillState(HashSpillFile *file, const SpillGroupState &group) {
  const uint8_t record_type = static_cast<uint8_t>(HashSpillRecordType::STATE);
  const uint64_t key_length = static_cast<uint64_t>(group.key.size());
  const uint32_t aggregate_count = static_cast<uint32_t>(group.aggregates.size());
  const uint32_t order_count = static_cast<uint32_t>(group.order_values.size());

  if (WriteHashSpillRaw(file, &record_type, sizeof(record_type)) ||
      WriteHashSpillRaw(file, &key_length, sizeof(key_length)) ||
      (key_length != 0 && WriteHashSpillRaw(file, group.key.data(), group.key.size())) ||
      WriteHashSpillBlob(file, group.representative_row.data(), group.representative_row.size()) ||
      WriteHashSpillRaw(file, &aggregate_count, sizeof(aggregate_count)))
    return true;

  for (const SpillAggregateState &state : group.aggregates) {
    const uint8_t has_value = state.has_value ? 1 : 0;
    const uint8_t decimal_value = state.decimal_value ? 1 : 0;
    if (WriteHashSpillRaw(file, &state.count, sizeof(state.count)) ||
        WriteHashSpillRaw(file, &has_value, sizeof(has_value)) ||
        WriteHashSpillRaw(file, &decimal_value, sizeof(decimal_value)) ||
        WriteHashSpillRaw(file, &state.real_sum, sizeof(state.real_sum)) ||
        WriteHashSpillDecimal(file, state.decimal_sum) ||
        WriteHashSpillBlob(file, state.extremum.data(), state.extremum.size()))
      return true;
  }

  if (WriteHashSpillRaw(file, &order_count, sizeof(order_count))) return true;
  for (const SpillOrderValue &value : group.order_values) {
    const uint8_t is_null = value.is_null ? 1 : 0;
    if (WriteHashSpillRaw(file, &is_null, sizeof(is_null)) ||
        WriteHashSpillBlob(file, value.data.data(), value.data.size()))
      return true;
  }

  ++file->records;
  return false;
}

bool VectorizedAggregateIterator::ReadHashSpillRecord(HashSpillFile *file, HashSpillRecordType *type,
                                                      SpillGroupState *state, std::vector<uchar> *row) {
  if (file == nullptr || type == nullptr || state == nullptr || row == nullptr) return true;

  state->clear();
  row->clear();

  uint8_t record_type = 0;
  if (ReadHashSpillRaw(file, &record_type, sizeof(record_type))) return true;
  if (record_type != static_cast<uint8_t>(HashSpillRecordType::ROW) &&
      record_type != static_cast<uint8_t>(HashSpillRecordType::STATE))
    return true;
  *type = static_cast<HashSpillRecordType>(record_type);

  if (ReadHashSpillString(file, &state->key)) return true;

  if (*type == HashSpillRecordType::ROW) {
    return ReadHashSpillBlob(file, row);
  }

  if (ReadHashSpillBlob(file, &state->representative_row)) return true;

  uint32_t aggregate_count = 0;
  if (ReadHashSpillRaw(file, &aggregate_count, sizeof(aggregate_count)) ||
      aggregate_count != m_vectorizer.aggregate_infos.size())
    return true;

  try {
    state->aggregates.resize(aggregate_count);
  } catch (const std::bad_alloc &) {
    return true;
  }

  for (SpillAggregateState &aggregate : state->aggregates) {
    uint8_t has_value = 0;
    uint8_t decimal_value = 0;
    if (ReadHashSpillRaw(file, &aggregate.count, sizeof(aggregate.count)) ||
        ReadHashSpillRaw(file, &has_value, sizeof(has_value)) ||
        ReadHashSpillRaw(file, &decimal_value, sizeof(decimal_value)) ||
        ReadHashSpillRaw(file, &aggregate.real_sum, sizeof(aggregate.real_sum)) ||
        ReadHashSpillDecimal(file, &aggregate.decimal_sum) || ReadHashSpillBlob(file, &aggregate.extremum))
      return true;
    aggregate.has_value = (has_value != 0);
    aggregate.decimal_value = (decimal_value != 0);
  }

  uint32_t expected_order_count = 0;
  for (ORDER *order = m_hash_output_order; order != nullptr; order = order->next) ++expected_order_count;

  uint32_t order_count = 0;
  if (ReadHashSpillRaw(file, &order_count, sizeof(order_count)) || order_count != expected_order_count) return true;
  try {
    state->order_values.resize(order_count);
  } catch (const std::bad_alloc &) {
    return true;
  }

  for (SpillOrderValue &value : state->order_values) {
    uint8_t is_null = 0;
    if (ReadHashSpillRaw(file, &is_null, sizeof(is_null)) || ReadHashSpillBlob(file, &value.data)) return true;
    value.is_null = (is_null != 0);
    if (value.is_null && !value.data.empty()) return true;
  }

  return false;
}

size_t VectorizedAggregateIterator::HashSpillPartitionForKey(const std::string &key, size_t depth) const {
  static_assert((kHashSpillFanout & (kHashSpillFanout - 1)) == 0, "spill fanout must be a power of two");
  constexpr uint64_t kSeedBase = 0x9E3779B97F4A7C15ULL;
  const uint64_t seed = kSeedBase ^ (static_cast<uint64_t>(depth + 1) * 0xD6E8FEB86659FD93ULL);
  const uint64_t hash = XXH64(key.data(), key.size(), seed);
  return static_cast<size_t>(hash) & (kHashSpillFanout - 1);
}

int VectorizedAggregateIterator::BeginHashSpill(size_t packed_row_capacity) {
  if (m_hash_spilled) return SpillCurrentInputRow(packed_row_capacity);
  if (m_hash_arena == nullptr) return ReportHashSpillError("bounded arena disappeared before spill");
  if (m_hash_arena->groups.empty()) {
    return ReportHashSpillError("a single hash group cannot fit within the configured memory limit");
  }

  for (const HashGroupState &group : m_hash_arena->groups) {
    std::string key(group.key.data(), group.key.size());
    const size_t partition_idx = HashSpillPartitionForKey(key, 0);
    if (m_hash_spill_partitions[partition_idx] == nullptr) {
      m_hash_spill_partitions[partition_idx] = CreateHashSpillFile();
      if (m_hash_spill_partitions[partition_idx] == nullptr)
        return ReportHashSpillError("could not create a temporary spill partition");
      ++m_stats.hash_spill_partitions;
    }
    if (WriteHashSpillState(m_hash_spill_partitions[partition_idx].get(), group))
      return ReportHashSpillError("could not write an in-memory aggregate state to spill");
    ++m_stats.hash_spill_groups;
  }

  m_hash_spilled = true;
  m_hash_arena.reset();
  return SpillCurrentInputRow(packed_row_capacity);
}

int VectorizedAggregateIterator::SpillCurrentInputRow(size_t packed_row_capacity) {
  std::string key;
  if (BuildHashGroupKey(&key)) return 1;

  String packed_row;
  if (packed_row.reserve(packed_row_capacity))
    return ReportHashSpillError("could not allocate spill row scratch space");
  if (StoreFromTableBuffers(m_tables, &packed_row)) return 1;

  const size_t partition_idx = HashSpillPartitionForKey(key, 0);
  if (m_hash_spill_partitions[partition_idx] == nullptr) {
    m_hash_spill_partitions[partition_idx] = CreateHashSpillFile();
    if (m_hash_spill_partitions[partition_idx] == nullptr)
      return ReportHashSpillError("could not create a temporary spill partition");
    ++m_stats.hash_spill_partitions;
  }

  if (WriteHashSpillRow(m_hash_spill_partitions[partition_idx].get(), key,
                        pointer_cast<const uchar *>(packed_row.ptr()), packed_row.length()))
    return ReportHashSpillError("could not write an input row to spill");

  ++m_stats.hash_spill_rows;
  return 0;
}

int VectorizedAggregateIterator::LoadHashSpillState(const SpillGroupState &state) {
  if (m_hash_arena == nullptr || state.aggregates.size() != m_vectorizer.aggregate_infos.size()) return 1;

  const uint64_t hash = XXH64(state.key.data(), state.key.size(), 0);
  const auto range = m_hash_arena->index.equal_range(hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second >= m_hash_arena->groups.size()) return 1;
    if (HashKeysEqual(m_hash_arena->groups[it->second].key, state.key))
      return ReportHashSpillError("duplicate prefix state encountered while rebuilding a spill partition");
  }

  auto *resource = &m_hash_arena->memory;
  const size_t groups_before = m_hash_arena->groups.size();
  try {
    m_hash_arena->groups.emplace_back(resource);
    HashGroupState &group = m_hash_arena->groups.back();
    group.key.assign(state.key.data(), state.key.size());
    group.representative_row.assign(state.representative_row.begin(), state.representative_row.end());

    group.aggregates.reserve(state.aggregates.size());
    for (const SpillAggregateState &source : state.aggregates) {
      group.aggregates.emplace_back(resource);
      HashAggregateState &target = group.aggregates.back();
      target.count = source.count;
      target.has_value = source.has_value;
      target.decimal_value = source.decimal_value;
      target.real_sum = source.real_sum;
      target.decimal_sum = source.decimal_sum;
      target.extremum.assign(source.extremum.begin(), source.extremum.end());
    }

    group.order_values.reserve(state.order_values.size());
    for (const SpillOrderValue &source : state.order_values) {
      group.order_values.emplace_back(resource);
      HashGroupOrderValue &target = group.order_values.back();
      target.is_null = source.is_null;
      target.data.assign(source.data.begin(), source.data.end());
    }

    m_hash_arena->index.emplace(hash, groups_before);
  } catch (const std::bad_alloc &) {
    if (m_hash_arena != nullptr && m_hash_arena->groups.size() > groups_before)
      m_hash_arena->groups.erase(m_hash_arena->groups.begin() + groups_before, m_hash_arena->groups.end());
    return HashMemoryLimitExceeded();
  }

  return 0;
}

int VectorizedAggregateIterator::RepartitionHashSpillFile(
    HashSpillFile *source, size_t depth, std::array<std::unique_ptr<HashSpillFile>, kHashSpillFanout> *children) {
  if (source == nullptr || children == nullptr || source->RewindForRead())
    return ReportHashSpillError("could not rewind a spill partition for repartitioning");

  ++m_stats.hash_spill_repartitions;

  for (uint64_t record_idx = 0; record_idx < source->records; ++record_idx) {
    HashSpillRecordType type;
    SpillGroupState state;
    std::vector<uchar> row;
    if (ReadHashSpillRecord(source, &type, &state, &row))
      return ReportHashSpillError("could not read a spill partition during repartitioning");

    const size_t partition_idx = HashSpillPartitionForKey(state.key, depth);
    if ((*children)[partition_idx] == nullptr) {
      (*children)[partition_idx] = CreateHashSpillFile();
      if ((*children)[partition_idx] == nullptr)
        return ReportHashSpillError("could not create a recursive spill partition");
      ++m_stats.hash_spill_partitions;
    }

    const bool write_error =
        type == HashSpillRecordType::STATE
            ? WriteHashSpillState((*children)[partition_idx].get(), state)
            : WriteHashSpillRow((*children)[partition_idx].get(), state.key, row.data(), row.size());
    if (write_error) return ReportHashSpillError("could not write a recursive spill partition");
  }

  return 0;
}

bool VectorizedAggregateIterator::SpillGroupLess(const SpillGroupState &left, const SpillGroupState &right) const {
  size_t index = 0;
  for (ORDER *order = m_hash_output_order; order != nullptr; order = order->next, ++index) {
    if (index >= left.order_values.size() || index >= right.order_values.size()) break;

    const SpillOrderValue &lhs = left.order_values[index];
    const SpillOrderValue &rhs = right.order_values[index];
    int cmp = 0;
    if (lhs.is_null != rhs.is_null) {
      cmp = lhs.is_null ? -1 : 1;
    } else if (!lhs.is_null) {
      if (order->item == nullptr || *order->item == nullptr) return false;
      Item *item = (*order->item)->real_item();
      if (item->type() != Item::FIELD_ITEM) return false;
      Field *field = down_cast<Item_field *>(item)->field;
      if (field == nullptr) return false;
      cmp = field->cmp(lhs.data.data(), rhs.data.data());
    }

    if (cmp == 0) continue;
    return order->direction == ORDER_DESC ? cmp > 0 : cmp < 0;
  }

  const size_t common = std::min(left.key.size(), right.key.size());
  const int key_cmp = common == 0 ? 0 : std::memcmp(left.key.data(), right.key.data(), common);
  if (key_cmp != 0) return key_cmp < 0;
  return left.key.size() < right.key.size();
}

int VectorizedAggregateIterator::MergeSortedSpillRuns(std::unique_ptr<HashSpillFile> left,
                                                      std::unique_ptr<HashSpillFile> right,
                                                      std::unique_ptr<HashSpillFile> *merged) {
  if (left == nullptr || right == nullptr || merged == nullptr || left->RewindForRead() || right->RewindForRead())
    return ReportHashSpillError("could not rewind sorted spill runs");

  auto output = CreateHashSpillFile();
  if (output == nullptr) return ReportHashSpillError("could not create a merged spill run");

  SpillGroupState left_group;
  SpillGroupState right_group;
  std::vector<uchar> row;
  HashSpillRecordType type;
  uint64_t left_read = 0;
  uint64_t right_read = 0;
  bool have_left = false;
  bool have_right = false;

  auto read_left = [&]() -> bool {
    if (left_read >= left->records) {
      have_left = false;
      return false;
    }
    if (ReadHashSpillRecord(left.get(), &type, &left_group, &row) || type != HashSpillRecordType::STATE) return true;
    ++left_read;
    have_left = true;
    return false;
  };
  auto read_right = [&]() -> bool {
    if (right_read >= right->records) {
      have_right = false;
      return false;
    }
    if (ReadHashSpillRecord(right.get(), &type, &right_group, &row) || type != HashSpillRecordType::STATE) return true;
    ++right_read;
    have_right = true;
    return false;
  };

  if (read_left() || read_right()) return ReportHashSpillError("could not read a sorted spill run");

  while (have_left || have_right) {
    if (!have_right || (have_left && SpillGroupLess(left_group, right_group))) {
      if (WriteHashSpillState(output.get(), left_group))
        return ReportHashSpillError("could not write a merged spill run");
      if (read_left()) return ReportHashSpillError("could not advance a sorted spill run");
    } else {
      if (WriteHashSpillState(output.get(), right_group))
        return ReportHashSpillError("could not write a merged spill run");
      if (read_right()) return ReportHashSpillError("could not advance a sorted spill run");
    }
  }

  *merged = std::move(output);
  return 0;
}

int VectorizedAggregateIterator::AccumulateOrderedSpillRun(std::unique_ptr<HashSpillFile> run,
                                                           std::unique_ptr<HashSpillFile> *ordered_output) {
  if (run == nullptr || ordered_output == nullptr) return 1;
  if (*ordered_output == nullptr) {
    *ordered_output = std::move(run);
    return 0;
  }

  std::unique_ptr<HashSpillFile> merged;
  if (MergeSortedSpillRuns(std::move(*ordered_output), std::move(run), &merged) != 0) return 1;
  *ordered_output = std::move(merged);
  return 0;
}

int VectorizedAggregateIterator::ProcessHashSpillPartition(std::unique_ptr<HashSpillFile> partition, size_t depth,
                                                           HashSpillFile *unordered_output,
                                                           std::unique_ptr<HashSpillFile> *ordered_output) {
  if (partition == nullptr || partition->records == 0) return 0;
  if (depth > kHashMaxSpillDepth) return ReportHashSpillError("maximum recursive spill depth exceeded");

  try {
    m_hash_arena = std::make_unique<HashArena>(m_hash_memory_limit);
  } catch (const std::bad_alloc &) {
    return ReportHashSpillError("could not recreate the bounded hash arena");
  }

  if (partition->RewindForRead()) return ReportHashSpillError("could not rewind a spill partition");

  const size_t packed_row_capacity = ComputeRowSizeUpperBound(m_tables);
  for (uint64_t record_idx = 0; record_idx < partition->records; ++record_idx) {
    HashSpillRecordType type;
    SpillGroupState state;
    std::vector<uchar> row;
    if (ReadHashSpillRecord(partition.get(), &type, &state, &row))
      return ReportHashSpillError("could not read a spill partition");

    int result = 0;
    if (type == HashSpillRecordType::STATE) {
      result = LoadHashSpillState(state);
    } else {
      if (row.empty()) return ReportHashSpillError("encountered an empty packed row in spill");
      LoadIntoTableBuffers(m_tables, row.data());
      result = ConsumeHashRow(packed_row_capacity);
    }

    if (result == kHashNeedSpill) {
      if (depth >= kHashMaxSpillDepth)
        return ReportHashSpillError("one recursive spill partition still exceeds the memory limit");

      // Discard partial work and repartition the original input records. This
      // preserves record order within each key, so floating-point SUM/AVG sees
      // the same per-group addition order as the non-spill path.
      m_hash_arena.reset();
      std::array<std::unique_ptr<HashSpillFile>, kHashSpillFanout> children;
      if (RepartitionHashSpillFile(partition.get(), depth + 1, &children) != 0) return 1;

      for (auto &child : children) {
        if (child != nullptr &&
            ProcessHashSpillPartition(std::move(child), depth + 1, unordered_output, ordered_output) != 0)
          return 1;
      }
      return 0;
    }

    if (result != 0) return result;
  }

  if (m_hash_arena == nullptr) return 1;
  m_stats.hash_memory_peak_bytes = std::max(m_stats.hash_memory_peak_bytes, m_hash_arena->memory.peak_bytes());

  if (m_hash_output_order != nullptr) {
    SortHashGroupsForOutput();
    auto run = CreateHashSpillFile();
    if (run == nullptr) return ReportHashSpillError("could not create a sorted spill output run");
    for (const HashGroupState &group : m_hash_arena->groups) {
      if (WriteHashSpillState(run.get(), group))
        return ReportHashSpillError("could not write a sorted spill output run");
    }
    m_hash_arena.reset();
    return AccumulateOrderedSpillRun(std::move(run), ordered_output);
  }

  if (unordered_output == nullptr) return 1;
  for (const HashGroupState &group : m_hash_arena->groups) {
    if (WriteHashSpillState(unordered_output, group))
      return ReportHashSpillError("could not write finalized spill output");
  }
  m_hash_arena.reset();
  return 0;
}

int VectorizedAggregateIterator::FinalizeHashSpill() {
  if (!m_hash_spilled) return 0;

  std::unique_ptr<HashSpillFile> ordered_output;
  std::unique_ptr<HashSpillFile> unordered_output;
  if (m_hash_output_order == nullptr) {
    unordered_output = CreateHashSpillFile();
    if (unordered_output == nullptr) return ReportHashSpillError("could not create finalized spill output");
  }

  for (auto &partition : m_hash_spill_partitions) {
    if (partition == nullptr) continue;
    if (ProcessHashSpillPartition(std::move(partition), 0, unordered_output.get(), &ordered_output) != 0) return 1;
  }

  m_hash_spill_output = m_hash_output_order != nullptr ? std::move(ordered_output) : std::move(unordered_output);
  if (m_hash_spill_output == nullptr) {
    m_hash_spill_output = CreateHashSpillFile();
    if (m_hash_spill_output == nullptr) return ReportHashSpillError("could not create empty spill output");
  }

  if (m_hash_spill_output->RewindForRead()) return ReportHashSpillError("could not rewind finalized spill output");

  m_hash_spill_output_read = 0;
  m_hash_arena.reset();
  return 0;
}

int VectorizedAggregateIterator::MaterializeSpillGroup(const SpillGroupState &group) {
  if (group.representative_row.empty() || group.aggregates.size() != m_vectorizer.aggregate_infos.size()) return 1;
  LoadIntoTableBuffers(m_tables, group.representative_row.data());
  (void)update_item_cache_if_changed(m_join->group_fields);
  SetRollupLevel(m_join->send_group_parts);

  for (size_t index = 0; index < m_vectorizer.aggregate_infos.size(); ++index) {
    const auto &info = m_vectorizer.aggregate_infos[index];
    const SpillAggregateState &state = group.aggregates[index];
    Item_sum *item = info.item;
    item->clear();

    switch (info.type) {
      case Item_sum::COUNT_FUNC:
        down_cast<Item_sum_count *>(item)->add_value(state.count);
        break;
      case Item_sum::SUM_FUNC: {
        if (!state.has_value) break;
        Item_sum_sum *sum = down_cast<Item_sum_sum *>(item);
        if (state.decimal_value) {
          if (m_vectorizer.Sum(sum, info.source_field, state.decimal_sum)) return 1;
        } else if (m_vectorizer.Sum(sum, info.source_field, state.real_sum)) {
          return 1;
        }
      } break;
      case Item_sum::AVG_FUNC: {
        if (!state.has_value || state.count == 0 || info.source_field == nullptr) break;
        // Hand the accumulated sum and count to Item_sum_avg directly rather
        // than storing a finalized double average back through the source
        // Field. Item_sum_avg performs the division itself (my_decimal_div
        // with prec_increment for DECIMAL_RESULT, sum/m_count for
        // REAL_RESULT), so this is exact for integer and decimal inputs --
        // the Field round-trip truncated those to the source column's scale.
        Item_sum_avg *avg = down_cast<Item_sum_avg *>(item);
        if (state.decimal_value) {
          if (m_vectorizer.Sum(avg, info.source_field, state.decimal_sum)) return 1;
        } else if (m_vectorizer.Sum(avg, info.source_field, state.real_sum)) {
          return 1;
        }
        avg->add_count(state.count);
      } break;
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
        if (state.has_value) {
          if (info.source_field == nullptr || state.extremum.size() != info.source_field->pack_length()) return 1;
          info.source_field->set_notnull();
          memcpy(info.source_field->field_ptr(), state.extremum.data(), state.extremum.size());
          Item *arg = item->get_arg(0);
          if (arg != nullptr) arg->null_value = false;
          if (item->aggregator_add()) return 1;
        }
        break;
      default:
        return 1;
    }
  }
  return 0;
}

bool VectorizedAggregateIterator::HashKeysEqual(const std::pmr::string &left, const std::string &right) const {
  return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size()) == 0);
}

int VectorizedAggregateIterator::BuildHashGroups() {
  if (m_hash_arena == nullptr) return 1;

  const int result = (m_hash_input_mode == HashInputMode::BATCH) ? BuildHashGroupsBatch() : BuildHashGroupsRow();
  if (result != 0) return result;

  return m_hash_spilled ? FinalizeHashSpill() : 0;
}

int VectorizedAggregateIterator::BuildHashGroupsBatch() {
  if (m_hash_arena == nullptr || m_batch_source == nullptr || !m_batch_chunks_initialized) {
    return 1;
  }

  const size_t packed_row_capacity = ComputeRowSizeUpperBound(m_tables);

  for (;;) {
    if (thd()->killed) {
      thd()->send_kill_message();
      return 1;
    }
    if (!EnsureBatchCapacity(m_vectorizer.opt_batch_size)) return 1;
    for (ColumnChunk &chunk : m_batch_col_chunks) chunk.clear();

    size_t rows = 0;
    const int result = m_batch_source->ReadBatch(m_batch_col_chunks, m_vectorizer.opt_batch_size, rows);
    if (result != 0 && result != HA_ERR_END_OF_FILE) return 1;

    for (size_t row = 0; row < rows; ++row) {
      if (m_hash_spilled) {
        // Spill format intentionally stays row-compatible so recursive rebuild
        // reuses the exact scalar semantics. Spill is the exceptional path.
        if (RestoreHashBatchRow(row)) return 1;
        ++m_stats.hash_row_materializations;
        if (SpillCurrentInputRow(packed_row_capacity) != 0) return 1;
        continue;
      }

      const int consume_result = ConsumeHashBatchRow(row, packed_row_capacity);
      if (consume_result == kHashNeedSpill) {
        // The failed direct update may not have materialized the whole row.
        // BeginHashSpill() serializes the current input row, so restore it once.
        if (RestoreHashBatchRow(row)) return 1;
        ++m_stats.hash_row_materializations;
        if (BeginHashSpill(packed_row_capacity) != 0) return 1;
      } else if (consume_result != 0) {
        return consume_result;
      }
    }

    ++m_stats.total_batches_processed;
    m_stats.total_rows_vectorized += rows;
    m_stats.rows_in += rows;
    m_stats.hash_batch_input_rows += rows;
    if (m_hash_arena != nullptr)
      m_stats.hash_memory_peak_bytes = std::max(m_stats.hash_memory_peak_bytes, m_hash_arena->memory.peak_bytes());

    if (result == HA_ERR_END_OF_FILE || rows == 0) break;
  }

  return 0;
}

int VectorizedAggregateIterator::BuildHashGroupsRow() {
  if (m_hash_arena == nullptr) return 1;

  const size_t packed_row_capacity = ComputeRowSizeUpperBound(m_tables);

  for (;;) {
    if (thd()->killed) {
      thd()->send_kill_message();
      return 1;
    }
    const int result = m_source->Read();
    if (result == -1) break;
    if (result != 0) return result;

    /*
     * RowIterator::Read() has already materialized the child row into the
     * TABLE/Field buffers, so no ColumnChunk restore is required. The hash
     * algorithm below is identical to the batch path.
     */
    if (m_hash_spilled) {
      if (SpillCurrentInputRow(packed_row_capacity) != 0) return 1;
    } else {
      const int consume_result = ConsumeHashRow(packed_row_capacity);
      if (consume_result == kHashNeedSpill) {
        if (BeginHashSpill(packed_row_capacity) != 0) return 1;
      } else if (consume_result != 0) {
        return consume_result;
      }
    }

    ++m_stats.hash_row_input_rows;
    ++m_stats.rows_in;
    if (m_hash_arena != nullptr)
      m_stats.hash_memory_peak_bytes = std::max(m_stats.hash_memory_peak_bytes, m_hash_arena->memory.peak_bytes());
  }

  return 0;
}

int VectorizedAggregateIterator::ConsumeHashRow(size_t packed_row_capacity) {
  if (m_hash_arena == nullptr) return 1;

  // BuildHashGroupKey() uses a one-row scratch std::string. Every retained
  // cardinality-dependent object is copied into the bounded PMR arena below.
  std::string &key = m_hash_key_scratch;
  if (BuildHashGroupKey(&key)) return 1;
  const uint64_t hash = XXH64(key.data(), key.size(), 0);
  auto *resource = &m_hash_arena->memory;
  const size_t groups_before = m_hash_arena->groups.size();
  bool inserted_group = false;

  try {
    size_t group_index = std::numeric_limits<size_t>::max();

    // The index retains only a 64-bit hash. Resolve collisions against the
    // canonical key stored exactly once in HashGroupState.
    const auto range = m_hash_arena->index.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
      ++m_stats.hash_probes;
      if (it->second >= m_hash_arena->groups.size()) return 1;
      if (HashKeysEqual(m_hash_arena->groups[it->second].key, key)) {
        group_index = it->second;
        break;
      }
      ++m_stats.hash_collisions;
    }

    if (group_index == std::numeric_limits<size_t>::max()) {
      group_index = m_hash_arena->groups.size();
      m_hash_arena->groups.emplace_back(resource);
      inserted_group = true;
      HashGroupState &group = m_hash_arena->groups.back();
      group.key.assign(key.data(), key.size());

      String representative;
      if (representative.reserve(packed_row_capacity)) return 1;
      if (StoreFromTableBuffers(m_tables, &representative)) return 1;
      const auto *begin = pointer_cast<const uchar *>(representative.ptr());
      group.representative_row.assign(begin, begin + representative.length());

      group.aggregates.reserve(m_vectorizer.aggregate_infos.size());
      for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
        group.aggregates.emplace_back(resource);
      }

      if (CaptureHashGroupOrderValues(&group)) return 1;

      /*
       * Apply the current row before publishing the lookup entry. If any PMR
       * allocation fails while initializing/updating the new group, the catch
       * path removes the entire group. The row can then be written exactly once
       * to the spill stream without duplicating a partial prefix state.
       */
      if (UpdateHashGroup(&group)) return 1;

      // Publish into the lookup index last. Every visible index entry therefore
      // references a fully initialized and fully updated group.
      m_hash_arena->index.emplace(hash, group_index);
      return 0;
    }

    if (UpdateHashGroup(&m_hash_arena->groups[group_index])) return 1;
  } catch (const std::bad_alloc &) {
    if (inserted_group && m_hash_arena != nullptr && m_hash_arena->groups.size() > groups_before) {
      m_hash_arena->groups.erase(m_hash_arena->groups.begin() + groups_before, m_hash_arena->groups.end());
    }
    return HashMemoryLimitExceeded();
  }

  return 0;
}

int VectorizedAggregateIterator::ConsumeHashBatchRow(size_t row_idx, size_t packed_row_capacity) {
  if (m_hash_arena == nullptr) return 1;

  std::string &key = m_hash_key_scratch;
  if (BuildHashGroupKeyFromBatch(row_idx, &key)) return 1;
  const uint64_t hash = XXH64(key.data(), key.size(), 0);
  auto *resource = &m_hash_arena->memory;
  const size_t groups_before = m_hash_arena->groups.size();
  bool inserted_group = false;

  try {
    size_t group_index = std::numeric_limits<size_t>::max();
    const auto range = m_hash_arena->index.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it) {
      ++m_stats.hash_probes;
      if (it->second >= m_hash_arena->groups.size()) return 1;
      if (HashKeysEqual(m_hash_arena->groups[it->second].key, key)) {
        group_index = it->second;
        break;
      }
      ++m_stats.hash_collisions;
    }

    if (group_index == std::numeric_limits<size_t>::max()) {
      // Only a new group needs a representative MySQL row image. Existing
      // groups stay entirely in the columnar path.
      if (RestoreHashBatchRow(row_idx)) return 1;
      ++m_stats.hash_row_materializations;
      ++m_stats.hash_new_group_materializations;

      group_index = m_hash_arena->groups.size();
      m_hash_arena->groups.emplace_back(resource);
      inserted_group = true;
      HashGroupState &group = m_hash_arena->groups.back();
      group.key.assign(key.data(), key.size());

      String representative;
      if (representative.reserve(packed_row_capacity)) return 1;
      if (StoreFromTableBuffers(m_tables, &representative)) return 1;
      const auto *begin = pointer_cast<const uchar *>(representative.ptr());
      group.representative_row.assign(begin, begin + representative.length());

      group.aggregates.reserve(m_vectorizer.aggregate_infos.size());
      for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) group.aggregates.emplace_back(resource);
      if (CaptureHashGroupOrderValues(&group)) return 1;

      if (UpdateHashGroupFromBatch(&group, row_idx)) return 1;
      m_hash_arena->index.emplace(hash, group_index);
      ++m_stats.hash_batch_direct_rows;
      return 0;
    }

    if (UpdateHashGroupFromBatch(&m_hash_arena->groups[group_index], row_idx)) return 1;
    ++m_stats.hash_batch_direct_rows;
  } catch (const std::bad_alloc &) {
    if (inserted_group && m_hash_arena != nullptr && m_hash_arena->groups.size() > groups_before)
      m_hash_arena->groups.erase(m_hash_arena->groups.begin() + groups_before, m_hash_arena->groups.end());
    return HashMemoryLimitExceeded();
  }

  return 0;
}

bool VectorizedAggregateIterator::RestoreHashBatchRow(size_t row_idx) {
  // Restore every projected field, not only GROUP BY and aggregate arguments.
  // MySQL permits non-grouped output fields when functional dependency proves
  // them single-valued (for example, grouping by a primary key); the group's
  // representative row must therefore be captured from the same input row.
  for (const auto &[field, chunk_idx] : m_field_to_batch_chunk_idx) {
    (void)chunk_idx;
    if (RestoreBatchField(field, row_idx)) return true;
  }
  ++m_stats.row_materializations;
  return false;
}

bool VectorizedAggregateIterator::RestoreBatchField(Field *field, size_t row_idx) {
  if (field == nullptr) return true;
  if (Utils::Util::is_string(field->type()) || Utils::Util::is_varlen(field->type())) return true;
  auto it = m_field_to_batch_chunk_idx.find(field);
  if (it == m_field_to_batch_chunk_idx.end() || it->second >= m_batch_col_chunks.size()) return true;
  const ColumnChunk &chunk = m_batch_col_chunks[it->second];
  if (!chunk.valid() || row_idx >= chunk.size()) return true;
  if (chunk.nullable_fast(row_idx)) {
    field->set_null();
    return false;
  }
  field->set_notnull();
  field->pack(const_cast<uchar *>(field->data_ptr()), chunk.data_fast(row_idx), chunk.width());
  m_stats.bytes_copied += chunk.width();
  return false;
}

bool VectorizedAggregateIterator::CanMaterializeBatchRows() const {
  for (const auto &[field, chunk_idx] : m_field_to_batch_chunk_idx) {
    if (field == nullptr || chunk_idx >= m_batch_col_chunks.size()) return false;
    if (Utils::Util::is_string(field->type()) || Utils::Util::is_varlen(field->type())) return false;
    if (!m_batch_col_chunks[chunk_idx].valid()) return false;
  }
  return true;
}

bool VectorizedAggregateIterator::CanBuildHashGroupKeyFromBatch() const {
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
      case MYSQL_TYPE_NEWDECIMAL:
      case MYSQL_TYPE_YEAR:
        break;
      default:
        return false;
    }
    auto it = m_field_to_batch_chunk_idx.find(field);
    if (it == m_field_to_batch_chunk_idx.end() || it->second >= m_batch_col_chunks.size()) return false;
  }
  return true;
}

bool VectorizedAggregateIterator::CanUseBatchGrouping() const {
  if (!CanMaterializeBatchRows()) return false;
  for (const Cached_item &cached : m_join->group_fields) {
    Item *item = cached.get_item();
    if (item == nullptr || item->type() != Item::FIELD_ITEM) return false;
    Field *field = down_cast<Item_field *>(item)->field;
    if (field == nullptr || Utils::Util::is_string(field->type()) || Utils::Util::is_varlen(field->type()))
      return false;
    auto it = m_field_to_batch_chunk_idx.find(field);
    if (it == m_field_to_batch_chunk_idx.end() || it->second >= m_batch_col_chunks.size()) return false;
  }
  return true;
}

bool VectorizedAggregateIterator::BuildHashGroupKeyFromBatch(size_t row_idx, std::string *key) {
  if (key == nullptr) return true;
  key->clear();

  for (const Cached_item &cached : m_join->group_fields) {
    Field *field = down_cast<Item_field *>(cached.get_item())->field;
    auto it = m_field_to_batch_chunk_idx.find(field);
    if (field == nullptr || it == m_field_to_batch_chunk_idx.end() || it->second >= m_batch_col_chunks.size())
      return true;
    const ColumnChunk &chunk = m_batch_col_chunks[it->second];
    if (!chunk.valid() || row_idx >= chunk.size()) return true;

    const char null_marker = chunk.nullable_fast(row_idx) ? 1 : 0;
    key->append(&null_marker, sizeof(null_marker));
    if (null_marker != 0) continue;

    const auto type = static_cast<uint8_t>(field->type());
    key->append(pointer_cast<const char *>(&type), sizeof(type));

    // Fast path for the dominant AP grouping keys. The serialized bytes match
    // BuildHashGroupKey() exactly, so spill/rebuild can mix batch and row input.
    if (!field->is_unsigned() && field->type() == MYSQL_TYPE_LONG && chunk.width() == sizeof(int32_t)) {
      int32_t raw;
      std::memcpy(&raw, chunk.data_fast(row_idx), sizeof(raw));
      const longlong value = static_cast<longlong>(raw);
      key->append(pointer_cast<const char *>(&value), sizeof(value));
      continue;
    }
    if (!field->is_unsigned() && field->type() == MYSQL_TYPE_LONGLONG && chunk.width() == sizeof(int64_t)) {
      int64_t raw;
      std::memcpy(&raw, chunk.data_fast(row_idx), sizeof(raw));
      const longlong value = static_cast<longlong>(raw);
      key->append(pointer_cast<const char *>(&value), sizeof(value));
      continue;
    }

    // Less common encodings still materialize only this key Field, never the
    // complete input row.
    if (RestoreBatchField(field, row_idx)) return true;
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
      default: {
        // Strings and temporals. The stored image is not a group key: under a
        // case- or accent-insensitive collation two values GROUP BY treats as
        // one have different bytes, so hashing the image would split the group.
        // Field::make_sort_key() is exactly the collation-folded, memcmp-
        // comparable image filesort orders by, so equal values produce equal
        // bytes by construction, for every type rather than a hand-kept list.
        //
        // Only the row ingestion path reaches this: CanBuildHashGroupKeyFromBatch()
        // still declines these types, so the two encoders cannot disagree about
        // a key that spill/rebuild might mix.
        if (!IsHashGroupSortKeyType(field->type())) return true;
        const size_t bytes = HashGroupSortKeyLength(field);
        if (bytes == 0) return true;
        const size_t offset = key->size();
        key->append(sizeof(uint32_t) + bytes, '\0');
        const size_t written =
            field->make_sort_key(pointer_cast<uchar *>(key->data()) + offset + sizeof(uint32_t), bytes);
        if (written > bytes) return true;
        const uint32_t length = static_cast<uint32_t>(written);
        std::memcpy(key->data() + offset, &length, sizeof(length));
        key->resize(offset + sizeof(uint32_t) + written);
      } break;
    }
  }
  return false;
}

bool VectorizedAggregateIterator::CaptureHashGroupOrderValues(HashGroupState *group) const {
  if (m_hash_output_order == nullptr) return false;
  if (group == nullptr || m_hash_arena == nullptr) return true;

  auto *resource = &m_hash_arena->memory;
  for (ORDER *order = m_hash_output_order; order != nullptr; order = order->next) {
    if (order->item == nullptr || *order->item == nullptr) return true;
    Item *item = (*order->item)->real_item();
    if (item->type() != Item::FIELD_ITEM) return true;
    Field *field = down_cast<Item_field *>(item)->field;
    if (field == nullptr) return true;

    group->order_values.emplace_back(resource);
    HashGroupOrderValue &value = group->order_values.back();
    value.is_null = field->is_null();
    if (!value.is_null) {
      value.data.assign(field->field_ptr(), field->field_ptr() + field->pack_length());
    }
  }
  return false;
}

void VectorizedAggregateIterator::SortHashGroupsForOutput() {
  if (m_hash_output_order == nullptr || m_hash_arena == nullptr || m_hash_arena->groups.size() < 2) {
    return;
  }

  auto &groups = m_hash_arena->groups;

  /*
   * Do not use stable_sort here. libstdc++'s stable_sort may allocate an
   * auxiliary buffer outside HashMemoryResource, which would defeat a hard
   * operator memory boundary. std::sort is an in-place introsort in the
   * supported MySQL toolchain and uses only O(log N) stack state.
   *
   * SQL does not promise an order among ORDER BY ties. We nevertheless use the
   * canonical GROUP BY key as a deterministic final tie breaker; this also
   * keeps result order reproducible for floating-point regression tests.
   */
  std::sort(groups.begin(), groups.end(), [this](const HashGroupState &left, const HashGroupState &right) {
    size_t index = 0;
    for (ORDER *order = m_hash_output_order; order != nullptr; order = order->next, ++index) {
      if (index >= left.order_values.size() || index >= right.order_values.size()) {
        break;
      }

      const HashGroupOrderValue &lhs = left.order_values[index];
      const HashGroupOrderValue &rhs = right.order_values[index];
      int cmp = 0;
      if (lhs.is_null != rhs.is_null) {
        cmp = lhs.is_null ? -1 : 1;
      } else if (!lhs.is_null) {
        Field *field = down_cast<Item_field *>((*order->item)->real_item())->field;
        cmp = field->cmp(lhs.data.data(), rhs.data.data());
      }

      if (cmp == 0) continue;
      return order->direction == ORDER_DESC ? cmp > 0 : cmp < 0;
    }

    const size_t common = std::min(left.key.size(), right.key.size());
    const int key_cmp = common == 0 ? 0 : std::memcmp(left.key.data(), right.key.data(), common);
    if (key_cmp != 0) return key_cmp < 0;
    return left.key.size() < right.key.size();
  });
}

bool VectorizedAggregateIterator::UpdateHashGroup(HashGroupState *group) {
  // Same contract as AppendCurrentRowToChunks(): the row is in the table
  // buffers, so expression aggregates must be derived before source_field is
  // read below. Spill replay restores the base columns first, so a replayed row
  // re-derives to exactly the same value.
  if (EvaluateExpressionFields()) return true;
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

bool VectorizedAggregateIterator::UpdateHashGroupFromBatch(HashGroupState *group, size_t row_idx) {
  if (group == nullptr) return true;

  auto add_decimal = [](HashAggregateState *state, const my_decimal &value) -> bool {
    if (!state->decimal_value) {
      my_decimal2decimal(&value, &state->decimal_sum);
      state->decimal_value = true;
      return false;
    }
    my_decimal result;
    if (my_decimal_add(E_DEC_FATAL_ERROR, &result, &state->decimal_sum, &value) > 1) return true;
    state->decimal_sum = result;
    return false;
  };

  for (size_t index = 0; index < m_vectorizer.aggregate_infos.size(); ++index) {
    const auto &info = m_vectorizer.aggregate_infos[index];
    HashAggregateState &state = group->aggregates[index];
    Field *field = info.source_field;

    if (field == nullptr) {
      if (info.type == Item_sum::COUNT_FUNC) {
        ++state.count;
        continue;
      }
      return true;
    }
    if (info.batch_chunk_idx == static_cast<size_t>(-1) || info.batch_chunk_idx >= m_batch_col_chunks.size())
      return true;
    const ColumnChunk &chunk = m_batch_col_chunks[info.batch_chunk_idx];
    if (!chunk.valid() || row_idx >= chunk.size()) return true;
    const bool is_null = chunk.nullable_fast(row_idx);

    if (info.type == Item_sum::COUNT_FUNC) {
      if (!is_null) ++state.count;
      continue;
    }
    if (is_null) continue;

    if (info.type == Item_sum::MIN_FUNC || info.type == Item_sum::MAX_FUNC) {
      // ColumnChunk contains IMCS normalized bytes, not a generic MySQL Field
      // image. Convert this one aggregate field through Field::pack() before
      // using Field::cmp(), while keeping the rest of the row columnar.
      if (RestoreBatchField(field, row_idx)) return true;
      const size_t bytes = field->pack_length();
      const uchar *value = field->field_ptr();
      if (!state.has_value) {
        state.extremum.assign(value, value + bytes);
        state.has_value = true;
      } else {
        const int cmp = field->cmp(value, state.extremum.data());
        if ((info.type == Item_sum::MIN_FUNC && cmp < 0) || (info.type == Item_sum::MAX_FUNC && cmp > 0))
          state.extremum.assign(value, value + bytes);
      }
      continue;
    }

    state.has_value = true;
    ++state.count;

    // AVG accumulates exactly like SUM -- the count is tracked separately just
    // above and handed to Item_sum_avg together with the sum, so integer and
    // decimal inputs go through the same exact my_decimal accumulation instead
    // of a lossy double. Per-row association order is preserved for FP; there
    // is no horizontal reduction here.

    switch (field->type()) {
      case MYSQL_TYPE_LONG: {
        if (field->is_unsigned() || chunk.width() != sizeof(int32_t)) return true;
        int32_t value;
        std::memcpy(&value, chunk.data_fast(row_idx), sizeof(value));
        my_decimal decimal;
        longlong2decimal(static_cast<longlong>(value), &decimal);
        if (add_decimal(&state, decimal)) return true;
      } break;
      case MYSQL_TYPE_LONGLONG: {
        if (field->is_unsigned() || chunk.width() != sizeof(int64_t)) return true;
        int64_t value;
        std::memcpy(&value, chunk.data_fast(row_idx), sizeof(value));
        my_decimal decimal;
        longlong2decimal(static_cast<longlong>(value), &decimal);
        if (add_decimal(&state, decimal)) return true;
      } break;
      case MYSQL_TYPE_NEWDECIMAL: {
        if (RestoreBatchField(field, row_idx)) return true;
        my_decimal value;
        field->val_decimal(&value);
        if (add_decimal(&state, value)) return true;
      } break;
      case MYSQL_TYPE_FLOAT: {
        if (chunk.width() != sizeof(float)) return true;
        float value;
        std::memcpy(&value, chunk.data_fast(row_idx), sizeof(value));
        state.real_sum += static_cast<double>(value);
      } break;
      case MYSQL_TYPE_DOUBLE: {
        if (chunk.width() != sizeof(double)) return true;
        double value;
        std::memcpy(&value, chunk.data_fast(row_idx), sizeof(value));
        state.real_sum += value;
      } break;
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
        Item_sum_sum *sum = down_cast<Item_sum_sum *>(item);
        if (state.decimal_value) {
          if (m_vectorizer.Sum(sum, info.source_field, state.decimal_sum)) return 1;
        } else if (m_vectorizer.Sum(sum, info.source_field, state.real_sum)) {
          return 1;
        }
      } break;
      case Item_sum::AVG_FUNC: {
        if (!state.has_value || state.count == 0 || info.source_field == nullptr) break;
        // Hand the accumulated sum and count to Item_sum_avg directly rather
        // than storing a finalized double average back through the source
        // Field. Item_sum_avg performs the division itself (my_decimal_div
        // with prec_increment for DECIMAL_RESULT, sum/m_count for
        // REAL_RESULT), so this is exact for integer and decimal inputs --
        // the Field round-trip truncated those to the source column's scale.
        Item_sum_avg *avg = down_cast<Item_sum_avg *>(item);
        if (state.decimal_value) {
          if (m_vectorizer.Sum(avg, info.source_field, state.decimal_sum)) return 1;
        } else if (m_vectorizer.Sum(avg, info.source_field, state.real_sum)) {
          return 1;
        }
        avg->add_count(state.count);
      } break;
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
        if (state.has_value) {
          info.source_field->set_notnull();
          memcpy(info.source_field->field_ptr(), state.extremum.data(), state.extremum.size());
          Item *arg = item->get_arg(0);
          if (arg != nullptr) arg->null_value = false;
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
  if (!m_batch_chunks_initialized) SetupBatchChunks();
  // STREAMING GROUP BY is batch-capable too. The old `&& !do_group_by`
  // condition made the boundary-detection branch below permanently
  // unreachable. Only use it when every representative Field can be safely
  // reconstructed from the IMCS fixed-width batch representation.
  // An expression aggregate has no column in the source's batch: its value is
  // derived per row by EvaluateExpressionFields(), which only runs on the
  // row-by-row path (from AppendCurrentRowToChunks()). Taking the batch path
  // would leave its chunk unfilled, so only the first row -- the one
  // reset_and_add() already consumed through the Item -- would reach the
  // aggregate. Read row-by-row instead; the aggregation itself stays vectorized.
  const bool use_batch =
      m_batch_source != nullptr && (!do_group_by || CanUseBatchGrouping()) && !HasExpressionAggregate();
  // reset_and_add() has already consumed the first row of this group.
  ++m_stats.rows_in;

  for (;;) {
    size_t rows_read = 0;
    bool is_eof = false;
    // Row-by-row only: true when the current row in table->field belongs
    // to the NEXT group (not appended to the current batch).
    bool next_group_row_in_table = false;

    if (use_batch) {
      if (!EnsureBatchCapacity(m_vectorizer.opt_batch_size)) return 1;
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
          StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
          break;  // current row → next group; do NOT append
        }

        if (AppendCurrentRowToChunks()) return 1;
        ++rows_read;

        if (m_vectorizer.current_batch.full()) break;
      }
    }

    // Row-by-row only: the row that starts the next group is already read into
    // table->field. It must be snapshotted now, before ProcessVectorizedAggregates()
    // below flushes the group just finished -- that flush replays the finished
    // group's rows through RestoreRowFromBatch(), which overwrites the very same
    // aggregate-source Field buffers (via aggregator_add()) with the last replayed
    // row's value, clobbering the next-group row still sitting in table->field.
    if (!use_batch && next_group_row_in_table) StoreFromTableBuffers(m_tables, &m_first_row_next_grp);

    if (rows_read == 0) {
      if (is_eof) {
        m_seen_eof = true;
        StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
        LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
        SetRollupLevel(m_join->send_group_parts);
        m_state = DONE_OUTPUTTING_ROWS;
        ++m_stats.rows_out;
        break;
      }
      if (next_group_row_in_table) {
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
        if (RestoreGroupKeyField(r)) return 1;
        if (update_item_cache_if_changed(m_join->group_fields) >= 0) {
          boundary = r;
          break;
        }
      }
    }

    if (boundary > 0) {
      const auto batch_started = std::chrono::steady_clock::now();
      if (use_batch) {
        if (ProcessVectorizedAggregates(m_batch_col_chunks, boundary) != 0) return 1;
      } else {
        if (ProcessVectorizedAggregates() != 0) return 1;
      }
      m_stats.total_batches_processed++;
      m_stats.total_rows_vectorized += boundary;
      m_stats.rows_in += boundary;
      const double elapsed_ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - batch_started).count();
      m_stats.total_vectorized_time_ms += elapsed_ms;
      m_stats.avg_batch_processing_time_ms =
          m_stats.total_vectorized_time_ms / static_cast<double>(m_stats.total_batches_processed);
      UpdateBatchSizeFromPerformance(elapsed_ms);
    }

    const bool group_changed = use_batch ? (boundary < rows_read) : next_group_row_in_table;
    if (group_changed) {
      if (use_batch) {
        if (m_batch_source->PushbackBatchTail(m_batch_col_chunks, boundary + 1, rows_read)) {
          my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
                   "Rapid aggregate could not buffer the rows following a GROUP BY boundary");
          return 1;
        }
        if (RestoreBoundaryRowToTableFields(boundary)) return 1;
        StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
      }
      // else (!use_batch): already captured into m_first_row_next_grp right after
      // it was detected, before ProcessVectorizedAggregates() could reuse the
      // aggregate source fields as scratch space and clobber table->field.

      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
      m_last_unchanged_grp_item_idx = 0;
      m_state = LAST_ROW_STARTED_NEW_GROUP;
      ++m_stats.rows_out;
      return 0;
    }

    if (is_eof) {
      m_seen_eof = true;
      StoreFromTableBuffers(m_tables, &m_first_row_next_grp);
      LoadIntoTableBuffers(m_tables, pointer_cast<const uchar *>(m_first_row_this_grp.ptr()));
      SetRollupLevel(m_join->send_group_parts);
      m_state = DONE_OUTPUTTING_ROWS;
      ++m_stats.rows_out;
      break;
    }
  }
  return 0;
}

bool VectorizedAggregateIterator::RestoreBoundaryRowToTableFields(size_t boundary) {
  for (const auto &[field, chunk_idx] : m_field_to_batch_chunk_idx) {
    (void)chunk_idx;
    if (RestoreBatchField(field, boundary)) return true;
  }
  ++m_stats.row_materializations;
  return false;
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
        auto add_scalar_rows = [&]() -> int {
          for (size_t row = 0; row < row_count; ++row) {
            if (chunk.nullable_fast(row)) {
              info.source_field->set_null();
            } else {
              info.source_field->set_notnull();
              info.source_field->pack(const_cast<uchar *>(info.source_field->data_ptr()), chunk.data_fast(row),
                                      chunk.width());
              m_stats.bytes_copied += chunk.width();
            }
            if (info.item->aggregator_add()) return 1;
          }
          m_stats.scalar_fallback_rows += row_count;
          return 0;
        };

        switch (info.source_field->type()) {
          case MYSQL_TYPE_LONGLONG: {
            if (non_null == 0) break;
            my_decimal delta = ColumnChunkOper::Sum<my_decimal>(chunk, row_count);
            if (m_vectorizer.Sum(sum_item, info.source_field, delta)) return 1;
          } break;
          case MYSQL_TYPE_LONG: {
            if (non_null == 0) break;
            my_decimal delta = ColumnChunkOper::Sum<my_decimal>(chunk, row_count);
            if (m_vectorizer.Sum(sum_item, info.source_field, delta)) return 1;
          } break;
          case MYSQL_TYPE_FLOAT:
          case MYSQL_TYPE_DOUBLE:
            if (non_null == 0) break;
            // MySQL's floating SUM is order-sensitive. A horizontal SIMD sum
            // (or even one scalar batch sum followed by add_value()) changes
            // the parenthesization across batch boundaries. Preserve the exact
            // row-wise Item_sum_sum update order.
            if (add_scalar_rows() != 0) return 1;
            break;
          case MYSQL_TYPE_NEWDECIMAL: {
            if (non_null == 0) break;
            my_decimal value = ColumnChunkOper::Sum<my_decimal>(chunk, row_count);
            if (m_vectorizer.Sum(sum_item, info.source_field, value)) return 1;
          } break;
          default:
            if (add_scalar_rows() != 0) return 1;
            break;
        }
        break;
      }
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC: {
        if (non_null == 0) break;
        const bool is_min = info.type == Item_sum::MIN_FUNC;
        bool reduced = true;
        bool fixed_width_simd_candidate = false;
        auto store_reduced_value = [&](const auto &store) -> bool {
          TABLE *table = info.source_field->table;
          const uint field_index = info.source_field->field_index();
          const bool restore_write_set =
              table != nullptr && table->write_set != nullptr && !bitmap_is_set(table->write_set, field_index);
          if (restore_write_set) bitmap_set_bit(table->write_set, field_index);
          info.source_field->set_notnull();
          const type_conversion_status status = store();
          if (restore_write_set) bitmap_clear_bit(table->write_set, field_index);
          return status == TYPE_ERR_BAD_VALUE;
        };
        switch (info.source_field->type()) {
          case MYSQL_TYPE_LONG: {
            if (chunk.width() != sizeof(int32_t)) {
              reduced = false;
              break;
            }
            const auto *data = reinterpret_cast<const int32_t *>(chunk.data_fast(0));
            const int32_t value = is_min ? Kernels::Min<int32_t>(data, chunk.get_null_mask()->data, row_count)
                                         : Kernels::Max<int32_t>(data, chunk.get_null_mask()->data, row_count);
            if (store_reduced_value([&] { return info.source_field->store(static_cast<longlong>(value), false); }))
              return 1;
            fixed_width_simd_candidate = true;
          } break;
          case MYSQL_TYPE_LONGLONG: {
            if (chunk.width() != sizeof(int64_t)) {
              reduced = false;
              break;
            }
            const auto *data = reinterpret_cast<const int64_t *>(chunk.data_fast(0));
            const int64_t value = is_min ? Kernels::Min<int64_t>(data, chunk.get_null_mask()->data, row_count)
                                         : Kernels::Max<int64_t>(data, chunk.get_null_mask()->data, row_count);
            if (store_reduced_value([&] { return info.source_field->store(static_cast<longlong>(value), false); }))
              return 1;
            fixed_width_simd_candidate = true;
          } break;
          case MYSQL_TYPE_NEWDECIMAL: {
            my_decimal value = is_min ? ColumnChunkOper::Min<my_decimal>(chunk, row_count)
                                      : ColumnChunkOper::Max<my_decimal>(chunk, row_count);
            if (store_reduced_value([&] { return info.source_field->store_decimal(&value); })) return 1;
          } break;
          default:
            // FLOAT/DOUBLE retain MySQL's scalar comparison semantics for NaN
            // and signed zero. This is a field-local fallback, not a full-row
            // materialization.
            reduced = false;
            break;
        }

        if (reduced) {
          Item *arg = info.item->get_arg(0);
          if (arg != nullptr) arg->null_value = false;
          if (info.item->aggregator_add()) return 1;
          if (fixed_width_simd_candidate) {
            if (Kernels::HasRuntimeSimd())
              m_stats.simd_rows += row_count;
            else
              m_stats.scalar_fallback_rows += row_count;
          }
          break;
        }

        for (size_t row = 0; row < row_count; ++row) {
          bool is_null = chunk.nullable(row);
          if (is_null) {
            info.source_field->set_null();
          } else {
            info.source_field->set_notnull();
            info.source_field->pack(const_cast<uchar *>(info.source_field->data_ptr()), chunk.data(row), chunk.width());
            m_stats.bytes_copied += chunk.width();
          }
          if (info.item->aggregator_add()) return 1;
        }
        m_stats.scalar_fallback_rows += row_count;
        break;
      }
      case Item_sum::AVG_FUNC: {
        // Item_sum_avg keeps a sum and a count, and both add_value() and
        // add_count() accumulate, so the batch's vectorized sum plus its
        // non-NULL count reproduce the row-by-row state exactly -- no batch
        // average is ever fed back through the source Field. FLOAT/DOUBLE stay
        // row-wise for the same order-sensitivity reason as SUM above.
        Item_sum_avg *avg_item = down_cast<Item_sum_avg *>(info.item);
        auto avg_scalar_rows = [&]() -> int {
          for (size_t row = 0; row < row_count; ++row) {
            if (chunk.nullable_fast(row)) {
              info.source_field->set_null();
            } else {
              info.source_field->set_notnull();
              info.source_field->pack(const_cast<uchar *>(info.source_field->data_ptr()), chunk.data_fast(row),
                                      chunk.width());
              m_stats.bytes_copied += chunk.width();
            }
            if (info.item->aggregator_add()) return 1;
          }
          m_stats.scalar_fallback_rows += row_count;
          return 0;
        };

        switch (info.source_field->type()) {
          case MYSQL_TYPE_LONG:
          case MYSQL_TYPE_LONGLONG:
          case MYSQL_TYPE_NEWDECIMAL: {
            if (non_null == 0) break;
            info.source_field->set_notnull();
            my_decimal delta = ColumnChunkOper::Sum<my_decimal>(chunk, row_count);
            if (m_vectorizer.Sum(avg_item, info.source_field, delta)) return 1;
            avg_item->add_count(non_null);
          } break;
          case MYSQL_TYPE_FLOAT:
          case MYSQL_TYPE_DOUBLE:
            if (non_null == 0) break;
            if (avg_scalar_rows() != 0) return 1;
            break;
          default:
            if (avg_scalar_rows() != 0) return 1;
            break;
        }
        break;
      }
      default: {
        for (size_t row = 0; row < row_count; ++row) {
          const bool is_null = chunk.nullable_fast(row);
          if (is_null) {
            info.source_field->set_null();
          } else {
            info.source_field->set_notnull();
            info.source_field->pack(const_cast<uchar *>(info.source_field->data_ptr()), chunk.data_fast(row),
                                    chunk.width());
            m_stats.bytes_copied += chunk.width();
          }
          if (info.item->aggregator_add()) return 1;
        }
        m_stats.scalar_fallback_rows += row_count;
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
    auto add_scalar_rows = [&]() -> int {
      for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
        RestoreRowFromBatch(row, idx);
        if (info.item->aggregator_add()) return 1;
      }
      m_stats.scalar_fallback_rows += m_vectorizer.current_batch.row_count;
      return 0;
    };
    field->set_notnull();
    switch (field->type()) {
      case MYSQL_TYPE_LONG: {
        my_decimal delta = ColumnChunkOper::Sum<my_decimal>(chunk, m_vectorizer.current_batch.row_count);
        if (m_vectorizer.Sum(sum_item, field, delta)) return 1;
      } break;
      case MYSQL_TYPE_LONGLONG: {
        my_decimal delta = ColumnChunkOper::Sum<my_decimal>(chunk, m_vectorizer.current_batch.row_count);
        if (m_vectorizer.Sum(sum_item, field, delta)) return 1;
      } break;
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
        if (add_scalar_rows() != 0) return 1;
        break;
      case MYSQL_TYPE_NEWDECIMAL: {
        auto sum_decimal = ColumnChunkOper::Sum<my_decimal>(chunk, m_vectorizer.current_batch.row_count);
        if (m_vectorizer.Sum(sum_item, field, sum_decimal)) return 1;
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
    Field *field = info.source_field;
    if (field == nullptr) return 1;
    if (ColumnChunkOper::CountNonNull(chunk, m_vectorizer.current_batch.row_count) == 0) continue;

    const bool is_min = info.type == Item_sum::MIN_FUNC;
    bool reduced = true;
    auto store_reduced_value = [&](const auto &store) -> bool {
      TABLE *table = field->table;
      const uint field_index = field->field_index();
      const bool restore_write_set =
          table != nullptr && table->write_set != nullptr && !bitmap_is_set(table->write_set, field_index);
      if (restore_write_set) bitmap_set_bit(table->write_set, field_index);
      field->set_notnull();
      const type_conversion_status status = store();
      if (restore_write_set) bitmap_clear_bit(table->write_set, field_index);
      return status == TYPE_ERR_BAD_VALUE;
    };
    switch (field->type()) {
      case MYSQL_TYPE_LONG: {
        if (chunk.width() != sizeof(int32_t)) {
          reduced = false;
          break;
        }
        const auto *data = reinterpret_cast<const int32_t *>(chunk.data_fast(0));
        const int32_t value =
            is_min ? Kernels::Min<int32_t>(data, chunk.get_null_mask()->data, m_vectorizer.current_batch.row_count)
                   : Kernels::Max<int32_t>(data, chunk.get_null_mask()->data, m_vectorizer.current_batch.row_count);
        if (store_reduced_value([&] { return field->store(static_cast<longlong>(value), false); })) return 1;
      } break;
      case MYSQL_TYPE_LONGLONG: {
        if (chunk.width() != sizeof(int64_t)) {
          reduced = false;
          break;
        }
        const auto *data = reinterpret_cast<const int64_t *>(chunk.data_fast(0));
        const int64_t value =
            is_min ? Kernels::Min<int64_t>(data, chunk.get_null_mask()->data, m_vectorizer.current_batch.row_count)
                   : Kernels::Max<int64_t>(data, chunk.get_null_mask()->data, m_vectorizer.current_batch.row_count);
        if (store_reduced_value([&] { return field->store(static_cast<longlong>(value), false); })) return 1;
      } break;
      case MYSQL_TYPE_NEWDECIMAL: {
        my_decimal value = is_min ? ColumnChunkOper::Min<my_decimal>(chunk, m_vectorizer.current_batch.row_count)
                                  : ColumnChunkOper::Max<my_decimal>(chunk, m_vectorizer.current_batch.row_count);
        if (store_reduced_value([&] { return field->store_decimal(&value); })) return 1;
      } break;
      default:
        reduced = false;
        break;
    }

    if (reduced) {
      Item *arg = info.item->get_arg(0);
      if (arg != nullptr) arg->null_value = false;
      if (info.item->aggregator_add()) return 1;
      continue;
    }

    // FLOAT/DOUBLE keep the exact scalar MySQL comparison path.
    // In particular, do not replace MySQL NaN/signed-zero MIN/MAX semantics
    // with hardware min/max semantics until those are proven equivalent.
    for (size_t row = 0; row < m_vectorizer.current_batch.row_count; ++row) {
      if (!chunk.nullable_fast(row)) {
        RestoreRowFromBatch(row, idx);
        if (info.item->aggregator_add()) return 1;
      }
    }
  }
  return 0;
}

int VectorizedAggregateIterator::ProcessAvgAggregates(const std::vector<size_t> &avg_indices) {
  // AVG needs a sum and a count, which is why this used to replay every row
  // through aggregator_add(). It does not have to: Item_sum_avg::add_value()
  // and add_count() are both accumulative, so a batch can hand over its
  // vectorized sum and its non-NULL count in one step -- the same route
  // MaterializeHashGroup() already takes. Only the parenthesization of a
  // floating-point sum is at stake, so FLOAT/DOUBLE stay row-wise exactly as
  // ProcessSumAggregates() keeps them.
  for (size_t idx : avg_indices) {
    const auto &info = m_vectorizer.aggregate_infos[idx];
    const auto &chunk = m_vectorizer.current_batch.column_chunks[idx];
    Field *field = info.source_field;
    const size_t rows = m_vectorizer.current_batch.row_count;

    auto add_scalar_rows = [&]() -> int {
      for (size_t row = 0; row < rows; ++row) {
        RestoreRowFromBatch(row, idx);
        if (info.item->aggregator_add()) return 1;
      }
      m_stats.scalar_fallback_rows += rows;
      return 0;
    };

    if (field == nullptr) {
      if (add_scalar_rows() != 0) return 1;
      continue;
    }

    const size_t non_null = ColumnChunkOper::CountNonNull(chunk, rows);
    if (non_null == 0) continue;  // AVG ignores NULLs: nothing to contribute

    Item_sum_avg *avg_item = down_cast<Item_sum_avg *>(info.item);
    field->set_notnull();
    switch (field->type()) {
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_NEWDECIMAL: {
        my_decimal delta = ColumnChunkOper::Sum<my_decimal>(chunk, rows);
        if (m_vectorizer.Sum(avg_item, field, delta)) return 1;
        avg_item->add_count(non_null);
      } break;
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE:
        if (add_scalar_rows() != 0) return 1;
        break;
      default:
        if (add_scalar_rows() != 0) return 1;
        break;
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

bool VectorizedAggregateIterator::EnsureBatchCapacity(size_t capacity) {
  if (capacity == 0) return false;
  for (ColumnChunk &chunk : m_batch_col_chunks) {
    if (chunk.valid() && chunk.capacity() < capacity && !chunk.grow(capacity)) return false;
  }
  if (m_vectorizer.current_batch.initialized) {
    for (ColumnChunk &chunk : m_vectorizer.current_batch.column_chunks) {
      if (chunk.valid() && chunk.capacity() < capacity && !chunk.grow(capacity)) return false;
    }
    m_vectorizer.current_batch.capacity = std::max(m_vectorizer.current_batch.capacity, capacity);
  }
  return true;
}

bool VectorizedAggregateIterator::RestoreGroupKeyField(size_t row_idx) {
  List_iterator<Cached_item> it(m_join->group_fields);
  Cached_item *cached;
  while ((cached = it++)) {
    Item *item = cached->get_item();
    if (!item || item->type() != Item::FIELD_ITEM) continue;

    Field *field = down_cast<Item_field *>(item)->field;
    if (field == nullptr || RestoreBatchField(field, row_idx)) return true;
  }
  return false;
}

bool VectorizedAggregateIterator::AnalyzeAggregatesForVectorization() {
  m_vectorizer.aggregate_infos.clear();
  bool has_aggregates = false;
  bool all_vectorizable = true;

  for (Item_sum **item = m_join->sum_funcs; *item != nullptr; ++item) {
    VectorizedGroupProcessor::AggregateInfo info;
    info.item = *item;
    info.type = (*item)->sum_func();
    info.vectorizable = IsSimpleAggregate(*item);
    info.source_field = GetPrimaryFieldForAggregate(*item);
    if (info.source_field == nullptr) {
      // SUM/AVG/COUNT over an expression: evaluate it into a synthetic field so
      // the rest of the pipeline treats it exactly like a column.
      if (Item *expr = GetAggregateValueExpr(*item); expr != nullptr) {
        if (Field *expr_field = CreateExpressionField(expr); expr_field != nullptr) {
          info.value_expr = expr;
          info.source_field = expr_field;
        }
      }
    }
    info.source_field_table_index = info.source_field ? static_cast<uint16_t>(info.source_field->field_index()) : 0;
    info.field_index = m_vectorizer.aggregate_infos.size();
    m_vectorizer.aggregate_infos.push_back(info);
    has_aggregates = true;
    all_vectorizable = all_vectorizable && info.vectorizable;
  }
  return has_aggregates && all_vectorizable;
}

bool VectorizedAggregateIterator::IsSimpleAggregate(Item_sum *item) const {
  if (item == nullptr || item->has_with_distinct()) return false;

  Field *field = GetPrimaryFieldForAggregate(item);

  // SUM/AVG/COUNT over an expression argument; the admissible shapes (and the
  // reason MIN/MAX are not among them) live in VectorizableAggregateValueExpr().
  if (field == nullptr && GetAggregateValueExpr(item) != nullptr) return true;

  switch (item->sum_func()) {
    case Item_sum::COUNT_FUNC: {
      if (field != nullptr) return true;
      if (item->arg_count == 0) return true;
      Item *arg = item->get_arg(0);
      return arg != nullptr && arg->const_item() && !arg->is_null();
    }
    case Item_sum::SUM_FUNC: {
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
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC: {
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

namespace {
/**
 * Whether `item` is an arithmetic tree this iterator may evaluate into a
 * synthetic field of the item's own declared type.
 *
 * The restriction is about scale, not about which functions are convenient.
 * Storing an evaluated value into a Field rounds it to that field's declared
 * scale, so this is only lossless where the exact result of the operation
 * already has exactly the declared scale. That holds for the arithmetic
 * operators below, but only after ExactDecimalScale() confirms the exact result
 * scale really equals the declared one -- MySQL clamps the declared scale at
 * DECIMAL_MAX_SCALE while the arithmetic itself does not, so the equality has
 * to be checked rather than assumed. An arbitrary function offers no such
 * guarantee either: its val_decimal() may carry more digits than its declared
 * type, in which case Item_sum would accumulate the unrounded value while we
 * would accumulate a rounded one.
 *
 * Leaves must be columns or constants; anything else (a subquery, another
 * aggregate, a CASE) is rejected.
 */
/**
 * Scale that my_decimal arithmetic actually produces for this subtree, or -1
 * when it cannot be established.
 *
 * This is NOT the same as item->decimals. Item_func_mul::result_precision()
 * declares decimals = min(s1 + s2, DECIMAL_MAX_SCALE), but decimal_op() returns
 * the exact my_decimal_mul result, which carries s1 + s2 digits. Whenever the
 * clamp bites -- DECIMAL(40,20) * DECIMAL(40,20) declares scale 30 and produces
 * scale 40 -- MySQL accumulates the unrounded value while storing it into a
 * field of the declared scale would round it. Comparing the two is what makes
 * the whitelist below an actual safety argument rather than a guess.
 */
int ExactDecimalScale(const Item *item, int depth = 0) {
  if (item == nullptr || depth > 16) return -1;

  const Item *real = item->real_item();
  if (real != nullptr && real->type() == Item::FIELD_ITEM) return real->decimals;
  if (item->const_item()) return item->decimals;
  if (item->type() != Item::FUNC_ITEM) return -1;

  const auto *func = down_cast<const Item_func *>(item);
  auto *mutable_func = const_cast<Item_func *>(func);
  switch (func->functype()) {
    case Item_func::NEG_FUNC:
      return ExactDecimalScale(mutable_func->arguments()[0], depth + 1);
    case Item_func::MUL_FUNC: {
      const int lhs = ExactDecimalScale(mutable_func->arguments()[0], depth + 1);
      const int rhs = ExactDecimalScale(mutable_func->arguments()[1], depth + 1);
      if (lhs < 0 || rhs < 0) return -1;
      return lhs + rhs;  // my_decimal_mul is exact
    }
    case Item_func::PLUS_FUNC:
    case Item_func::MINUS_FUNC: {
      const int lhs = ExactDecimalScale(mutable_func->arguments()[0], depth + 1);
      const int rhs = ExactDecimalScale(mutable_func->arguments()[1], depth + 1);
      if (lhs < 0 || rhs < 0) return -1;
      return std::max(lhs, rhs);  // my_decimal_add/sub is exact at max(s1,s2)
    }
    default:
      return -1;
  }
}

bool IsStorableArithmeticTree(const Item *item, int depth = 0) {
  if (item == nullptr || depth > 16) return false;

  const Item *real = item->real_item();
  if (real != nullptr && real->type() == Item::FIELD_ITEM) return true;
  if (item->const_item()) return true;
  if (item->type() != Item::FUNC_ITEM) return false;

  const auto *func = down_cast<const Item_func *>(item);
  switch (func->functype()) {
    case Item_func::PLUS_FUNC:
    case Item_func::MINUS_FUNC:
    case Item_func::MUL_FUNC:
    case Item_func::NEG_FUNC:
      break;
    default:
      // Division is excluded deliberately. my_decimal_div rounds to a scale
      // derived from div_precision_increment, which is a session variable, so
      // the "exact result already has the declared scale" argument that makes
      // the others safe does not hold for it by construction.
      return false;
  }

  auto *mutable_func = const_cast<Item_func *>(func);
  for (uint i = 0; i < func->argument_count(); ++i) {
    if (!IsStorableArithmeticTree(mutable_func->arguments()[i], depth + 1)) return false;
  }
  return true;
}
}  // namespace

Item *VectorizedAggregateIterator::GetAggregateValueExpr(Item_sum *item) const {
  if (m_strategy != AggregateStrategy::HASH) return nullptr;
  return VectorizableAggregateValueExpr(item);
}

Item *VectorizableAggregateValueExpr(Item_sum *item) {
  // Only SUM/AVG/COUNT: their vectorized routes hand the reduced value straight
  // to Item_sum::add_value()/add_count() and never re-drive the aggregate
  // through aggregator_add(), which would re-evaluate args[0] from the base
  // columns of whatever row is current. See the header for why MIN/MAX cannot
  // join them.
  if (item == nullptr) return nullptr;
  switch (item->sum_func()) {
    case Item_sum::SUM_FUNC:
    case Item_sum::AVG_FUNC:
    case Item_sum::COUNT_FUNC:
      break;
    default:
      return nullptr;
  }
  if (item->has_with_distinct()) return nullptr;

  // Only a genuine expression qualifies: a bare column is the fast path above,
  // and a constant needs no per-row evaluation at all.
  if (item->arg_count != 1) return nullptr;
  Item *arg = item->get_arg(0);
  if (arg == nullptr) return nullptr;
  if (arg->real_item() != nullptr && arg->real_item()->type() == Item::FIELD_ITEM) return nullptr;
  if (arg->const_item()) return nullptr;
  if (!IsStorableArithmeticTree(arg)) return nullptr;

  // The exact result scale must equal the scale the synthetic field will have,
  // or store_decimal() would round a value MySQL accumulates unrounded. This is
  // where DECIMAL(40,20) * DECIMAL(40,20) is turned away: exact scale 40,
  // declared scale clamped to DECIMAL_MAX_SCALE = 30.
  if (arg->result_type() == DECIMAL_RESULT) {
    const int exact_scale = ExactDecimalScale(arg);
    if (exact_scale < 0 || exact_scale != static_cast<int>(arg->decimals)) return nullptr;
  }

  // Exact result types only. A REAL expression is excluded because
  // ProcessSumAggregates()/ProcessAvgAggregates() route FLOAT/DOUBLE through
  // their scalar fallback, which replays rows via aggregator_add() and would
  // re-evaluate the expression from the base columns.
  //
  // Integer expressions are carried in a DECIMAL field rather than an integer
  // one -- see CreateExpressionField() for why that costs nothing.
  switch (arg->result_type()) {
    case DECIMAL_RESULT:
    case INT_RESULT:
      break;
    default:
      return nullptr;
  }
  if (arg->unsigned_flag) return nullptr;  // matches the bare-column restriction
  return arg;
}

Field *VectorizedAggregateIterator::CreateExpressionField(Item *expr) const {
  if (expr == nullptr) return nullptr;

  /*
    Always a DECIMAL field, even when the expression is integral.

    The obvious choice for an INT_RESULT expression is Field_longlong, but this
    field has to work with table == nullptr (below), and Field_longlong::store()
    and val_int() dereference table->s->db_low_byte_first unconditionally.
    Field_new_decimal goes straight through my_decimal2binary/binary2my_decimal
    and touches nothing else.

    Carrying an integer as DECIMAL costs no accuracy and no generality: an
    integer converts to decimal exactly (scale 0), and MySQL already accumulates
    SUM/AVG over an integer argument in a my_decimal -- Item_sum_sum resolves to
    DECIMAL_RESULT for an INT_RESULT argument. ProcessSumAggregates() likewise
    already reduces a bare BIGINT column with ColumnChunkOper::Sum<my_decimal>
    and hands the result to Item_sum_sum::add_value(). So an integer expression
    lands on exactly the same route a plain integer column already takes.
  */
  const uint8 scale = (expr->result_type() == INT_RESULT) ? 0 : std::min<uint>(expr->decimals, DECIMAL_MAX_SCALE);
  uint precision = std::min<uint>(expr->decimal_precision(), DECIMAL_MAX_PRECISION);
  if (precision < scale) precision = scale;
  if (precision == 0) precision = 1;

  const uint32 length = my_decimal_precision_to_length(precision, scale, expr->unsigned_flag);
  Field *field = new (current_thd->mem_root)
      Field_new_decimal(length, /*is_nullable=*/true, expr->item_name.ptr(), scale, expr->unsigned_flag);
  if (field == nullptr) return nullptr;

  // The field arrives without storage. Give it a private buffer: one leading
  // null byte followed by the value, which is the layout move_field() expects,
  // so is_null()/set_null() answer from our own buffer.
  const size_t bytes = field->pack_length() + 1;
  uchar *buffer = pointer_cast<uchar *>(current_thd->mem_root->Alloc(bytes));
  if (buffer == nullptr) return nullptr;
  std::memset(buffer, 0, bytes);
  field->move_field(buffer + 1, buffer, 1);

  // No table. This field is not a column of one: it has no slot in
  // read_set/write_set, so leaving a table pointer set would make every
  // ASSERT_COLUMN_MARKED_FOR_READ/WRITE consult an unrelated column's bit and
  // abort a debug build on the first store. A null table is the documented
  // state for a field built purely to hold a converted value (Field::set_warning
  // says so), and both asserts short-circuit on it.
  field->table = nullptr;
  return field;
}

bool VectorizedAggregateIterator::HasExpressionAggregate() const {
  for (const auto &info : m_vectorizer.aggregate_infos) {
    if (info.value_expr != nullptr) return true;
  }
  return false;
}

bool VectorizedAggregateIterator::EvaluateExpressionFields() {
  for (auto &info : m_vectorizer.aggregate_infos) {
    if (info.value_expr == nullptr || info.source_field == nullptr) continue;

    Item *expr = info.value_expr;
    if (expr->result_type() == DECIMAL_RESULT) {
      my_decimal buffer;
      my_decimal *value = expr->val_decimal(&buffer);
      if (expr->null_value || value == nullptr) {
        info.source_field->set_null();
      } else {
        info.source_field->set_notnull();
        if (info.source_field->store_decimal(value) > 1) return true;
      }
    } else {
      // INT_RESULT: exact conversion into the same decimal representation.
      const longlong value = expr->val_int();
      if (expr->null_value) {
        info.source_field->set_null();
      } else {
        my_decimal buffer;
        int2my_decimal(E_DEC_FATAL_ERROR, value, expr->unsigned_flag, &buffer);
        info.source_field->set_notnull();
        if (info.source_field->store_decimal(&buffer) > 1) return true;
      }
    }
    if (current_thd->is_error()) return true;
  }
  return false;
}

// Row-level helpers
bool VectorizedAggregateIterator::AppendCurrentRowToChunks() {
  // The row is live in the table buffers here; derive expression values before
  // anything reads source_field. A failure must abort the statement rather than
  // append the previous row's leftover value a second time.
  if (EvaluateExpressionFields()) return true;
  for (size_t i = 0; i < m_vectorizer.aggregate_infos.size(); ++i) {
    const auto &agg_info = m_vectorizer.aggregate_infos[i];
    if (!agg_info.vectorizable || !agg_info.source_field) continue;
    auto &chunk = m_vectorizer.current_batch.column_chunks[i];
    Field *field = agg_info.source_field;
    bool is_null = field->is_null();
    const uchar *data = is_null ? nullptr : field->data_ptr();
    size_t data_len = is_null ? 0 : field->pack_length();
    if (chunk.add(const_cast<uchar *>(data), data_len, is_null) && !is_null) m_stats.bytes_copied += data_len;
  }
  m_vectorizer.current_batch.row_count++;
  return false;
}

void VectorizedAggregateIterator::RestoreRowFromBatch(size_t row_idx, size_t agg_idx) {
  if (agg_idx >= m_vectorizer.aggregate_infos.size() || row_idx >= m_vectorizer.current_batch.row_count) return;
  const auto &info = m_vectorizer.aggregate_infos[agg_idx];
  // Restoring is only meaningful for a real column: callers follow it with
  // aggregator_add(), which re-reads args[0]. For an expression aggregate that
  // would re-evaluate against whatever base columns are currently loaded and
  // silently produce a wrong result, so IsSimpleAggregate() must never admit an
  // expression whose path lands here.
  assert(info.value_expr == nullptr);
  if (info.value_expr != nullptr) return;
  auto &chunk = m_vectorizer.current_batch.column_chunks[agg_idx];
  if (chunk.nullable(row_idx)) {
    info.source_field->set_null();
  } else {
    info.source_field->set_notnull();
    memcpy((void *)info.source_field->data_ptr(), chunk.data(row_idx), chunk.width());
    m_stats.bytes_copied += chunk.width();
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
    size_t candidate = m_vectorizer.opt_batch_size;
    if (avg_time > m_target_batch_time_ms * 2.0)
      candidate = std::max(candidate / 2, m_min_batch_size);
    else if (avg_time < m_target_batch_time_ms && candidate < m_max_batch_size)
      candidate = std::min(candidate * 2, m_max_batch_size);

    // Capacity follows the adaptive batch size. Previously opt_batch_size
    // could grow past the buffers allocated by Setup*Chunks(), making the next
    // ReadBatch() fail exactly when the adaptive policy decided to scale up.
    if (candidate <= m_vectorizer.opt_batch_size || EnsureBatchCapacity(candidate))
      m_vectorizer.opt_batch_size = candidate;
  }
}

void VectorizedAggregateIterator::LogPerformanceMetrics() {
  sql_print_information(
      "VectorizedAggregateIterator Performance: "
      "Batches=%zu, VectorizedRows=%zu, Fallbacks=%zu, "
      "AvgBatchTime=%.2fms, TotalVectorizedTime=%.2fms, "
      "HashMemLimit=%zu, HashMemPeak=%zu, HashMemLimitHits=%zu, "
      "RowsIn=%zu, RowsOut=%zu, AvgBatchRows=%.2f, RowMaterializations=%zu, BytesCopied=%zu, "
      "SimdRows=%zu, ScalarFallbackRows=%zu, HashProbes=%zu, HashCollisions=%zu, "
      "HashBatchInputRows=%zu, HashRowInputRows=%zu, HashBatchDirectRows=%zu, "
      "HashRowMaterializations=%zu, HashNewGroupMaterializations=%zu, SpillRows=%zu, "
      "SpillGroups=%zu, SpillPartitions=%zu, SpillRepartitions=%zu, SpillBytes=%zu",
      m_stats.total_batches_processed, m_stats.total_rows_vectorized, m_stats.traditional_fallbacks,
      m_stats.avg_batch_processing_time_ms, m_stats.total_vectorized_time_ms, m_stats.hash_memory_limit_bytes,
      m_stats.hash_memory_peak_bytes, m_stats.hash_memory_limit_hits, m_stats.rows_in, m_stats.rows_out,
      m_stats.avg_batch_rows(), m_stats.row_materializations, m_stats.bytes_copied, m_stats.simd_rows,
      m_stats.scalar_fallback_rows, m_stats.hash_probes, m_stats.hash_collisions, m_stats.hash_batch_input_rows,
      m_stats.hash_row_input_rows, m_stats.hash_batch_direct_rows, m_stats.hash_row_materializations,
      m_stats.hash_new_group_materializations, m_stats.hash_spill_rows, m_stats.hash_spill_groups,
      m_stats.hash_spill_partitions, m_stats.hash_spill_repartitions, m_stats.hash_spill_bytes_written);
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

VectorizedTemptableAggregateIterator::VectorizedTemptableAggregateIterator(
    THD *thd, unique_ptr_destroy_only<RowIterator> subquery_iterator, Temp_table_param *temp_table_param, TABLE *table,
    unique_ptr_destroy_only<RowIterator> table_iterator, JOIN *join, int ref_slice, pack_rows::TableCollection tables,
    bool rollup, AggregateStrategy strategy, ORDER *hash_output_order, double expected_rows, size_t hash_memory_limit)
    : TableRowIterator(thd, table),
      m_agg_iterator(NewIterator<VectorizedAggregateIterator>(thd, thd->mem_root, std::move(subquery_iterator), join,
                                                              std::move(tables), rollup, strategy, hash_output_order,
                                                              expected_rows, hash_memory_limit)),
      m_table_iterator(std::move(table_iterator)),
      m_temp_table_param(temp_table_param),
      m_join(join),
      m_ref_slice(ref_slice) {}

bool VectorizedTemptableAggregateIterator::WriteCurrentGroup() {
  TABLE *t = table();

  // Materialize this group's non-aggregate SELECT expressions into the temp row.
  if (copy_funcs(m_temp_table_param, thd(), CFT_FIELDS)) return true;

  if (using_hash_key()) {
    // Recompute all copied funcs so the hash_field is populated.
    if (copy_funcs(m_temp_table_param, thd())) return true;
  } else {
    // Evaluate the GROUP BY key items into the temp-table group fields and
    // stash the per-key null flag, mirroring TemptableAggregateIterator.
    for (ORDER *group = t->group; group != nullptr; group = group->next) {
      Item *item = *group->item;
      item->save_org_in_field(group->field_in_tmp_table);
      if (thd()->is_error()) return true;
      if (item->is_nullable()) group->buff[-1] = static_cast<char>(group->field_in_tmp_table->is_null());
    }
  }

  {
    // SELECT-list expressions that feed the temp table must be evaluated with
    // the tmp-table ref slice active (same reason as core MySQL).
    Switch_ref_item_slice slice_switch(m_join, m_ref_slice);

    if (!using_hash_key()) {
      ORDER *group = t->group;
      KEY_PART_INFO *key_part = t->key_info[0].key_part;
      for (; group != nullptr; group = group->next, ++key_part) {
        if (key_part->null_bit) memcpy(t->record[0] + key_part->offset - 1, group->buff - 1, 1);
      }
      if (copy_funcs(m_temp_table_param, thd())) return true;
    }

    // The group is already fully aggregated in the Item_sum objects (Rapid
    // accumulated it), so store their finished values straight into the temp
    // columns.  This differs from core MySQL's init/update_tmptable_sum_func(),
    // which accumulate row-by-row into result_field from args[0].
    for (Item_sum **f = m_join->sum_funcs; *f != nullptr; ++f) {
      Item_sum *func = *f;
      Field *result_field = func->get_result_field();
      if (result_field == nullptr) continue;
      result_field->set_notnull();
      func->save_in_field(result_field, /*no_conversions=*/true);
      if (thd()->is_error()) return true;
    }
  }

  int error = t->file->ha_write_row(t->record[0]);
  if (error == 0) return false;

  if (error == HA_ERR_FOUND_DUPP_KEY || error == HA_ERR_FOUND_DUPP_UNIQUE) {
    // A correct hash/stream aggregate emits every group exactly once, so a
    // duplicate temp-table key means two group keys that are distinct in Rapid
    // collapsed to the same temp-table key. The known cause is grouping on a
    // TIMESTAMP in a DST timezone (non-deterministic); mirror core MySQL's
    // TemptableAggregateIterator diagnostic for that case instead of a generic
    // plugin error, and never silently merge the groups.
    if (error == HA_ERR_FOUND_DUPP_KEY) {
      for (ORDER *group = t->group; group != nullptr; group = group->next) {
        if (group->field_in_tmp_table != nullptr && group->field_in_tmp_table->type() == MYSQL_TYPE_TIMESTAMP) {
          my_error(ER_GROUPING_ON_TIMESTAMP_IN_DST, MYF(0));
          return true;
        }
      }
    }
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid vectorized GROUP BY produced a duplicate temp-table key");
    return true;
  }

  // In-memory temp table exhausted: convert it to an on-disk table. With
  // insert_last_record=true, create_ondisk_from_heap re-inserts record[0] into
  // the new table itself (see sql/sql_tmp_table.cc and core MySQL's
  // TemptableAggregateIterator::move_table_to_disk), so this group must NOT be
  // written again here.
  if (create_ondisk_from_heap(thd(), t, error, /*insert_last_record=*/true, /*ignore_last_dup=*/false,
                              /*is_duplicate=*/nullptr))
    return true;
  return false;
}

bool VectorizedTemptableAggregateIterator::Init() {
  if (!m_materialized) {
    // Feed the aggregate from base-table values.
    m_join->set_ref_item_slice(REF_SLICE_SAVED_BASE);

    if (m_agg_iterator->Init()) return true;

    if (!table()->is_created()) {
      if (instantiate_tmp_table(thd(), table())) return true;
      empty_record(table());
    } else {
      if (table()->file->inited) table()->file->ha_index_or_rnd_end();
      table()->file->ha_delete_all_rows();
    }

    for (;;) {
      const int read_error = m_agg_iterator->Read();
      if (read_error > 0 || thd()->is_error()) return true;
      if (read_error < 0) break;  // EOF: every group produced
      if (thd()->killed) {
        thd()->send_kill_message();
        return true;
      }
      if (WriteCurrentGroup()) return true;
    }

    table()->materialized = true;
    m_materialized = true;
  }

  if (m_ref_slice != -1 && !m_join->ref_items[m_ref_slice].is_null()) m_join->set_ref_item_slice(m_ref_slice);
  return m_table_iterator->Init();
}

int VectorizedTemptableAggregateIterator::Read() {
  if (m_join != nullptr && m_ref_slice != -1 && !m_join->ref_items[m_ref_slice].is_null()) {
    m_join->set_ref_item_slice(m_ref_slice);
  }
  return m_table_iterator->Read();
}
}  // namespace Executor
}  // namespace ShannonBase
