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
#include "storage/rapid_engine/imcs/imcu.h"

#include <limits.h>

#include "sql/field.h"                    //Field
#include "sql/field_common_properties.h"  // is_numeric_type
#include "sql/sql_class.h"
#include "sql/table.h"  //TABLE

#include "storage/innobase/include/mach0data.h"

#include "storage/rapid_engine/imcs/imcs.h"  // imcs:pool
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Imcs {
Imcu::Imcu(RpdTable *owner, TableMetadata &table_meta, row_id_t start_row, size_t capacity,
           std::shared_ptr<Utils::MemoryPool> mem_pool)
    : m_memory_pool(mem_pool), m_owner_table(owner) {
  m_header.imcu_id = owner->meta().total_imcus.fetch_add(1);
  m_header.start_row = start_row;
  m_header.end_row = start_row + capacity;
  m_header.capacity = capacity;
  m_header.current_rows.store(0, std::memory_order_relaxed);
  m_header.created_at = std::chrono::system_clock::now();
  m_header.last_modified = std::chrono::system_clock::now();

  // to indicate which row is deleted or not. row-level shared.
  m_header.del_mask = std::make_unique<bit_array_t>(m_header.capacity);

  m_column_units.reserve(owner->meta().fields.size());
  m_cu_array.reserve(owner->meta().fields.size());
  m_header.null_masks.reserve(owner->meta().fields.size());

  for (auto &fld_meta : owner->meta().fields) {
    if (fld_meta.is_secondary_field) {
      auto cu_fld = std::make_unique<CU>(this, fld_meta, fld_meta.field_id, m_header.capacity, m_memory_pool);
      m_column_units.emplace(fld_meta.field_id, std::move(cu_fld));
      m_cu_array.push_back(m_column_units[fld_meta.field_id].get());

      m_header.null_masks.emplace_back(std::make_unique<bit_array_t>(m_header.capacity));
    } else {  // NOT SECONDARY FIELD.
      m_column_units.emplace(fld_meta.field_id, nullptr);
      m_cu_array.push_back(nullptr);
      m_header.null_masks.emplace_back(nullptr);
    }
  }

  // create transaction journal associated with this imuc.
  m_header.txn_journal = std::make_unique<TransactionJournal>(m_header.capacity);

  // create storage index associated with this imcu.
  m_header.storage_index = std::make_unique<StorageIndex>(table_meta.num_columns);

  // create row dir index associated with this imcu.
  m_header.row_directory = std::make_unique<RowDirectory>(m_header.capacity, table_meta.num_columns);
}

row_id_t Imcu::insert_row(const Rapid_load_context *context, const RowBuffer &row_data) {
  // 1. allocate local row_id.
  row_id_t local_row_id = allocate_row_id();

  if (local_row_id == INVALID_ROW_ID) {  // IMCU full.
    return INVALID_ROW_ID;
  }

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64 scn = context->m_extra_info.m_scn;

  // 2. record Transaction_Journal, if it's not `load` oper.
  if (context->m_extra_info.m_oper != Rapid_context::extra_info_t::OperType::LOAD) {
    {
      std::unique_lock lock(m_header_mutex);
      TransactionJournal::Entry entry;
      entry.row_id = local_row_id;
      entry.txn_id = txn_id;
      entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_INSERT);
      entry.status = (scn > 0) ? TransactionJournal::COMMITTED : TransactionJournal::ACTIVE;
      entry.scn = scn;
      entry.timestamp = std::chrono::system_clock::now();

      m_header.txn_journal->add_entry(std::move(entry));

      m_header.insert_count.fetch_add(1);
    }
  }

  // 3. write to each column.
  for (size_t col_idx = 0; col_idx < row_data.get_num_columns(); col_idx++) {
    if (!m_cu_array[col_idx]) continue;  // means is `NOT_SECONDARY` field.

    auto row_col_data = row_data.get_column(col_idx);
    // dealing with NULL
    if (row_col_data->flags.is_null) {
      assert(row_col_data->data == nullptr);
      assert(m_header.null_masks[col_idx].get());

      std::unique_lock lock(m_header_mutex);
      Utils::Util::bit_array_set(m_header.null_masks[col_idx].get(), local_row_id);
    }

    // write data（dont create version due to its insertion）
    m_cu_array[col_idx]->write(context, local_row_id, row_col_data->data, row_col_data->length);

    // update Storage Index
    // TODO: using this `m_header.storage_index->update_storage_index()` ???;
    if (row_col_data->data && (is_numeric_type(row_col_data->type) || is_temporal_type(row_col_data->type))) {
      auto src_fld = m_owner_table->meta().fields[col_idx].source_fld;
      double numeric_val = Utils::Util::get_field_numeric<double>(src_fld, row_col_data->data, nullptr,
                                                                  m_owner_table->meta().db_low_byte_first);
      m_header.storage_index->update(col_idx, numeric_val);
    }
  }

  increment_version();

  return local_row_id;
}

