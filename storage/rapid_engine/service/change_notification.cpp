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

   Copyright (c) 2026, Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/service/change_notification.h"
#include <cstring>
#include <optional>
#include "sql/dd/types/foreign_key.h"
#include "sql/field.h"
#include "sql/log.h"
#include "sql/partition_info.h"
#include "sql/sql_class.h"
#include "sql/sql_data_change.h"
#include "sql/sql_lex.h"
#include "sql/sql_partition.h"
#include "sql/table.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_table_info.h"
#include "storage/rapid_engine/populate/log_binlog_parser.h"
#include "storage/rapid_engine/populate/log_dml_notification.h"
#include "storage/rapid_engine/populate/log_populate.h"
#include "storage/rapid_engine/recovery/recovery.h"
#include "storage/rapid_engine/utils/utils.h"

/**
 * @brief Read and copy BLOB-type off-page data from table
 *
 * The main purpose of this function is to address the storage characteristics of BLOB data types in MySQL:
 * - BLOB fields only store pointers and length information in the row record, actual data is stored in off-page areas
 * - When inserting multiple records consecutively, MySQL may reuse the same memory area for BLOB data
 * - If we only copy pointers from record[0], subsequent operations will overwrite the actual data pointed to
 *
 * The function ensures data integrity through the following steps:
 * 1. Use pre-cached BLOB field indices to avoid traversing all fields
 * 2. Parse BLOB field length prefixes and pointer information
 * 3. Create independent memory copies for each BLOB data
 * 4. Store copied data in off_page_data structure for later use
 *
 * @param table MySQL table structure pointer
 * @param off_page_data Output parameter, stores field indices and corresponding BLOB data copies
 */
static inline bool table_has_off_page_blob_data(const TABLE *table) {
  return table != nullptr && table->s != nullptr && table->s->blob_fields > 0;
}

/**
 * Point every Field of @a table at @a record for the lifetime of the guard.
 *
 * Field accessors read through Field::ptr, which is bound to record[0]. A
 * COPY_INFO UPDATE has to capture the *pre*-image too, and that lives in
 * record[1], so the fields have to be walked over there and back. This is the
 * same move_field_offset() idiom the partitioning and trigger code uses.
 */
namespace {
class RecordImageGuard {
 public:
  RecordImageGuard(TABLE *table, const uchar *record) : m_table(table), m_diff(record - table->record[0]) {
    move(m_diff);
  }

  ~RecordImageGuard() { move(-m_diff); }

  RecordImageGuard(const RecordImageGuard &) = delete;
  RecordImageGuard &operator=(const RecordImageGuard &) = delete;

 private:
  void move(ptrdiff_t diff) {
    if (diff == 0) return;
    // Bound by s->fields rather than a nullptr sentinel: the loop in
    // read_off_page_data() below indexes the same way.
    for (uint idx = 0; idx < m_table->s->fields; ++idx) m_table->field[idx]->move_field_offset(diff);
  }

  TABLE *m_table;
  ptrdiff_t m_diff;
};
}  // namespace

/**
 * Copy the out-of-line bytes of every off-page column of @a record.
 *
 * The row image only stores a length plus a pointer into the primary engine's
 * blob heap, and that heap is released when the originating statement ends --
 * long before the asynchronous propagation worker parses the record. So the
 * payload has to be copied here, at capture time, for *both* the pre- and the
 * post-image: RowBuffer::extract_field_data() otherwise falls through to the
 * raw-pointer path and dereferences freed memory.
 *
 * Columns are admitted regardless of the statement's read_set. An UPDATE need
 * not read a blob column it does not touch, but the apply side still has to
 * encode that column into the row's index keys, and a map that is non-empty
 * yet missing an entry is exactly the shape extract_field_data() cannot
 * resolve.
 */
