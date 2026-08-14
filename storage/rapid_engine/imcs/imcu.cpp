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
#include <shared_mutex>
#include <thread>

#include <iterator>
#include <limits>
#include <sstream>

#include "sql/field.h"                    //Field
#include "sql/field_common_properties.h"  // is_numeric_type
#include "sql/sql_class.h"
#include "sql/table.h"  //TABLE

#include "storage/innobase/include/mach0data.h"

#include "storage/rapid_engine/imcs/cu_recovery.h"
#include "storage/rapid_engine/imcs/imcs.h"  // imcs:pool
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/crc.h"
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
  m_header.storage_index = std::make_unique<StorageIndex>(table_meta.num_columns, this);

  // create row dir index associated with this imcu.
  // Enable column offset tables so that per-column offsets are tracked for
  // fast random access (Oracle IM-style Row Directory with column strides).
  m_header.row_directory = std::make_unique<RowDirectory>(m_header.capacity, table_meta.num_columns,
                                                          /*enable_column_offsets=*/true);
}

row_id_t Imcu::insert_row(const Rapid_load_context *context, const RowBuffer &row_data) {
  // Hold the DML barrier for the entire mutation so a concurrent checkpoint
  // cannot observe a half-written row (checkpoint takes it exclusively).
  std::unique_lock<std::shared_mutex> dml_lock(m_mutation_mutex);

  // 1. allocate local row_id.
  row_id_t local_row_id = allocate_row_id();

  if (local_row_id == INVALID_ROW_ID) {  // IMCU full.
    return INVALID_ROW_ID;
  }

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64 scn = context->m_extra_info.m_scn;
  const bool is_load = (context->m_extra_info.m_oper == Rapid_context::extra_info_t::OperType::LOAD);
  auto *recovery = m_owner_table->recovery_manager();

  // 2. WAL-before-image (PREPARE): persist one atomic redo group for the whole
  // row, fsync it, then apply memory, then fsync the COMMIT marker.  Any
  // failure before COMMIT leaves a PREPARE-without-COMMIT that recovery
  // ignores, so a failed INSERT is never resurrected and a torn tail cannot
  // produce a half-applied row.
  uint64_t op_id = 0;
  uint32_t op_crc = 0;
  uint32_t redo_count = 0;
  if (!is_load && recovery) {
    std::vector<WalCell> cells;
    cells.reserve(row_data.get_num_columns());
    for (size_t col_idx = 0; col_idx < row_data.get_num_columns(); col_idx++) {
      if (!m_cu_array[col_idx]) continue;
      auto *row_col_data = row_data.get_column(col_idx);
      WalCell cell;
      cell.col_id = static_cast<uint32_t>(col_idx);
      cell.is_null = row_col_data->flags.is_null;
      if (!cell.is_null && row_col_data->data && row_col_data->length > 0)
        cell.value.assign(row_col_data->data, row_col_data->data + row_col_data->length);
      cells.push_back(std::move(cell));
    }

    redo_count = static_cast<uint32_t>(cells.size());
    op_id = recovery->log_row_prepare(m_header.imcu_id, local_row_id, txn_id, scn, WAL_MUT_INSERT, cells, &op_crc);
    if (op_id == 0) {
      rollback_inserted_row_locked(local_row_id);
      return INVALID_ROW_ID;
    }
    if (!recovery->sync()) {  // redo not durable → do not mutate memory
      rollback_inserted_row_locked(local_row_id);
      return INVALID_ROW_ID;
    }
  }

  // 3. write to each column.
  bool row_has_null = false;
  for (size_t col_idx = 0; col_idx < row_data.get_num_columns(); col_idx++) {
    if (!m_cu_array[col_idx]) continue;  // means is `NOT_SECONDARY` field.

    auto row_col_data = row_data.get_column(col_idx);
    // dealing with NULL
    if (row_col_data->flags.is_null) {
      assert(row_col_data->data == nullptr);
      assert(m_header.null_masks[col_idx].get());

      std::unique_lock lock(m_header_mutex);
      Utils::Util::bit_array_set(m_header.null_masks[col_idx].get(), local_row_id);
      m_header.storage_index->update_null(col_idx);
      row_has_null = true;
    }

    // write data（dont create version due to its insertion）
    const int write_ret = m_cu_array[col_idx]->write(context, local_row_id, row_col_data->data, row_col_data->length);
    if (write_ret != ShannonBase::SHANNON_SUCCESS) {
      // CU write failed (out-of-range / allocation failure / decompress
      // failure): never leave a half-written row behind as a "success".
      rollback_inserted_row_locked(local_row_id);
      return INVALID_ROW_ID;
    }

    // update Storage Index
    auto src_fld = m_owner_table->meta().fields[col_idx].source_fld;
    bool is_numeric_or_enum =
        is_numeric_type(row_col_data->type) || is_temporal_type(row_col_data->type) ||
        (src_fld && (src_fld->real_type() == MYSQL_TYPE_ENUM || src_fld->real_type() == MYSQL_TYPE_SET));
    if (row_col_data->data && is_numeric_or_enum) {
      double numeric_val = Utils::Util::get_field_numeric<double>(src_fld, row_col_data->data, nullptr,
                                                                  m_owner_table->meta().db_low_byte_first);
      m_header.storage_index->update(col_idx, numeric_val);
    }
  }

  // 4. Build Row Directory entry + column offset table (Oracle IM-style).
  {
    const size_t num_cols = row_data.get_num_columns();
    std::vector<uint16> col_offsets(num_cols);
    std::vector<size_t> col_lengths(num_cols);
    size_t total_row_width = 0;

    for (size_t col_idx = 0; col_idx < num_cols; col_idx++) {
      col_offsets[col_idx] = static_cast<uint16>(total_row_width);
      if (m_cu_array[col_idx]) {
        const auto norm_len = m_cu_array[col_idx]->get_normalized_length();
        const auto *row_col_data = row_data.get_column(col_idx);
        col_lengths[col_idx] = (row_col_data && !row_col_data->flags.is_null && row_col_data->length != UNIV_SQL_NULL)
                                   ? row_col_data->length
                                   : 0;
        total_row_width += norm_len;
      } else {
        col_lengths[col_idx] = 0;
      }
    }

    m_header.row_directory->set_row_entry(local_row_id, static_cast<uint32>(local_row_id * total_row_width),
                                          static_cast<uint32>(total_row_width));
    m_header.row_directory->build_column_offset_table(local_row_id, col_offsets, col_lengths);

    // Record row-level NULL flag so that predicate evaluation can skip
    // per-column null_mask checks when the entire row is non-NULL.
    if (row_has_null) m_header.row_directory->mark_has_null(local_row_id);
  }

  // 5. COMMIT (append + fsync).  Only once the commit marker is known durable
  // do we publish bookkeeping.  A clean append failure leaves an uncommitted
  // PREPARE; an fsync failure is outcome-unknown and log_row_commit() raises
  // recovery_required because restart may discover a durable COMMIT.
  if (!is_load && recovery) {
    const uint64_t commit_lsn = recovery->log_row_commit(op_id, m_header.imcu_id, redo_count, op_crc);
    if (commit_lsn == 0) {
      rollback_inserted_row_locked(local_row_id);
      return INVALID_ROW_ID;
    }
    recovery->mark_applied(commit_lsn);
  }

  // 6. Publish transaction journal + statistics only after the operation is
  // committed (so a failed INSERT leaves no journal/counter residue).
  if (!is_load) {
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

  increment_version();

  return local_row_id;
}

void Imcu::rollback_inserted_row_locked(row_id_t local_row_id) {
  // Caller must already hold m_mutation_mutex exclusively for the whole mutation.
  if (local_row_id >= m_header.current_rows.load(std::memory_order_acquire)) return;

  {
    std::unique_lock lock(m_header_mutex);
    Utils::Util::bit_array_set(m_header.del_mask.get(), local_row_id);
    if (m_header.row_directory) m_header.row_directory->mark_deleted(local_row_id);
  }
  if (m_header.storage_index) m_header.storage_index->invalidate_pruning();
}

void Imcu::rollback_inserted_row(row_id_t local_row_id) {
  std::unique_lock<std::shared_mutex> dml_lock(m_mutation_mutex);
  rollback_inserted_row_locked(local_row_id);
}

int Imcu::delete_row(const Rapid_load_context *context, row_id_t local_row_id) {
  std::unique_lock<std::shared_mutex> dml_lock(m_mutation_mutex);

  if (local_row_id >= m_header.current_rows.load()) return HA_ERR_KEY_NOT_FOUND;

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64 scn = context->m_extra_info.m_scn;
  auto *recovery = m_owner_table->recovery_manager();

  {
    std::shared_lock lock(m_header_mutex);
    if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id)) return HA_ERR_RECORD_DELETED;
  }

  // DELETE participates in the same operation-commit protocol as INSERT and
  // UPDATE.  A durable PREPARE without COMMIT is ignored by recovery; a
  // committed delete is replayed atomically.  This removes the legacy window
  // where a failed standalone delete sync could still be replayed after crash.
  uint64_t commit_lsn = 0;
  if (recovery) {
    static const std::vector<WalCell> kNoCells;
    uint32_t op_crc = 0;
    const uint64_t op_id =
        recovery->log_row_prepare(m_header.imcu_id, local_row_id, txn_id, scn, WAL_MUT_DELETE, kNoCells, &op_crc);
    if (op_id == 0) return HA_ERR_GENERIC;
    if (!recovery->sync()) return HA_ERR_GENERIC;  // PREPARE may survive, but is not committed.

    commit_lsn = recovery->log_row_commit(op_id, m_header.imcu_id, 0, op_crc);
    if (commit_lsn == 0) return HA_ERR_GENERIC;
  }

  // Publication cannot fail: all structures are preallocated/fixed-size for
  // an existing row.  Publish only after the delete operation is durable.
  {
    std::unique_lock lock(m_header_mutex);

    TransactionJournal::Entry entry;
    entry.row_id = local_row_id;
    entry.txn_id = txn_id;
    entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_DELETE);
    entry.status = (scn > 0) ? TransactionJournal::COMMITTED : TransactionJournal::ACTIVE;
    entry.scn = scn;
    entry.timestamp = std::chrono::system_clock::now();
    m_header.txn_journal->add_entry(std::move(entry));

    Utils::Util::bit_array_set(m_header.del_mask.get(), local_row_id);
    if (m_header.row_directory) m_header.row_directory->mark_deleted(local_row_id);

    m_header.delete_count.fetch_add(1);
    m_header.delete_ratio = static_cast<double>(m_header.delete_count.load()) / m_header.current_rows.load();
  }

  increment_version();
  if (m_header.storage_index) m_header.storage_index->invalidate_pruning();
  if (recovery) recovery->mark_applied(commit_lsn);

  return ShannonBase::SHANNON_SUCCESS;
}

