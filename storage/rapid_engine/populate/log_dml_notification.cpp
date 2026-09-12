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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   Copyright (c) 2023, 2024, 2025, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.
*/
#include <algorithm>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "template_utils.h"  // down_cast

#include "current_thd.h"
#include "log0log.h"          // log_sys, log_get_lsn
#include "sql/replication.h"  // Trans_param, TRANS_IS_REAL_TRANS

#include "storage/innobase/handler/ha_innodb.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/populate/log_dml_notification.h"
#include "storage/rapid_engine/populate/log_populate.h"

namespace ShannonBase {
// Defined in ha_shannon_rapid.cc; the registered participant of the captured
// changes has to be the Rapid handlerton itself.
extern handlerton *shannon_rapid_hton_ptr;

namespace Populate {
namespace DML {
namespace {
/**
 * @brief Resolve the Rapid table a change record has to be applied to.
 *
 * A change record carries the id of the logical table. For a partitioned table
 * that id only reaches the parent PartTable, which owns no rows of its own:
 * every row lives in the per-partition sub-table named by the routing key the
 * capture side resolved (ha_shannon_rapid.cc: resolve_change_partitions).
 *
 * @param table_id  logical table id from the change record.
 * @param part_key  partition routing key, empty for a non-partitioned table.
 * @param[out] droppable  when nullptr is returned, whether the record simply
 *                        has no Rapid target any more (the table was unloaded,
 *                        or the partition was never loaded) and can be retired
 *                        rather than reported as a propagation failure.
 * @return the target table, or nullptr.
 */
ShannonBase::Imcs::RpdTable *resolve_change_target(const table_id_t &table_id, const std::string &part_key,
                                                   bool *droppable) {
  auto *imcs = ShannonBase::Imcs::Imcs::instance();
  *droppable = false;

  if (auto *rpd_table = imcs->get_rpd_table(table_id)) return rpd_table;

  auto *part_table = imcs->get_rpd_parttable(table_id);
  if (part_table == nullptr) {
    // Table may have been unloaded between when the record was enqueued and
    // now — drop the record gracefully.
    *droppable = !ShannonBase::Populate::pop_buff_contains(table_id);
    return nullptr;
  }

  // Partitioned table whose record carries no routing key: it cannot be
  // applied to the parent, which holds no rows. Report it rather than losing
  // the change silently.
  if (part_key.empty()) return nullptr;

  // A partition that was never loaded holds none of this table's Rapid rows;
  // the scan side skips it the same way (ha_rapidpart::rnd_init_in_part).
  auto *partition = down_cast<ShannonBase::Imcs::PartTable *>(part_table)->get_partition(part_key);
  *droppable = (partition == nullptr);
  return partition;
}

/**
 * @brief Apply an UPDATE that moves a row from one partition to another.
 *
 * Rapid partitions are independent sub-tables with their own row ids and
 * indexes, so a partitioning-column update is a delete from the old partition
 * plus an insert into the new one — the same thing the primary engine does.
 * insert_row()/delete_row() maintain the indexes of their own partition, so no
 * separate ART bookkeeping is needed here.
 *
 * @return the consumed record size, or 0 on failure.
 */
int apply_cross_partition_update(Rapid_load_context *context, const table_id_t &table_id, const byte *old_start,
                                 const byte *old_end_ptr, const byte *new_start,
                                 ShannonBase::Imcs::RpdTable *old_table) {
  const size_t row_size = old_end_ptr - old_start;

  auto global_row_id = old_table->locate_row(context, (uchar *)old_start);
  if (global_row_id == INVALID_ROW_ID) {
    sql_print_warning("Rapid COPY_INFO UPDATE cannot locate source row by PRIMARY key for table %llu",
                      static_cast<unsigned long long>(table_id));
    return 0;
  }

  if (old_table->delete_row(context, global_row_id)) {
    std::ostringstream oss;
    oss << "[popragate] cross-partition update (delete side) in rapid " << context->m_schema_name.c_str() << "."
        << context->m_table_name.c_str() << " failed";
    my_error(ER_SECONDARY_ENGINE, MYF(0), oss.str().c_str());
    return 0;
  }

  bool droppable{false};
  auto *new_table = resolve_change_target(table_id, context->m_extra_info.m_part_key, &droppable);
  if (new_table == nullptr) {
    // The destination partition holds no Rapid rows; removing the pre-image
    // was the whole of this change as far as Rapid is concerned.
    if (droppable) return row_size;
    std::ostringstream oss;
    oss << "Cannot get the table " << context->m_schema_name << "." << context->m_table_name << " from loaded tables";
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), oss.str().c_str());
    return 0;
  }

