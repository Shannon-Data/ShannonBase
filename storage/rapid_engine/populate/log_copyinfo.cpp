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

#include <string>

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/populate/log_copyinfo.h"

namespace ShannonBase {
namespace Populate {

int CopyInfoParser::parse_table_meta(Rapid_load_context *context, const TABLE *table) {
  assert(table);

  m_n_fields = table->s->fields;

  m_col_offsets.resize(m_n_fields);
  m_null_byte_offsets.resize(m_n_fields);
  m_null_bitmasks.resize(m_n_fields);

  for (uint idx = 0; idx < table->s->fields; idx++) {
    auto fld = *(table->field + idx);
    m_col_offsets[idx] = fld->offset(table->record[0]);
    m_null_byte_offsets[idx] = fld->null_offset();
    m_null_bitmasks[idx] = fld->null_bit;
  }

  return ShannonBase::SHANNON_SUCCESS;
}

uint CopyInfoParser::parse_copy_info(Rapid_load_context *context, change_record_buff_t::OperType oper_type, byte *start,
                                     byte *end_ptr) {
  // Open target table
  TABLE *table =
      ShannonBase::Utils::Util::open_table_by_name(current_thd, context->m_schema_name, context->m_table_name, TL_READ);
  if (!table) {
    std::string err_msg = "Cannot open table ";
    err_msg.append(context->m_schema_name).append(".").append(context->m_table_name).append(" for parsing");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err_msg.c_str());
    return HA_ERR_GENERIC;
  }

  TableGuard tb_gurad(current_thd, table);
  context->m_table = table;

  if (parse_table_meta(context, table)) {
    std::string err_msg = "Cannot get the openned table ";
    err_msg.append(context->m_schema_name).append(".").append(context->m_table_name).append(" meta information");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err_msg.c_str());
    return HA_ERR_GENERIC;
  }

  // Dispatch by operation type
  auto ret{ShannonBase::SHANNON_SUCCESS};
  switch (oper_type) {
    case change_record_buff_t::OperType::UPDATE:
      ret = parse_and_apply_update(context, table, start, end_ptr);
      break;
    case change_record_buff_t::OperType::INSERT:
      ret = parse_and_apply_insert(context, table, start, end_ptr);
      break;
    case change_record_buff_t::OperType::DELETE:
      ret = parse_and_apply_delete(context, table, start, end_ptr);
      break;
    default:
      sql_print_warning("Unknown operation type in change record");
      assert(false);
      break;
  }
  return ret;
}

int CopyInfoParser::parse_and_apply_update(Rapid_load_context *context, TABLE *table, const byte *start,
                                           const byte *end_ptr) {
  std::string sch_tb_name = context->m_schema_name;
  sch_tb_name.append(":").append(context->m_table_name);

  size_t row_size = end_ptr - start;
  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_table(sch_tb_name);
  if (!rpd_table) {
    std::string err_msg = "Cannot get the table ";
    err_msg.append(context->m_schema_name).append(".").append(context->m_table_name).append(" from loaded tables");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err_msg.c_str());
    return 0;
  }
  // auto ret = rpd_table->write();

  return row_size;  // parsed_bytes
}

int CopyInfoParser::parse_and_apply_insert(Rapid_load_context *context, TABLE *table, const byte *start,
                                           const byte *end_ptr) {
  std::string sch_tb_name = context->m_schema_name;
  sch_tb_name.append(":").append(context->m_table_name);

  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_table(sch_tb_name);
  if (!rpd_table) {
    std::string err_msg = "Cannot get the table ";
    err_msg.append(context->m_schema_name).append(".").append(context->m_table_name).append(" from loaded tables");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err_msg.c_str());
    return 0;  // parsed bytes.
  }

  size_t row_size = end_ptr - start;
  if (rpd_table->write(context, (uchar *)start, row_size, m_col_offsets.data(), m_n_fields, m_null_byte_offsets.data(),
                       m_null_bitmasks.data())) {
    std::string errmsg;
    errmsg.append("load data from ")
        .append(context->m_schema_name.c_str())
        .append(".")
        .append(context->m_table_name.c_str())
        .append(" to imcs failed.");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return 0;
  }
  return row_size;
}

int CopyInfoParser::parse_and_apply_delete(Rapid_load_context *context, TABLE *table, const byte *start,
                                           const byte *end_ptr) {
  std::string sch_tb_name = context->m_schema_name;
  sch_tb_name.append(":").append(context->m_table_name);

  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_table(sch_tb_name);
  if (!rpd_table) {
    std::string err_msg = "Cannot get the table ";
    err_msg.append(context->m_schema_name).append(".").append(context->m_table_name).append(" from loaded tables");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err_msg.c_str());
    return 0;  // parsed bytes.
  }

  size_t row_size = end_ptr - start;
  if (rpd_table->delete_row(context, (uchar *)start, row_size, m_col_offsets.data(), m_n_fields,
                            m_null_byte_offsets.data(), m_null_bitmasks.data())) {
    std::string errmsg;
    errmsg.append("load data from ")
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
