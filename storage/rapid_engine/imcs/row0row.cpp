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

   The fundmental code for imcs.

   Copyright (c) 2023, 2024, 2025 Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/imcs/row0row.h"

#include <limits.h>

#include "sql/field.h"                    //Field
#include "sql/field_common_properties.h"  // is_numeric_type
#include "sql/sql_class.h"
#include "sql/table.h"  //TABLE

#include "storage/innobase/include/mach0data.h"
#include "storage/rapid_engine/imcs/imcs.h"  // imcs:pool
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/trx/transaction.h"  // TransactionJournal
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {
void StorageIndex::update(uint32_t col_idx, double value) {
  if (col_idx >= m_num_columns) return;

  std::unique_lock lock(m_mutex);
  auto &stats = m_column_stats[col_idx];
  stats.sum.fetch_add(value, std::memory_order_relaxed);

  double current_min = stats.min_value.load(std::memory_order_relaxed);
  while (value < current_min) {
    if (stats.min_value.compare_exchange_weak(current_min, value, std::memory_order_relaxed)) {
      m_dirty.store(true, std::memory_order_relaxed);
      break;
    }
  }

  double current_max = stats.max_value.load(std::memory_order_relaxed);
  while (value > current_max) {
    if (stats.max_value.compare_exchange_weak(current_max, value, std::memory_order_relaxed)) {
      m_dirty.store(true, std::memory_order_relaxed);
      break;
    }
  }
}

void StorageIndex::update_null(uint32_t col_idx) {
  if (col_idx >= m_num_columns) return;

  std::unique_lock lock(m_mutex);
  auto &stats = m_column_stats[col_idx];
  stats.null_count.fetch_add(1, std::memory_order_relaxed);
  stats.has_null.store(true, std::memory_order_relaxed);
  m_dirty.store(true, std::memory_order_relaxed);
}

void StorageIndex::rebuild(const Imcu *imcu) {}

const StorageIndex::ColumnStats *StorageIndex::get_column_stats_snapshot(uint32_t col_idx) const {
  if (col_idx >= m_num_columns) return nullptr;

  std::shared_lock lock(m_mutex);
  return &m_column_stats[col_idx];
}

bool StorageIndex::can_skip_imcu(const std::vector<std::unique_ptr<Predicate>> &predicates) const {
  for (const auto &predict : predicates) {
    auto pred = down_cast<Simple_Predicate *>(predict.get());
    uint32_t col_idx = pred->column_id;
    if (col_idx >= m_num_columns) continue;

    // Use atomic loads for fast path checking
    const double min_val = get_min_value(col_idx);
    const double max_val = get_max_value(col_idx);
    const bool has_nulls = get_has_null(col_idx);
    const size_t null_cnt = get_null_count(col_idx);

    // Check range predicates using atomic values
    switch (pred->op) {
      case PredicateOperator::EQUAL:
        // col = value
        if (pred->value.as_double() < min_val || pred->value.as_double() > max_val) return true;  // Can skip
        break;
      case PredicateOperator::GREATER_THAN:
        // col > value
        if (max_val <= pred->value.as_double()) return true;  // Entire IMCU's max <= value, skip
        break;
      case PredicateOperator::LESS_THAN:
        // col < value
        if (min_val >= pred->value.as_double()) return true;  // Entire IMCU's min >= value, skip
        break;
      case PredicateOperator::BETWEEN:
        // col BETWEEN min AND max
        if (max_val < pred->value.as_double() || min_val > pred->value2.as_double())
          return true;  // Ranges don't overlap, skip
        break;
      case PredicateOperator::IS_NULL:
        // col IS NULL
        if (!has_nulls) return true;  // No NULL values, skip
        break;
      case PredicateOperator::IS_NOT_NULL:
        // col IS NOT NULL
        if (null_cnt == m_num_columns)  // Using m_num_columns instead of vector size
          return true;                  // All NULL, skip
        break;
      default:
        assert(false);
        break;
    }
  }
  return false;  // Cannot skip
}

double StorageIndex::estimate_selectivity(const std::vector<Predicate> &predicates) const {
  if (predicates.empty()) return 1.0;
  double selectivity = 1.0;

  for (const auto &pred : predicates) {
    // Each predicate estimates its own selectivity using this StorageIndex
    double pred_selectivity = pred.estimate_selectivity(this);
    // Combine selectivities (assume independence)
    selectivity *= pred_selectivity;
  }
  // Clamp result to reasonable range
  return std::max(0.0001, std::min(1.0, selectivity));
}

void StorageIndex::update_string_stats(uint32_t col_idx, const std::string &value) {
  if (col_idx >= m_num_columns) return;

  auto &stats = m_column_stats[col_idx];
  std::lock_guard lock(stats.m_string_mutex);
  if (stats.min_string.empty() || value < stats.min_string) {
    stats.min_string = value;
    m_dirty.store(true, std::memory_order_relaxed);
  }
  if (stats.max_string.empty() || value > stats.max_string) {
    stats.max_string = value;
    m_dirty.store(true, std::memory_order_relaxed);
  }
}

bool StorageIndex::serialize(std::ostream &out) const {
  std::shared_lock lock(m_mutex);

  // Write number of columns
  out.write(reinterpret_cast<const char *>(&m_num_columns), sizeof(m_num_columns));

  // Write statistics for each column (load atomic values)
  for (const auto &stats : m_column_stats) {
    double min_val = stats.min_value.load(std::memory_order_acquire);
    double max_val = stats.max_value.load(std::memory_order_acquire);
    double sum_val = stats.sum.load(std::memory_order_acquire);
    double avg_val = stats.avg.load(std::memory_order_acquire);
    size_t null_cnt = stats.null_count.load(std::memory_order_acquire);
    bool has_nulls = stats.has_null.load(std::memory_order_acquire);
    size_t distinct_cnt = stats.distinct_count.load(std::memory_order_acquire);

    out.write(reinterpret_cast<const char *>(&min_val), sizeof(min_val));
    out.write(reinterpret_cast<const char *>(&max_val), sizeof(max_val));
    out.write(reinterpret_cast<const char *>(&sum_val), sizeof(sum_val));
    out.write(reinterpret_cast<const char *>(&avg_val), sizeof(avg_val));
    out.write(reinterpret_cast<const char *>(&null_cnt), sizeof(null_cnt));
    out.write(reinterpret_cast<const char *>(&has_nulls), sizeof(has_nulls));
    out.write(reinterpret_cast<const char *>(&distinct_cnt), sizeof(distinct_cnt));

    // Serialize string data with protection
    std::lock_guard string_lock(stats.m_string_mutex);
    size_t min_str_len = stats.min_string.size();
    size_t max_str_len = stats.max_string.size();
    out.write(reinterpret_cast<const char *>(&min_str_len), sizeof(min_str_len));
    if (min_str_len > 0) {
      out.write(stats.min_string.c_str(), min_str_len);
    }
    out.write(reinterpret_cast<const char *>(&max_str_len), sizeof(max_str_len));
    if (max_str_len > 0) {
      out.write(stats.max_string.c_str(), max_str_len);
    }
  }
  return out.good();
}

bool StorageIndex::deserialize(std::istream &in) {
  std::unique_lock lock(m_mutex);

  // Read number of columns
  in.read(reinterpret_cast<char *>(&m_num_columns), sizeof(m_num_columns));
  m_column_stats.resize(m_num_columns);
  // Read statistics for each column (store to atomic values)
  for (auto &stats : m_column_stats) {
    double min_val, max_val, sum_val, avg_val;
    size_t null_cnt, distinct_cnt;
    bool has_nulls;

    in.read(reinterpret_cast<char *>(&min_val), sizeof(min_val));
    in.read(reinterpret_cast<char *>(&max_val), sizeof(max_val));
    in.read(reinterpret_cast<char *>(&sum_val), sizeof(sum_val));
    in.read(reinterpret_cast<char *>(&avg_val), sizeof(avg_val));
    in.read(reinterpret_cast<char *>(&null_cnt), sizeof(null_cnt));
    in.read(reinterpret_cast<char *>(&has_nulls), sizeof(has_nulls));
    in.read(reinterpret_cast<char *>(&distinct_cnt), sizeof(distinct_cnt));

    stats.min_value.store(min_val);
    stats.max_value.store(max_val);
    stats.sum.store(sum_val);
    stats.avg.store(avg_val);
    stats.null_count.store(null_cnt);
    stats.has_null.store(has_nulls);
    stats.distinct_count.store(distinct_cnt);

    // Deserialize string data with protection
    std::lock_guard string_lock(stats.m_string_mutex);
    size_t min_str_len, max_str_len;
    in.read(reinterpret_cast<char *>(&min_str_len), sizeof(min_str_len));
    if (min_str_len > 0) {
      stats.min_string.resize(min_str_len);
      in.read(&stats.min_string[0], min_str_len);
    }
    in.read(reinterpret_cast<char *>(&max_str_len), sizeof(max_str_len));
    if (max_str_len > 0) {
      stats.max_string.resize(max_str_len);
      in.read(&stats.max_string[0], max_str_len);
    }
  }
  return in.good();
}

RowBuffer::ColumnValue::ColumnValue() : data(nullptr), length(0), type(MYSQL_TYPE_NULL) {
  std::memset(&flags, 0, sizeof(flags));
}

RowBuffer::ColumnValue::ColumnValue(const ColumnValue &other)
    : length(other.length), flags(other.flags), type(other.type) {
  if (other.flags.is_zero_copy) {
    // Zero-copy, directly copy pointer
    data = other.data;
  } else if (other.owned_buffer) {
    // Deep copy
    owned_buffer = std::make_unique<uchar[]>(length);
    std::memcpy(owned_buffer.get(), other.data, length);
    data = owned_buffer.get();
  } else {
    data = nullptr;
  }
}

RowBuffer::ColumnValue::ColumnValue(ColumnValue &&other) noexcept
    : data(other.data),
      length(other.length),
      flags(other.flags),
      owned_buffer(std::move(other.owned_buffer)),
      type(other.type) {
  other.data = nullptr;
  other.length = 0;
}

RowBuffer::ColumnValue &RowBuffer::ColumnValue::operator=(const ColumnValue &other) {
  if (this != &other) {
    length = other.length;
    flags = other.flags;
    type = other.type;

    if (other.flags.is_zero_copy) {
      data = other.data;
      owned_buffer.reset();
    } else if (other.owned_buffer) {
      owned_buffer = std::make_unique<uchar[]>(length);
      std::memcpy(owned_buffer.get(), other.data, length);
      data = owned_buffer.get();
    } else {
      data = nullptr;
      owned_buffer.reset();
    }
  }
  return *this;
}

RowBuffer::ColumnValue &RowBuffer::ColumnValue::operator=(ColumnValue &&other) noexcept {
  if (this != &other) {
    data = other.data;
    length = other.length;
    flags = other.flags;
    owned_buffer = std::move(other.owned_buffer);
    type = other.type;

    other.data = nullptr;
    other.length = 0;
  }
  return *this;
}

void RowBuffer::set_column_zero_copy(uint32_t col_idx, const uchar *data, size_t length, enum_field_types type) {
  if (col_idx >= m_num_columns) return;

  ColumnValue &col = m_columns[col_idx];
  col.data = data;
  col.length = length;
  col.type = type;
  col.flags.is_zero_copy = 1;
  col.flags.is_null = (length == UNIV_SQL_NULL) ? 1 : 0;
  col.owned_buffer.reset();
}

void RowBuffer::set_columns_zero_copy(const uchar **data_ptrs, const size_t *lengths) {
  for (size_t i = 0; i < m_num_columns; i++) {
    size_t len = lengths ? lengths[i] : 0;
    set_column_zero_copy(i, data_ptrs[i], len);
  }
}

void RowBuffer::set_column_copy(uint32_t col_idx, const uchar *data, size_t length, enum_field_types type) {
  if (col_idx >= m_num_columns) return;
  ColumnValue &col = m_columns[col_idx];
  col.length = length;
  col.type = type;
  col.flags.is_zero_copy = 0;
  col.flags.is_null = (length == UNIV_SQL_NULL || data == nullptr) ? 1 : 0;

  if (!col.flags.is_null) {
    // Allocate buffer and copy
    col.owned_buffer = std::make_unique<uchar[]>(length);
    std::memset(col.owned_buffer.get(), 0x0, length);
    std::memcpy(col.owned_buffer.get(), data, length);
    col.data = col.owned_buffer.get();

    m_is_all_zero_copy = false;
  } else {
    col.data = nullptr;
    col.owned_buffer.reset();
  }
}

int64_t RowBuffer::get_column_int(uint32_t col_idx) const {
  const ColumnValue *col = get_column(col_idx);
  if (!col || col->flags.is_null) return 0;

  switch (col->type) {
    case MYSQL_TYPE_TINY:
      return *reinterpret_cast<const int8_t *>(col->data);
    case MYSQL_TYPE_SHORT:
      return *reinterpret_cast<const int16_t *>(col->data);
    case MYSQL_TYPE_LONG:
      return *reinterpret_cast<const int32_t *>(col->data);
    case MYSQL_TYPE_LONGLONG:
      return *reinterpret_cast<const int64_t *>(col->data);
    default:
      return 0;
  }
}

double RowBuffer::get_column_double(uint32_t col_idx) const {
  const ColumnValue *col = get_column(col_idx);
  if (!col || col->flags.is_null) return 0.0;

  switch (col->type) {
    case MYSQL_TYPE_FLOAT:
      return *reinterpret_cast<const float *>(col->data);
    case MYSQL_TYPE_DOUBLE:
      return *reinterpret_cast<const double *>(col->data);
    default:
      return 0.0;
  }
}

size_t RowBuffer::get_column_string(uint32_t col_idx, char *buffer, size_t buffer_size) const {
  const ColumnValue *col = get_column(col_idx);
  if (!col || col->flags.is_null || !col->data) {
    if (buffer_size > 0) buffer[0] = '\0';
    return 0;
  }

  size_t copy_len = std::min(col->length, buffer_size - 1);
  std::memcpy(buffer, col->data, copy_len);
  buffer[copy_len] = '\0';
  return copy_len;
}

void RowBuffer::clear() {
  m_row_id = INVALID_ROW_ID;

  for (auto &col : m_columns) {
    col.data = nullptr;
    col.length = 0;
    col.flags.is_null = 1;
    col.owned_buffer.reset();
  }
  m_is_all_zero_copy = true;
}

void RowBuffer::convert_to_owned() {
  if (m_is_all_zero_copy) {
    for (auto &col : m_columns) {
      if (col.flags.is_zero_copy && !col.flags.is_null) {
        // Convert to copy mode
        auto buffer = std::make_unique<uchar[]>(col.length);
        std::memcpy(buffer.get(), col.data, col.length);

        col.data = buffer.get();
        col.owned_buffer = std::move(buffer);
        col.flags.is_zero_copy = 0;
      }
    }
    m_is_all_zero_copy = false;
  }
}

bool RowBuffer::serialize(std::ostream &out) const {
  // Write row ID
  out.write(reinterpret_cast<const char *>(&m_row_id), sizeof(m_row_id));
  // Write number of columns
  out.write(reinterpret_cast<const char *>(&m_num_columns), sizeof(m_num_columns));
  // Write each column's data
  for (const auto &col : m_columns) {
    // Write length
    out.write(reinterpret_cast<const char *>(&col.length), sizeof(col.length));
    // Write flags
    out.write(reinterpret_cast<const char *>(&col.flags), sizeof(col.flags));
    // Write type
    out.write(reinterpret_cast<const char *>(&col.type), sizeof(col.type));
    // Write data
    if (!col.flags.is_null && col.data) out.write(reinterpret_cast<const char *>(col.data), col.length);
  }
  return out.good();
}

bool RowBuffer::deserialize(std::istream &in) {
  // Read row ID
  in.read(reinterpret_cast<char *>(&m_row_id), sizeof(m_row_id));
  // Read number of columns
  size_t num_cols;
  in.read(reinterpret_cast<char *>(&num_cols), sizeof(num_cols));

  if (num_cols != m_num_columns) {
    m_num_columns = num_cols;
    m_columns.resize(num_cols);
  }

  // Read each column's data
  for (auto &col : m_columns) {
    // Read length
    in.read(reinterpret_cast<char *>(&col.length), sizeof(col.length));
    // Read flags
    in.read(reinterpret_cast<char *>(&col.flags), sizeof(col.flags));
    // Read type
    in.read(reinterpret_cast<char *>(&col.type), sizeof(col.type));
    // Read data
    if (!col.flags.is_null && col.length > 0) {
      col.owned_buffer = std::make_unique<uchar[]>(col.length);
      in.read(reinterpret_cast<char *>(col.owned_buffer.get()), col.length);
      col.data = col.owned_buffer.get();
      col.flags.is_zero_copy = 0;
    } else {
      col.data = nullptr;
      col.owned_buffer.reset();
    }
  }
  m_is_all_zero_copy = false;
  return in.good();
}

RowBuffer::FieldDataInfo RowBuffer::extract_field_data(const Rapid_load_context *context, Field *fld, size_t col_idx,
                                                       uchar *rowdata, ulong *col_offsets, ulong *null_byte_offsets,
                                                       ulong *null_bitmasks) {
  FieldDataInfo info{nullptr, 0, false};
  // Determine data source pointer
  uchar *base_ptr = nullptr;
  if (rowdata && col_offsets) {  // Mode 2: Use provided rowdata buffer + offsets
    base_ptr = rowdata + col_offsets[col_idx];
    info.is_null = (fld->is_nullable()) ? is_field_null(col_idx, rowdata, null_byte_offsets, null_bitmasks) : false;
  } else {  // Mode 1: Use Field object directly
    base_ptr = fld->field_ptr();
    info.is_null = fld->is_nullable() ? fld->is_null() : false;
  }
  if (info.is_null) return info;

  // Extract data based on field type (unified logic)
  switch (fld->type()) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB: {
      if (context->m_offpage_data0 && context->m_offpage_data0->size()) {  // in propagation mode, in hook mode.
        auto it = context->m_offpage_data0->find(col_idx);
        assert(it != context->m_offpage_data0->end());
        info.data_len = it->second.first;
        info.data_ptr = it->second.second.get();
      } else {  // parsed from record[0],record[1] by `load` operation.
        auto bfld = down_cast<Field_blob *>(fld);
        uint pack_len = bfld->pack_length_no_ptr();
        switch (pack_len) {
          case 1:
            info.data_len = *base_ptr;
            break;
          case 2:
            info.data_len = uint2korr(base_ptr);
            break;
          case 3:
            info.data_len = uint3korr(base_ptr);
            break;
          case 4:
            info.data_len = uint4korr(base_ptr);
            break;
        }
        // Advance past length prefix
        base_ptr += pack_len;
        // BLOB data maybe not in the page. stores off the page. Dereference blob pointer
        uchar *blob_ptr = nullptr;
        memcpy(&blob_ptr, base_ptr, sizeof(uchar *));
        info.data_ptr = blob_ptr;
      }
      break;
    }
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      uint extra_offset = (fld->field_length > 256 ? 2 : 1);
      info.data_len = (extra_offset == 1) ? mach_read_from_1(base_ptr) : mach_read_from_2_little_endian(base_ptr);
      info.data_ptr = base_ptr + extra_offset;
      break;
    }
    default: {  // Fixed-length types (numeric, date, etc.)
      info.data_ptr = base_ptr;
      info.data_len = fld->pack_length();
      break;
    }
  }
  return info;
}