int Imcu::delete_row(const Rapid_load_context *context, row_id_t local_row_id) {
  // 1. boundary check.
  if (local_row_id >= m_header.current_rows.load()) return HA_ERR_KEY_NOT_FOUND;

  // 2. check whether it deleted or not.
  {
    std::shared_lock lock(m_header_mutex);
    if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id))
      return HA_ERR_RECORD_DELETED;  // alread deleted.
  }

  // 3. record transaction journal.
  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64 scn = context->m_extra_info.m_scn;  // if committed.

  {
    std::unique_lock lock(m_header_mutex);

    // 3.1 create TxnJ record.
    TransactionJournal::Entry entry;
    entry.row_id = local_row_id;
    entry.txn_id = txn_id;
    entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_DELETE);
    entry.status = (scn > 0) ? TransactionJournal::COMMITTED : TransactionJournal::ACTIVE;
    entry.scn = scn;
    entry.timestamp = std::chrono::system_clock::now();

    // 3.2 add entry.
    m_header.txn_journal->add_entry(std::move(entry));

    // 3.3 mark it deleted.
    Utils::Util::bit_array_set(m_header.del_mask.get(), local_row_id);

    // 3.4 update statistics.
    m_header.delete_count.fetch_add(1);
    m_header.delete_ratio = static_cast<double>(m_header.delete_count.load()) / m_header.current_rows.load();
  }

  // 4. increase the version counter（optimistic concurrent）
  increment_version();

  // 5. update Storage Index（mark it need to rebuild）
  m_header.storage_index->mark_dirty();

  return ShannonBase::SHANNON_SUCCESS;
}

size_t Imcu::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &local_row_ids) {
  if (local_row_ids.empty()) return 0;

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64 scn = context->m_extra_info.m_scn;

  std::unique_lock lock(m_header_mutex);

  size_t deleted = 0;

  for (row_id_t local_row_id : local_row_ids) {
    // boundary check.
    if (local_row_id >= m_header.current_rows.load()) continue;

    // already deleted or not.
    if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id)) continue;

    // build up a TxnJ
    TransactionJournal::Entry entry;
    entry.row_id = local_row_id;
    entry.txn_id = txn_id;
    entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_DELETE);
    entry.status = (scn > 0) ? TransactionJournal::EntryStatus::COMMITTED : TransactionJournal::EntryStatus::ACTIVE;
    entry.scn = scn;
    entry.timestamp = std::chrono::system_clock::now();

    m_header.txn_journal->add_entry(std::move(entry));

    // to set deleted flag.
    Utils::Util::bit_array_set(m_header.del_mask.get(), local_row_id);

    deleted++;
  }

  // update statistics.
  m_header.delete_count.fetch_add(deleted);
  m_header.delete_ratio = static_cast<double>(m_header.delete_count.load()) / m_header.current_rows.load();

  increment_version();
  m_header.storage_index->mark_dirty();

  return deleted;
}