  if (!new_table->insert_row(context, (uchar *)new_start).ok()) {
    std::ostringstream oss;
    oss << "[popragate] cross-partition update (insert side) in rapid " << context->m_schema_name.c_str() << "."
        << context->m_table_name.c_str() << " failed";
    my_error(ER_SECONDARY_ENGINE, MYF(0), oss.str().c_str());
    return 0;
  }

  return row_size;
}
}  // namespace

uint CopyInfoParser::parse_copy_info(Rapid_load_context *context, table_id_t &table_id,
                                     change_record_buff_t::OperType oper_type, byte *start, byte *end_ptr,
                                     byte *new_start, byte *new_end_ptr) {
  // Dispatch by operation type
  auto ret{ShannonBase::SHANNON_SUCCESS};
  switch (oper_type) {
    case change_record_buff_t::OperType::UPDATE:
      ret = parse_and_apply_update(context, table_id, start, end_ptr, new_start, new_end_ptr);
      break;
    case change_record_buff_t::OperType::INSERT:
      ret = parse_and_apply_insert(context, table_id, start, end_ptr);
      break;
    case change_record_buff_t::OperType::DELETE:
      ret = parse_and_apply_delete(context, table_id, start, end_ptr);
      break;
    default:
      sql_print_warning("Unknown operation type in change record");
      assert(false);
      break;
  }
  return ret;
}

int CopyInfoParser::parse_and_apply_update(Rapid_load_context *context, table_id_t &table_id, const byte *old_start,
                                           const byte *old_end_ptr, const byte *new_start, const byte *new_end_ptr) {
  bool droppable{false};
  auto rpd_table = resolve_change_target(table_id, context->m_extra_info.m_old_part_key, &droppable);
  if (!rpd_table) {
    if (droppable) return old_end_ptr - old_start;
    std::ostringstream oss;
    oss << "Cannot get the table " << context->m_schema_name << "." << context->m_table_name << " from loaded tables";
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), oss.str().c_str());
    return 0;
  }

  // An UPDATE that changes a partitioning column moves the row to another
  // partition. The two sub-tables have independent row ids and indexes, so
  // that is a delete from the old partition plus an insert into the new one,
  // exactly as the primary engine does it.
  if (context->m_extra_info.m_part_key != context->m_extra_info.m_old_part_key) {
    return apply_cross_partition_update(context, table_id, old_start, old_end_ptr, new_start, rpd_table);
  }

  auto global_row_id = rpd_table->locate_row(context, (uchar *)old_start);
  if (global_row_id == INVALID_ROW_ID) {
    sql_print_warning("Rapid COPY_INFO UPDATE cannot locate source row by PRIMARY key for table %llu",
                      static_cast<unsigned long long>(table_id));
    return 0;
  }

  // step 1: to parse the changed fields. <changed col id, new_value>
  auto n_cols = rpd_table->meta().num_columns;
  ShannonBase::Imcs::RowBuffer new_row_data(n_cols);
  // new_start is record[0]/m_buff1; its off-page BLOB/JSON/VECTOR data was
  // captured into m_offpage_data1, not m_offpage_data0 (old row).
  new_row_data.copy_from_mysql_fields(context, const_cast<uchar *>(new_start), rpd_table->meta().fields,
                                      rpd_table->meta().col_offsets.data(), rpd_table->meta().null_byte_offsets.data(),
                                      rpd_table->meta().null_bitmasks.data(), /*use_offpage_data1=*/true);

  size_t row_size = old_end_ptr - old_start;
  std::unordered_map<uint32_t, ShannonBase::Imcs::RowBuffer::ColumnValue> updates;
  for (size_t idx = 0; idx < n_cols; idx++) {
    Field *field = rpd_table->meta().fields[idx].source_fld;

    ptrdiff_t offset = rpd_table->meta().col_offsets[idx];

    bool null_changed{false};
    if (field && field->is_nullable()) {
      ulong byte_off = rpd_table->meta().null_byte_offsets[idx];
      ulong bitmask = rpd_table->meta().null_bitmasks[idx];
      bool old_null = (old_start[byte_off] & bitmask) != 0;
      bool new_null = (new_start[byte_off] & bitmask) != 0;
      null_changed = (old_null != new_null);
    }

    bool identical = false;
    if (!null_changed && field != nullptr) {
      const auto ftype = field->type();
      // Every out-of-line type (BLOB family, GEOMETRY, JSON, VECTOR) stores only a pointer to blob-heap data in the row
      // image.
      if (ftype != MYSQL_TYPE_BLOB && ftype != MYSQL_TYPE_TINY_BLOB && ftype != MYSQL_TYPE_MEDIUM_BLOB &&
          ftype != MYSQL_TYPE_LONG_BLOB && ftype != MYSQL_TYPE_GEOMETRY && ftype != MYSQL_TYPE_JSON &&
          ftype != MYSQL_TYPE_VECTOR) {
        identical =
            field->cmp_binary(const_cast<uchar *>(old_start + offset), const_cast<uchar *>(new_start + offset)) == 0;
      }
    }

    if (!identical) {
      auto col_val = new_row_data.get_column_mutable(idx);
      updates.emplace(idx, std::move(*col_val));
    }
  }

  // step 1b: swap ART entries for any secondary index whose key actually changes.
  for (const auto &key : rpd_table->meta().keys) {
    bool key_touched = false;
    for (const auto &part : key.key_parts) {
      if (updates.count(part.key_field_ind)) {
        key_touched = true;
        break;
      }
    }
    if (!key_touched) continue;

    const auto *index_desc = rpd_table->get_art_index_descriptor(key.key_name);
    auto *index = rpd_table->get_index(key.key_name);
    if (!index_desc || !index) continue;

    ShannonBase::Imcs::Index::RapidKeyCodec::KeyBuffer old_key, new_key;
    const bool old_ok = ShannonBase::Imcs::Index::RapidKeyCodec::EncodeRowKey(
        *index_desc, old_start, rpd_table->meta().col_offsets.data(), rpd_table->meta().null_byte_offsets.data(),
        rpd_table->meta().null_bitmasks.data(), &old_key);
    const bool new_ok = ShannonBase::Imcs::Index::RapidKeyCodec::EncodeRowKey(
        *index_desc, new_start, rpd_table->meta().col_offsets.data(), rpd_table->meta().null_byte_offsets.data(),
        rpd_table->meta().null_bitmasks.data(), &new_key);
    if (!old_ok || !new_ok) {
      std::ostringstream oss;
      oss << "[popragate] update (index key encode) in rapid " << context->m_schema_name.c_str() << "."
          << context->m_table_name.c_str() << " failed";
      my_error(ER_SECONDARY_ENGINE, MYF(0), oss.str().c_str());
      return 0;
    }
    if (old_key == new_key) continue;  // byte-identical key; nothing to swap

    index->remove(old_key.data(), old_key.size(), &global_row_id, sizeof(global_row_id));
    index->insert(new_key.data(), new_key.size(), &global_row_id, sizeof(global_row_id));
  }

  // step 2: update row.
  if (rpd_table->update_row(context, global_row_id, updates)) {
    std::ostringstream oss;
    oss << "[popragate] update in rapid " << context->m_schema_name.c_str() << "." << context->m_table_name.c_str()
        << " failed";
    my_error(ER_SECONDARY_ENGINE, MYF(0), oss.str().c_str());
    return 0;
  }
  return row_size;
}