static bool read_off_page_data(TABLE *table, const uchar *record,
                               ShannonBase::Populate::change_record_buff_t::off_page_data_t &off_page_data) {
  if (!table_has_off_page_blob_data(table) || record == nullptr) return true;

  RecordImageGuard image(table, record);
  ShannonBase::Utils::ColumnMapGuard columns(table, ShannonBase::Utils::ColumnMapGuard::TYPE::READ);

  for (uint idx = 0; idx < table->s->fields; idx++) {
    Field *fld = *(table->field + idx);
    if (!bitmap_is_set(table->read_set, idx) || fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    // Must admit exactly the types RowBuffer::extract_field_data() reads back
    // out of this map. JSON, GEOMETRY and VECTOR are Field_blob subclasses
    // whose data is off-page just like a BLOB's, but they report their own
    // field type, so a BLOB-only filter left them uncaptured. The reader then
    // found a non-empty map without an entry for that column and asserted.
    if (!ShannonBase::Utils::IsOffPageField(fld)) continue;
    if (fld->is_null()) continue;

    auto bfld = down_cast<Field_blob *>(fld);
    const size_t data_len = bfld->get_length();
    const uchar *actual_blob_data = bfld->get_blob_data();
    if (actual_blob_data == nullptr) continue;

    std::shared_ptr<uchar[]> blob_copy;
    try {
      blob_copy.reset(new uchar[data_len ? data_len : 1]);
      std::memcpy(blob_copy.get(), actual_blob_data, data_len);
      off_page_data.emplace(idx, std::make_pair(data_len, std::move(blob_copy)));
    } catch (const std::bad_alloc &) {
      off_page_data.clear();
      return false;
    }
  }

  return true;
}

/**
 * @brief Resolve which partition(s) a captured row change belongs to.
 *
 * The propagation worker owns no TABLE and cannot evaluate a partition
 * function, so the routing decision is made here while partition_info and the
 * row images are still in hand. An UPDATE is allowed to move a row between
 * partitions, hence the separate pre-image / post-image keys.
 *
 * Both keys stay empty for a non-partitioned table.
 *
 * @return false when the partition cannot be resolved. The caller must then
 *         quarantine the table instead of enqueueing a record that the apply
 *         worker would not be able to route.
 */
static bool resolve_change_partitions(TABLE *table, ShannonBase::Populate::change_record_buff_t::OperType oper,
                                      const uchar *old_row, const uchar *new_row, std::string &part_key,
                                      std::string &old_part_key) {
  using OperType = ShannonBase::Populate::change_record_buff_t::OperType;

  partition_info *part_info = table->part_info;
  assert(part_info != nullptr);

  uint32 part_id{NOT_A_PARTITION_ID}, old_part_id{NOT_A_PARTITION_ID};
  longlong func_value{0};
  int error{0};

  // The partitioning fields are not necessarily in the statement's read_set;
  // mark them readable exactly like Partition_helper::ph_write_row() does.
  ShannonBase::Utils::ColumnMapGuard guard(table, ShannonBase::Utils::ColumnMapGuard::TYPE::READ);
  switch (oper) {
    case OperType::INSERT:
      error = part_info->get_partition_id(part_info, &part_id, &func_value);
      old_part_id = part_id;
      break;
    case OperType::DELETE:
      error = get_part_for_delete(old_row, table->record[0], part_info, &old_part_id);
      part_id = old_part_id;
      break;
    case OperType::UPDATE:
      error = get_parts_for_update(old_row, new_row, table->record[0], part_info, &old_part_id, &part_id, &func_value);
      break;
    default:
      error = HA_ERR_GENERIC;
      break;
  }

  if (error != 0) return false;

  part_key = ShannonBase::Utils::rapid_partition_key(part_info, part_id);
  old_part_key = ShannonBase::Utils::rapid_partition_key(part_info, old_part_id);
  return !part_key.empty() && !old_part_key.empty();
}

namespace {
/**
 * True when @a field holds a different value in the two raw row images.
 */
bool FieldDiffersBetweenRows(const Field *field, const uchar *old_row, const uchar *new_row) {
  if (field->is_nullable()) {
    const bool was_null = (old_row[field->null_offset()] & field->null_bit) != 0;
    const bool is_null = (new_row[field->null_offset()] & field->null_bit) != 0;
    if (was_null != is_null) return true;
    if (was_null) return false;
  }
  const ptrdiff_t off = field->offset(field->table->record[0]);
  return std::memcmp(old_row + off, new_row + off, field->pack_length()) != 0;
}

/**
 * ON UPDATE CASCADE only needs to touch the child when the UPDATE actually
 * changed one of the parent's unique-key columns; without this an ordinary
 * UPDATE of an unrelated column would stale every loaded child table.
 *
 * Only unique keys are inspected. MySQL also permits a foreign key to
 * reference a non-unique index, and TABLE_SHARE_FOREIGN_KEY_PARENT_INFO
 * carries no referenced-column list, so such a target is missed here.
 */
bool ParentUniqueKeyChanged(const TABLE *table, const uchar *old_row, const uchar *new_row) {
  if (table->key_info == nullptr) return true;  // cannot tell: stay conservative
  for (uint k = 0; k < table->s->keys; ++k) {
    const KEY &key = table->key_info[k];
    if ((key.flags & HA_NOSAME) == 0) continue;  // not unique: never an FK target
    for (uint part = 0; part < key.user_defined_key_parts; ++part) {
      const Field *field = key.key_part[part].field;
      if (field != nullptr && FieldDiffersBetweenRows(field, old_row, new_row)) return true;
    }
  }
  return false;
}

// A failed row notification has no SQL error channel: the callback runs after
// write_record() and must not touch the statement diagnostics. Quarantine the
// affected tables instead, so Rapid stops serving rows it did not capture.
void QuarantineFailedNotification(const TABLE *table) noexcept {
  if (!table || !table->s) return;
  auto *imcs = ShannonBase::Imcs::Imcs::instance();
  auto loaded = imcs->get_rpd_table_by_name(table->s->db.str, table->s->table_name.str);
  if (loaded) ShannonBase::Populate::QuarantinePropagationTable(loaded->meta().table_id);
  // The callback failed after primary DML. Either cascade may already have run;
  // visit the existing FK metadata without constructing a temporary ID vector.
  for (uint i = 0; table->s->foreign_key_parent && i < table->s->foreign_key_parents; ++i) {
    const auto &fk = table->s->foreign_key_parent[i];
    auto child = imcs->get_rpd_table_by_name(fk.referencing_table_db.str, fk.referencing_table_name.str);
    if (child) ShannonBase::Populate::QuarantinePropagationTable(child->meta().table_id);
  }
  sql_print_error("Rapid row notification failed; affected loaded tables require reload");
}

void QuarantineCascadeChildren(const TABLE *table, bool for_delete) {
  if (table == nullptr || table->s == nullptr) return;
  // Single integer test on the hot path: almost no table is an FK parent.
  if (table->s->foreign_key_parents == 0 || table->s->foreign_key_parent == nullptr) return;

  for (uint i = 0; i < table->s->foreign_key_parents; ++i) {
    const auto &fk = table->s->foreign_key_parent[i];
    const auto rule = for_delete ? fk.delete_rule : fk.update_rule;
    // NO ACTION / RESTRICT reject the parent DML instead of touching the child.
    if (rule != dd::Foreign_key::RULE_CASCADE && rule != dd::Foreign_key::RULE_SET_NULL &&
        rule != dd::Foreign_key::RULE_SET_DEFAULT)
      continue;

    auto child = ShannonBase::Imcs::Imcs::instance()->get_rpd_table_by_name(fk.referencing_table_db.str,
                                                                            fk.referencing_table_name.str);
    if (!child) continue;

    ShannonBase::Populate::QuarantinePropagationTable(child->meta().table_id);
    ShannonBase::RpdMirror::Registry::mark_stale(static_cast<uint>(child->meta().table_id),
                                                 ShannonBase::stale_reason_t::RELOAD_REQUIRED);
    sql_print_warning(
        "Rapid: %s on %s.%s cascades into loaded table %s.%s, which change propagation cannot observe; "
        "the table is now stale. Reload it to resume change propagation.",
        for_delete ? "DELETE" : "UPDATE", table->s->db.str, table->s->table_name.str, fk.referencing_table_db.str,
        fk.referencing_table_name.str);
  }
}

/**
 * Capture one row change as a COPY_INFO record and hand it to change propagation.
 *
 * @param pre   image copied into buff0: the old row, or record[0] for INSERT
 * @param post  image copied into buff1 (UPDATE only), otherwise nullptr
 * @param partition_role  wording for the "cannot resolve partition" warning
 */
bool BuildRowChange(TABLE *table, ShannonBase::table_id_t table_id,
                    ShannonBase::Populate::change_record_buff_t::OperType oper, const uchar *pre, const uchar *post,
                    const char *partition_role,
                    std::optional<ShannonBase::Populate::change_record_buff_t> &record) noexcept {
  DBUG_EXECUTE_IF("rapid_notification_bad_alloc", {
    QuarantineFailedNotification(table);
    return false;
  });
  try {
    record.emplace(ShannonBase::Populate::Source::COPY_INFO, table->s->rec_buff_length);

#ifndef NDEBUG
    record->m_schema_name = table->s->db.str;
    record->m_table_name = table->s->table_name.str;
#endif
  } catch (...) {
    QuarantineFailedNotification(table);
    return false;
  }
  auto &rec = *record;
  rec.m_oper = oper;
  rec.m_table_id = table_id;

  if (table->part_info &&
      !resolve_change_partitions(table, oper, pre, post ? post : pre, rec.m_part_key, rec.m_old_part_key)) {
    ShannonBase::Populate::QuarantinePropagationTable(table_id);
    sql_print_warning("Rapid COPY_INFO could not resolve the %s on table %llu", partition_role,
                      static_cast<unsigned long long>(table_id));
    return false;
  }

  // read_off_page_data() is a no-op for tables without blob-like columns.
  std::memcpy(rec.m_buff0.get(), pre, table->s->rec_buff_length);
  if (!read_off_page_data(table, pre, rec.m_offpage_data0)) {
    ShannonBase::Populate::QuarantinePropagationTable(table_id);
    sql_print_warning("Rapid COPY_INFO could not copy the off-page columns of table %llu",
                      static_cast<unsigned long long>(table_id));
    return false;
  }
  if (post) {
    std::memcpy(rec.m_buff1.get(), post, table->s->rec_buff_length);
    if (!read_off_page_data(table, post, rec.m_offpage_data1)) {
      ShannonBase::Populate::QuarantinePropagationTable(table_id);
      sql_print_warning("Rapid COPY_INFO could not copy the off-page columns of table %llu",
                        static_cast<unsigned long long>(table_id));
      return false;
    }
  }

  return true;
}

void EnqueueRowChange(THD *thd, TABLE *table, const std::shared_ptr<ShannonBase::Imcs::RpdTable> &loaded,
                      ShannonBase::Populate::change_record_buff_t::OperType oper, const uchar *pre, const uchar *post,
                      const char *partition_role) {
  if (!loaded) return;

  std::optional<ShannonBase::Populate::change_record_buff_t> record;
  if (!BuildRowChange(table, loaded->meta().table_id, oper, pre, post, partition_role, record)) return;
  ShannonBase::Populate::RegisterCopyInfoParticipant(thd);
  if (ShannonBase::Populate::EnqueueCopyInfo(thd, std::move(*record)) != ShannonBase::SHANNON_SUCCESS) {
    ShannonBase::Populate::QuarantinePropagationTable(loaded->meta().table_id);
    sql_print_warning("Rapid COPY_INFO could not enqueue row change for table %llu; reload required",
                      static_cast<unsigned long long>(loaded->meta().table_id));
  }
}
}  // namespace

namespace ShannonBase {
namespace Service {
using RowChangeOper = ShannonBase::Populate::change_record_buff_t::OperType;

void notify_insert(THD *thd, TABLE *table, COPY_INFO *info) {
  auto loaded = ShannonBase::Imcs::Imcs::instance()->get_rpd_table_by_name(table->s->db.str, table->s->table_name.str);
  if (!loaded) ShannonBase::Recovery::revoke_unregistered_table_image(table->s->db.str, table->s->table_name.str);

  if (ShannonBase::Populate::configured_change_propagation_mode.load() ==
      ShannonBase::Populate::CopyInfoMode::COMMITTED_BINLOG) {
    if (loaded) ShannonBase::Populate::CommittedBinlog::notify_dml(thd, table);
    return;
  }

  if (thd->lex->is_ignore() || info->get_duplicate_handling() != DUP_ERROR) {
    // This server hook reports successful write_record(), not a successful
    // physical INSERT. IGNORE can report a rejected row; REPLACE can remove
    // several unique-key conflicts; ON DUPLICATE KEY can perform an UPDATE.
    // Its cumulative counters and final record buffer do not describe every
    // affected pre-image. Never certify the attempted INSERT in the WAL.
    // Keep this guard engine-local until a complete row outcome is available.
    if (info->get_duplicate_handling() == DUP_REPLACE) QuarantineCascadeChildren(table, true);
    if (info->get_duplicate_handling() == DUP_UPDATE) QuarantineCascadeChildren(table, false);
    if (loaded) ShannonBase::Populate::QuarantinePropagationTable(loaded->meta().table_id);
    return;
  }
  EnqueueRowChange(thd, table, loaded, RowChangeOper::INSERT, table->record[0], nullptr,
                   "target partition of an INSERT");
}

void notify_update(THD *thd, TABLE *table, const uchar *old_row, const uchar *new_row) {
  auto loaded = ShannonBase::Imcs::Imcs::instance()->get_rpd_table_by_name(table->s->db.str, table->s->table_name.str);
  if (!loaded) ShannonBase::Recovery::revoke_unregistered_table_image(table->s->db.str, table->s->table_name.str);
  if (ShannonBase::Populate::configured_change_propagation_mode.load() ==
      ShannonBase::Populate::CopyInfoMode::COMMITTED_BINLOG) {
    if (loaded) ShannonBase::Populate::CommittedBinlog::notify_dml(thd, table);
    return;
  }

  // Runs whether or not this table is itself loaded: the parent may live only
  // in InnoDB while the child it cascades into is loaded in Rapid.  Only an
  // UPDATE that moves a referenced unique key can cascade.
  if (table->s->foreign_key_parents != 0 && ParentUniqueKeyChanged(table, old_row, new_row))
    QuarantineCascadeChildren(table, /*for_delete=*/false);

  EnqueueRowChange(thd, table, loaded, RowChangeOper::UPDATE, old_row, new_row, "target partition of an UPDATE");
}

void notify_delete(THD *thd, TABLE *table, const uchar *old_row) {
  auto loaded = ShannonBase::Imcs::Imcs::instance()->get_rpd_table_by_name(table->s->db.str, table->s->table_name.str);
  if (!loaded) ShannonBase::Recovery::revoke_unregistered_table_image(table->s->db.str, table->s->table_name.str);
  if (ShannonBase::Populate::configured_change_propagation_mode.load() ==
      ShannonBase::Populate::CopyInfoMode::COMMITTED_BINLOG) {
    if (loaded) ShannonBase::Populate::CommittedBinlog::notify_dml(thd, table);
    return;
  }

  // Runs whether or not this table is itself loaded: the parent may live only
  // in InnoDB while the child it cascades into is loaded in Rapid.
  QuarantineCascadeChildren(table, /*for_delete=*/true);

  EnqueueRowChange(thd, table, loaded, RowChangeOper::DELETE, old_row, nullptr, "source partition of a DELETE");
}
}  // namespace Service
}  // namespace ShannonBase