size_t Imcu::delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &local_row_ids) {
  if (local_row_ids.empty()) return 0;

  std::unique_lock<std::shared_mutex> dml_lock(m_mutation_mutex);

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64 scn = context->m_extra_info.m_scn;
  auto *recovery = m_owner_table->recovery_manager();

  std::vector<row_id_t> candidates;
  {
    std::shared_lock lock(m_header_mutex);
    candidates.reserve(local_row_ids.size());
    for (row_id_t local_row_id : local_row_ids) {
      if (local_row_id >= m_header.current_rows.load()) continue;
      if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id)) continue;
      candidates.push_back(local_row_id);
    }
  }
  if (candidates.empty()) return 0;

  struct PendingDelete {
    row_id_t row_id;
    uint64_t op_id;
    uint32_t op_crc;
  };
  std::vector<row_id_t> committed_rows;
  uint64_t max_commit_lsn = 0;

  if (recovery) {
    static const std::vector<WalCell> kNoCells;
    std::vector<PendingDelete> pending;
    pending.reserve(candidates.size());

    // Batch all PREPARE records behind one durability boundary.
    for (row_id_t local_row_id : candidates) {
      uint32_t op_crc = 0;
      const uint64_t op_id =
          recovery->log_row_prepare(m_header.imcu_id, local_row_id, txn_id, scn, WAL_MUT_DELETE, kNoCells, &op_crc);
      if (op_id == 0) break;
      pending.push_back({local_row_id, op_id, op_crc});
    }
    if (pending.empty()) return 0;
    if (!recovery->sync()) return 0;  // only uncommitted PREPAREs may remain durable.

    committed_rows.reserve(pending.size());
    for (const auto &p : pending) {
      const uint64_t lsn = recovery->log_row_commit(p.op_id, m_header.imcu_id, 0, p.op_crc);
      if (lsn == 0) break;  // ambiguous COMMIT sets recovery_required; stop issuing more WAL.
      committed_rows.push_back(p.row_id);
      max_commit_lsn = std::max(max_commit_lsn, lsn);
    }
  } else {
    committed_rows = std::move(candidates);
  }

  size_t deleted = 0;
  {
    std::unique_lock lock(m_header_mutex);
    for (row_id_t local_row_id : committed_rows) {
      // DML is serialized by m_mutation_mutex, so the candidate cannot have
      // changed between validation and publication.
      TransactionJournal::Entry entry;
      entry.row_id = local_row_id;
      entry.txn_id = txn_id;
      entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_DELETE);
      entry.status = (scn > 0) ? TransactionJournal::EntryStatus::COMMITTED : TransactionJournal::EntryStatus::ACTIVE;
      entry.scn = scn;
      entry.timestamp = std::chrono::system_clock::now();
      m_header.txn_journal->add_entry(std::move(entry));

      Utils::Util::bit_array_set(m_header.del_mask.get(), local_row_id);
      if (m_header.row_directory) m_header.row_directory->mark_deleted(local_row_id);
      ++deleted;
    }

    if (deleted > 0) {
      m_header.delete_count.fetch_add(deleted);
      m_header.delete_ratio = static_cast<double>(m_header.delete_count.load()) / m_header.current_rows.load();
    }
  }

  if (deleted > 0) {
    increment_version();
    if (m_header.storage_index) m_header.storage_index->invalidate_pruning();
    if (recovery && max_commit_lsn > 0) recovery->mark_applied(max_commit_lsn);
  }

  return deleted;
}

