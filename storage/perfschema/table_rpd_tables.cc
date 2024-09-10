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
  @file storage/perfschema/table_rpd_tables.cc
  Table table_rpd_tables (implementation).
*/
#include "storage/perfschema/table_rpd_tables.h"

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
  Callbacks implementation for RPD_TABLES.
*/

THR_LOCK table_rpd_tables::m_table_lock;

Plugin_table table_rpd_tables::m_table_def(
    /* Schema name */
    "performance_schema",
    /* Name */
    "rpd_tables",
    /* Definition */
    "  ID BIGINT unsigned not null,\n"
    "  SNAPSHOT_SCN BIGINT unsigned not null,\n"
    "  PERSISTED_SCN BIGINT unsigned not null,\n"
    "  POOL_TYPE BIGINT unsigned not null,\n"
    "  DATA_PLACEMENT_TYPE BIGINT unsigned not null,\n"
    "  T_NROWS BIGINT unsigned not null,\n"
    "  LOAD_STATUS BIGINT unsigned not null,\n"
    "  LOAD_PROGRESS BIGINT unsigned not null,\n"
    "  SIZE_BYTES BIGINT unsigned not null,\n"
    "  TRANSFORMATION_BYTES BIGINT unsigned not null,\n"
    "  E_NROWS BIGINT unsigned not null,\n"
    "  QUERY_COUNT BIGINT unsigned not null,\n"
    "  LAST_QUERIED timestamp not null,\n"
    "  LOAD_START_TIMESTAMP timestamp not null,\n"
    "  LOAD_END_TIMESTAMP timestamp not null,\n"
    "  RECOVERY_SOURCE CHAR(128) collate utf8mb4_bin not null,\n"
    "  RECOVERY_START_TIMESTAMP timestamp not null,\n"
    "  RECOVERY_END_TIMESTAMP timestamp not null\n",
    /* Options */
    " ENGINE=PERFORMANCE_SCHEMA",
    /* Tablespace */
    nullptr);

PFS_engine_table_share table_rpd_tables::m_share = {
    &pfs_readonly_acl,
    &table_rpd_tables::create,
    nullptr, /* write_row */
    nullptr, /* delete_all_rows */
    table_rpd_tables::get_row_count,
    sizeof(pos_t), /* ref length */
    &m_table_lock,
    &m_table_def,
    true, /* perpetual */
    PFS_engine_table_proxy(),
    {0},
    false /* m_in_purgatory */
};

PFS_engine_table *table_rpd_tables::create(
    PFS_engine_table_share *) {
  return new table_rpd_tables();
}

table_rpd_tables::table_rpd_tables()
    : PFS_engine_table(&m_share, &m_pos), m_pos(0), m_next_pos(0) {
  m_row.id = 0;
  m_row.snapshot_scn = 0;
  m_row.persisted_scn = 0;
  m_row.pool_type = 0;
  m_row.data_placement_type = 0;
  m_row.table_nrows = 0;
  m_row.load_status = 0;
  m_row.load_progress = 0;
  m_row.size_byte = 0;
  m_row.transformation_bytes = 0;
  m_row.extranl_nrows = 0;
  m_row.query_count = 0;
  m_row.last_queried = 0;
  m_row.load_start_timestamp = 0;
  m_row.load_end_timestamp = 0;
  m_row.reconvery_start_timestamp = 0;
  m_row.reconvery_end_timestamp = 0;
  memset (m_row.recovery_source, 0x0, NAME_LEN);
}

table_rpd_tables::~table_rpd_tables() {
  //clear.
}

void table_rpd_tables::reset_position() {
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
}

ha_rows table_rpd_tables::get_row_count() {
  return ShannonBase::meta_rpd_columns_infos.size();
}

