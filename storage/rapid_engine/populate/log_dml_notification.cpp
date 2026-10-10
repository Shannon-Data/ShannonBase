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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "template_utils.h"  // down_cast

#include "current_thd.h"
#include "include/scope_guard.h"
#include "log0log.h"         // log_sys, log_get_lsn
#include "sql/debug_sync.h"  // DBUG_SIGNAL_WAIT_FOR
#include "sql/mysqld.h"
#include "sql/replication.h"  // Trans_param, TRANS_IS_REAL_TRANS
#include "sql/sql_class.h"
#include "sql/xa.h"
#include "storage/innobase/include/srv0srv.h"
#include "storage/rapid_engine/recovery/table_persistence.h"

#include "storage/innobase/handler/ha_innodb.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_config.h"  // shannon_rpd_engine_cfg
#include "storage/rapid_engine/populate/log_dml_notification.h"
#include "storage/rapid_engine/populate/log_populate.h"
#include "storage/rapid_engine/utils/utils.h"

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
 * @param change_id process-local id of the change being applied. A partition
 *                  whose load watermark is at or above it was filled by a scan
 *                  that already read this change out of the primary engine, so
 *                  applying it again would duplicate the row (or fail to find
 *                  one the load already removed).
 * @param[out] droppable  when nullptr is returned, whether the record simply
 *                        has no Rapid target any more (the table was unloaded,
 *                        the partition was never loaded, or its load already
 *                        folded this change in) and can be retired rather than
 *                        reported as a propagation failure.
 * @return the target table, or nullptr.
 */
