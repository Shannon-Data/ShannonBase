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
#include "storage/rapid_engine/populate/log_copyinfo.h"

#include <sstream>
#include <string>
#include <unordered_map>

#include "storage/innobase/handler/ha_innodb.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/populate/log_populate.h"

namespace ShannonBase {
namespace Populate {

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
  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_rpd_table(table_id);
  if (!rpd_table) {
    if (!ShannonBase::Populate::pop_buff_contains(table_id)) return old_end_ptr - old_start;
    std::ostringstream oss;
    oss << "Cannot get the table " << context->m_schema_name << "." << context->m_table_name << " from loaded tables";
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), oss.str().c_str());
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

<<<<<<< HEAD
  // step 1b: swap ART entries for any secondary index whose key actually changes.
=======
  // step 1b: swap ART entries for any secondary index whose key actually
  // changes. The row keeps its physical rowid; imcu->update_row() (step 2)
  // already versions the CU column data correctly for MVCC, so the only gap
  // is the ART key itself, which is rowid-only with no per-key version.
  // Doing an in-place remove(old key)+insert(new key) on just the affected
  // index -- rather than rebuilding every index via delete_row()+insert_row()
  // -- avoids creating a second, duplicate PRIMARY KEY leaf entry for this
  // row: ART_search() returns leaf->values[0] (the *oldest* inserted value)
  // for a duplicate key, so a second PK insert would make future PK lookups
  // silently resolve to the stale, since-deleted physical row.
  //
  // Known limitation (matches the pre-existing DELETE/old-ReadView gap
  // documented in shannon_art_reverse_cursor.test): a transaction with a
  // ReadView older than this change that scans the affected index by the old
  // or new value around the time of the swap can observe an inconsistent
  // result, because the ART leaf itself carries no version information.
>>>>>>> dff06c5ea (feat(refactor):refactor rapid engine)
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
  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_rpd_table(table_id);
  if (!rpd_table) {
    if (!ShannonBase::Populate::pop_buff_contains(table_id)) return end_ptr - start;
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
  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_rpd_table(table_id);
  if (!rpd_table) {
    // Table may have been unloaded between when the record was enqueued
    // and now — drop the record gracefully.
    if (!ShannonBase::Populate::pop_buff_contains(table_id)) return row_size;
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
}  // namespace Populate
}  // namespace ShannonBase