int table_rpd_tables::rnd_next() {
  for (m_pos.set_at(&m_next_pos); m_pos.m_index < get_row_count();
       m_pos.next()) {
    make_row(m_pos.m_index);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_rpd_tables::rnd_pos(const void *pos) {
  if (get_row_count() == 0) {
    return HA_ERR_END_OF_FILE;
  }

  if (m_pos.m_index >= get_row_count()) {
    return HA_ERR_RECORD_DELETED;
  }

  set_position(pos);
  return make_row(m_pos.m_index);
}

int table_rpd_tables::make_row(uint index[[maybe_unused]]) {
  DBUG_TRACE;
  // Set default values.
  if (index >= ShannonBase::meta_rpd_columns_infos.size()) {
    return HA_ERR_END_OF_FILE;
  } else {
    m_row.id = ShannonBase::meta_rpd_columns_infos[index].table_id;
    m_row.snapshot_scn = 0;
    m_row.persisted_scn = 0;
    m_row.pool_type = 0;
    m_row.data_placement_type = 0;
    m_row.table_nrows = 0;
    m_row.load_status = 0;
    m_row.load_progress = 0;
    m_row.size_byte = 0;
    m_row.transformation_bytes = 0;
    m_row.extranl_nrows = 0;
    m_row.query_count = 0;
    m_row.last_queried = 0;
    m_row.load_start_timestamp = 0;
    m_row.load_end_timestamp = 0;
    m_row.reconvery_start_timestamp = 0;
    m_row.reconvery_end_timestamp = 0;
    strncpy(m_row.recovery_source, "InnoDB/ObjectStorage", strlen("InnoDB/ObjectStorage") + 1);
  }

  return 0;
}

int table_rpd_tables::read_row_values(TABLE *table,
                                         unsigned char *buf,
                                         Field **fields,
                                         bool read_all) {
  Field *f;

  //assert(table->s->null_bytes == 0);
  buf[0] = 0;

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      switch (f->field_index()) {
        case 0: /** ID */
          set_field_ulonglong(f, m_row.id);
          break;
        case 1: /** SNAPSHOT_SCN */
          set_field_ulonglong(f, m_row.snapshot_scn);
          break;
        case 2: /** PERSISTED_SCN */
          set_field_ulonglong(f, m_row.persisted_scn);
          break;
        case 3: /**pool_type */
          set_field_ulonglong(f, m_row.pool_type);
          break;
        case 4: /**data_placement_type */
          set_field_ulonglong(f, m_row.data_placement_type);
          break;
        case 5: /**table_nrows */
          set_field_ulonglong(f, m_row.table_nrows);
          break;
        case 6: /**load_status */
          set_field_ulonglong(f, m_row.load_status);
          break;
        case 7: /**load_progress */
          set_field_ulonglong(f, m_row.load_progress);
          break;
        case 8: /**SIZE_BYTES */
          set_field_ulonglong(f, m_row.size_byte);
          break;
        case 9: /**transformation_bytes */
          set_field_ulonglong(f, m_row.transformation_bytes);
          break;
        case 10: /**extranl_nrows */
          set_field_ulonglong(f, m_row.extranl_nrows);
          break;
        case 11: /**query_count */
          set_field_ulonglong(f, m_row.query_count);
          break;
        case 12: /**last_queried */
          set_field_timestamp(f, m_row.last_queried);
          break;
        case 13: /**load_start_timestamp */
          set_field_timestamp(f, m_row.load_start_timestamp);
          break;
        case 14: /**load_end_timestamp */
          set_field_timestamp(f, m_row.load_end_timestamp);
          break;
        case 15: /**RECOVERY_SOURCE */
          set_field_char_utf8mb4(f, m_row.recovery_source, strlen(m_row.recovery_source));
          break;
        case 16: /**reconvery_start_timestamp */
          set_field_timestamp(f, m_row.reconvery_start_timestamp);
          break;
        case 17: /**reconvery_end_timestamp */
          set_field_timestamp(f, m_row.reconvery_end_timestamp);
          break;
        default:
          assert(false);
      }
    }
  }
  return 0;
}