ShannonBase::Imcs::RpdTable *resolve_change_target(const table_id_t &table_id, const std::string &part_key,
                                                   uint64_t change_id, bool *droppable,
                                                   std::shared_ptr<ShannonBase::Imcs::RpdTable> *lifetime_guard) {
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
  *lifetime_guard = down_cast<ShannonBase::Imcs::PartTable *>(part_table)->get_partition(part_key);
  if (!*lifetime_guard) {
    *droppable = true;
    return nullptr;
  }

  if (change_id != 0 && change_id <= (*lifetime_guard)->load_watermark()) {
    // Already in this partition's rows. The load's own scan read it from the
    // primary engine, and the record is only reaching us now because the
    // propagation queue was behind when the load started.
    lifetime_guard->reset();
    *droppable = true;
    return nullptr;
  }

  return lifetime_guard->get();
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

  bool droppable{false};
  std::shared_ptr<ShannonBase::Imcs::RpdTable> new_table_guard;
  auto *new_table = resolve_change_target(table_id, context->m_extra_info.m_part_key, context->m_extra_info.m_change_id,
                                          &droppable, &new_table_guard);
  if (new_table == nullptr) {
    // The destination partition holds no Rapid rows; removing the pre-image
    // was the whole of this change as far as Rapid is concerned.
    if (droppable) {
      if (old_table->delete_row(context, global_row_id)) return 0;
      return row_size;
    }
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

  // Install the destination image before removing the source. If insertion
  // fails, the old partition is untouched. If source deletion fails, undo the
  // destination insert so the propagation retry sees its original pre-image.
  if (old_table->delete_row(context, global_row_id)) {
    auto inserted_row = new_table->locate_row(context, (uchar *)new_start);
    const bool rollback_ok = inserted_row != INVALID_ROW_ID && !new_table->delete_row(context, inserted_row);
    if (!rollback_ok) {
      sql_print_error("Rapid COPY_INFO cross-partition UPDATE rollback failed for table %llu",
                      static_cast<unsigned long long>(table_id));
    }
    std::ostringstream oss;
    oss << "[popragate] cross-partition update (delete side) in rapid " << context->m_schema_name.c_str() << "."
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

namespace {
/**
 * True when every non-NULL out-of-line column of @a rowdata has its bytes in
 * the record's pre-image capture.
 *
 * The UPDATE apply path encodes keys straight out of the pre-image --
 * locate_row() for the PRIMARY key, and the removal half of every index key
 * swap. Those go through Field::get_key_image(), which for a BLOB reads the
 * pointer stored in the row image. In a detached record that pointer belongs
 * to a blob heap that died with the capturing statement, so if the bytes were
 * not captured there is nothing safe to read. Fail the record instead of
 * dereferencing it: the table is quarantined and a reload fixes it, which is
 * enormously preferable to a key built from freed memory.
 */
bool PreImageOffPageDataIsComplete(const ShannonBase::Imcs::RpdTable *rpd_table, const Rapid_load_context *context,
                                   const byte *rowdata) {
  const auto &meta = const_cast<ShannonBase::Imcs::RpdTable *>(rpd_table)->meta();
  for (size_t idx = 0; idx < meta.fields.size(); ++idx) {
    Field *field = meta.fields[idx].source_fld;
    // A NOT_SECONDARY column is never captured, so it is never missing.
    if (field == nullptr || !meta.fields[idx].is_secondary_field) continue;
    if (!ShannonBase::Utils::IsOffPageField(field)) continue;

    // A NULL column has no out-of-line bytes to capture.
    if (field->is_nullable() && (rowdata[meta.null_byte_offsets[idx]] & meta.null_bitmasks[idx]) != 0) continue;

    if (context->m_offpage_data0 == nullptr) return false;
    if (context->m_offpage_data0->find(idx) == context->m_offpage_data0->end()) return false;
  }
  return true;
}
}  // namespace

int CopyInfoParser::parse_and_apply_update(Rapid_load_context *context, table_id_t &table_id, const byte *old_start,
                                           const byte *old_end_ptr, const byte *new_start, const byte *new_end_ptr) {
  bool droppable{false};
  std::shared_ptr<ShannonBase::Imcs::RpdTable> rpd_table_guard;
  auto rpd_table = resolve_change_target(table_id, context->m_extra_info.m_old_part_key,
                                         context->m_extra_info.m_change_id, &droppable, &rpd_table_guard);

  // If the source partition is unloaded but the destination is loaded, the
  // old image is intentionally absent from Rapid, but the post-image still
  // belongs in the loaded destination. Dropping the whole UPDATE here leaves
  // the destination stale and makes a later reverse move fail to find its
  // source row.
  if (!rpd_table && context->m_extra_info.m_part_key != context->m_extra_info.m_old_part_key) {
    if (!droppable) {
      std::ostringstream oss;
      oss << "Cannot get the table " << context->m_schema_name << "." << context->m_table_name << " from loaded tables";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), oss.str().c_str());
      return 0;
    }

    bool destination_droppable{false};
    std::shared_ptr<ShannonBase::Imcs::RpdTable> destination_guard;
    auto *destination =
        resolve_change_target(table_id, context->m_extra_info.m_part_key, context->m_extra_info.m_change_id,
                              &destination_droppable, &destination_guard);
    if (destination == nullptr) {
      if (destination_droppable) return old_end_ptr - old_start;
      std::ostringstream oss;
      oss << "Cannot get the table " << context->m_schema_name << "." << context->m_table_name << " from loaded tables";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), oss.str().c_str());
      return 0;
    }

    if (!destination->insert_row(context, (uchar *)new_start).ok()) {
      std::ostringstream oss;
      oss << "[popragate] cross-partition update (destination-only insert) in rapid " << context->m_schema_name.c_str()
          << "." << context->m_table_name.c_str() << " failed";
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), oss.str().c_str());
      return 0;
    }
    return old_end_ptr - old_start;
  }

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

  if (context->m_detached_row_image && !PreImageOffPageDataIsComplete(rpd_table, context, old_start)) {
    my_error(ER_SECONDARY_ENGINE, MYF(0),
             "Rapid COPY_INFO UPDATE has no captured pre-image data for an out-of-line column; "
             "its keys cannot be encoded without reading freed memory");
    return 0;
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
      // An out-of-line type stores only a pointer to blob-heap data in the
      // row image, so the images cannot be compared byte by byte.
      if (!ShannonBase::Utils::IsOffPageField(field)) {
        identical =
            field->cmp_binary(const_cast<uchar *>(old_start + offset), const_cast<uchar *>(new_start + offset)) == 0;
      }
    }

    if (!identical) {
      auto col_val = new_row_data.get_column_mutable(idx);
      updates.emplace(idx, std::move(*col_val));
    }
  }

  // Both images are detached, and the secondary-index keys below are encoded
  // straight out of them. locate_row() already did this for the pre-image;
  // the post-image's payload lives in the other capture map.
  ShannonBase::Imcs::Index::RapidKeyCodec::PatchDetachedOffPagePointers(
      context, rpd_table->meta(), const_cast<uchar *>(new_start), /*use_offpage_data1=*/true);
  ShannonBase::Imcs::Index::RapidKeyCodec::PatchDetachedOffPagePointers(
      context, rpd_table->meta(), const_cast<uchar *>(old_start), /*use_offpage_data1=*/false);

  // step 1b: work out the ART key swaps, but do not apply them yet.
  struct PendingIndexSwap {
    ShannonBase::Imcs::Index::Index<uchar, ShannonBase::row_id_t> *index;
    ShannonBase::Imcs::Index::RapidKeyCodec::KeyBuffer old_key;
    ShannonBase::Imcs::Index::RapidKeyCodec::KeyBuffer new_key;
  };
  std::vector<PendingIndexSwap> pending_index_swaps;
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

    pending_index_swaps.push_back({index, std::move(old_key), std::move(new_key)});
  }

  // Publish new candidates before changing the row. Keep old candidates for
  // rollback and older ReadViews. ART insertion deduplicates (key, rowid)
  // under its tree lock, so retries never need a remove/insert gap.
  for (auto &swap : pending_index_swaps) {
    swap.index->retain_versioned_keys();
    if (swap.index->insert(swap.new_key.data(), swap.new_key.size(), &global_row_id, sizeof(global_row_id)) != 0) {
      my_error(ER_SECONDARY_ENGINE, MYF(0), "Rapid could not retain an updated index candidate");
      return 0;
    }
  }
  // Readers compare each candidate's key against the selected row version.
  // A failed update therefore leaves only harmless, filtered candidates.
  if (rpd_table->update_row(context, global_row_id, updates)) {
    my_error(ER_SECONDARY_ENGINE, MYF(0), "Rapid could not apply an indexed row update");
    return 0;
  }
  return row_size;
}