int RowBuffer::copy_from_mysql_fields(const Rapid_load_context *context, Field **fields, size_t num_fields,
                                      uchar *rowdata, ulong *col_offsets, ulong *null_byte_offsets,
                                      ulong *null_bitmasks) {
  if (num_fields != m_num_columns) return HA_ERR_GENERIC;

  for (size_t idx = 0; idx < num_fields; idx++) {
    Field *fld = fields[idx];
    auto info = extract_field_data(context, fld, idx, rowdata, col_offsets, null_byte_offsets, null_bitmasks);
    if (info.is_null)
      set_column_null(idx);
    else  // Copy mode (safe)
      set_column_copy(idx, info.data_ptr, info.data_len, fld->type());
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int RowBuffer::copy_to_mysql_fields(const TABLE *to, const Table_Metadata *meta) const {
  size_t read_set_col_idx{0};
  for (uint32_t col_idx = 0; col_idx < to->s->fields; col_idx++) {
    Field *source_fld = to->field[col_idx];
    // Skip if not in read_set or marked as NOT_SECONDARY
    if (!bitmap_is_set(to->read_set, col_idx) || source_fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    assert(read_set_col_idx < m_columns.size());
    const ColumnValue &col_value = m_columns[read_set_col_idx];
    read_set_col_idx++;

    Utils::ColumnMapGuard guard(const_cast<TABLE *>(to), Utils::ColumnMapGuard::TYPE::WRITE);
    // Handle NULL values
    if (col_value.flags.is_null) {
      source_fld->set_null();
      continue;
    }

    source_fld->set_notnull();
    // Convert based on field type
    if (Utils::Util::is_string(source_fld->type()) || Utils::Util::is_blob(source_fld->type())) {
      if (source_fld->real_type() == MYSQL_TYPE_ENUM) {  // Handle ENUM type
        source_fld->pack(const_cast<uchar *>(source_fld->data_ptr()), col_value.data, source_fld->pack_length());
      } else {  // Handle string/blob with dictionary encoding
        auto rpd_field = meta->fields[col_idx];
        if (rpd_field.dictionary) {  // Decode from dictionary
          auto text_id = *reinterpret_cast<const uint32 *>(col_value.data);
          auto text = rpd_field.dictionary->get(text_id);
          source_fld->store(text.c_str(), text.length(), source_fld->charset());
        } else {  // Direct string storage
          source_fld->store(reinterpret_cast<const char *>(col_value.data), col_value.length, source_fld->charset());
        }
      }
    } else {  // Numeric types - direct pack
      source_fld->pack(const_cast<uchar *>(source_fld->data_ptr()), col_value.data, col_value.length);
    }
  }

  assert(read_set_col_idx == m_num_columns);
  return ShannonBase::SHANNON_SUCCESS;
}

boost::asio::awaitable<int> RowBuffer::copy_to_mysql_fields_async(const TABLE *to, const Table_Metadata *meta,
                                                                  boost::asio::any_io_executor &executor,
                                                                  size_t max_batch_size) const {
  // Build index mapping for read_set columns
  std::vector<size_t> read_set_indices;
  read_set_indices.reserve(m_num_columns);

  for (uint32_t col_idx = 0; col_idx < to->s->fields; col_idx++) {
    Field *source_fld = to->field[col_idx];
    if (!bitmap_is_set(to->read_set, col_idx) || source_fld->is_flag_set(NOT_SECONDARY_FLAG)) {
      continue;
    }
    read_set_indices.push_back(col_idx);
  }

  assert(read_set_indices.size() == m_num_columns);

  // Process columns in batches for parallel execution
  const size_t n_fields = read_set_indices.size();
  for (size_t batch_start = 0; batch_start < n_fields; batch_start += max_batch_size) {
    size_t batch_end = std::min(batch_start + max_batch_size, n_fields);
    size_t batch_size = batch_end - batch_start;

    // Shared state for this batch
    auto counter = std::make_shared<std::atomic<size_t>>(batch_size);
    auto batch_promise = std::make_shared<ShannonBase::Utils::shared_promise<int>>();
    auto promise_set = std::make_shared<std::atomic<bool>>(false);

    auto field_read_pool = ShannonBase::Imcs::Imcs::pool();
    // Launch parallel tasks for each column in the batch
    for (size_t idx = batch_start; idx < batch_end; ++idx) {
      uint32_t col_idx = read_set_indices[idx];
      Field *source_fld = to->field[col_idx];
      // Spawn async task for this column
      boost::asio::co_spawn(
          *field_read_pool,
          [this, to, meta, col_idx, idx, source_fld, counter, batch_promise,
           promise_set]() -> boost::asio::awaitable<void> {
            // RAII guard to decrement counter
            DeferGuard defer([counter, promise_set, batch_promise]() {
              if (counter->fetch_sub(1, std::memory_order_release) == 1 &&
                  !promise_set->exchange(true, std::memory_order_acq_rel)) {
                batch_promise->set_value(ShannonBase::SHANNON_SUCCESS);
              }
            });

            try {
              // Get column value from local columns array
              assert(idx < m_columns.size());
              const ColumnValue &col_value = m_columns[idx];

              // Prefetch next column data (if exists)
              if (idx + 1 < m_columns.size()) {
                const auto &next_col = m_columns[idx + 1];
                if (next_col.data) SHANNON_PREFETCH_R(next_col.data);
              }

              Utils::ColumnMapGuard guard(const_cast<TABLE *>(to), Utils::ColumnMapGuard::TYPE::WRITE);
              if (col_value.flags.is_null) {  // Handle NULL values
                source_fld->set_null();
                co_return;
              }

              source_fld->set_notnull();
              // Convert based on field type
              if (Utils::Util::is_string(source_fld->type()) || Utils::Util::is_blob(source_fld->type())) {
                if (source_fld->real_type() == MYSQL_TYPE_ENUM) {  // Handle ENUM type
                  source_fld->pack(const_cast<uchar *>(source_fld->data_ptr()), col_value.data,
                                   source_fld->pack_length());
                } else {  // Handle string/blob with dictionary encoding
                  auto rpd_field = meta->fields[col_idx];
                  if (rpd_field.dictionary) {  // Decode from dictionary
                    auto text_id = *reinterpret_cast<const uint32 *>(col_value.data);
                    auto text = rpd_field.dictionary->get(text_id);
                    source_fld->store(text.c_str(), text.length(), source_fld->charset());
                  } else {  // Direct string storage
                    source_fld->store(reinterpret_cast<const char *>(col_value.data), col_value.length,
                                      source_fld->charset());
                  }
                }
              } else {  // Numeric types - direct pack
                source_fld->pack(const_cast<uchar *>(source_fld->data_ptr()), col_value.data, col_value.length);
              }
            } catch (...) {
              // On error, set error status if no one has set it yet
              if (!promise_set->exchange(true, std::memory_order_acq_rel)) batch_promise->set_value(HA_ERR_GENERIC);
            }
            co_return;
          },
          boost::asio::detached);
    }

    // Wait for this batch to complete
    auto batch_result = co_await batch_promise->get_awaitable(executor);
    if (batch_result) co_return batch_result;
  }
  co_return ShannonBase::SHANNON_SUCCESS;
}

void RowBuffer::dump(std::ostream &out) const {
  out << "Row " << m_row_id << " (" << m_num_columns << " columns):\n";
  for (size_t i = 0; i < m_num_columns; i++) {
    const ColumnValue &col = m_columns[i];
    out << "  Column " << i << ": ";

    if (col.flags.is_null) {
      out << "NULL\n";
    } else {
      out << "length=" << col.length << ", zero_copy=" << (col.flags.is_zero_copy ? "yes" : "no")
          << ", type=" << static_cast<int>(col.type);

      // Print partial data (first 16 bytes)
      if (col.data) {
        out << ", data=[";
        size_t print_len = std::min(col.length, size_t(16));
        for (size_t j = 0; j < print_len; j++)
          out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(col.data[j]) << " ";
        if (col.length > 16) out << "...";
        out << "]";
      }
      out << "\n";
    }
  }
}

bool RowBuffer::validate() const {
  for (const auto &col : m_columns) {
    // Check pointer validity in zero-copy mode
    if (col.flags.is_zero_copy && !col.flags.is_null && !col.data) return false;
    // Check buffer consistency in copy mode
    if (!col.flags.is_zero_copy && !col.flags.is_null) {
      if (col.owned_buffer && col.data != col.owned_buffer.get()) return false;
    }

    // Check NULL flag consistency
    if (col.flags.is_null && col.data != nullptr) return false;
  }
  return true;
}

RowDirectory::RowDirectory(RowDirectory &&other) noexcept
    : m_capacity(other.m_capacity),
      m_enable_column_offsets(other.m_enable_column_offsets),
      m_num_columns(other.m_num_columns),
      m_total_data_size(other.m_total_data_size.load()),
      m_compressed_data_size(other.m_compressed_data_size.load()),
      m_overflow_count(other.m_overflow_count.load()) {
  // Transfer ownership of unique_ptr members
  m_entries = std::move(other.m_entries);
  m_column_offset_tables = std::move(other.m_column_offset_tables);

  // Reset the source object
  other.m_capacity = 0;
  other.m_num_columns = 0;
  other.m_total_data_size = 0;
  other.m_compressed_data_size = 0;
  other.m_overflow_count = 0;
}

RowDirectory &RowDirectory::operator=(RowDirectory &&other) noexcept {
  if (this != &other) {
    m_capacity = other.m_capacity;
    m_num_columns = other.m_num_columns;
    m_enable_column_offsets = other.m_enable_column_offsets;
    m_total_data_size = other.m_total_data_size.load();
    m_compressed_data_size = other.m_compressed_data_size.load();
    m_overflow_count = other.m_overflow_count.load();

    // Transfer ownership
    m_entries = std::move(other.m_entries);
    m_column_offset_tables = std::move(other.m_column_offset_tables);

    // Reset the source object
    other.m_capacity = 0;
    other.m_num_columns = 0;
    other.m_total_data_size = 0;
    other.m_compressed_data_size = 0;
    other.m_overflow_count = 0;
  }
  return *this;
}

void RowDirectory::set_row_entry(row_id_t row_id, uint32_t offset, uint32_t length, bool is_compressed) {
  if (row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);
  RowEntry &entry = m_entries[row_id];
  entry.offset = offset;
  entry.length = length;
  entry.flags.is_compressed = is_compressed ? 1 : 0;
  entry.checksum = compute_checksum(offset, length);

  m_total_data_size.fetch_add(length);
  if (is_compressed) {
    m_compressed_data_size.fetch_add(length);
  }
}

const RowDirectory::RowEntry *RowDirectory::get_row_entry(row_id_t row_id) const {
  if (row_id >= m_capacity) return nullptr;

  std::shared_lock lock(m_mutex);
  return &m_entries[row_id];
}

void RowDirectory::mark_deleted(row_id_t row_id) {
  if (row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);
  m_entries[row_id].flags.is_deleted = 1;
}

void RowDirectory::mark_has_null(row_id_t row_id) {
  if (row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);
  m_entries[row_id].flags.has_null = 1;
}

void RowDirectory::mark_overflow(row_id_t row_id) {
  if (row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);
  m_entries[row_id].flags.is_overflow = 1;
  m_overflow_count.fetch_add(1);
}

void RowDirectory::build_column_offset_table(row_id_t row_id, const std::vector<uint16_t> &column_offsets,
                                             const std::vector<uint16_t> &column_lengths) {
  if (!m_enable_column_offsets || row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);
  auto table = std::make_unique<RowDirectory::ColumnOffsetTable>(m_num_columns);
  table->column_offsets = column_offsets;
  table->column_lengths = column_lengths;
  m_column_offset_tables[row_id] = std::move(table);
}

const RowDirectory::ColumnOffsetTable *RowDirectory::get_column_offset_table(row_id_t row_id) const {
  if (!m_enable_column_offsets || row_id >= m_capacity) return nullptr;

  std::shared_lock lock(m_mutex);

  auto it = m_column_offset_tables.find(row_id);
  return (it != m_column_offset_tables.end()) ? it->second.get() : nullptr;
}

uint16_t RowDirectory::get_column_offset(row_id_t row_id, uint32_t col_idx) const {
  const RowDirectory::ColumnOffsetTable *table = get_column_offset_table(row_id);
  if (table && col_idx < table->column_offsets.size()) {
    return table->column_offsets[col_idx];
  }
  return UINT16_MAX;
}

uint16_t RowDirectory::get_column_length(row_id_t row_id, uint32_t col_idx) const {
  const RowDirectory::ColumnOffsetTable *table = get_column_offset_table(row_id);
  if (table && col_idx < table->column_lengths.size()) {
    return table->column_lengths[col_idx];
  }
  return 0;
}

void RowDirectory::get_batch_offsets(row_id_t start_row, size_t count, uint32_t *offsets, uint32_t *lengths) const {
  std::shared_lock lock(m_mutex);
  size_t end = std::min(start_row + count, m_capacity);
  for (size_t i = start_row; i < end; i++) {
    size_t idx = i - start_row;
    offsets[idx] = m_entries[i].offset;
    lengths[idx] = m_entries[i].length;
  }
}

void RowDirectory::update_compression_stats(row_id_t row_id, size_t original_size, size_t compressed_size) {
  if (row_id >= m_capacity) return;

  std::unique_lock lock(m_mutex);
  m_entries[row_id].flags.is_compressed = 1;
  m_entries[row_id].length = compressed_size;
  m_compressed_data_size.fetch_add(compressed_size);
}

double RowDirectory::get_compression_ratio() const {
  size_t total = m_total_data_size.load();
  size_t compressed = m_compressed_data_size.load();
  if (total == 0) return 0.0;
  return static_cast<double>(compressed) / total;
}

size_t RowDirectory::get_directory_size() const {
  size_t base_size = m_capacity * sizeof(RowEntry);
  std::shared_lock lock(m_mutex);
  size_t offset_table_size = 0;
  for (const auto &[row_id, table] : m_column_offset_tables) {
    offset_table_size += table->column_offsets.size() * sizeof(uint16_t);
    offset_table_size += table->column_lengths.size() * sizeof(uint16_t);
  }
  return base_size + offset_table_size;
}

bool RowDirectory::validate() const {
  std::shared_lock lock(m_mutex);
  for (size_t i = 0; i < m_capacity; i++) {
    const RowEntry &entry = m_entries[i];
    // Verify checksum
    uint32_t expected_checksum = compute_checksum(entry.offset, entry.length);
    if (entry.checksum != expected_checksum) return false;
  }

  return true;
}

void RowDirectory::dump_summary(std::ostream &out) const {
  std::shared_lock lock(m_mutex);

  out << "Row Directory Summary:\n";
  out << "  Capacity: " << m_capacity << "\n";
  out << "  Total Data Size: " << m_total_data_size.load() << " bytes\n";
  out << "  Compressed Data Size: " << m_compressed_data_size.load() << " bytes\n";
  out << "  Compression Ratio: " << get_compression_ratio() << "\n";
  out << "  Overflow Count: " << m_overflow_count.load() << "\n";
  out << "  Directory Size: " << get_directory_size() << " bytes\n";
  out << "  Column Offset Tables: " << m_column_offset_tables.size() << "\n";
}

std::unique_ptr<RowDirectory> RowDirectory::clone() const {
  auto new_dir = std::make_unique<RowDirectory>(m_capacity, m_num_columns, m_enable_column_offsets);
  std::shared_lock lock(m_mutex);
  // Copy row entries
  for (size_t i = 0; i < m_capacity; i++) new_dir->m_entries[i] = m_entries[i];

  // Copy column offset tables
  for (const auto &[row_id, table] : m_column_offset_tables) {
    auto new_table = std::make_unique<RowDirectory::ColumnOffsetTable>(m_num_columns);
    new_table->column_offsets = table->column_offsets;
    new_table->column_lengths = table->column_lengths;
    new_dir->m_column_offset_tables[row_id] = std::move(new_table);
  }

  // Copy atomic values
  new_dir->m_total_data_size.store(m_total_data_size.load());
  new_dir->m_compressed_data_size.store(m_compressed_data_size.load());
  new_dir->m_overflow_count.store(m_overflow_count.load());

  return new_dir;
}
}  // namespace Imcs
}  // namespace ShannonBase