int CopyInfoParser::parse_and_apply_insert(Rapid_load_context *context, table_id_t &table_id, const byte *start,
                                           const byte *end_ptr) {
  bool droppable{false};
  auto rpd_table = resolve_change_target(table_id, context->m_extra_info.m_part_key, &droppable);
  if (!rpd_table) {
    if (droppable) return end_ptr - start;
    std::ostringstream oss;
    oss << "Cannot get the table " << context->m_schema_name << "." << context->m_table_name << " from loaded tables";
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), oss.str().c_str());
    return 0;
  }

  size_t row_size = end_ptr - start;
  if (!rpd_table->insert_row(context, (uchar *)start).ok()) {
    std::ostringstream oss;
    oss << "[popragate] inset into rapid " << context->m_schema_name.c_str() << "." << context->m_table_name.c_str()
        << " to imcs failed";
    my_error(ER_SECONDARY_ENGINE, MYF(0), oss.str().c_str());
    return 0;
  }

  return row_size;
}

int CopyInfoParser::parse_and_apply_delete(Rapid_load_context *context, table_id_t &table_id, const byte *start,
                                           const byte *end_ptr) {
  size_t row_size = end_ptr - start;
  bool droppable{false};
  auto rpd_table = resolve_change_target(table_id, context->m_extra_info.m_old_part_key, &droppable);
  if (!rpd_table) {
    // Table (or partition) may have been unloaded between when the record was
    // enqueued and now — drop the record gracefully.
    if (droppable) return row_size;
    std::ostringstream oss;
    oss << "Cannot get the table " << context->m_schema_name << "." << context->m_table_name << " from loaded tables";
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), oss.str().c_str());
    return 0;
  }

  auto global_row_id = rpd_table->locate_row(context, (uchar *)start);
  if (global_row_id == INVALID_ROW_ID) {
    sql_print_warning("Rapid COPY_INFO DELETE cannot locate source row by PRIMARY key for table %llu",
                      static_cast<unsigned long long>(table_id));
    return 0;
  }

  if (rpd_table->delete_row(context, global_row_id)) {
    std::ostringstream oss;
    oss << "[popragate] delete from rapid " << context->m_schema_name.c_str() << "." << context->m_table_name.c_str()
        << " to imcs failed.";
    my_error(ER_SECONDARY_ENGINE, MYF(0), oss.str().c_str());
    return 0;
  }
  return row_size;
}