int CopyInfoParser::parse_and_apply_insert(Rapid_load_context *context, table_id_t &table_id, const byte *start,
                                           const byte *end_ptr) {
  bool droppable{false};
  std::shared_ptr<ShannonBase::Imcs::RpdTable> rpd_table_guard;
  auto rpd_table = resolve_change_target(table_id, context->m_extra_info.m_part_key, context->m_extra_info.m_change_id,
                                         &droppable, &rpd_table_guard);
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
  std::shared_ptr<ShannonBase::Imcs::RpdTable> rpd_table_guard;
  auto rpd_table = resolve_change_target(table_id, context->m_extra_info.m_old_part_key,
                                         context->m_extra_info.m_change_id, &droppable, &rpd_table_guard);
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

bool CopyInfoParser::validate_record(const change_record_buff_t &record) {
  return record.m_source_trx_id != 0 || record.m_source == Source::COMMITTED_BINLOG;
}

ChangeApplyResult CopyInfoParser::apply_change(Rapid_load_context &context, change_record_buff_t &record,
                                               uint64_t change_id) {
  DBUG_SIGNAL_WAIT_FOR(current_thd, "rapid_pause_before_copy_apply", "rapid_copy_apply_waiting",
                       "rapid_copy_apply_continue");
  ChangeApplyResult result;
  result.stale_reason = stale_reason_t::UNIDENTIFIED_ERROR;
  auto table_guard = Imcs::Imcs::instance()->get_rpd_table_shared(record.m_table_id);
  auto *manager = table_guard ? table_guard->recovery_manager() : nullptr;
  auto *capture = manager ? manager->notifications() : nullptr;
  // The pending notification remains registered until mutation and terminal
  // reconciliation finish. Checkpoints therefore cannot freeze during apply;
  // disabled trackers cannot publish checkpoints either. Do not hold the
  // producer gate across row decoding or IMCU mutation.
  std::unique_lock<std::recursive_mutex> replay_gate;
  if (capture && !record.m_notification_sequence)
    replay_gate = std::unique_lock<std::recursive_mutex>(capture->mutex());
  DBUG_SIGNAL_WAIT_FOR(current_thd, "rapid_pause_copy_apply_mutation", "rapid_apply_mutation_waiting",
                       "rapid_apply_mutation_continue");
  auto mark_applied = create_scope_guard([&] {
    if (capture && result.status == ChangeApplyResult::Status::APPLIED && record.m_notification_sequence)
      capture->applied(record.m_notification_sequence);
  });

  // Direct row-image propagation must preserve the real primary InnoDB writer
  // identity. commit_scn == 0 is valid: it represents an ACTIVE Rapid MVCC
  // version, not a missing transaction.
  const bool replay = record.m_source == Source::COMMITTED_BINLOG;
  if (record.m_source_trx_id == 0 && !replay) {
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
    if (!replay) TransactionManager::instance().on_change_applied(source_txn_id, record.m_table_id);
    return result;
  }

  context.m_trx = nullptr;
  context.m_table_id = record.m_table_id;
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
  // Both images are detached copies; their in-row blob pointers are dead.
  context.m_detached_row_image = true;
  context.m_offpage_data0 = record.m_offpage_data0.empty() ? nullptr : &record.m_offpage_data0;
  context.m_offpage_data1 = record.m_offpage_data1.empty() ? nullptr : &record.m_offpage_data1;
  // Physical partition routing resolved by the capture side; empty for a
  // non-partitioned table.
  context.m_extra_info.m_part_key = record.m_part_key;
  context.m_extra_info.m_old_part_key = record.m_old_part_key;
  // Which change this is, so a partition loaded after it can recognize the
  // change as already folded into its rows (RpdTable::load_watermark()).
  context.m_extra_info.m_change_id = change_id;

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
    if (!replay) TransactionManager::instance().on_change_applied(source_txn_id, record.m_table_id);
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

int EnqueueCopyInfo(THD *thd, change_record_buff_t &&record) {
  if (thd == nullptr) return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);

  if (ShannonBase::Transaction::get_or_create_trx(thd) == nullptr)
    return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);

  auto registration = ShannonBase::Populate::TransactionManager::instance().register_change(thd, record.m_table_id);
  if (!registration) return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);

  record.m_source_trx_id = registration.source_trx_id;
  record.m_commit_scn = 0;

  auto table = Imcs::Imcs::instance()->get_rpd_table_shared(record.m_table_id);
  auto *manager = table ? table->recovery_manager() : nullptr;
  auto *capture = manager ? manager->notifications() : nullptr;
  std::unique_lock<std::recursive_mutex> gate;
  if (capture) {
    gate = std::unique_lock<std::recursive_mutex>(capture->mutex());
    if (!thd->variables.sql_log_bin || sync_binlog_period != 1 || srv_flush_log_at_trx_commit != 1) {
      // Fence old checkpoints before the primary is allowed to commit a write
      // whose durability/history cannot be certified by committed binlog.
      if (!capture->disabled() && !manager->revoke_fast_recovery()) {
        QuarantinePropagationTable(record.m_table_id);
        return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);
      }
    }
    if (!thd->get_transaction()->xid_state()->has_state(XID_STATE::XA_NOTR)) {
      // XA PREPARE can invoke after_commit without a final source commit,
      // and detached XA may be resolved by another THD. Do not certify it
      // with the ordinary transaction protocol.
      QuarantinePropagationTable(record.m_table_id);
    }
    if (!capture->disabled()) {
      if (IsPropagationBroken(record.m_table_id)) {
        // Already quarantined: this change will never be applied, so journaling it
        // would only leave an unapplied sequence that blocks every checkpoint.
        // Skipping it makes the journal incomplete, so revoke it instead.
        QuarantinePropagationTable(record.m_table_id);
      } else {
        record.m_notification_sequence = capture->notify(record.m_source_trx_id);
        if (!record.m_notification_sequence) {
          QuarantinePropagationTable(record.m_table_id);
          return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);
        }
      }
    }
  }
  // Preserve capture order through enqueue, without waiting for column apply.
  const auto capture_lsn = log_get_lsn(*log_sys);
  const auto result = ShannonBase::Populate::Populator::write(nullptr, capture_lsn, &record);
  return result;
}