int Imcu::update_row(const Rapid_load_context *context, row_id_t local_row_id,
                     const std::unordered_map<uint32, RowBuffer::ColumnValue> &updates) {
  // 1. check boundary.
  if (local_row_id >= m_header.current_rows.load()) return HA_ERR_KEY_NOT_FOUND;

  // 2. check deleted or not.
  {
    std::shared_lock lock(m_header_mutex);
    if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id)) return HA_ERR_RECORD_DELETED;
  }

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64 scn = context->m_extra_info.m_scn;

  // 3. record row level TxnJ.
  {
    std::unique_lock lock(m_header_mutex);

    TransactionJournal::Entry entry;
    entry.row_id = local_row_id;
    entry.txn_id = txn_id;
    entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_UPDATE);
    entry.status = (scn > 0) ? TransactionJournal::COMMITTED : TransactionJournal::ACTIVE;
    entry.scn = scn;
    entry.timestamp = std::chrono::system_clock::now();

    // mark update bit
    for (const auto &[col_idx, value] : updates) entry.modified_columns.set(col_idx);

    m_header.txn_journal->add_entry(std::move(entry));

    m_header.update_count.fetch_add(1);
  }

  // 4. Create column-level versions for each modified column and write new values
  // Key point: Only operate on modified columns! Do not touch unmodified columns at all
  for (const auto &[col_idx, new_value] : updates) {
    CU *cu = get_cu(col_idx);
    if (!cu) continue;

    // CU-leve update（create row-level version）
    cu->update(context, local_row_id, new_value.data, new_value.length);
  }

  // 5. update Storage Index（only apply changed column）
  for (const auto &[col_idx, new_value] : updates) {
    CU *cu = get_cu(col_idx);
    if (!cu) continue;

    assert(cu->get_source_field()->type() == cu->get_type());
    if (is_numeric_type(cu->get_type()) || is_temporal_type(cu->get_type())) {
      double numeric_val = Utils::Util::get_field_numeric<double>(cu->get_source_field(), new_value.data, nullptr,
                                                                  m_owner_table->meta().db_low_byte_first);
      m_header.storage_index->update(col_idx, numeric_val);
    }
  }

  increment_version();

  return ShannonBase::SHANNON_SUCCESS;
}

size_t Imcu::scan(Rapid_scan_context *context, const std::vector<std::unique_ptr<Predicate>> &predicates,
                  const std::vector<uint32> &projection, RowCallback callback) {
  return scan_range(context, 0, SHANNON_BATCH_NUM, predicates, projection, callback);
}

size_t Imcu::scan_range(Rapid_scan_context *context, size_t start_offset, size_t limit,
                        const std::vector<std::unique_ptr<Predicate>> &predicates,
                        const std::vector<uint32> &projection, RowCallback callback) {
  size_t num_rows = m_header.current_rows.load();
  if (start_offset >= num_rows) return 0;

  size_t scanned = 0;
  std::vector<const uchar *> row_buffer(projection.size());
  const size_t batch_unit = std::min({limit > 0 ? limit : static_cast<size_t>(SHANNON_BATCH_NUM),
                                      static_cast<size_t>(SHANNON_BATCH_NUM), num_rows - start_offset});
  // visibility mask.
  bit_array_t visibility_mask(batch_unit);
  // batch vector process.
  for (size_t start = start_offset; start < num_rows && scanned < limit; start += batch_unit) {
    size_t end = std::min(start + batch_unit, num_rows);
    size_t batch_size = end - start;

    // 1. Batch Visibility Check (Core: One-time determination benefits all columns)
    check_visibility_batch(context, static_cast<row_id_t>(start), batch_size, visibility_mask);

    // 2. Apply predicate filtering on visible rows
    std::vector<row_id_t> matching_rows;
    matching_rows.reserve(batch_size);

    for (size_t i = 0; i < batch_size; i++) {
      scanned++;
      if (!Utils::Util::bit_array_get(&visibility_mask, i)) continue;  // invisible，skip.

      row_id_t local_row_id = start + i;

      // apply prediction.
      bool match = true;
      if (!predicates.empty()) {
        // Row-level cache to avoid repeated column reads
        std::unordered_map<uint32, const uchar *> row_cache;
        for (const auto &pred_ptr : predicates) {
          if (!pred_ptr) continue;  // Skip null predicates
          // Evaluate predicate (handles Simple and Compound)
          if (!evaluate_predicate(pred_ptr.get(), local_row_id, row_cache)) {
            match = false;
            break;  // Short-circuit: one failed predicate → row rejected
          }
        }
      }
      if (match) matching_rows.push_back(local_row_id);
      if (scanned >= limit) break;
    }

    // 3. Read projection columns of matched rows
    for (row_id_t local_row_id : matching_rows) {
      // read the loaded CUs.
      for (size_t i = 0; i < projection.size(); i++) {
        uint32 col_idx = projection[i];
        if (Utils::Util::bit_array_get(m_header.null_masks[col_idx].get(), local_row_id)) {
          row_buffer[i] = nullptr;
        } else {
          CU *cu = get_cu(col_idx);
          row_buffer[i] = cu->get_data_address(local_row_id);  // return data addr directly, no data cpy.
        }
      }

      // 4. extra callback.
      row_id_t global_row_id = m_header.start_row + local_row_id;
      callback(global_row_id, row_buffer);
      context->rows_returned++;
      // check the LIMIT options.
      if (context->limit > 0 && context->rows_returned >= context->limit) return scanned;
    }
  }
  return scanned;
}