bool CopyInfoParser::validate_record(const change_record_buff_t &record) { return record.m_source_trx_id != 0; }

ChangeApplyResult CopyInfoParser::apply_change(Rapid_load_context &context, change_record_buff_t &record) {
  ChangeApplyResult result;
  result.stale_reason = stale_reason_t::UNIDENTIFIED_ERROR;

  // Direct row-image propagation must preserve the real primary InnoDB writer
  // identity. commit_scn == 0 is valid: it represents an ACTIVE Rapid MVCC
  // version, not a missing transaction.
  if (record.m_source_trx_id == 0) {
    result.status = ChangeApplyResult::Status::PERMANENT;
    return result;
  }

  const Transaction::ID source_txn_id = static_cast<Transaction::ID>(record.m_source_trx_id);
  uint64_t terminal_scn = 0;
  const auto outcome = TransactionManager::instance().get_outcome(source_txn_id, &terminal_scn);

  if (outcome == TransactionManager::Outcome::ABORTED) {
    // The primary rollback won the race before this record reached Rapid. Do not
    // manufacture an ABORTED physical version only to undo it again.
    result.status = ChangeApplyResult::Status::APPLIED;
    result.parsed_bytes = record.m_size;
    TransactionManager::instance().on_change_applied(source_txn_id, record.m_table_id);
    return result;
  }

  context.m_trx = nullptr;
  context.m_extra_info.m_trxid = source_txn_id;
  // A commit can also win the race before this record is applied. In that case
  // create the version as COMMITTED directly; otherwise 0 deliberately means
  // ACTIVE until the primary outcome arrives.
  context.m_extra_info.m_scn = (outcome == TransactionManager::Outcome::COMMITTED) ? terminal_scn : record.m_commit_scn;
#ifndef NDEBUG
  context.m_schema_name = record.m_schema_name;
  context.m_table_name = record.m_table_name;
  context.m_sch_tb_name = context.m_schema_name + "." + context.m_table_name;
#endif
  context.m_offpage_data0 = record.m_offpage_data0.empty() ? nullptr : &record.m_offpage_data0;
  context.m_offpage_data1 = record.m_offpage_data1.empty() ? nullptr : &record.m_offpage_data1;
  // Physical partition routing resolved by the capture side; empty for a
  // non-partitioned table.
  context.m_extra_info.m_part_key = record.m_part_key;
  context.m_extra_info.m_old_part_key = record.m_old_part_key;

  const byte *old_start = record.m_buff0.get();
  const byte *old_end = old_start + record.m_size;
  const byte *new_start = record.m_buff1.get();
  const byte *new_end = new_start + record.m_size;
  result.parsed_bytes =
      parse_copy_info(&context, record.m_table_id, record.m_oper, const_cast<byte *>(old_start),
                      const_cast<byte *>(old_end), const_cast<byte *>(new_start), const_cast<byte *>(new_end));

  if (result.parsed_bytes == record.m_size) {
    result.status = ChangeApplyResult::Status::APPLIED;
    // The primary COMMIT/ROLLBACK callback may have raced ahead of this
    // asynchronous apply. Finalize this source transaction (if terminal) before
    // the worker drops inflight_size; once inflight reaches zero the table is
    // eligible for Rapid offload again.
    TransactionManager::instance().on_change_applied(source_txn_id, record.m_table_id);
  } else {
    result.status = ChangeApplyResult::Status::RETRYABLE;
  }
  return result;
}
}  // namespace DML