namespace {
// Register with the server delegate itself: RUN_HOOK skips the delegate when
// no observers exist, even if a handlerton provides an se_after_commit hook.
Trans_observer rapid_transaction_observer{sizeof(Trans_observer),
                                          nullptr,
                                          nullptr,
                                          [](Trans_param *param) -> int {
                                            DML::rapid_before_rollback(param);
                                            return 0;
                                          },
                                          [](Trans_param *param) -> int {
                                            DML::rapid_after_commit(param);
                                            return 0;
                                          },
                                          nullptr,
                                          nullptr};
}  // namespace

void TransactionManager::ensure_subscribed() {
  if (m_subscribed.load(std::memory_order_acquire)) return;

  std::lock_guard<std::mutex> lock(m_subscription_mutex);
  if (m_subscribed.load(std::memory_order_relaxed)) return;

  // Finish the owning container allocation before registering with the server,
  // so allocation failure cannot leave a partially installed observer behind.
  Transaction::subscribe(this);
  if (register_trans_observer(&rapid_transaction_observer, hton2plugin(shannon_rapid_hton_ptr->slot))) {
    Transaction::unsubscribe(this);
    sql_print_error("Rapid could not register its source transaction observer");
    return;
  }
  m_subscribed.store(true, std::memory_order_release);
}

