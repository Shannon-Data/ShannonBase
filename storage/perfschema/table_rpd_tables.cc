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
  @file storage/perfschema/table_rpd_tables.cc
  Table table_rpd_tables (implementation).
*/
#include "storage/perfschema/table_rpd_tables.h"

#include <stddef.h>

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
#include "storage/rapid_engine/include/rapid_table_info.h"
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
    "  POOL_TYPE ENUM(\'SNAPSHOT\', \'TRANSACTIONAL\', \'VOLATILE\') not null default \'SNAPSHOT\',\n"
    "  DATA_PLACEMENT_TYPE BIGINT unsigned not null,\n"
    "  NROWS BIGINT unsigned not null,\n"
    "  LOAD_STATUS ENUM(\'NOLOAD_RPDGSTABSTATE\', \'LOADING_RPDGSTABSTATE\', \'AVAIL_RPDGSTABSTATE\', "
    "\'UNLOADING_RPDGSTABSTATE\', \'INRECOVERY_RPDGSTABSTATE\', \'STALE_RPDGSTABSTATE\', \'UNAVAIL_RPDGSTABSTATE\', "
    "\'RECOVERYFAILED_RPDGSTABSTATE\') not null default \'AVAIL_RPDGSTABSTATE\',\n"
    "  LOAD_PROGRESS INT unsigned not null,\n"
    "  SIZE_BYTES BIGINT unsigned not null,\n"
    "  TRANSFORMATION_BYTES BIGINT unsigned not null,\n"
    "  QUERY_COUNT BIGINT unsigned not null,\n"
    "  LAST_QUERIED timestamp not null,\n"
    "  LOAD_START_TIMESTAMP timestamp not null,\n"
    "  LOAD_END_TIMESTAMP timestamp not null,\n"
    "  RECOVERY_SOURCE ENUM(\'MYSQL\',\'OBJECTSTORAGE\') not null default \'MYSQL\',\n"
    "  RECOVERY_START_TIMESTAMP timestamp not null,\n"
    "  RECOVERY_END_TIMESTAMP timestamp not null,\n"
    "  LOAD_TYPE ENUM(\'USER\',\'SELF\') not null default \'USER\',\n"
    "  LOGICAL_PARTS_LOADED_AT_SCN JSON ,\n"
    "  AUTO_ZMP_COLUMNS JSON ,\n"
    "  ACE_MODEL bool,\n"
    "  STALE_REASON ENUM(\'OK\', \'ERROR_CLUSTER_OOM\', \'ERROR_PKEY_NOT_FOUND\', \'ERROR_UU_OVERLAP\', "
    "\'ERROR_INVALID_CP_PACKET\', \'ERROR_UU_TOO_LARGE\', \'DECOMPRESSION_TIMEOUT\', \'ERROR_CLUSTER_OTHER\', "
    "\'TABLE_TYPE_MISMATCH\', \'RPD_CP_RESET\', \'DROP_PARTITION_FAILED\', \'ADD_PARTITION_FAILED\', "
    "\'ALTER_PARTITION_FAILED\', \'PARTITION_UNLOAD_FAILED\', \'PARTITION_LOAD_FAILED\', \'RELOAD_REQUIRED\', "
    "\'RPD_PARSING_OOM\', \'RPD_PARSER_ERROR\', \'UNIDENTIFIED_ERROR\') not null default \'OK\'\n",
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

PFS_engine_table *table_rpd_tables::create(PFS_engine_table_share *) {
   return new table_rpd_tables(); 
}

table_rpd_tables::table_rpd_tables() : PFS_engine_table(&m_share, &m_pos), m_pos(0), m_next_pos(0) {
  // rpd_tables reports the tables that have been targeted for load into Rapid,
  // in whatever load state they are currently in.  Filtering on
  // stats.state == LOADED hid exactly the states an operator wants to observe
  // (LOADING while a load runs, STALE after a propagation failure), so the
  // filter is on load_status instead: NOLOAD means the table was never loaded
  // into Rapid and belongs only in rpd_mirror.
  for (auto &info : ShannonBase::Autopilot::SelfLoadManager::snapshot()) {
    if (info.meta_info.load_status == ShannonBase::load_status_t::NOLOAD_RPDGSTABSTATE) continue;
    m_tables.push_back(std::move(info));
  }
}

table_rpd_tables::~table_rpd_tables() = default;

void table_rpd_tables::reset_position() {
  m_pos.m_index = 0;
  m_next_pos.m_index = 0;
}

ha_rows table_rpd_tables::get_row_count() { return ShannonBase::Autopilot::SelfLoadManager::table_count(); }