namespace {
/// Mirrors ha_shannon_rapid.cc:rpd_thd_trx_is_auto_commit(). Deliberately not
/// InnoDB's thd_trx_is_auto_commit(): that one also demands thd_is_query_block(),
/// which is false for the DML statements this path captures. Kept local so that
/// the populate layer does not have to reach into the handler for a one line
/// predicate about primary transaction ownership.
bool statement_owns_transaction(THD *thd) {
  return (thd != nullptr && !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN));
}
}  // namespace

void RegisterCopyInfoParticipant(THD *thd) {
  if (thd == nullptr) return;

  trans_register_ha(thd, false, shannon_rapid_hton_ptr, nullptr);
  if (!statement_owns_transaction(thd)) trans_register_ha(thd, true, shannon_rapid_hton_ptr, nullptr);
}

bool EnqueueCopyInfo(THD *thd, change_record_buff_t &&record) {
  if (thd == nullptr) return false;

  if (ShannonBase::Transaction::get_or_create_trx(thd) == nullptr) return false;

  auto registration = ShannonBase::Populate::TransactionManager::instance().register_change(thd, record.m_table_id);
  if (!registration) return false;

  record.m_source_trx_id = registration.source_trx_id;
  record.m_commit_scn = 0;

  const uint64_t capture_lsn = log_get_lsn(*log_sys);
  ShannonBase::Populate::Populator::write(nullptr, capture_lsn, &record);
  return true;
}

void TransactionManager::ensure_subscribed() {
  if (m_subscribed.load(std::memory_order_acquire)) return;

  std::lock_guard<std::mutex> lock(m_subscription_mutex);
  if (m_subscribed.load(std::memory_order_relaxed)) return;

  Transaction::subscribe(this);
  m_subscribed.store(true, std::memory_order_release);
}

TransactionManager::Registration TransactionManager::register_change(THD *thd, table_id_t table_id) {
  ensure_subscribed();
  if (thd == nullptr || table_id == 0) return {};

  trx_t *source_trx = thd_to_trx(thd);
  if (source_trx == nullptr || source_trx->id == 0) return {};

  const Transaction::ID current_id = static_cast<Transaction::ID>(source_trx->id);

  std::lock_guard<std::mutex> lock(m_mutex);
  auto &participant = m_participants[thd];
  if (participant.fail_closed) return {};

  if (participant.source_trx_id == 0) {
    participant.source_trx_id = current_id;
  } else if (participant.source_trx_id != current_id) {
    // A new InnoDB writer id while old COPY_INFO participation is still
    // retained means a terminal lifecycle callback was missed. Never mix two
    // source transactions in the same THD participant.
    if (!participant.touched_tables.empty()) {
      ib::error() << "Rapid: source trx id changed while COPY_INFO propagation state is still active "
                  << "(captured=" << participant.source_trx_id << ", current=" << current_id << ")";
      return {};
    }
    participant = Participant{};
    participant.source_trx_id = current_id;
  }

  participant.touched_tables.insert(table_id);
  participant.statement_has_changes = true;
  ++m_transactions[current_id].tables[table_id].registered;
  return Registration{current_id};
}

void TransactionManager::quarantine_participant(THD *thd, bool require_statement_change, const char *reason) {
  if (thd == nullptr) return;

  std::vector<table_id_t> tables;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_participants.find(thd);
    if (it == m_participants.end()) return;

    Participant &participant = it->second;
    if (require_statement_change && !participant.statement_has_changes) return;
    if (participant.touched_tables.empty()) {
      participant.statement_has_changes = false;
      return;
    }

    participant.fail_closed = true;
    participant.statement_has_changes = false;
    tables.assign(participant.touched_tables.begin(), participant.touched_tables.end());
  }

  std::sort(tables.begin(), tables.end());
  QuarantinePropagationTables(tables);
  sql_print_warning(
      "Rapid immediate COPY_INFO quarantined %zu table(s): %s. "
      "Transaction-level COMMIT/ROLLBACK is supported; partial statement/savepoint undo requires per-operation "
      "Rapid undo and the affected tables must be reloaded before offload",
      tables.size(), reason != nullptr ? reason : "partial rollback after DML was already propagated");
}