TransactionManager::Registration TransactionManager::register_change(THD *thd, table_id_t table_id) try {
  ensure_subscribed();
  if (!m_subscribed.load(std::memory_order_acquire) || thd == nullptr || table_id == 0) return {};

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
} catch (const std::bad_alloc &) {
  // Registration owns the participant and per-transaction map allocations.
  // Revoke any partial registration after releasing their mutex.
  quarantine_failed_transaction(thd);
  return {};
}

void TransactionManager::quarantine_participant(THD *thd, bool require_statement_change, const char *reason) {
  if (thd == nullptr) return;

  std::vector<table_id_t> tables;
  try {
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
  } catch (...) {
    quarantine_failed_transaction(thd);
    return;
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

void TransactionManager::quarantine_failed_transaction(THD *thd) noexcept {
  // Walk the already-owned set without allocating a snapshot. Do not hold the
  // participant mutex across notification gates (apply takes the reverse order).
  table_id_t previous = 0;
  for (;;) {
    table_id_t next = 0;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      auto it = m_participants.find(thd);
      if (it == m_participants.end()) break;
      it->second.fail_closed = true;
      for (auto id : it->second.touched_tables)
        if (id > previous && (next == 0 || id < next)) next = id;
    }
    if (next == 0) break;
    QuarantinePropagationTable(next);
    previous = next;
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_participants.find(thd);
    if (it != m_participants.end()) {
      m_transactions.erase(it->second.source_trx_id);
      m_participants.erase(it);
    }
  }
  sql_print_error("Rapid transaction callback failed; affected loaded tables require reload");
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

void TransactionManager::on_transaction_commit(THD *thd, const Recovery::BinlogPosition &position) {
  if (thd == nullptr) return;

  Transaction::ID source_trx_id = 0;
  bool has_changes = false;
  std::vector<table_id_t> captured_tables;
  DBUG_EXECUTE_IF("rapid_transaction_commit_bad_alloc", {
    quarantine_failed_transaction(thd);
    return;
  });
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_participants.find(thd);
    if (it == m_participants.end()) return;

    source_trx_id = it->second.source_trx_id;
    has_changes = !it->second.touched_tables.empty();
    captured_tables.assign(it->second.touched_tables.begin(), it->second.touched_tables.end());
  } catch (...) {
    quarantine_failed_transaction(thd);
    return;
  }

  if (source_trx_id == 0 || !has_changes) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_participants.erase(thd);
    return;
  }

  // Primary binlog/redo own durability. This callback publishes memory only.
  // Inject on the source callback thread: finalization may otherwise run on
  // the apply worker after this session has already cleared its debug flag.
  DBUG_EXECUTE_IF("rapid_transaction_finalize_bad_alloc", {
    quarantine_failed_transaction(thd);
    return;
  });
  for (table_id_t id : captured_tables) {
    auto table = Imcs::Imcs::instance()->get_rpd_table_shared(id);
    if (auto *manager = table ? table->recovery_manager() : nullptr) {
      const bool logged_durably =
          position.valid() && thd->variables.sql_log_bin && sync_binlog_period == 1 && srv_flush_log_at_trx_commit == 1;
      manager->notifications()->committed(source_trx_id, logged_durably ? position : Recovery::BinlogPosition{});
      // Unlogged/weakly durable writes invalidate old durable images. This is
      // an exceptional recovery fence, never an extra source commit flush.
      if (!logged_durably) (void)manager->revoke_fast_recovery();
    }
  }

  // Rapid SCN is physical publication/retention metadata. SQL creator
  // visibility remains exclusively the InnoDB ReadView.
  const uint64_t commit_scn = TransactionCoordinator::instance().allocate_scn();
  publish_commit(source_trx_id, commit_scn);
  std::lock_guard<std::mutex> lock(m_mutex);
  m_participants.erase(thd);
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
  }

  if (source_trx_id != 0 && has_changes) publish_rollback(source_trx_id);
  std::lock_guard<std::mutex> lock(m_mutex);
  m_participants.erase(thd);
}

