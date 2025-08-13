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

  Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.
*/

/**
  @file storage/perfschema/table_rpd_mirror.cc
  Table table_rpd_mirror (implementation).
*/
#include "storage/perfschema/table_rpd_mirror.h"

#include <stddef.h>
#include <chrono>
#include <ctime>

#include "thr_lock.h"
#include "my_compiler.h"
#include "my_dbug.h"

#include "sql/field.h"
#include "sql/plugin_table.h"
#include "sql/table.h"
#include "sql/sql_table.h" // rpd_columns_info

#include "storage/perfschema/pfs_instr.h"
#include "storage/perfschema/pfs_instr_class.h"
#include "storage/perfschema/table_helper.h"

/*
  Callbacks implementation for RPD_TABLES.
*/

THR_LOCK table_rpd_mirror::m_table_lock;

Plugin_table table_rpd_mirror::m_table_def(
    /* Schema name */
    "performance_schema",
    /* Name */
    "rpd_mirror",
    /* Definition */
    "  SCHEMA_NAME CHAR(128) collate utf8mb4_bin not null,\n"
    "  TABLE_NAME CHAR(128) collate utf8mb4_bin not null,\n"
    "  MYSQL_ACCESS_COUNT BIGINT unsigned not null,\n"
    "  HEATWAVE_ACCESS_COUNT BIGINT unsigned not null,\n"
    "  LAST_QUERIED timestamp not null,\n"
    "  LAST_QUERIED_IN_RAPID timestamp not null,\n"
    "  STATE ENUM('NOT_LOADED', 'LOADED', 'INSUFFICIENT_MEMORY') NOT NULL\n",
    /* Options */
    " ENGINE=PERFORMANCE_SCHEMA",
    /* Tablespace */
    nullptr);

PFS_engine_table_share table_rpd_mirror::m_share = {
    &pfs_readonly_acl,
    &table_rpd_mirror::create,
    nullptr, /* write_row */
    nullptr, /* delete_all_rows */
    table_rpd_mirror::get_row_count,
    sizeof(pos_t), /* ref length */
    &m_table_lock,
    &m_table_def,
    true, /* perpetual */
    PFS_engine_table_proxy(),
    {0},
    false /* m_in_purgatory */
};

PFS_engine_table *table_rpd_mirror::create(
    PFS_engine_table_share *) {
  return new table_rpd_mirror();
}

table_rpd_mirror::table_rpd_mirror()
    : PFS_engine_table(&m_share, &m_pos), m_pos(0), m_next_pos(0) {
  m_row.msyql_access_count = 0;
  m_row.rpd_access_count = 0;
  m_row.last_queried_timestamp = 0;
  m_row.last_queried_in_rpd_timestamp = 0;
  m_row.state = STAT_ENUM::NOT_LOADED;
  memset (m_row.schema_name, 0x0, NAME_LEN);
  memset (m_row.table_name, 0x0, NAME_LEN);
  m_it = ShannonBase::Autopilot::SelfLoadManager::instance()->get_all_tables().begin();
}

table_rpd_mirror::~table_rpd_mirror() {
  //clear.
}

void table_rpd_mirror::reset_position() {
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
  m_it = ShannonBase::Autopilot::SelfLoadManager::instance()->get_all_tables().begin();
}

ha_rows table_rpd_mirror::get_row_count() {
  return ShannonBase::Autopilot::SelfLoadManager::instance()->get_all_tables().size();
}

int table_rpd_mirror::rnd_next() {
  for (m_pos.set_at(&m_next_pos); m_pos.m_index < get_row_count();
       m_pos.next()) {
    make_row(m_pos.m_index);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_rpd_mirror::rnd_pos(const void *pos) {
  if (get_row_count() == 0) {
    return HA_ERR_END_OF_FILE;
  }

  if (m_pos.m_index >= get_row_count()) {
    return HA_ERR_RECORD_DELETED;
  }

  set_position(pos);
  return make_row(m_pos.m_index);
}

int table_rpd_mirror::make_row(uint index[[maybe_unused]]) {
  DBUG_TRACE;
  assert(index < ShannonBase::Autopilot::SelfLoadManager::instance()->get_all_tables().size());

  auto rpd_mirr_table = m_it->second.get();

  memset(m_row.schema_name, 0x0, NAME_LEN);
  strncpy(m_row.schema_name, rpd_mirr_table->schema_name.c_str(),
         (rpd_mirr_table->schema_name.length() > NAME_LEN) ? NAME_LEN : rpd_mirr_table->schema_name.length());

  memset(m_row.table_name, 0x0, NAME_LEN);
  strncpy(m_row.table_name, rpd_mirr_table->table_name.c_str(),
         (rpd_mirr_table->table_name.length() > NAME_LEN) ? NAME_LEN : rpd_mirr_table->table_name.length());

  auto last_qt_rpd = rpd_mirr_table->stats.last_queried_time_in_rpd.time_since_epoch();
  m_row.last_queried_in_rpd_timestamp =
    std::chrono::duration_cast<std::chrono::duration<ulonglong>>(last_qt_rpd).count();

  auto last_qt = rpd_mirr_table->stats.last_queried_time.time_since_epoch();
  m_row.last_queried_timestamp =
    std::chrono::duration_cast<std::chrono::duration<ulonglong>>(last_qt).count();

  m_row.msyql_access_count = rpd_mirr_table->stats.mysql_access_count.load();
  m_row.rpd_access_count = rpd_mirr_table->stats.heatwave_access_count.load();

  if (rpd_mirr_table->stats.state == ShannonBase::Autopilot::TableAccessStats::NOT_LOADED)
    m_row.state = STAT_ENUM::NOT_LOADED;
  else if (rpd_mirr_table->stats.state == ShannonBase::Autopilot::TableAccessStats::LOADED)
    m_row.state = STAT_ENUM::LOADED;
  else
    m_row.state = STAT_ENUM::INSUFFICIENT_MEMORY;

  std::advance(m_it, 1);
  return 0;
}

int table_rpd_mirror::read_row_values(TABLE *table,
                                         unsigned char *buf,
                                         Field **fields,
                                         bool read_all) {
  Field *f;

  //assert(table->s->null_bytes == 0);
  buf[0] = 0;

  for (; (f = *fields); fields++) {
    if (read_all || bitmap_is_set(table->read_set, f->field_index())) {
      switch (f->field_index()) {
        case 0: /** SCHEMA_NAME */
          set_field_char_utf8mb4(f, m_row.schema_name, strlen(m_row.schema_name));
          break;
        case 1: /** TABLE_NAME */
          set_field_char_utf8mb4(f, m_row.table_name, strlen(m_row.table_name));
          break;
        case 2: /** msyql_access_count */
          set_field_ulonglong(f, m_row.msyql_access_count);
          break;
        case 3: /**rpd_access_count */
          set_field_ulonglong(f, m_row.rpd_access_count);
          break;
        case 4: /**last_queried_timestamp */
          set_field_timestamp(f, m_row.last_queried_timestamp);
          break;
        case 5: /**last_queried_in_rpd_timestamp */
          set_field_timestamp(f, m_row.last_queried_in_rpd_timestamp);
          break;
        case 6: /**state */
          set_field_enum(f, m_row.state);
          break;
        default:
          assert(false);
      }
    }
  }
  return 0;
}