bool Imcu::evaluate_predicate(const Predicate *pred, row_id_t local_row_id,
                              std::unordered_map<uint32, const uchar *> &row_cache) const {
  if (!pred) return true;  // No predicate → always matches

  // Dispatch based on predicate type
  return (pred->is_compound())
             ? evaluate_compound_predicate(static_cast<const Compound_Predicate *>(pred), local_row_id, row_cache)
             : evaluate_simple_predicate(static_cast<const Simple_Predicate *>(pred), local_row_id, row_cache);
}

bool Imcu::evaluate_compound_predicate(const Compound_Predicate *pred, row_id_t local_row_id,
                                       std::unordered_map<uint32, const uchar *> &row_cache) const {
  if (!pred || pred->children.empty()) return true;

  switch (pred->op) {
    case PredicateOperator::AND: {
      // Logical AND: ALL children must match
      for (const auto &child : pred->children) {
        if (!evaluate_predicate(child.get(), local_row_id, row_cache)) return false;  // Short-circuit
      }
      return true;  // All children matched
    } break;
    case PredicateOperator::OR: {
      // Logical OR: ANY child can match
      for (const auto &child : pred->children) {
        if (evaluate_predicate(child.get(), local_row_id, row_cache)) return true;
      }
      return false;  // No children matched
    } break;
    case PredicateOperator::NOT: {
      // Logical NOT: inverse of child
      if (pred->children.size() != 1) {
        DBUG_PRINT("imcu_scan", ("NOT predicate has %zu children (expected 1)", pred->children.size()));
        return false;
      }
      bool child_result = evaluate_predicate(pred->children[0].get(), local_row_id, row_cache);
      return !child_result;
    } break;
    default:
      return false;
  }
}

bool Imcu::evaluate_simple_predicate(const Simple_Predicate *pred, row_id_t local_row_id,
                                     std::unordered_map<uint32, const uchar *> &row_cache) const {
  if (!pred) return true;
  uint32 col_id = pred->column_id;
  auto cu = get_cu(col_id);
  assert(cu);
  // Get column value (with caching to avoid repeated access)
  const uchar *value = get_column_value(col_id, local_row_id, row_cache);
  std::string str_val;
  if (cu->dictionary() && value) {  // means string type.
    auto str_id = *reinterpret_cast<const uint32 *>(value);
    str_val = cu->dictionary()->get(str_id);
    value = reinterpret_cast<const uchar *>(str_val.c_str());
  }
  // Use Predicate's evaluate() method, Pass as array of pointers
  std::vector<const uchar *> value_array(col_id ? (col_id + 1) : 1, value);
  const_cast<Simple_Predicate *>(pred)->field_meta = cu->field();
  auto is_low_order = m_owner_table->meta().db_low_byte_first;
  try {
    return pred->evaluate(value_array.data(), m_column_units.size(), is_low_order);
  } catch (const std::exception &e) {
    DBUG_PRINT("imcu_scan", ("Predicate evaluation error: %s", e.what()));
    return false;  // Conservative: reject on error
  }
}

const uchar *Imcu::get_column_value(uint32 col_id, row_id_t local_row_id,
                                    std::unordered_map<uint32, const uchar *> &row_cache) const {
  // Check cache first
  auto it = row_cache.find(col_id);
  if (it != row_cache.end()) return it->second;

  // Check NULL mask
  if (Utils::Util::bit_array_get(m_header.null_masks[col_id].get(), local_row_id)) {
    row_cache[col_id] = nullptr;
    return nullptr;
  }
  // Get CU and read value
  const auto *cu = get_cu(col_id);
  assert(cu);
  const uchar *value = cu->get_data_address(local_row_id);
  row_cache[col_id] = value;
  return value;
}