void TransactionManager::record_source_abort(THD *thd) {
  Transaction::ID source_trx_id = 0;
  std::vector<table_id_t> tables;
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_participants.find(thd);
    if (it == m_participants.end()) return;
    source_trx_id = it->second.source_trx_id;
    tables.assign(it->second.touched_tables.begin(), it->second.touched_tables.end());
  } catch (...) {
    quarantine_failed_transaction(thd);
    return;
  }
  // Only the server's source rollback decision is durable evidence. Defensive
  // facade cleanup/detach is NOT proof that a source transaction did not commit.
  for (table_id_t id : tables) {
    auto table = Imcs::Imcs::instance()->get_rpd_table_shared(id);
    auto *manager = table ? table->recovery_manager() : nullptr;
    auto *capture = manager ? manager->notifications() : nullptr;
    if (capture) capture->aborted(source_trx_id);
  }
}

void TransactionManager::on_transaction_detach(THD *thd) {
  // Defensive teardown: unresolved COPY_INFO participation must not survive a
  // THD facade. Normal COMMIT/ROLLBACK has already erased the participant, so
  // this is idempotent.
  on_transaction_rollback(thd);
}

bool TransactionManager::finalize_imcu(Transaction::ID txn_id, table_id_t table_id, Outcome outcome,
                                       uint64_t commit_scn, const std::shared_ptr<Imcs::Imcu> &imcu) {
  if (!imcu) return true;
  DBUG_EXECUTE_IF("rapid_finalize_imcu_failure", {
    sql_print_error("Rapid transaction callback failed; affected loaded tables require reload");
    return false;
  });
  try {
    if (outcome == Outcome::COMMITTED) {
      imcu->commit_transaction(txn_id, commit_scn);
    } else if (outcome == Outcome::ABORTED && !imcu->rollback_transaction(txn_id)) {
      ib::error() << "Rapid: failed to rollback propagated source transaction " << txn_id << " on table " << table_id;
      return false;
    }
  } catch (const std::bad_alloc &) {
    sql_print_error("Rapid transaction callback failed; affected loaded tables require reload");
    return false;
  }
  return true;
}

bool TransactionManager::on_imcu_applied(Transaction::ID txn_id, table_id_t table_id,
                                         const std::shared_ptr<Imcs::Imcu> &imcu) {
  Outcome outcome{Outcome::ACTIVE};
  uint64_t commit_scn{0};
  bool recorded{true};
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto txn = m_transactions.find(txn_id);
    // Recovery replay and initial loading have no live COPY_INFO registration.
    if (txn == m_transactions.end()) return true;
    auto table = txn->second.tables.find(table_id);
    if (table == txn->second.tables.end()) return true;
    outcome = txn->second.outcome;
    commit_scn = txn->second.commit_scn;
    if (outcome == Outcome::ACTIVE) recorded = table->second.imcus.add(imcu);
  }
  if (!recorded) {
    QuarantinePropagationTable(table_id);
    sql_print_error("Rapid transaction participant allocation failed; affected loaded tables require reload");
    return false;
  }
  // The source outcome may have arrived between reading it in apply_change()
  // and mutating this IMCU. Settle that version before acknowledging the row.
  if (outcome == Outcome::ACTIVE || finalize_imcu(txn_id, table_id, outcome, commit_scn, imcu)) return true;
  QuarantinePropagationTable(table_id);
  return false;
}