int Imcu::update_row(const Rapid_load_context *context, row_id_t local_row_id,
                     const std::unordered_map<uint32, RowBuffer::ColumnValue> &updates) {
  // UPDATE is a multi-column atomic mutation.  Serialize DML at IMCU scope so
  // rollback can only pop versions created by this operation, never a racing
  // writer's version head.
  std::unique_lock<std::shared_mutex> dml_lock(m_mutation_mutex);

  if (local_row_id >= m_header.current_rows.load()) return HA_ERR_KEY_NOT_FOUND;

  {
    std::shared_lock lock(m_header_mutex);
    if (Utils::Util::bit_array_get(m_header.del_mask.get(), local_row_id)) return HA_ERR_RECORD_DELETED;
  }

  Transaction::ID txn_id = context->m_extra_info.m_trxid;
  uint64 scn = context->m_extra_info.m_scn;
  auto *recovery = m_owner_table->recovery_manager();

  if (m_header.storage_index) m_header.storage_index->invalidate_pruning();

  uint64_t op_id = 0;
  uint32_t op_crc = 0;
  uint32_t redo_count = 0;
  if (recovery) {
    std::vector<WalCell> cells;
    cells.reserve(updates.size());
    for (const auto &[col_idx, new_value] : updates) {
      if (!get_cu(col_idx)) continue;
      WalCell cell;
      cell.col_id = col_idx;
      cell.is_null = new_value.flags.is_null;
      if (!cell.is_null && new_value.data && new_value.length > 0)
        cell.value.assign(new_value.data, new_value.data + new_value.length);
      cells.push_back(std::move(cell));
    }

    redo_count = static_cast<uint32_t>(cells.size());
    op_id = recovery->log_row_prepare(m_header.imcu_id, local_row_id, txn_id, scn, WAL_MUT_UPDATE, cells, &op_crc);
    if (op_id == 0) return HA_ERR_GENERIC;
    if (!recovery->sync()) return HA_ERR_GENERIC;
  }

  struct AppliedColumn {
    uint32 col_idx;
    bool was_null;
  };
  std::vector<AppliedColumn> applied;
  applied.reserve(updates.size());

  auto rollback_applied = [&]() {
    bool rollback_ok = true;
    for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
      auto *cu = get_cu(it->col_idx);
      if (!cu || cu->rollback_update(local_row_id) != ShannonBase::SHANNON_SUCCESS) rollback_ok = false;

      std::unique_lock header_lock(m_header_mutex);
      if (it->col_idx < m_header.null_masks.size() && m_header.null_masks[it->col_idx]) {
        it->was_null ? Utils::Util::bit_array_set(m_header.null_masks[it->col_idx].get(), local_row_id)
                     : Utils::Util::bit_array_reset(m_header.null_masks[it->col_idx].get(), local_row_id);
      }
    }
    m_header.storage_index->invalidate_pruning();
    if (!rollback_ok && recovery) recovery->require_recovery();
    return rollback_ok;
  };

  for (const auto &[col_idx, new_value] : updates) {
    auto *cu = get_cu(col_idx);
    if (!cu) continue;

    bool was_null = false;
    {
      std::shared_lock header_lock(m_header_mutex);
      if (col_idx < m_header.null_masks.size() && m_header.null_masks[col_idx])
        was_null = Utils::Util::bit_array_get(m_header.null_masks[col_idx].get(), local_row_id);
    }

    const int update_ret = cu->update(context, local_row_id, new_value.data, new_value.length);
    if (update_ret != ShannonBase::SHANNON_SUCCESS) {
      rollback_applied();
      return update_ret;
    }
    applied.push_back({col_idx, was_null});

    std::unique_lock header_lock(m_header_mutex);
    if (col_idx < m_header.null_masks.size() && m_header.null_masks[col_idx]) {
      new_value.flags.is_null ? Utils::Util::bit_array_set(m_header.null_masks[col_idx].get(), local_row_id)
                              : Utils::Util::bit_array_reset(m_header.null_masks[col_idx].get(), local_row_id);
    }
  }

  if (recovery) {
    const uint64_t commit_lsn = recovery->log_row_commit(op_id, m_header.imcu_id, redo_count, op_crc);
    if (commit_lsn == 0) {
      rollback_applied();
      return HA_ERR_GENERIC;
    }
    recovery->mark_applied(commit_lsn);
  }

  if (m_header.row_directory) {
    for (const auto &[col_idx, new_value] : updates) {
      if (!get_cu(col_idx)) continue;
      const size_t logical_len = (new_value.flags.is_null || new_value.length == UNIV_SQL_NULL) ? 0 : new_value.length;
      m_header.row_directory->set_column_length(local_row_id, col_idx, logical_len);
    }
  }

  {
    std::unique_lock lock(m_header_mutex);

    TransactionJournal::Entry entry;
    entry.row_id = local_row_id;
    entry.txn_id = txn_id;
    entry.operation = static_cast<uint8_t>(OPER_TYPE::OPER_UPDATE);
    entry.status = (scn > 0) ? TransactionJournal::COMMITTED : TransactionJournal::ACTIVE;
    entry.scn = scn;
    entry.timestamp = std::chrono::system_clock::now();

    for (const auto &[col_idx, value] : updates) entry.modified_columns.set(col_idx);

    m_header.txn_journal->add_entry(std::move(entry));
    m_header.update_count.fetch_add(1);
  }

  increment_version();
  return ShannonBase::SHANNON_SUCCESS;
}

void Imcu::evaluate_predicates_vectorized(const std::vector<std::unique_ptr<Predicate>> &predicates, row_id_t start_row,
                                          size_t num_rows, bit_array_t &result) {
  result.set();
  bit_array_t predicate_result = result.clone_empty();
  for (const auto &predicate : predicates) {
    const auto *pred = predicate.get();
    if (!pred) continue;

    predicate_result.reset();
    if (pred->is_compound()) {
      evaluate_compound_predicate_vectorized(static_cast<const Compound_Predicate *>(pred), start_row, num_rows,
                                             predicate_result);
    } else {
      evaluate_simple_predicate_vectorized(static_cast<const Simple_Predicate *>(pred), start_row, num_rows,
                                           predicate_result);
    }
    result.and_with(predicate_result);
    if (result.is_all_false()) break;
  }
}

void Imcu::evaluate_compound_predicate_vectorized(const Compound_Predicate *pred, const row_id_t start_row,
                                                  size_t num_rows, bit_array_t &result) {
  if (!pred || pred->children.empty()) {
    result.reset();
    return;
  }

  switch (pred->op) {
    case PredicateOperator::AND: {
      result.set();
      bit_array_t child_result = result.clone_empty();
      for (const auto &child : pred->children) {
        if (!child) continue;
        child_result.reset();  // clear before each child evaluation
        child->is_compound()
            ? evaluate_compound_predicate_vectorized(static_cast<const Compound_Predicate *>(child.get()), start_row,
                                                     num_rows, child_result)
            : evaluate_simple_predicate_vectorized(static_cast<const Simple_Predicate *>(child.get()), start_row,
                                                   num_rows, child_result);
        result.and_with(child_result);
      }
    } break;
    case PredicateOperator::OR: {
      result.reset();
      bit_array_t child_result = result.clone_empty();
      for (const auto &child : pred->children) {
        if (!child) continue;
        child_result.reset();
        child->is_compound()
            ? evaluate_compound_predicate_vectorized(static_cast<const Compound_Predicate *>(child.get()), start_row,
                                                     num_rows, child_result)
            : evaluate_simple_predicate_vectorized(static_cast<const Simple_Predicate *>(child.get()), start_row,
                                                   num_rows, child_result);
        result.or_with(child_result);
      }
    } break;
    case PredicateOperator::NOT: {
      // evaluate_predicates_vectorized; NOT has exactly one operand.
      if (pred->children.size() != 1) {
        DBUG_PRINT("imcu_scan", ("NOT predicate has %zu children (expected 1)", pred->children.size()));
        result.reset();
        break;
      }
      const auto &only_child = pred->children[0];
      if (!only_child) {
        result.reset();
        break;
      }
      // SQL three-valued logic: NOT UNKNOWN == UNKNOWN, so a bit inversion of
      // the child's TRUE-only mask is wrong.  Evaluate per row instead.
      for (size_t i = 0; i < num_rows; ++i) {
        const TruthValue tv = evaluate_predicate_truth_at_row(only_child.get(), start_row + i);
        // WHERE keeps only TRUE rows.  For NOT(child), that means the child
        // itself must be FALSE; TRUE becomes FALSE and UNKNOWN stays UNKNOWN.
        (tv == TruthValue::FALSE_VALUE) ? Utils::Util::bit_array_set(&result, i)
                                        : Utils::Util::bit_array_reset(&result, i);
      }
    } break;
    default:
      result.reset();
      break;
  }
}