bool Imcu::is_row_visible(Rapid_scan_context *context, row_id_t local_row_id, Transaction::ID reader_txn_id,
                          uint64 reader_scn) const {
  return 0;
}

void Imcu::check_visibility_batch(Rapid_scan_context *context, row_id_t start_row, size_t count,
                                  bit_array_t &visibility_mask) const {
  std::shared_lock lock(m_header_mutex);

  // Using Transaction Journal for Batch Visibility Determination
  m_header.txn_journal->check_visibility_batch(start_row, count, context->m_extra_info.m_trxid,
                                               context->m_extra_info.m_scn, visibility_mask);

  // filter out the deleted rows.
  for (size_t i = 0; i < count; i++) {
    if (Utils::Util::bit_array_get(&visibility_mask, i)) {
      row_id_t local_row_id = start_row + i;
      // check delete mask bit.
      if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id)) {
        // delete，Need to further check if it is visible in the snapshot.
        if (!m_header.txn_journal->is_visible(local_row_id, context->m_extra_info.m_trxid,
                                              context->m_extra_info.m_scn)) {
          Utils::Util::bit_array_reset(&visibility_mask, i);
        }
      }
    }
  }
}

bool Imcu::read_row(Rapid_scan_context *context, row_id_t local_row_id, const std::vector<uint32> &col_indices,
                    RowBuffer &output) {
  return 0;
}

bool Imcu::can_skip_imcu(const std::vector<std::unique_ptr<Predicate>> &predicates) const {
  std::shared_lock lock(m_header_mutex);
  return m_header.storage_index->can_skip_imcu(predicates);
}

void Imcu::update_storage_index() {
  if (!m_header.storage_index) return;

  std::unique_lock lock(m_header_mutex);
  size_t num_rows = m_header.current_rows.load(std::memory_order_acquire);
  if (num_rows == 0) return;

  // Iterate through all columns to update statistics
  for (auto &[col_idx, cu] : m_column_units) {
    if (!cu) continue;  // Skip NOT_SECONDARY fields

    Field *source_field = cu->get_source_field();
    if (!source_field) continue;
    enum_field_types field_type = cu->get_type();
    bool is_numeric = is_numeric_type(field_type) || is_temporal_type(field_type);

    // Reset column statistics before rebuilding
    m_header.storage_index->get_column_stats_snapshot(col_idx);

    // Traverse all valid (non-deleted) rows
    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
      // Skip deleted rows
      if (Utils::Util::bit_array_get(m_header.del_mask.get(), row_idx)) continue;
      // Check if NULL
      if (m_header.null_masks[col_idx] && Utils::Util::bit_array_get(m_header.null_masks[col_idx].get(), row_idx)) {
        m_header.storage_index->update_null(col_idx);
        continue;
      }
      // Get column data address
      const uchar *data = cu->get_data_address(row_idx);
      if (!data) continue;

      // Update statistics based on data type
      if (is_numeric) {
        // Extract numeric value
        double numeric_val = Utils::Util::get_field_numeric<double>(source_field, data, nullptr,
                                                                    m_owner_table->meta().db_low_byte_first);
        m_header.storage_index->update(col_idx, numeric_val);
      } else {
        // Handle string types
        switch (field_type) {
          case MYSQL_TYPE_VARCHAR:
          case MYSQL_TYPE_VAR_STRING:
          case MYSQL_TYPE_STRING: {
            // Extract string value
            size_t str_len = 0;
            const char *str_data = nullptr;
            // Determine length prefix size
            size_t length_bytes = (source_field->field_length > 256) ? 2 : 1;
            if (length_bytes == 1) {
              str_len = mach_read_from_1(data);
              str_data = reinterpret_cast<const char *>(data + 1);
            } else {
              str_len = mach_read_from_2_little_endian(data);
              str_data = reinterpret_cast<const char *>(data + 2);
            }
            if (str_data && str_len > 0) {
              std::string str_value(str_data, str_len);
              m_header.storage_index->update_string_stats(col_idx, str_value);
            }
          } break;
          case MYSQL_TYPE_BLOB:
          case MYSQL_TYPE_TINY_BLOB:
          case MYSQL_TYPE_MEDIUM_BLOB:
          case MYSQL_TYPE_LONG_BLOB: {
            // For BLOB types, extract length and data pointer
            auto blob_field = down_cast<Field_blob *>(source_field);
            uint pack_len = blob_field->pack_length_no_ptr();
            size_t blob_len = 0;
            switch (pack_len) {
              case 1:
                blob_len = *data;
                break;
              case 2:
                blob_len = uint2korr(data);
                break;
              case 3:
                blob_len = uint3korr(data);
                break;
              case 4:
                blob_len = uint4korr(data);
                break;
            }
            // Get actual blob data pointer
            const uchar *blob_ptr = nullptr;
            memcpy(&blob_ptr, data + pack_len, sizeof(uchar *));
            if (blob_ptr && blob_len > 0) {
              std::string blob_value(reinterpret_cast<const char *>(blob_ptr),
                                     std::min(blob_len, size_t(256)));  // Limit for statistics
              m_header.storage_index->update_string_stats(col_idx, blob_value);
            }
          } break;
          default:
            // For other types, treat as binary and skip string statistics
            break;
        }
      }
    }
  }

  // Clear dirty flag after update
  m_header.storage_index->clear_dirty();

  // Update last modified time
  m_header.last_modified = std::chrono::system_clock::now();
}

