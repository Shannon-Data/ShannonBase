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

#include <string>
#include <unordered_map>

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"

namespace ShannonBase {
namespace Populate {

int CopyInfoParser::parse_table_meta(Rapid_load_context *context, const TABLE *table) {
  assert(table);

  std::string key{table->s->db.str};
  key.append(":").append(table->s->table_name.str);
  if (m_col_offsets.find(key) != m_col_offsets.end() && m_null_byte_offsets.find(key) != m_null_byte_offsets.end() &&
      m_null_bitmasks.find(key) != m_null_bitmasks.end())
    return ShannonBase::SHANNON_SUCCESS;

  m_n_fields = table->s->fields;
  std::vector<ulong> col_offsets(m_n_fields);
  std::vector<ulong> null_byte_offsets(m_n_fields);
  std::vector<ulong> null_bitmasks(m_n_fields);

  for (uint idx = 0; idx < m_n_fields; idx++) {
    Field *fld = table->field[idx];

    if (!fld) {
      sql_print_error("Field[%u] is NULL in table %s.%s", idx, table->s->db.str, table->s->table_name.str);
      return HA_ERR_GENERIC;
    }

    col_offsets[idx] = fld->offset(table->record[0]);
    if (fld->is_nullable()) {
      null_byte_offsets[idx] = fld->null_offset();
      null_bitmasks[idx] = fld->null_bit;
    }
  }

  assert(m_col_offsets.find(key) == m_col_offsets.end());
  m_col_offsets.emplace(key, std::move(col_offsets));

  assert(m_null_byte_offsets.find(key) == m_null_byte_offsets.end());
  m_null_byte_offsets.emplace(key, std::move(null_byte_offsets));

  assert(m_null_bitmasks.find(key) == m_null_bitmasks.end());
  m_null_bitmasks.emplace(key, std::move(null_bitmasks));

  return ShannonBase::SHANNON_SUCCESS;
}

uint CopyInfoParser::parse_copy_info(Rapid_load_context *context, change_record_buff_t::OperType oper_type, byte *start,
                                     byte *end_ptr, byte *new_start, byte *new_end_ptr) {
  // Open target table
  auto key = context->m_schema_name + ":" + context->m_table_name;
  if ((m_col_offsets.find(key) == m_col_offsets.end()) && parse_table_meta(context, context->m_table)) {
    std::string err_msg = "Cannot get the openned table ";
    err_msg.append(context->m_schema_name).append(".").append(context->m_table_name).append(" meta information");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err_msg.c_str());
    return 0;
  }

  // Dispatch by operation type
  auto ret{ShannonBase::SHANNON_SUCCESS};
  switch (oper_type) {
    case change_record_buff_t::OperType::UPDATE:
      ret = parse_and_apply_update(context, context->m_table, start, end_ptr, new_start, new_end_ptr);
      break;
    case change_record_buff_t::OperType::INSERT:
      ret = parse_and_apply_insert(context, context->m_table, start, end_ptr);
      break;
    case change_record_buff_t::OperType::DELETE:
      ret = parse_and_apply_delete(context, context->m_table, start, end_ptr);
      break;
    default:
      sql_print_warning("Unknown operation type in change record");
      assert(false);
      break;
  }
  return ret;
}

int CopyInfoParser::parse_and_apply_update(Rapid_load_context *context, TABLE *table, const byte *old_start,
                                           const byte *old_end_ptr, const byte *new_start, const byte *new_end_ptr) {
  assert((old_end_ptr - old_start) == context->m_table->s->rec_buff_length);
  assert((new_end_ptr - new_start) == context->m_table->s->rec_buff_length);

  std::string sch_tb_name = context->m_sch_tb_name;
  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_rpd_table(sch_tb_name);
  if (!rpd_table) {
    std::string err_msg = "Cannot get the table ";
    err_msg.append(context->m_schema_name).append(".").append(context->m_table_name).append(" from loaded tables");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err_msg.c_str());
    return 0;  // parsed bytes.
  }