TruthValue Imcu::evaluate_predicate_truth_at_row(const Predicate *pred, row_id_t local_row_id) const {
  if (!pred) return TruthValue::FALSE_VALUE;

  if (!pred->is_compound()) {
    const auto *simple = static_cast<const Simple_Predicate *>(pred);
    const uint32 col_id = simple->column_id;
    const CU *cu = get_cu(col_id);
    if (!cu) return TruthValue::FALSE_VALUE;

    // Deleted rows are treated as NULL (fail the predicate).
    const auto *row_entry = m_header.row_directory ? m_header.row_directory->get_row_entry(local_row_id) : nullptr;
    if (row_entry && row_entry->flags.is_deleted) return TruthValue::FALSE_VALUE;

    simple->field_meta.store(cu->field(), std::memory_order_release);
    simple->low_order.store(m_owner_table->meta().db_low_byte_first, std::memory_order_release);
    simple->column_type.store(cu->type(), std::memory_order_release);

    if (col_id < m_header.null_masks.size() && m_header.null_masks[col_id] &&
        Utils::Util::bit_array_get(m_header.null_masks[col_id].get(), local_row_id)) {
      const uchar *null_value = nullptr;
      return simple->evaluate_truth(null_value);
    }

    auto dict = cu->dictionary();
    if (dict && (cu->real_type() == MYSQL_TYPE_ENUM || cu->real_type() == MYSQL_TYPE_SET)) dict = nullptr;

    auto data_guard = cu->resolve_data(local_row_id);
    if (dict) {
      uint32 str_id = 0;
      std::memcpy(&str_id, data_guard.get(), sizeof(str_id));
      thread_local std::string str_storage;
      str_storage = dict->get(str_id);
      const uchar *value = reinterpret_cast<const uchar *>(str_storage.data());
      return simple->evaluate_truth_with_length(value, str_storage.size());
    }

    const uchar *value = data_guard.get();
    if (cu->has_varlen_pool()) return simple->evaluate_truth_with_length(value, cu->get_logical_length(local_row_id));
    return simple->evaluate_truth(value);
  }

  const auto *compound = static_cast<const Compound_Predicate *>(pred);
  switch (compound->op) {
    case PredicateOperator::AND: {
      TruthValue result = TruthValue::TRUE_VALUE;
      for (const auto &child : compound->children) {
        const TruthValue c = evaluate_predicate_truth_at_row(child.get(), local_row_id);
        if (c == TruthValue::FALSE_VALUE) return TruthValue::FALSE_VALUE;
        if (c == TruthValue::UNKNOWN) result = TruthValue::UNKNOWN;
      }
      return result;
    }
    case PredicateOperator::OR: {
      TruthValue result = TruthValue::FALSE_VALUE;
      for (const auto &child : compound->children) {
        const TruthValue c = evaluate_predicate_truth_at_row(child.get(), local_row_id);
        if (c == TruthValue::TRUE_VALUE) return TruthValue::TRUE_VALUE;
        if (c == TruthValue::UNKNOWN) result = TruthValue::UNKNOWN;
      }
      return result;
    }
    case PredicateOperator::NOT: {
      if (compound->children.empty()) return TruthValue::FALSE_VALUE;
      const TruthValue c = evaluate_predicate_truth_at_row(compound->children[0].get(), local_row_id);
      switch (c) {
        case TruthValue::TRUE_VALUE:
          return TruthValue::FALSE_VALUE;
        case TruthValue::FALSE_VALUE:
          return TruthValue::TRUE_VALUE;
        case TruthValue::UNKNOWN:
        default:
          return TruthValue::UNKNOWN;
      }
    }
    default:
      return TruthValue::FALSE_VALUE;
  }
}

void Imcu::evaluate_simple_predicate_vectorized(const Simple_Predicate *pred, row_id_t start_row, size_t num_rows,
                                                bit_array_t &result) {
  const uint32 col_id = pred->column_id;
  const CU *cu = get_cu(col_id);
  if (!cu) {
    result.reset();  // no column unit → all rows fail
    return;
  }

  pred->field_meta.store(cu->field(), std::memory_order_release);
  pred->low_order.store(m_owner_table->meta().db_low_byte_first, std::memory_order_release);
  pred->column_type.store(cu->type(), std::memory_order_release);

  if (cu->has_varlen_pool()) {
    for (size_t i = 0; i < num_rows; ++i) {
      const row_id_t local_row_id = start_row + i;
      const auto *row_entry = m_header.row_directory->get_row_entry(local_row_id);
      if (row_entry && row_entry->flags.is_deleted) {
        Utils::Util::bit_array_reset(&result, i);
        continue;
      }

      const bool is_null = col_id < m_header.null_masks.size() && m_header.null_masks[col_id] &&
                           Utils::Util::bit_array_get(m_header.null_masks[col_id].get(), local_row_id);
      if (is_null) {
        const uchar *null_value = nullptr;
        pred->evaluate(null_value) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
        continue;
      }

      auto data_guard = cu->resolve_data(local_row_id);
      const uchar *value = data_guard.get();
      const TruthValue tv = pred->evaluate_truth_with_length(value, cu->get_logical_length(local_row_id));
      (tv == TruthValue::TRUE_VALUE) ? Utils::Util::bit_array_set(&result, i)
                                     : Utils::Util::bit_array_reset(&result, i);
    }
    return;
  }

  auto dict = cu->dictionary();
  if (dict && (cu->real_type() == MYSQL_TYPE_ENUM || cu->real_type() == MYSQL_TYPE_SET)) dict = nullptr;
  if (dict) {
    for (size_t i = 0; i < num_rows; ++i) {
      const row_id_t local_row_id = start_row + i;
      const auto *row_entry = m_header.row_directory->get_row_entry(local_row_id);
      if (row_entry && row_entry->flags.is_deleted) {
        Utils::Util::bit_array_reset(&result, i);
        continue;
      }
      const bool is_null = col_id < m_header.null_masks.size() && m_header.null_masks[col_id] &&
                           Utils::Util::bit_array_get(m_header.null_masks[col_id].get(), local_row_id);
      if (is_null) {
        const uchar *null_value = nullptr;
        const TruthValue tv = pred->evaluate_truth(null_value);
        (tv == TruthValue::TRUE_VALUE) ? Utils::Util::bit_array_set(&result, i)
                                       : Utils::Util::bit_array_reset(&result, i);
        continue;
      }

      auto data_guard = cu->resolve_data(local_row_id);
      if (!data_guard.get()) {
        Utils::Util::bit_array_reset(&result, i);
        continue;
      }
      uint32 str_id = 0;
      std::memcpy(&str_id, data_guard.get(), sizeof(str_id));
      const std::string decoded = dict->get(str_id);
      const uchar *value = reinterpret_cast<const uchar *>(decoded.data());
      const TruthValue tv = pred->evaluate_truth_with_length(value, decoded.size());
      (tv == TruthValue::TRUE_VALUE) ? Utils::Util::bit_array_set(&result, i)
                                     : Utils::Util::bit_array_reset(&result, i);
    }
    return;
  }

  // Fixed-width values have stable CU-owned addresses and can be batched.
  std::vector<const uchar *> values(num_rows);
  for (size_t i = 0; i < num_rows; ++i) {
    const row_id_t local_row_id = start_row + i;
    const auto *row_entry = m_header.row_directory->get_row_entry(local_row_id);
    if (row_entry && row_entry->flags.is_deleted) {
      values[i] = nullptr;  // visibility filtering removes deleted rows later
      continue;
    }

    const bool is_null = col_id < m_header.null_masks.size() && m_header.null_masks[col_id] &&
                         Utils::Util::bit_array_get(m_header.null_masks[col_id].get(), local_row_id);
    if (is_null) {
      values[i] = nullptr;
      continue;
    }

    auto data_ptr = cu->resolve_data(local_row_id);
    if (!data_ptr.get()) {
      values[i] = nullptr;
      continue;
    }
    values[i] = data_ptr.get();
  }
  const_cast<Simple_Predicate *>(pred)->evaluate_vectorized(values, num_rows, result);
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
  auto *cu = get_cu(col_id);
  assert(cu);
  const uchar *value = cu->resolve_data(local_row_id);
  row_cache[col_id] = value;
  return value;
}

bool Imcu::is_row_visible(Rapid_scan_context *context, row_id_t local_row_id, Transaction::ID reader_txn_id,
                          uint64 reader_scn) const {
  assert(context);
  context->m_extra_info.m_trxid = reader_txn_id;
  context->m_extra_info.m_scn = reader_scn;

  const size_t num_rows = m_header.current_rows.load(std::memory_order_acquire);
  if (local_row_id >= num_rows) return false;

  {
    bit_array_t visibility_mask(1);
    check_visibility_batch(context, local_row_id, 1, visibility_mask);
    if (!Utils::Util::bit_array_get(&visibility_mask, 0))
      return false;  // row is invisible (uncommitted insert, or committed delete)
  }

  return true;
}