size_t Imcu::garbage_collect(uint64 min_active_scn) {
  size_t freed = 0;
  // 1. purege TxnJ.
  freed += m_header.txn_journal->purge(min_active_scn);
  // 2. clear version of every column.
  for (auto &[col_idx, cu] : m_column_units) {
    freed += cu->purge_versions(nullptr, min_active_scn);
  }
  // 3. update statistics.
  m_header.version_count = 0;  // estimate_version_count();
  m_header.last_gc_time = std::chrono::system_clock::now();
  return freed;
}

Imcu *Imcu::compact() {
  size_t num_rows = m_header.current_rows.load();

  // 1. calc un-deleted # of rows.
  std::vector<row_id_t> valid_rows;
  valid_rows.reserve(num_rows);
  {
    std::shared_lock lock(m_header_mutex);
    for (size_t i = 0; i < num_rows; i++) {
      if (!Utils::Util::bit_array_get(m_header.del_mask.get(), i)) {
        valid_rows.push_back(i);
        valid_rows.push_back(i);
      }
    }
  }

  // 2. create a new IMCU.
  auto new_imcu = std::make_shared<Imcu>(m_owner_table, m_owner_table->meta(), m_header.start_row, valid_rows.size(),
                                         m_memory_pool);

  // 3. cp the un-deleted rows.
  for (size_t new_row_id = 0; new_row_id < valid_rows.size(); new_row_id++) {
    row_id_t old_row_id = valid_rows[new_row_id];
    // cp the columns data.
    for (auto &[col_idx, old_cu] : m_column_units) {
      if (!old_cu) continue;  // NOT_SECONDARY_LOAD
      CU *new_cu = new_imcu->get_cu(col_idx);
      // read old data.
      uchar buffer[MAX_FIELD_WIDTH];
      size_t len = old_cu->read(nullptr, old_row_id, buffer);
      // write to the new place.
      new_cu->write(nullptr, new_row_id, len == UNIV_SQL_NULL ? nullptr : buffer, len);
      // cp null bits mask.
      if (Utils::Util::bit_array_get(m_header.null_masks[col_idx].get(), old_row_id)) {
        Utils::Util::bit_array_set(new_imcu->m_header.null_masks[col_idx].get(), new_row_id);
      }
    }
  }

  // 4. re-build Storage Index.
  new_imcu->update_storage_index();

  // 5. re-build ART index.
  //[TODO]

  // 6. update statistic.
  new_imcu->m_header.current_rows.store(valid_rows.size());
  new_imcu->m_header.delete_count.store(0);
  new_imcu->m_header.delete_ratio = 0.0;
  new_imcu->m_header.last_compact_time = std::chrono::system_clock::now();
  return new_imcu.get();
}