  auto rec_len = old_end_ptr - old_start;
  auto global_row_id =
      rpd_table->locate_row(context, (uchar *)old_start, rec_len, m_col_offsets[sch_tb_name].data(), m_n_fields,
                            m_null_byte_offsets[sch_tb_name].data(), m_null_bitmasks[sch_tb_name].data());

  // step 1: to parse the changed fields. <changed col id, new_value>

  ShannonBase::Imcs::RowBuffer new_row_data(m_n_fields);
  new_row_data.copy_from_mysql_fields(context, context->m_table->field, m_n_fields, const_cast<uchar *>(new_start),
                                      m_col_offsets[sch_tb_name].data(), m_null_byte_offsets[sch_tb_name].data(),
                                      m_null_bitmasks[sch_tb_name].data());

  size_t row_size = old_end_ptr - old_start;
  std::unordered_map<uint32_t, ShannonBase::Imcs::RowBuffer::ColumnValue> updates;
  for (size_t idx = 0; idx < m_n_fields; idx++) {
    Field *field = table->field[idx];

    ptrdiff_t offset = m_col_offsets[sch_tb_name][idx];
    size_t field_length = field->pack_length();

    // comp field is changed or not.
    if (std::memcmp(old_start + offset, new_start + offset, field_length) != 0) {  // record has been changed.
      // read the new value.
      auto col_val = new_row_data.get_column_mutable(idx);
      updates.emplace(idx, std::move(*col_val));
    }
  }

  // step 2: update row.
  if (rpd_table->update_row(context, global_row_id, updates)) {
    std::string errmsg;
    errmsg.append("[popragate] update in rapid ")
        .append(context->m_schema_name.c_str())
        .append(".")
        .append(context->m_table_name.c_str())
        .append(" failed");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return 0;
  }
  return row_size;
}

int CopyInfoParser::parse_and_apply_insert(Rapid_load_context *context, TABLE *table, const byte *start,
                                           const byte *end_ptr) {
  std::string sch_tb_name = context->m_sch_tb_name;
  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_rpd_table(sch_tb_name);
  if (!rpd_table) {
    std::string err_msg = "Cannot get the table ";
    err_msg.append(context->m_schema_name).append(".").append(context->m_table_name).append(" from loaded tables");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err_msg.c_str());
    return 0;  // parsed bytes.
  }

  size_t row_size = end_ptr - start;
  assert(row_size == context->m_table->s->rec_buff_length);
  if (rpd_table->insert_row(context, (uchar *)start, row_size, m_col_offsets[sch_tb_name].data(), m_n_fields,
                            m_null_byte_offsets[sch_tb_name].data(),
                            m_null_bitmasks[sch_tb_name].data()) == INVALID_ROW_ID) {
    std::string errmsg;
    errmsg.append("[popragate] inset into rapid ")
        .append(context->m_schema_name.c_str())
        .append(".")
        .append(context->m_table_name.c_str())
        .append(" to imcs failed");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return 0;
  }

  return row_size;
}

int CopyInfoParser::parse_and_apply_delete(Rapid_load_context *context, TABLE *table, const byte *start,
                                           const byte *end_ptr) {
  size_t row_size = end_ptr - start;
  assert(row_size == context->m_table->s->rec_buff_length);

  std::string sch_tb_name = context->m_sch_tb_name;
  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_rpd_table(sch_tb_name);
  if (!rpd_table) {
    std::string err_msg = "Cannot get the table ";
    err_msg.append(context->m_schema_name).append(".").append(context->m_table_name).append(" from loaded tables");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err_msg.c_str());
    return 0;  // parsed bytes.
  }

  auto global_row_id =
      rpd_table->locate_row(context, (uchar *)start, row_size, m_col_offsets[sch_tb_name].data(), m_n_fields,
                            m_null_byte_offsets[sch_tb_name].data(), m_null_bitmasks[sch_tb_name].data());
  if (rpd_table->delete_row(context, global_row_id)) {
    std::string errmsg;
    errmsg.append("[popragate] delete from rapid ")
        .append(context->m_schema_name.c_str())
        .append(".")
        .append(context->m_table_name.c_str())
        .append(" to imcs failed.");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return 0;
  }
  return row_size;
}
}  // namespace Populate
}  // namespace ShannonBase
