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

#include "my_compiler.h"
#include "my_dbug.h"
#include "thr_lock.h"

#include "sql/field.h"
#include "sql/plugin_table.h"
#include "sql/sql_table.h"
#include "sql/table.h"

#include "storage/perfschema/pfs_instr.h"
#include "storage/perfschema/pfs_instr_class.h"
#include "storage/perfschema/table_helper.h"

#include "storage/rapid_engine/autopilot/loader.h"

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
    "  IMPORTANCE DOUBLE unsigned not null,\n"
    "  LAST_QUERIED timestamp not null,\n"
    "  LAST_QUERIED_IN_HEATWAVE timestamp not null,\n"
    "  STATE ENUM(\'NOT_LOADED\',\'LOADED\') not null default \'NOT_LOADED\',\n"
    "  RECOMMENDED_READ_THREADS INT NOT NULL,\n"
    "  QUERIED_PARTITIONS JSON\n",
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

PFS_engine_table *table_rpd_mirror::create(PFS_engine_table_share *) { return new table_rpd_mirror(); }

table_rpd_mirror::table_rpd_mirror() : PFS_engine_table(&m_share, &m_pos), m_pos(0), m_next_pos(0) {
  m_it = ShannonBase::Autopilot::SelfLoadManager::tables().begin();
}

table_rpd_mirror::~table_rpd_mirror() {
  // clear.
}

void table_rpd_mirror::reset_position() {
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
  m_it = ShannonBase::Autopilot::SelfLoadManager::tables().begin();
}

ha_rows table_rpd_mirror::get_row_count() { return ShannonBase::Autopilot::SelfLoadManager::tables().size(); }

int table_rpd_mirror::rnd_next() {
  for (m_pos.set_at(&m_next_pos); m_pos.m_index < get_row_count(); m_pos.next()) {
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

static uint64_t to_milliseconds_timestamp(const std::chrono::system_clock::time_point &tp) {
  auto duration = tp.time_since_epoch();
  auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration);
  return static_cast<uint64_t>(micros.count());
}

static Json_wrapper logical_part_vector_to_json(
    const std::vector<ShannonBase::logical_part_loaded_t> &logical_part_loaded_at_scn) {
  Json_array *json_array = new (std::nothrow) Json_array();
  if (!json_array) return Json_wrapper();

  if (logical_part_loaded_at_scn.empty()) return Json_wrapper(json_array);
  for (const auto &part : logical_part_loaded_at_scn) {
    Json_object *json_obj = new (std::nothrow) Json_object();
    if (!json_obj) {
      delete json_array;
      return Json_wrapper();
    }

    json_obj->add_alias("id", new (std::nothrow) Json_uint(part.id));
    json_obj->add_alias("name", new (std::nothrow) Json_string(part.name.c_str(), part.name.length()));
    json_obj->add_alias("scn", new (std::nothrow) Json_uint(part.load_scn));
    if (part.load_type == ShannonBase::load_type_t::USER)
      json_obj->add_alias("type", new (std::nothrow) Json_string("USER"));
    else
      json_obj->add_alias("type", new (std::nothrow) Json_string("SELF"));

    json_array->append_alias(json_obj);
  }
  return Json_wrapper(json_array);
}

int table_rpd_mirror::make_row(uint index [[maybe_unused]]) {
  DBUG_TRACE;
  auto rpd_mirr_table = m_it->second.get();

  memset(m_row.schema_name, 0x0, NAME_LEN);
  strncpy(m_row.schema_name, rpd_mirr_table->schema_name.c_str(),
          (rpd_mirr_table->schema_name.length() > NAME_LEN) ? NAME_LEN : rpd_mirr_table->schema_name.length());

  memset(m_row.table_name, 0x0, NAME_LEN);
  strncpy(m_row.table_name, rpd_mirr_table->table_name.c_str(),
          (rpd_mirr_table->table_name.length() > NAME_LEN) ? NAME_LEN : rpd_mirr_table->table_name.length());

  m_row.msyql_access_count = rpd_mirr_table->stats.mysql_access_count.load();
  m_row.rpd_access_count = rpd_mirr_table->stats.heatwave_access_count.load();

  m_row.importance = rpd_mirr_table->stats.importance;

  m_row.last_queried_in_rpd_timestamp = to_milliseconds_timestamp(rpd_mirr_table->stats.last_queried_time_in_rpd);
  m_row.last_queried_timestamp = to_milliseconds_timestamp(rpd_mirr_table->stats.last_queried_time);

  if (rpd_mirr_table->stats.state == ShannonBase::table_access_stats_t::NOT_LOADED)
    m_row.state = 0;  // STAT_ENUM::NOT_LOADED;
  else if (rpd_mirr_table->stats.state == ShannonBase::table_access_stats_t::LOADED)
    m_row.state = 1;  // STAT_ENUM::LOADED;

  m_row.recommend_thr_num = ShannonBase::Autopilot::SelfLoadManager::get_innodb_thread_num();
  // auto tid =0;
  std::string sch_tb = m_row.schema_name;
  sch_tb = sch_tb + ":" + m_row.table_name;
  auto parts = ShannonBase::Autopilot::SelfLoadManager::tables()[sch_tb]->meta_info.logical_part_loaded_at_scn;
  m_row.query_partitions = logical_part_vector_to_json(parts);

  std::advance(m_it, 1);
  return 0;
}

int table_rpd_mirror::read_row_values(TABLE *table, unsigned char *buf, Field **fields, bool read_all) {
  Field *f;

  // assert(table->s->null_bytes == 0);
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
        case 4: /**importance */
          set_field_double(f, m_row.importance);
          break;
        case 5: /**last_queried_timestamp */
          set_field_timestamp(f, m_row.last_queried_timestamp);
          break;
        case 6: /**last_queried_in_rpd_timestamp */
          set_field_timestamp(f, m_row.last_queried_in_rpd_timestamp);
          break;
        case 7: /**state */
          set_field_enum(f, m_row.state + 1);
          break;
        case 8: /**recommend read thread */
          set_field_long(f, m_row.recommend_thr_num);
          break;
        case 9: /**recommend read thread */
          set_field_json(f, &m_row.query_partitions);
          break;
        default:
          assert(false);
      }
    }
  }
  return 0;
}