int table_rpd_tables::rnd_next() {
  for (m_pos.set_at(&m_next_pos); m_pos.m_index < row_count(); m_pos.next()) {
    make_row(m_pos.m_index);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_rpd_tables::rnd_pos(const void *pos) {
  if (row_count() == 0) {
    return HA_ERR_END_OF_FILE;
  }

  set_position(pos);
  if (m_pos.m_index >= row_count()) {
    return HA_ERR_RECORD_DELETED;
  }

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

static Json_wrapper auto_zmp_columns_to_json(const std::vector<int> &auto_zmp_columns) {
  Json_array *json_array = new (std::nothrow) Json_array();
  if (!json_array) return Json_wrapper();
  if (auto_zmp_columns.empty()) return Json_wrapper(json_array);

  for (int col_id : auto_zmp_columns) {
    Json_int *json_value = new (std::nothrow) Json_int(col_id);
    if (!json_value) {
      delete json_array;
      return Json_wrapper();
    }
    json_array->append_alias(json_value);
  }
  return Json_wrapper(json_array);
}

int table_rpd_tables::make_row(uint index [[maybe_unused]]) {
  DBUG_TRACE;
  // Set default values.
  if (index >= m_tables.size()) {
    return HA_ERR_END_OF_FILE;
  } else {
    const auto &table_info = m_tables[index];
    m_row.id = table_info.tid;
    const auto &meta = table_info.meta_info;

    m_row.snapshot_scn = meta.snapshot_scn;
    m_row.persisted_scn = meta.persisted_scn;
    m_row.pool_type = static_cast<ulonglong>(meta.pool_type);
    m_row.data_placement_type = meta.data_placement_type;
    m_row.table_nrows = meta.nrows;
    m_row.load_status = (ulonglong)meta.load_status;
    m_row.load_progress = meta.loading_progress * 100;
    m_row.size_byte = meta.size_bytes;
    m_row.transformation_bytes = meta.transformation_bytes;
    // meta.query_cnt / meta.last_queried have no writer anywhere in the tree;
    // the counters SelfLoadManager actually maintains live in the access stats
    // beside them.  rpd_tables describes the table as loaded in Rapid, so use
    // the Rapid-side counters (rpd_mirror reports the DB-System side).
    m_row.query_count = table_info.heatwave_access_count;
    m_row.last_queried = to_milliseconds_timestamp(table_info.last_queried_time_in_rpd);
    m_row.load_start_timestamp = to_milliseconds_timestamp(meta.load_start_stamp);
    m_row.load_end_timestamp = to_milliseconds_timestamp(meta.load_end_stamp);
    m_row.reconvery_start_timestamp = to_milliseconds_timestamp(meta.recovery_start_stamp);
    m_row.reconvery_end_timestamp = to_milliseconds_timestamp(meta.recovery_end_stamp);
    m_row.recovery_source = static_cast<ulonglong>(meta.recovery_source);
    m_row.load_type = static_cast<ulonglong>(meta.load_type);
    m_row.logical_part_loaded_at_scn = logical_part_vector_to_json(meta.logical_part_loaded_at_scn);
    m_row.auto_zmp_columns = auto_zmp_columns_to_json(meta.auto_zmp_columns);
    m_row.ace_model = meta.ace_model;
    m_row.stale_reason = static_cast<ulonglong>(meta.stale_reason);
  }
  return 0;
}

int table_rpd_tables::read_row_values(TABLE *table, unsigned char *buf, Field **fields, bool read_all) {
  Field *f;

  // assert(table->s->null_bytes == 0);
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
        case 3:                                    /**pool_type */
          set_field_enum(f, m_row.pool_type + 1);  // ENUM in MySQL is 1-based indexed.
          break;
        case 4: /**data_placement_type */
          set_field_ulonglong(f, m_row.data_placement_type);
          break;
        case 5: /**table_nrows */
          set_field_ulonglong(f, m_row.table_nrows);
          break;
        case 6: /**load_status */
          set_field_enum(f, m_row.load_status + 1);
          break;
        case 7: /**load_progress */
          set_field_long(f, m_row.load_progress);
          break;
        case 8: /**SIZE_BYTES */
          set_field_ulonglong(f, m_row.size_byte);
          break;
        case 9: /**transformation_bytes */
          set_field_ulonglong(f, m_row.transformation_bytes);
          break;
        case 10: /**querycount */
          set_field_ulonglong(f, m_row.query_count);
          break;
        case 11: /**last_queried */
          set_field_timestamp(f, m_row.last_queried);
          break;
        case 12: /**load_start_timestamp */
          set_field_timestamp(f, m_row.load_start_timestamp);
          break;
        case 13: /**load_end_timestamp */
          set_field_timestamp(f, m_row.load_end_timestamp);
          break;
        case 14: /**RECOVERY_SOURCE */
          set_field_enum(f, m_row.recovery_source + 1);
          break;
        case 15: /**reconvery_start_timestamp */
          set_field_timestamp(f, m_row.reconvery_start_timestamp);
          break;
        case 16: /**reconvery_end_timestamp */
          set_field_timestamp(f, m_row.reconvery_end_timestamp);
          break;
        case 17: /**LOAD_TYPE */
          set_field_enum(f, m_row.load_type + 1);
          break;
        case 18: /**LOGICAL_PARTS_LOADED_AT_SCN */
          set_field_json(f, &m_row.logical_part_loaded_at_scn);
          break;
        case 19: /**AUTO_ZMP_COLUMNS */
          set_field_json(f, &m_row.auto_zmp_columns);
          break;
        case 20: /**ACE_MODEL */
          set_field_tiny(f, m_row.ace_model);
          break;
        case 21: /**STALE_REASON */
          set_field_enum(f, m_row.stale_reason + 1);
          break;
        default:
          assert(false);
      }
    }
  }
  return 0;
}