void TransactionManager::finalize_table(Transaction::ID txn_id, table_id_t table_id, Outcome outcome,
                                        uint64_t commit_scn) {
  // Keep the logical table (and its partition owners) alive across unload.
  auto rpd_table = Imcs::Imcs::instance()->get_rpd_table_shared(table_id);
  if (!rpd_table) return;
  DBUG_EXECUTE_IF("rapid_transaction_finalize_bad_alloc", {
    QuarantinePropagationTable(table_id);
    sql_print_error("Rapid transaction callback failed; affected loaded tables require reload");
    return;
  });
  TransactionImcuParticipants<Imcs::Imcu>::Set imcus;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto txn = m_transactions.find(txn_id);
    if (txn == m_transactions.end()) return;
    auto table = txn->second.tables.find(table_id);
    if (table == txn->second.tables.end()) return;
    imcus = table->second.imcus.take();
    DBUG_EXECUTE_IF("rapid_finalize_one_imcu", { ut_a(imcus.size() == 1); });
    DBUG_EXECUTE_IF("rapid_finalize_two_imcus", { ut_a(imcus.size() == 2); });
    DBUG_EXECUTE_IF("rapid_finalize_three_imcus", { ut_a(imcus.size() == 3); });
    DBUG_EXECUTE_IF("rapid_finalize_four_imcus", { ut_a(imcus.size() == 4); });
    if (imcus.empty()) return;
    ++table->second.finalizing;
  }
  bool finalized{true};
  {
    auto settled = create_scope_guard([&] {
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto txn = m_transactions.find(txn_id);
        if (txn != m_transactions.end()) {
          auto table = txn->second.tables.find(table_id);
          if (table != txn->second.tables.end()) --table->second.finalizing;
        }
      }
      m_finalize_cv.notify_all();
    });
    // No table-wide snapshot or scan: only IMCUs whose mutations completed
    // before publication are here. Later mutations finalize in on_imcu_applied().
    for (const auto &[identity, imcu] : imcus) {
      (void)identity;
      if (!finalize_imcu(txn_id, table_id, outcome, commit_scn, imcu)) {
        finalized = false;
        break;
      }
    }
    if (!finalized) {
      // Fence reads and checkpoints before waking an apply worker. Durable
      // revocation may need its capture gate, so perform that after notification.
      rpd_table->quarantine_propagation();
      if (auto *manager = rpd_table->recovery_manager(); manager) manager->require_recovery();
      for (const auto &[identity, imcu] : imcus) {
        (void)identity;
        auto *owner = imcu->owner();
        auto *manager = owner ? owner->recovery_manager() : nullptr;
        if (manager) manager->require_recovery();
      }
      RpdMirror::Registry::mark_stale(static_cast<uint>(table_id), stale_reason_t::RELOAD_REQUIRED);
    }
  }
  if (!finalized) QuarantinePropagationTable(table_id);
}

void TransactionManager::erase_if_complete_locked(Transaction::ID txn_id) {
  auto it = m_transactions.find(txn_id);
  if (it == m_transactions.end() || it->second.outcome == Outcome::ACTIVE) return;

  for (const auto &[table_id, progress] : it->second.tables) {
    (void)table_id;
    if (progress.applied < progress.registered || progress.finalizing || !progress.imcus.empty()) return;
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
  uint64_t commit_scn{0};
  auto outcome = get_outcome(txn_id, &commit_scn);
  if (outcome != Outcome::ACTIVE) finalize_table(txn_id, table_id, outcome, commit_scn);

  std::unique_lock<std::mutex> lock(m_mutex);
  // A source callback may still be finalizing an earlier applied record. Do
  // not release this record's inflight accounting before that work finishes.
  m_finalize_cv.wait(lock, [&] {
    auto txn = m_transactions.find(txn_id);
    if (txn == m_transactions.end()) return true;
    auto table = txn->second.tables.find(table_id);
    return table == txn->second.tables.end() || table->second.finalizing == 0;
  });
  auto txn = m_transactions.find(txn_id);
  if (txn == m_transactions.end()) return;
  auto table = txn->second.tables.find(table_id);
  if (table == txn->second.tables.end()) return;
  ++table->second.applied;
  erase_if_complete_locked(txn_id);
}

void TransactionManager::publish_commit(Transaction::ID txn_id, uint64_t commit_scn) {
  if (txn_id == 0 || commit_scn == 0) return;
  DBUG_EXECUTE_IF("rapid_transaction_publish_bad_alloc", {
    quarantine_failed_transaction(current_thd);
    return;
  });
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_transactions.find(txn_id);
    if (it == m_transactions.end()) return;
    it->second.outcome = Outcome::COMMITTED;
    it->second.commit_scn = commit_scn;
    try {
      DBUG_EXECUTE_IF("rapid_commit_queue_bad_alloc", { throw std::bad_alloc(); });
      m_committed_queue.push(txn_id);
    } catch (const std::bad_alloc &) {
      // Outcome publication already succeeded. The queue is only a hint;
      // retry by scanning authoritative transaction state in the background.
      m_commit_scan_needed = true;
    }
    m_commits_pending.store(true, std::memory_order_release);
  }
  // The existing 200ms coordinator timer batches terminal outcomes. Capacity
  // and query-demand triggers still wake it immediately; TP does not signal
  // an event for every commit.
}