void Imcu::check_visibility_batch(Rapid_scan_context *context, row_id_t start_row, size_t count,
                                  bit_array_t &visibility_mask) const {
  std::vector<row_id_t> ids(count);
  for (size_t i = 0; i < count; ++i) ids[i] = static_cast<row_id_t>(start_row + i);
  check_visibility_for_rows(context, ids, visibility_mask);
}

void Imcu::check_visibility_for_rows(Rapid_scan_context *context, const std::vector<row_id_t> &row_ids,
                                     bit_array_t &visibility_mask) const {
  std::shared_lock lock(m_header_mutex);
  const size_t count = row_ids.size();

  // Fast path: if all transactions are committed, only the delete mask matters.
  if (m_header.txn_journal->is_all_committed()) {
    for (size_t i = 0; i < count; ++i) {
      if (!Utils::Util::bit_array_get(m_header.del_mask.get(), row_ids[i]))
        Utils::Util::bit_array_set(&visibility_mask, i);
      else
        Utils::Util::bit_array_reset(&visibility_mask, i);
    }
    return;
  }

  // Slow path: consult transaction journal for MVCC visibility.
  for (size_t i = 0; i < count; ++i) {
    if (m_header.txn_journal->is_row_visible(row_ids[i], context->m_extra_info.m_trxid, context->m_extra_info.m_scn))
      Utils::Util::bit_array_set(&visibility_mask, i);
    else
      Utils::Util::bit_array_reset(&visibility_mask, i);
  }

  // Filter out rows marked deleted in the delete mask.
  for (size_t i = 0; i < count; i++) {
    if (Utils::Util::bit_array_get(&visibility_mask, i)) {
      if (Utils::Util::bit_array_get(m_header.del_mask.get(), row_ids[i])) {
        if (!m_header.txn_journal->is_row_visible(row_ids[i], context->m_extra_info.m_trxid,
                                                  context->m_extra_info.m_scn))
          Utils::Util::bit_array_reset(&visibility_mask, i);
      }
    }
  }
}

void Imcu::evaluate_predicates_for_rows(const std::vector<std::unique_ptr<Predicate>> &predicates,
                                        const std::vector<row_id_t> &row_ids, bit_array_t &result) {
  for (size_t i = 0; i < row_ids.size(); ++i) {
    bit_array_t row_result(1);
    evaluate_predicates_vectorized(predicates, row_ids[i], 1, row_result);
    if (Utils::Util::bit_array_get(&row_result, 0))
      Utils::Util::bit_array_set(&result, i);
    else
      Utils::Util::bit_array_reset(&result, i);
  }
}

bool Imcu::read_row(Rapid_scan_context *context, row_id_t local_row_id, const std::vector<uint32> &col_indices,
                    RowBuffer &output) {
  const size_t num_rows = m_header.current_rows.load(std::memory_order_acquire);
  if (local_row_id >= num_rows) return false;

  const auto *row_entry = m_header.row_directory->get_row_entry(local_row_id);
  if (row_entry && row_entry->flags.is_deleted) return false;

  {
    bit_array_t visibility_mask(1);
    check_visibility_batch(context, local_row_id, 1, visibility_mask);
    if (!Utils::Util::bit_array_get(&visibility_mask, 0)) return false;
  }

  output.set_row_id(m_header.start_row + local_row_id);
  for (uint32 col_idx : col_indices) {
    auto *cu = get_cu(col_idx);
    assert(cu);
    if (col_idx < m_header.null_masks.size() && m_header.null_masks[col_idx] &&
        Utils::Util::bit_array_get(m_header.null_masks[col_idx].get(), local_row_id)) {
      output.set_column_null(col_idx);
      continue;
    }

    auto data_guard = cu->resolve_data(local_row_id);
    const uchar *data_ptr = data_guard.get();
    if (!data_ptr) {
      output.set_column_null(col_idx);
      continue;
    }

    // Dictionary CUs store a uint32 id in the fixed slot, not the logical
    // string.  Decode before exposing the row value.
    auto *dict = cu->dictionary();
    if (dict && cu->real_type() != MYSQL_TYPE_ENUM && cu->real_type() != MYSQL_TYPE_SET) {
      uint32 dict_id = 0;
      std::memcpy(&dict_id, data_ptr, sizeof(dict_id));
      const std::string decoded = dict->get(dict_id);
      output.set_column_copy(col_idx, reinterpret_cast<const uchar *>(decoded.data()), decoded.size(), cu->type());
      continue;
    }

    size_t data_len = m_header.row_directory ? m_header.row_directory->get_column_length(local_row_id, col_idx) : 0;
    if (data_len == 0) data_len = cu->get_logical_length(local_row_id);

    if (cu->has_varlen_pool()) {
      // resolve_data() returns a VarlenReadGuard.  A zero-copy pointer would
      // outlive that guard when read_row() returns, so copy varlen payloads
      // while the guard is still pinned.
      output.set_column_copy(col_idx, data_ptr, data_len, cu->type());
    } else {
      output.set_column_zero_copy(col_idx, data_ptr, data_len, cu->type());
    }
  }
  return true;
}

void Imcu::update_storage_index() {
  if (!m_header.storage_index) return;

  std::unique_lock<std::shared_mutex> dml_lock(m_mutation_mutex);
  std::unique_lock lock(m_header_mutex);
  size_t num_rows = m_header.current_rows.load(std::memory_order_acquire);

  m_header.storage_index->reset_stats();
  if (num_rows == 0) {
    m_header.storage_index->clear_dirty();
    m_header.last_modified = std::chrono::system_clock::now();
    return;
  }

  for (auto &[col_idx, cu] : m_column_units) {
    if (!cu) continue;

    Field *source_field = cu->get_source_field();
    if (!source_field) continue;
    enum_field_types field_type = cu->type();
    bool is_numeric = is_numeric_type(field_type) || is_temporal_type(field_type) ||
                      (cu->real_type() == MYSQL_TYPE_ENUM || cu->real_type() == MYSQL_TYPE_SET);

    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
      if (Utils::Util::bit_array_get(m_header.del_mask.get(), row_idx)) continue;
      if (m_header.null_masks[col_idx] && Utils::Util::bit_array_get(m_header.null_masks[col_idx].get(), row_idx)) {
        m_header.storage_index->update_null(col_idx);
        continue;
      }

      auto data_guard = cu->resolve_data(row_idx);
      const uchar *data = data_guard.get();
      if (!data) continue;

      if (is_numeric) {
        double numeric_val = Utils::Util::get_field_numeric<double>(source_field, data, nullptr,
                                                                    m_owner_table->meta().db_low_byte_first);
        m_header.storage_index->update(col_idx, numeric_val);
        continue;
      }

      switch (field_type) {
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING: {
          auto *dict = cu->dictionary();
          if (dict && cu->real_type() != MYSQL_TYPE_ENUM && cu->real_type() != MYSQL_TYPE_SET) {
            uint32 dict_id = 0;
            std::memcpy(&dict_id, data, sizeof(dict_id));
            m_header.storage_index->update_string_stats(col_idx, dict->get(dict_id));
          } else {
            const size_t str_len = cu->get_logical_length(row_idx);
            m_header.storage_index->update_string_stats(col_idx,
                                                        std::string(reinterpret_cast<const char *>(data), str_len));
          }
        } break;
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB: {
          // resolve_data() already points at the logical BLOB payload.  Do not
          // reinterpret payload bytes as a packed Field_blob header/pointer.
          const size_t blob_len = cu->get_logical_length(row_idx);
          m_header.storage_index->update_string_stats(
              col_idx, std::string(reinterpret_cast<const char *>(data), std::min(blob_len, size_t(256))));
        } break;
        default:
          break;
      }
    }
  }

  m_header.storage_index->clear_dirty();
  m_header.last_modified = std::chrono::system_clock::now();
}

