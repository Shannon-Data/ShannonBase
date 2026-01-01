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

#ifndef __TABLE_SHANNONBASE_RPD_TABLES_H__
#define __TABLE_SHANNONBASE_RPD_TABLES_H__
/**
  @file storage/perfschema/table_rpd_table.h
  Table table_rpd_table (declarations).
*/

#include <stddef.h>
#include <sys/types.h>

#include "my_base.h"
#include "my_inttypes.h"
#include "mysql_com.h"  // NAME_LENGTH
#include "mysql_com.h"
#include "sql-common/json_dom.h"
#include "sql/sql_const.h"                        // UUID_LENGTH
#include "storage/perfschema/pfs_engine_table.h"  // json.

/**
  @addtogroup performance_schema_tables
  @{
*/

/**
  A row in node status table. The fields with string values have an additional
  length field denoted by @<field_name@>_length.
*/
struct st_row_rpd_tables {
  ulonglong id{0};
  ulonglong snapshot_scn{0};
  ulonglong persisted_scn{0};
  ulonglong pool_type{0};
  ulonglong data_placement_type{0};
  ulonglong table_nrows{0};
  ulonglong load_status{0};
  ulonglong load_progress{0};
  ulonglong size_byte{0};
  ulonglong transformation_bytes{0};
  ulonglong query_count{0};
  double last_queried{0};
  double load_start_timestamp{0};
  double load_end_timestamp{0};
  ulonglong recovery_source;
  double reconvery_start_timestamp{0};
  double reconvery_end_timestamp{0};
  ulonglong load_type{0};
  Json_wrapper logical_part_loaded_at_scn;
  Json_wrapper auto_zmp_columns;
  bool ace_model{false};
};

/** Table PERFORMANCE_SCHEMA.RPD_TABLES. */
class table_rpd_tables : public PFS_engine_table {
  typedef PFS_simple_index pos_t;

 private:
  int make_row(uint index);

  /** Table share lock. */
  static THR_LOCK m_table_lock;
  /** Table definition. */
  static Plugin_table m_table_def;

  /** Current row */
  st_row_rpd_tables m_row;
  /** Current position. */
  pos_t m_pos;
  /** Next position. */
  pos_t m_next_pos;

 protected:
  /**
    Read the current row values.
    @param table            Table handle
    @param buf              row buffer
    @param fields           Table fields
    @param read_all         true if all columns are read.
  */

  int read_row_values(TABLE *table, unsigned char *buf, Field **fields, bool read_all) override;

  table_rpd_tables();

 public:
  ~table_rpd_tables() override;

  /** Table share. */
  static PFS_engine_table_share m_share;
  static PFS_engine_table *create(PFS_engine_table_share *);
  static ha_rows get_row_count();
  int rnd_next() override;
  int rnd_pos(const void *pos) override;
  void reset_position() override;
};

/** @} */
#endif  //__TABLE_SHANNONBASE_RPD_TABLES_H__