bool Imcu::prune(const Predicate *pred) const {
  // Use min/max to check if IMCU can be pruned
  // Add other operators: OP_LT, OP_BETWEEN, etc.
  // For OP_LT: return m_header.min_value >= cmp->value.double_value;
  // Handle different types (int, date) via template or switch

  return false;
}

bool Imcu::serialize(std::ostream &out) const { return false; }
bool Imcu::deserialize(std::istream &in) { return false; }

void VectorizedPredicateEvaluator::evaluate_batch(const Predicate *pred, const Imcu *imcu,
                                                  const std::vector<row_id_t> &row_ids, bit_array_t &result) {
  if (!pred || row_ids.empty()) return;

  if (pred->is_compound()) {
    evaluate_compound_batch(static_cast<const Compound_Predicate *>(pred), imcu, row_ids, result);
  } else {
    evaluate_simple_batch(static_cast<const Simple_Predicate *>(pred), imcu, row_ids, result);
  }
}

void VectorizedPredicateEvaluator::evaluate_simple_batch(const Simple_Predicate *pred, const Imcu *imcu,
                                                         const std::vector<row_id_t> &row_ids, bit_array_t &result) {
  uint32 col_id = pred->column_id;
  auto *cu = imcu->get_cu(col_id);
  if (!cu) {
    // No CU → all rows fail
    for (size_t i = 0; i < row_ids.size(); i++) {
      Utils::Util::bit_array_reset(&result, i);
    }
    return;
  }

  // Build array of pointers for batch evaluation
  std::vector<const uchar *> values(row_ids.size());
  for (size_t i = 0; i < row_ids.size(); i++) {
    row_id_t local_row_id = row_ids[i];
    if (Utils::Util::bit_array_get(const_cast<Imcu *>(imcu)->get_null_masks()[col_id].get(), local_row_id)) {
      values[i] = nullptr;
    } else {
      values[i] = cu->get_data_address(local_row_id);
    }
  }

  // Use Predicate's vectorized evaluate_batch() method
  pred->evaluate_batch(values.data(), row_ids.size(), 1, result);
}

void VectorizedPredicateEvaluator::evaluate_compound_batch(const Compound_Predicate *pred, const Imcu *imcu,
                                                           const std::vector<row_id_t> &row_ids, bit_array_t &result) {
  switch (pred->op) {
    case PredicateOperator::AND: {
      // Start with all true
      for (size_t i = 0; i < row_ids.size(); i++) {
        Utils::Util::bit_array_set(&result, i);
      }

      // AND each child's results
      bit_array_t child_result(row_ids.size());
      for (const auto &child : pred->children) {
        evaluate_batch(child.get(), imcu, row_ids, child_result);
        // Bitwise AND
        for (size_t i = 0; i < row_ids.size(); i++) {
          if (!Utils::Util::bit_array_get(&child_result, i)) Utils::Util::bit_array_reset(&result, i);
        }
      }
    } break;
    case PredicateOperator::OR: {
      // Start with all false
      for (size_t i = 0; i < row_ids.size(); i++) {
        Utils::Util::bit_array_reset(&result, i);
      }

      // OR each child's results
      bit_array_t child_result(row_ids.size());
      for (const auto &child : pred->children) {
        evaluate_batch(child.get(), imcu, row_ids, child_result);
        // Bitwise OR
        for (size_t i = 0; i < row_ids.size(); i++) {
          if (Utils::Util::bit_array_get(&child_result, i)) Utils::Util::bit_array_set(&result, i);
        }
      }
    } break;
    case PredicateOperator::NOT: {
      if (!pred->children.empty()) {
        evaluate_batch(pred->children[0].get(), imcu, row_ids, result);
        // Bitwise NOT
        for (size_t i = 0; i < row_ids.size(); i++) {
          (Utils::Util::bit_array_get(&result, i)) ? Utils::Util::bit_array_reset(&result, i)
                                                   : Utils::Util::bit_array_set(&result, i);
        }
      }
    } break;
    default:
      // Unknown operator → all false
      for (size_t i = 0; i < row_ids.size(); i++) {
        Utils::Util::bit_array_reset(&result, i);
      }
      break;
  }
}
}  // namespace Imcs
}  // namespace ShannonBase