size_t Imcu::garbage_collect(uint64 min_active_scn) {
  size_t freed = 0;
  // 1. purge TxnJ.
  freed += m_header.txn_journal->purge(min_active_scn);
  // 2. clear version of every column.
  for (auto &[col_idx, cu] : m_column_units) {
    if (cu) {  // Add null check
      freed += cu->purge_versions(nullptr, min_active_scn);
    }
  }
  // 3. update statistics.
  m_header.version_count = 0;  // estimate_version_count();
  m_header.last_gc_time = std::chrono::system_clock::now();
  return freed;
}

std::shared_ptr<Imcu> Imcu::compact() {
  // Compaction is intentionally disabled for now (Option A: stable row ids).
  //
  // Physically renumbering surviving rows (the previous implementation) breaks
  // the global row-id contract: the primary/secondary indexes still hold
  // `imcu.start_row + old_local_row_id`, but the local ids would shift after
  // compaction, so subsequent index lookups would resolve to the wrong record.
  //
  // Until an index / reorg framework maintains an old->new row-id remap (or
  // until compaction keeps slots stable), we must not renumber rows.  Returning
  // nullptr keeps the original IMCU in place and makes callers skip compaction.
  return nullptr;
}

namespace {
constexpr uint32_t kImcuMagic = 0x494D4355u;  // "IMCU" (LE)
constexpr uint16_t kImcuVersion = 1u;

// Upper bound for a single serialized CU payload.  CU::deserialize() enforces
// its own tighter per-CU limits; this only guards the local allocation here.
constexpr size_t kMaxImcuCuPayloadSize = 1u << 28;  // 256 MiB

template <typename T>
void write_pod(std::ostream &out, const T &v) {
  out.write(reinterpret_cast<const char *>(&v), sizeof(T));
}

struct MemStreamBuf : std::streambuf {
  MemStreamBuf(const char *data, size_t size) {
    char *p = const_cast<char *>(data);
    setg(p, p, p + size);
  }
};

struct ByteCursor {
  const char *p;
  const char *end;

  bool read(void *dst, size_t n) {
    if (static_cast<size_t>(end - p) < n) return false;
    if (n > 0) std::memcpy(dst, p, n);
    p += n;
    return true;
  }

  template <typename T>
  bool read_pod(T &v) {
    return read(&v, sizeof(T));
  }
};

bool write_bit_array(std::ostream &out, const bit_array_t *ba) {
  if (!ba) {
    write_pod(out, static_cast<uint64_t>(0));
    write_pod(out, static_cast<uint64_t>(0));
    return out.good();
  }
  write_pod(out, static_cast<uint64_t>(ba->rows));
  write_pod(out, static_cast<uint64_t>(ba->size));
  if (ba->size > 0 && ba->data) {
    out.write(reinterpret_cast<const char *>(ba->data), static_cast<std::streamsize>(ba->size));
  }
  return out.good();
}

bool read_bit_array(ByteCursor &cur, bit_array_t *ba, size_t expected_rows, size_t expected_bytes) {
  uint64_t rows = 0, bytes = 0;
  if (!cur.read_pod(rows) || !cur.read_pod(bytes)) return false;
  if (ba == nullptr) return rows == 0 && bytes == 0;
  if (rows != expected_rows || bytes != expected_bytes) return false;
  if (bytes == 0) return true;
  if (!ba->data || ba->size != bytes) return false;
  return cur.read(ba->data, bytes);
}
}  // namespace

bool Imcu::serialize(std::ostream &out) const {
  std::ostringstream body(std::ios::binary);

  const uint64_t start_row = static_cast<uint64_t>(m_header.start_row);
  const uint64_t end_row = static_cast<uint64_t>(m_header.end_row);
  const uint64_t capacity = static_cast<uint64_t>(m_header.capacity);
  const uint64_t current_rows = static_cast<uint64_t>(m_header.current_rows.load(std::memory_order_acquire));
  const uint8_t status = static_cast<uint8_t>(m_header.status.load(std::memory_order_acquire));

  write_pod(body, kImcuMagic);
  write_pod(body, kImcuVersion);
  write_pod(body, static_cast<uint16_t>(0));  // flags (reserved)
  write_pod(body, m_header.imcu_id);
  write_pod(body, start_row);
  write_pod(body, end_row);
  write_pod(body, capacity);
  write_pod(body, current_rows);
  write_pod(body, status);
  const uint8_t reserved[7] = {};
  body.write(reinterpret_cast<const char *>(reserved), sizeof(reserved));

  write_bit_array(body, m_header.del_mask.get());

  const uint64_t nm_count = static_cast<uint64_t>(m_header.null_masks.size());
  write_pod(body, nm_count);
  for (const auto &nm : m_header.null_masks) {
    write_bit_array(body, nm.get());
  }

  // Length-delimited CU payloads (only non-null CUs; column_id disambiguates).
  uint32_t cu_count = 0;
  std::ostringstream cus(std::ios::binary);
  for (size_t col = 0; col < m_cu_array.size(); ++col) {
    CU *cu = m_cu_array[col];
    if (!cu) continue;
    std::ostringstream cu_buf(std::ios::binary);
    const int rc = cu->serialize(cu_buf, current_rows);
    if (rc != ShannonBase::SHANNON_SUCCESS) return false;
    const std::string payload = cu_buf.str();
    write_pod(cus, static_cast<uint32_t>(col));
    write_pod(cus, static_cast<uint64_t>(payload.size()));
    cus.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    ++cu_count;
  }
  write_pod(body, cu_count);
  const std::string cu_payloads = cus.str();
  body.write(cu_payloads.data(), static_cast<std::streamsize>(cu_payloads.size()));

  const std::string bytes = body.str();
  const uint32_t crc = Utils::crc32c_compute(bytes.data(), bytes.size(), 0);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  write_pod(out, crc);
  return out.good();
}

bool Imcu::deserialize(std::istream &in) {
  const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (data.size() < sizeof(uint32_t)) return false;

  const size_t body_size = data.size() - sizeof(uint32_t);
  uint32_t stored_crc = 0;
  std::memcpy(&stored_crc, data.data() + body_size, sizeof(uint32_t));
  const uint32_t computed_crc = Utils::crc32c_compute(data.data(), body_size, 0);
  if (stored_crc != computed_crc) return false;

  ByteCursor cur{data.data(), data.data() + body_size};

  uint32_t magic = 0;
  uint16_t version = 0, flags = 0;
  uint32_t imcu_id = 0;
  uint64_t start_row = 0, end_row = 0, capacity = 0, current_rows = 0;
  uint8_t status = 0;
  uint8_t reserved[7] = {};

  if (!cur.read_pod(magic) || magic != kImcuMagic) return false;
  if (!cur.read_pod(version) || version != kImcuVersion) return false;
  if (!cur.read_pod(flags)) return false;
  if (!cur.read_pod(imcu_id) || imcu_id != m_header.imcu_id) return false;
  if (!cur.read_pod(start_row)) return false;
  if (!cur.read_pod(end_row)) return false;
  if (!cur.read_pod(capacity) || capacity != static_cast<uint64_t>(m_header.capacity)) return false;
  if (!cur.read_pod(current_rows) || current_rows > capacity) return false;
  if (!cur.read_pod(status)) return false;
  if (!cur.read(reserved, sizeof(reserved))) return false;
  if (end_row < start_row || (end_row - start_row) != capacity) return false;

  const size_t expected_mask_bytes = (m_header.capacity + 7) / 8;
  if (!read_bit_array(cur, m_header.del_mask.get(), m_header.capacity, expected_mask_bytes)) return false;

  uint64_t nm_count = 0;
  if (!cur.read_pod(nm_count) || nm_count > m_header.null_masks.size()) return false;
  for (uint64_t i = 0; i < nm_count; ++i) {
    bit_array_t *nm = (i < m_header.null_masks.size()) ? m_header.null_masks[i].get() : nullptr;
    if (!read_bit_array(cur, nm, m_header.capacity, expected_mask_bytes)) return false;
  }

  uint32_t cu_count = 0;
  if (!cur.read_pod(cu_count) || cu_count > m_cu_array.size()) return false;

  for (uint32_t i = 0; i < cu_count; ++i) {
    uint32_t column_id = 0;
    uint64_t payload_size = 0;
    if (!cur.read_pod(column_id) || !cur.read_pod(payload_size)) return false;
    if (payload_size > kMaxImcuCuPayloadSize) return false;
    if (static_cast<uint64_t>(cur.end - cur.p) < payload_size) return false;
    if (column_id >= m_cu_array.size() || !m_cu_array[column_id]) return false;

    MemStreamBuf msb(cur.p, static_cast<size_t>(payload_size));
    std::istream cu_in(&msb);
    const int rc = m_cu_array[column_id]->deserialize(cu_in);
    if (rc != ShannonBase::SHANNON_SUCCESS) return false;
    cur.p += static_cast<ptrdiff_t>(payload_size);
  }

  // All validation passed — commit to live header state.
  m_header.start_row = static_cast<row_id_t>(start_row);
  m_header.end_row = static_cast<row_id_t>(end_row);
  m_header.current_rows.store(static_cast<size_t>(current_rows), std::memory_order_release);
  m_header.status.store(static_cast<imcu_header_t::Status>(status), std::memory_order_release);

  return true;
}

