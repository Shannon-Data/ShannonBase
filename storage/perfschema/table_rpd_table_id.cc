/*
  Copyright (c) 2014, 2023, Oracle and/or its affiliates.

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

  Shannon Data AI.
*/

/**
  @file storage/perfschema/table_rpd_table_id.cc
  Table table_rpd_table_id (implementation).
*/

#include "storage/perfschema/table_rpd_table_id.h"

#include <stddef.h>

#include "thr_lock.h"
#include "my_compiler.h"
#include "my_dbug.h"

#include "sql/field.h"
#include "sql/plugin_table.h"
#include "sql/table.h"
#include "sql/sql_table.h" // meta_rpd_columns_infos

#include "storage/perfschema/pfs_instr.h"
#include "storage/perfschema/pfs_instr_class.h"
#include "storage/perfschema/table_helper.h"
#include "storage/rapid_engine/include/rapid_stats.h"
/*
  Callbacks implementation for RPD_TABLE_ID.
*/

THR_LOCK table_rpd_table_id::m_table_lock;

Plugin_table table_rpd_table_id::m_table_def(
    /* Schema name */
    "performance_schema",
    /* Name */
    "rpd_table_id",
    /* Definition */
    "  ID BIGINT unsigned not null,\n"
    "  NAME CHAR(64) collate utf8mb4_bin not null,\n"
    "  SCHEMA_NAME CHAR(64) collate utf8mb4_bin not null,\n"
    "  TABLE_NAME CHAR(64) collate utf8mb4_bin not null\n",
    /* Options */
    " ENGINE=PERFORMANCE_SCHEMA",
    /* Tablespace */
    nullptr);

PFS_engine_table_share table_rpd_table_id::m_share = {
    &pfs_readonly_acl,
    &table_rpd_table_id::create,
    nullptr, /* write_row */
    nullptr, /* delete_all_rows */
    table_rpd_table_id::get_row_count,
    sizeof(pos_t), /* ref length */
    &m_table_lock,
    &m_table_def,
    true, /* perpetual */
    PFS_engine_table_proxy(),
    {0},
    false /* m_in_purgatory */
};

PFS_engine_table *table_rpd_table_id::create(
    PFS_engine_table_share *) {
  return new table_rpd_table_id();
}

table_rpd_table_id::table_rpd_table_id()
    : PFS_engine_table(&m_share, &m_pos), m_pos(0), m_next_pos(0) {
  m_row.table_id = 0;
  memset (m_row.full_table_name, 0x0, NAME_LEN);
  memset (m_row.schema_name, 0x0, NAME_LEN);
  memset (m_row.table_name, 0x0, NAME_LEN);
}

table_rpd_table_id::~table_rpd_table_id() {
  //clear.
}

void table_rpd_table_id::reset_position() {
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
}

ha_rows table_rpd_table_id::get_row_count() {
  return ShannonBase::meta_rpd_columns_infos.size();
}

int table_rpd_table_id::rnd_next() {
  for (m_pos.set_at(&m_next_pos); m_pos.m_index < get_row_count();
       m_pos.next()) {
    make_row(m_pos.m_index);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_rpd_table_id::rnd_pos(const void *pos) {
  if (get_row_count() == 0) {
    return HA_ERR_END_OF_FILE;
  }

  if (m_pos.m_index >= get_row_count()) {
    return HA_ERR_RECORD_DELETED;
  }

  set_position(pos);
  return make_row(m_pos.m_index);
}

int table_rpd_table_id::make_row(uint index[[maybe_unused]]) {
  DBUG_TRACE;
  // Set default values.
  if (index >= ShannonBase::meta_rpd_columns_infos.size()) {
    return HA_ERR_END_OF_FILE;
  } else {
    m_row.table_id = ShannonBase::meta_rpd_columns_infos[index].table_id;

    std::string full_name;
    full_name += ShannonBase::meta_rpd_columns_infos[index].schema_name;
    full_name += "\\";
    full_name += ShannonBase::meta_rpd_columns_infos[index].table_name;
    strncpy(m_row.full_table_name, full_name.c_str(), full_name.length());
    strncpy(m_row.schema_name, ShannonBase::meta_rpd_columns_infos[index].schema_name,
            strlen(ShannonBase::meta_rpd_columns_infos[index].schema_name));
    strncpy(m_row.table_name, ShannonBase::meta_rpd_columns_infos[index].table_name,
            strlen(ShannonBase::meta_rpd_columns_infos[index].table_name));
  }
  return 0;
}

int table_rpd_table_id::read_row_values(TABLE *table,
                                         unsigned char *buf,
                                         Field **fields,
                                         bool read_all) {
  Field *f;

  //assert(table->s->null_bytes == 0);
  buf[0] = 0;

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      switch (f->field_index()) {
        case 0: /** TABLE_ID */
          set_field_ulonglong(f, m_row.table_id);
          break;
        case 1: /** full table name */
          set_field_char_utf8mb4(f, m_row.full_table_name, strlen(m_row.full_table_name));
          break;
        case 2: /** schema_name */
          set_field_char_utf8mb4(f, m_row.schema_name, strlen(m_row.schema_name));
          break;
        case 3: /** table_name */
          set_field_char_utf8mb4(f, m_row.table_name, strlen(m_row.table_name));
          break;
        default:
          assert(false);
      }
    }
  }
  return 0;
}