void TransactionManager::quarantine_partial_rollback(THD *thd, const char *reason) {
  quarantine_participant(thd, false, reason);
}

void TransactionManager::on_statement_commit(THD *thd) {
  if (thd == nullptr) return;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_participants.find(thd);
  if (it != m_participants.end()) it->second.statement_has_changes = false;
}

void TransactionManager::on_statement_rollback(THD *thd) {
  quarantine_participant(thd, true, "statement rollback after DML was already propagated");
}

void TransactionManager::on_transaction_commit(THD *thd) {
  if (thd == nullptr) return;

  Transaction::ID source_trx_id = 0;
  bool has_changes = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_participants.find(thd);
    if (it == m_participants.end()) return;

    source_trx_id = it->second.source_trx_id;
    has_changes = !it->second.touched_tables.empty();
    m_participants.erase(it);
  }

  if (source_trx_id == 0 || !has_changes) return;

  // Rapid SCN is physical publication/retention metadata. SQL creator
  // visibility remains exclusively the InnoDB ReadView.
  const uint64_t commit_scn = TransactionCoordinator::instance().allocate_scn();
  publish_commit(source_trx_id, commit_scn);
}

void TransactionManager::on_transaction_rollback(THD *thd) {
  if (thd == nullptr) return;

  Transaction::ID source_trx_id = 0;
  bool has_changes = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_participants.find(thd);
    if (it == m_participants.end()) return;

    source_trx_id = it->second.source_trx_id;
    has_changes = !it->second.touched_tables.empty();
    m_participants.erase(it);
  }

  if (source_trx_id != 0 && has_changes) publish_rollback(source_trx_id);
}

void TransactionManager::on_transaction_detach(THD *thd) {
  // Defensive teardown: unresolved COPY_INFO participation must not survive a
  // THD facade. Normal COMMIT/ROLLBACK has already erased the participant, so
  // this is idempotent.
  on_transaction_rollback(thd);
}

void TransactionManager::finalize_table(Transaction::ID txn_id, table_id_t table_id, Outcome outcome,
                                        uint64_t commit_scn) {
  auto *imcs = ShannonBase::Imcs::Imcs::instance();
  if (imcs == nullptr) return;

  // Both maps: a partitioned table's propagated versions live in its partition
  // sub-tables, and PartTable::get_imcus() aggregates them. Without this the
  // rows a partitioned table propagates would stay ACTIVE forever and no
  // ReadView would ever see them. The shared_ptr also keeps the table alive
  // across a concurrent unload, which this commit-time callback cannot hold a
  // lock against.
  auto rpd_table = imcs->get_rpd_table_shared(table_id);
  if (!rpd_table) return;

  for (auto &imcu : rpd_table->get_imcus()) {
    if (!imcu) continue;
    if (outcome == Outcome::COMMITTED) {
      imcu->commit_transaction(txn_id, commit_scn);
    } else if (outcome == Outcome::ABORTED) {
      if (!imcu->rollback_transaction(txn_id)) {
        ib::error() << "Rapid: failed to rollback propagated source transaction " << txn_id << " on table " << table_id;
      }
    }
    TransactionCoordinator::instance().invalidate_visibility_cache(imcu.get());
  }
}

void TransactionManager::erase_if_complete_locked(Transaction::ID txn_id) {
  auto it = m_transactions.find(txn_id);
  if (it == m_transactions.end() || it->second.outcome == Outcome::ACTIVE) return;

  for (const auto &[table_id, progress] : it->second.tables) {
    (void)table_id;
    if (progress.applied < progress.registered) return;
  }
  m_transactions.erase(it);
}

TransactionManager::Outcome TransactionManager::get_outcome(Transaction::ID txn_id, uint64_t *commit_scn) {
  if (commit_scn != nullptr) *commit_scn = 0;
  if (txn_id == 0) return Outcome::ACTIVE;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_transactions.find(txn_id);
  if (it == m_transactions.end()) return Outcome::ACTIVE;
  if (commit_scn != nullptr) *commit_scn = it->second.commit_scn;
  return it->second.outcome;
}