double ImcuPruningAnalyzer::estimate_skip_ratio(Item *condition) {
  if (!condition || !m_rpd_table) return 0.0;

  std::vector<RangeCondition> ranges;
  extract_range_conditions(condition, ranges);

  if (ranges.empty()) return 0.0;

  // Get total number of IMCUs for the table
  const auto &table_meta = m_rpd_table->meta();
  size_t total_imcus = table_meta.total_imcus.load(std::memory_order_relaxed);

  if (total_imcus == 0) return 0.0;

  // For each range condition, estimate the number of skippable IMCUs
  // Take the maximum across multiple conditions (optimistic estimate)
  size_t max_skippable = 0;

  for (const auto &rc : ranges) {
    // Get column statistics
    if (rc.col_idx >= table_meta.fields.size()) continue;

    const auto &field_meta = table_meta.fields[rc.col_idx];
    const auto *col_stats = field_meta.statistics.get();

    size_t skippable = estimate_skippable_imcus(col_stats, rc, total_imcus);
    max_skippable = std::max(max_skippable, skippable);
  }

  return static_cast<double>(max_skippable) / total_imcus;
}

double ImcuPruningAnalyzer::estimate_row_selectivity(Item *condition) {
  if (!condition || !m_rpd_table) return 1.0;

  std::vector<RangeCondition> ranges;
  extract_range_conditions(condition, ranges);

  if (ranges.empty()) return 1.0;

  const auto &table_meta = m_rpd_table->meta();

  // Multiply selectivities of multiple conditions (assuming independence)
  double selectivity = 1.0;

  for (const auto &rc : ranges) {
    if (rc.col_idx >= table_meta.fields.size()) continue;

    const auto &field_meta = table_meta.fields[rc.col_idx];
    const auto *col_stats = field_meta.statistics.get();

    double sel = estimate_row_selectivity_from_range(col_stats, rc);
    selectivity *= sel;
  }

  return std::max(0.0, std::min(1.0, selectivity));
}

void ImcuPruningAnalyzer::extract_range_conditions(Item *item, std::vector<RangeCondition> &out) {
  if (!item) return;

  if (item->type() == Item::COND_ITEM) {
    auto *cond = static_cast<Item_cond *>(item);

    // AND: recursively extract all subconditions
    if (cond->functype() == Item_func::COND_AND_FUNC) {
      List_iterator<Item> it(*cond->argument_list());
      Item *child;
      while ((child = it++)) {
        extract_range_conditions(child, out);
      }
      return;
    }

    // OR: not handled (conservative estimate)
    if (cond->functype() == Item_func::COND_OR_FUNC) {
      return;
    }
  }

  if (item->type() == Item::FUNC_ITEM) {
    auto *func = static_cast<Item_func *>(item);
    RangeCondition range;

    // BETWEEN
    if (func->functype() == Item_func::BETWEEN) {
      if (extract_between_range(static_cast<Item_func_between *>(func), range)) {
        out.push_back(range);
      }
      return;
    }

    // IN
    if (func->functype() == Item_func::IN_FUNC) {
      if (extract_in_range(static_cast<Item_func_in *>(func), range)) {
        out.push_back(range);
      }
      return;
    }

    // IS NULL / IS NOT NULL
    if (func->functype() == Item_func::ISNULL_FUNC || func->functype() == Item_func::ISNOTNULL_FUNC) {
      if (extract_null_range(func, range)) {
        out.push_back(range);
      }
      return;
    }

    // Simple comparisons (=, !=, <, <=, >, >=)
    if (extract_simple_range(func, range)) {
      out.push_back(range);
    }
  }
}

bool ImcuPruningAnalyzer::extract_simple_range(Item_func *func, RangeCondition &range) {
  if (func->argument_count() != 2) return false;

  Item *left = func->arguments()[0];
  Item *right = func->arguments()[1];

  Item *col_item{nullptr}, *val_item{nullptr};
  bool reversed = false;

  if (left->type() == Item::FIELD_ITEM && right->const_item()) {
    col_item = left;
    val_item = right;
    reversed = false;
  } else if (right->type() == Item::FIELD_ITEM && left->const_item()) {
    col_item = right;
    val_item = left;
    reversed = true;
  } else {
    return false;  // Not in col op const form
  }

  if (!get_column_index(col_item, &range.col_idx, &range.col_type)) {
    return false;
  }

  double value;
  if (!extract_numeric_value(val_item, &value)) {
    std::string str_value;
    if (!extract_string_value(val_item, &str_value)) {
      return false;
    }
    range.is_string_range = true;
    range.str_lower = str_value;
    range.str_upper = str_value;
  }

  Item_func::Functype op = func->functype();
  if (reversed) {
    // Reverse operator (value op col → col op' value)
    switch (op) {
      case Item_func::LT_FUNC:
        op = Item_func::GT_FUNC;
        break;
      case Item_func::LE_FUNC:
        op = Item_func::GE_FUNC;
        break;
      case Item_func::GT_FUNC:
        op = Item_func::LT_FUNC;
        break;
      case Item_func::GE_FUNC:
        op = Item_func::LE_FUNC;
        break;
      default:
        break;
    }
  }

  switch (op) {
    case Item_func::EQ_FUNC:
      // col = value
      range.is_equality = true;
      range.equality_value = value;
      range.lower_bound = value;
      range.upper_bound = value;
      range.lower_inclusive = true;
      range.upper_inclusive = true;
      break;

    case Item_func::NE_FUNC:
      // col != value (does not generate a range, cannot be used for IMCU pruning)
      return false;

    case Item_func::LT_FUNC:
      // col < value
      range.upper_bound = value;
      range.upper_inclusive = false;
      break;

    case Item_func::LE_FUNC:
      // col <= value
      range.upper_bound = value;
      range.upper_inclusive = true;
      break;

    case Item_func::GT_FUNC:
      // col > value
      range.lower_bound = value;
      range.lower_inclusive = false;
      break;

    case Item_func::GE_FUNC:
      // col >= value
      range.lower_bound = value;
      range.lower_inclusive = true;
      break;

    default:
      return false;
  }

  return true;
}

bool ImcuPruningAnalyzer::extract_between_range(Item_func_between *between, RangeCondition &range) {
  if (between->argument_count() != 3) return false;

  Item *col_item = between->arguments()[0];
  Item *min_item = between->arguments()[1];
  Item *max_item = between->arguments()[2];

  // Check column
  if (col_item->type() != Item::FIELD_ITEM) return false;
  if (!get_column_index(col_item, &range.col_idx, &range.col_type)) {
    return false;
  }

  // Check constants
  if (!min_item->const_item() || !max_item->const_item()) {
    return false;
  }

  // Extract boundary values
  double min_val, max_val;
  if (!extract_numeric_value(min_item, &min_val) || !extract_numeric_value(max_item, &max_val)) {
    return false;
  }

  // col BETWEEN min AND max
  range.lower_bound = min_val;
  range.upper_bound = max_val;
  range.lower_inclusive = true;
  range.upper_inclusive = true;

  return true;
}