void TransactionManager::finalize_committed() {
  DBUG_EXECUTE_IF("rapid_finalize_commits_stall", { return; });
  if (!m_commits_pending.exchange(false, std::memory_order_acq_rel)) return;
  struct Work {
    Transaction::ID txn;
    table_id_t table;
    uint64_t scn;
  };
  std::vector<Work> work;
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto append = [&](Transaction::ID txn_id, const TxnProgress &transaction) {
      if (transaction.outcome != Outcome::COMMITTED) return;
      for (const auto &[table_id, progress] : transaction.tables)
        work.push_back({txn_id, table_id, transaction.commit_scn});
    };
    if (m_commit_scan_needed) {
      for (const auto &[txn_id, transaction] : m_transactions) append(txn_id, transaction);
    } else {
      while (!m_committed_queue.empty()) {
        const auto txn_id = m_committed_queue.front();
        const auto it = m_transactions.find(txn_id);
        // Apply/unload can retire a transaction before the batch timer fires.
        if (it != m_transactions.end()) append(txn_id, it->second);
        m_committed_queue.pop();
      }
    }
    while (!m_committed_queue.empty()) m_committed_queue.pop();
    m_commit_scan_needed = false;
  } catch (const std::bad_alloc &) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // A partially built work vector is discarded, including entries already
    // popped. Unfinalized IMCUs retain their transaction state for this retry.
    m_commit_scan_needed = true;
    m_commits_pending.store(true, std::memory_order_release);
    return;  // Retry in the coordinator; never lose the terminal outcome.
  }
  for (const auto &entry : work) {
    finalize_table(entry.txn, entry.table, Outcome::COMMITTED, entry.scn);
    std::lock_guard<std::mutex> lock(m_mutex);
    erase_if_complete_locked(entry.txn);
  }
}

void TransactionManager::publish_rollback(Transaction::ID txn_id) {
  if (txn_id == 0) return;

  DBUG_EXECUTE_IF("rapid_transaction_rollback_bad_alloc", {
    quarantine_failed_transaction(current_thd);
    return;
  });
  std::vector<table_id_t> tables;
  try {
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
  } catch (...) {
    quarantine_failed_transaction(current_thd);
    return;
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
  while (!m_committed_queue.empty()) m_committed_queue.pop();
  m_commit_scan_needed = false;
  m_commits_pending.store(false, std::memory_order_release);
}

void TransactionManager::start() { ensure_subscribed(); }

void TransactionManager::shutdown() {
  {
    std::lock_guard<std::mutex> lock(m_subscription_mutex);
    if (m_subscribed.exchange(false, std::memory_order_acq_rel)) {
      unregister_trans_observer(&rapid_transaction_observer, nullptr);
      Transaction::unsubscribe(this);
    }
  }
  clear();
}

namespace DML {
void rapid_after_commit(void *arg) {
  const auto *param = static_cast<const Trans_param *>(arg);
  if (param == nullptr || (param->flags & TRANS_IS_REAL_TRANS) == 0) return;

  // The existing server hook supplies thread_id, but does not populate thd.
  // Keep this implementation engine-local: certify an outcome only when the
  // callback's current THD matches that identity. An unidentifiable callback
  // leaves the durable outcome unresolved, requiring primary reload.
  THD *thd = current_thd;
  if (!thd || thd->thread_id() != param->thread_id || thd->lex->sql_command == SQLCOM_XA_PREPARE) return;
  auto *trx = thd ? ShannonBase::Transaction::find_trx(thd) : nullptr;
  if (trx != nullptr) {
    TransactionManager::instance().on_transaction_commit(
        thd,
        Recovery::BinlogPosition{param->log_file ? param->log_file : "", static_cast<uint64_t>(param->log_pos), {}});
  }
}

void rapid_before_rollback(void *arg) {
  const auto *param = static_cast<const Trans_param *>(arg);
  if (param == nullptr || (param->flags & TRANS_IS_REAL_TRANS) == 0) return;

  THD *thd = current_thd;
  auto *trx = thd ? ShannonBase::Transaction::find_trx(thd) : nullptr;
  if (!thd || thd->thread_id() != param->thread_id) return;
  if (trx != nullptr) {
    TransactionManager::instance().record_source_abort(thd);
    trx->rollback();
  }
}
}  // namespace DML
}  // namespace Populate
}  // namespace ShannonBase