void TransactionManager::on_change_applied(Transaction::ID txn_id, table_id_t table_id) {
  if (txn_id == 0 || table_id == 0) return;

  Outcome outcome = Outcome::ACTIVE;
  uint64_t commit_scn = 0;
  bool table_complete = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto &txn = m_transactions[txn_id];
    auto &table = txn.tables[table_id];
    ++table.applied;
    table_complete = table.applied >= table.registered;
    outcome = txn.outcome;
    commit_scn = txn.commit_scn;
  }

  // This runs before Populator decrements inflight_size. If the transaction
  // outcome raced ahead of async apply, finalize the last registered record
  // before the table becomes eligible for offload again.
  if (outcome != Outcome::ACTIVE && table_complete) {
    finalize_table(txn_id, table_id, outcome, commit_scn);
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  erase_if_complete_locked(txn_id);
}

void TransactionManager::publish_commit(Transaction::ID txn_id, uint64_t commit_scn) {
  if (txn_id == 0 || commit_scn == 0) return;

  std::vector<table_id_t> tables;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_transactions.find(txn_id);
    if (it == m_transactions.end()) return;

    it->second.outcome = Outcome::COMMITTED;
    it->second.commit_scn = commit_scn;
    tables.reserve(it->second.tables.size());
    for (const auto &[table_id, ignored] : it->second.tables) {
      (void)ignored;
      tables.push_back(table_id);
    }
  }

  TransactionCoordinator::instance().observe_commit_scn(commit_scn);
  for (table_id_t table_id : tables) finalize_table(txn_id, table_id, Outcome::COMMITTED, commit_scn);

  std::lock_guard<std::mutex> lock(m_mutex);
  erase_if_complete_locked(txn_id);
}

void TransactionManager::publish_rollback(Transaction::ID txn_id) {
  if (txn_id == 0) return;

  std::vector<table_id_t> tables;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_transactions.find(txn_id);
    if (it == m_transactions.end()) return;

    it->second.outcome = Outcome::ABORTED;
    it->second.commit_scn = 0;
    tables.reserve(it->second.tables.size());
    for (const auto &[table_id, ignored] : it->second.tables) {
      (void)ignored;
      tables.push_back(table_id);
    }
  }

  for (table_id_t table_id : tables) finalize_table(txn_id, table_id, Outcome::ABORTED, 0);

  std::lock_guard<std::mutex> lock(m_mutex);
  erase_if_complete_locked(txn_id);
}

void TransactionManager::forget_table(table_id_t table_id) {
  if (table_id == 0) return;

  std::lock_guard<std::mutex> lock(m_mutex);

  for (auto it = m_participants.begin(); it != m_participants.end();) {
    it->second.touched_tables.erase(table_id);
    if (it->second.touched_tables.empty())
      it = m_participants.erase(it);
    else
      ++it;
  }

  for (auto it = m_transactions.begin(); it != m_transactions.end();) {
    it->second.tables.erase(table_id);
    if (it->second.tables.empty())
      it = m_transactions.erase(it);
    else
      ++it;
  }
}

void TransactionManager::clear() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_participants.clear();
  m_transactions.clear();
}

void TransactionManager::start() { ensure_subscribed(); }

void TransactionManager::shutdown() {
  {
    std::lock_guard<std::mutex> lock(m_subscription_mutex);
    if (m_subscribed.exchange(false, std::memory_order_acq_rel)) {
      Transaction::unsubscribe(this);
    }
  }
  clear();
}

namespace DML {
void rapid_after_commit(void *arg) {
  const auto *param = static_cast<const Trans_param *>(arg);
  if (param == nullptr || (param->flags & TRANS_IS_REAL_TRANS) == 0) return;

  THD *thd = current_thd;
  auto *trx = thd ? ShannonBase::Transaction::find_trx(thd) : nullptr;
  if (trx != nullptr) trx->commit();
}

void rapid_before_rollback(void *arg) {
  const auto *param = static_cast<const Trans_param *>(arg);
  if (param == nullptr || (param->flags & TRANS_IS_REAL_TRANS) == 0) return;

  THD *thd = current_thd;
  auto *trx = thd ? ShannonBase::Transaction::find_trx(thd) : nullptr;
  if (trx != nullptr) trx->rollback();
}
}  // namespace DML
}  // namespace Populate
}  // namespace ShannonBase