bool ImcuPruningAnalyzer::extract_in_range(Item_func_in *in_func, RangeCondition &range) {
  if (in_func->argument_count() < 2) return false;

  Item *col_item = in_func->arguments()[0];

  // Check column
  if (col_item->type() != Item::FIELD_ITEM) return false;
  if (!get_column_index(col_item, &range.col_idx, &range.col_type)) {
    return false;
  }

  // Extract all values, find min and max
  double min_val = std::numeric_limits<double>::max();
  double max_val = std::numeric_limits<double>::lowest();
  bool found_any = false;

  for (uint i = 1; i < in_func->argument_count(); ++i) {
    Item *val_item = in_func->arguments()[i];
    if (!val_item->const_item()) continue;

    double value;
    if (extract_numeric_value(val_item, &value)) {
      min_val = std::min(min_val, value);
      max_val = std::max(max_val, value);
      found_any = true;
    }
  }

  if (!found_any) return false;

  // col IN (v1, v2, v3) → col BETWEEN min(v1,v2,v3) AND max(v1,v2,v3)
  range.lower_bound = min_val;
  range.upper_bound = max_val;
  range.lower_inclusive = true;
  range.upper_inclusive = true;

  return true;
}

bool ImcuPruningAnalyzer::extract_null_range(Item_func *func, RangeCondition &range) {
  if (func->argument_count() != 1) return false;

  Item *col_item = func->arguments()[0];
  if (col_item->type() != Item::FIELD_ITEM) return false;

  if (!get_column_index(col_item, &range.col_idx, &range.col_type)) {
    return false;
  }

  // IS NULL / IS NOT NULL use special markers
  // No numeric range generated here, return false
  // Let StorageIndex handle NULL statistics directly
  return false;
}

size_t ImcuPruningAnalyzer::estimate_skippable_imcus(const ColumnStatistics *col_stats, const RangeCondition &rc,
                                                     size_t total_imcus) {
  if (total_imcus == 0) return 0;

  // Per-IMCU zone maps are always more accurate than any table-level estimate:
  // they give an exact count rather than a statistical guess.  Prefer them
  // whenever the table pointer is available (which it always should be when
  // called from estimate_skip_ratio / estimate_row_selectivity).
  if (m_rpd_table) {
    return estimate_skippable_imcus_from_zone_maps(rc, total_imcus);
  }

  // Fall back to table-level global min/max when we have no table handle.
  if (col_stats) {
    const auto &basic = col_stats->get_basic_stats();
    return estimate_skippable_imcus_from_minmax(basic.min_value, basic.max_value, rc, total_imcus);
  }

  // No statistics available at all.
  return 0;
}

size_t ImcuPruningAnalyzer::estimate_skippable_imcus_from_minmax(double global_min, double global_max,
                                                                 const RangeCondition &rc, size_t total_imcus) {
  // Query range
  double query_min = rc.lower_bound;
  double query_max = rc.upper_bound;

  // Global range
  if (global_max <= global_min) return 0;  // Invalid range

  // Check overlap
  if (!has_overlap(query_min, query_max, global_min, global_max)) {
    // No overlap → all IMCUs are skippable
    return total_imcus;
  }

  // Calculate overlap ratio
  double overlap_ratio = calculate_overlap_ratio(query_min, query_max, global_min, global_max);

  // Skip ratio = 1 - overlap ratio
  double skip_ratio = 1.0 - overlap_ratio;

  return static_cast<size_t>(total_imcus * skip_ratio);
}

size_t ImcuPruningAnalyzer::estimate_skippable_imcus_from_zone_maps(const RangeCondition &rc, size_t total_imcus) {
  // Walk every IMCU and count those whose column zone map [min, max] has no
  // overlap with the query range.  This gives an *exact* skippable count,
  // not an estimate, and correctly respects open/closed bound semantics.
  size_t skippable_count = 0;

  m_rpd_table->foreach_imcu([&](Imcu *imcu) {
    const auto *cu = imcu->get_cu(rc.col_idx);
    if (!cu) return;  // Column not stored in this IMCU → cannot skip

    const double imcu_min = cu->get_min_value();
    const double imcu_max = cu->get_max_value();

    // Determine "no overlap" with correct open/closed semantics:
    //
    //  Query lower bound:
    //   inclusive (>=): IMCU is below if imcu_max <  lower_bound
    //   exclusive (>):  IMCU is below if imcu_max <= lower_bound
    //
    //  Query upper bound:
    //   inclusive (<=): IMCU is above if imcu_min >  upper_bound
    //   exclusive (<):  IMCU is above if imcu_min >= upper_bound
    const bool below_range = rc.lower_inclusive ? (imcu_max < rc.lower_bound) : (imcu_max <= rc.lower_bound);
    const bool above_range = rc.upper_inclusive ? (imcu_min > rc.upper_bound) : (imcu_min >= rc.upper_bound);

    if (below_range || above_range) {
      ++skippable_count;
    }
  });

  return skippable_count;
}

double ImcuPruningAnalyzer::estimate_row_selectivity_from_range(const ColumnStatistics *col_stats,
                                                                const RangeCondition &rc) {
  if (!col_stats) return 0.1;  // No statistics, default to 10%.

  return rc.is_equality ? col_stats->estimate_equality_selectivity(rc.equality_value)
                        : col_stats->estimate_range_selectivity(rc.lower_bound, rc.upper_bound);
}

bool ImcuPruningAnalyzer::extract_numeric_value(Item *item, double *value) {
  if (!item || !value) return false;

  if (item->is_null()) return false;

  switch (item->result_type()) {
    case INT_RESULT: {
      longlong int_val = item->val_int();
      *value = static_cast<double>(int_val);
      return true;
    }
    case REAL_RESULT:
    case DECIMAL_RESULT: {
      *value = item->val_real();
      return true;
    }
    case STRING_RESULT: {
      // Try to convert string to number
      String str_val;
      String *str = item->val_str(&str_val);
      if (str) {
        char *end;
        *value = std::strtod(str->c_ptr(), &end);
        return (end != str->c_ptr());  // Conversion successful
      }
      return false;
    }
    default:
      return false;
  }
}

bool ImcuPruningAnalyzer::extract_string_value(Item *item, std::string *value) {
  if (!item || !value) return false;

  if (item->is_null()) return false;

  if (item->result_type() == STRING_RESULT) {
    String str_val;
    String *str = item->val_str(&str_val);
    if (str) {
      value->assign(str->ptr(), str->length());
      return true;
    }
  }

  return false;
}

bool ImcuPruningAnalyzer::get_column_index(Item *item, uint32_t *col_idx, enum_field_types *col_type) {
  if (!item || item->type() != Item::FIELD_ITEM) {
    return false;
  }

  auto *field_item = static_cast<Item_field *>(item);
  Field *field = field_item->field;

  if (!field) return false;

  *col_idx = field->field_index();
  if (col_type) {
    *col_type = field->type();
  }

  return true;
}

bool ImcuPruningAnalyzer::has_overlap(double min1, double max1, double min2, double max2) {
  // Check if [min1, max1] and [min2, max2] overlap
  return !(max1 < min2 || max2 < min1);
}

double ImcuPruningAnalyzer::calculate_overlap_ratio(double min1, double max1, double min2, double max2) {
  if (!has_overlap(min1, max1, min2, max2)) {
    return 0.0;
  }

  // Overlap interval
  double overlap_min = std::max(min1, min2);
  double overlap_max = std::min(max1, max2);
  double overlap_range = overlap_max - overlap_min;

  // Query range
  double query_range = max1 - min1;
  if (query_range <= 0) return 0.0;

  return overlap_range / query_range;
}
}  // namespace Imcs
}  // namespace ShannonBase