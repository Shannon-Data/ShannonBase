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

#ifndef __TABLE_SHANNONBASE_RPD_COLUMNS_H__
#define __TABLE_SHANNONBASE_RPD_COLUMNS_H__

/**
  @file storage/perfschema/table_rpd_columns.h
  Table table_rpd_columns (declarations).
*/

#include <stddef.h>
#include <sys/types.h>

#include "my_base.h"
#include "mysql_com.h" // NAME_LENGTH
#include "my_inttypes.h"
#include "mysql_com.h"
#include "sql/sql_const.h"  // UUID_LENGTH
#include "storage/perfschema/pfs_engine_table.h"

class Field;
class Plugin_table;
struct TABLE;
struct THR_LOCK;

/**
  @addtogroup performance_schema_tables
  @{
*/

/**
  A row in node status table. The fields with string values have an additional
  length field denoted by @<field_name@>_length.
*/

struct st_row_rpd_columns {
  ulonglong table_id {0};
  ulonglong colum_id {0};
  ulonglong ndv {0};
  char encoding[NAME_LEN] ={0}; 
  uint data_placement_index {0};
  uint dict_size_bytes {0};
};

/** Table PERFORMANCE_SCHEMA.RPD_COLUMNS. */
class table_rpd_columns : public PFS_engine_table {
  typedef PFS_simple_index pos_t;

 private:
  int make_row(uint index);

  /** Table share lock. */
  static THR_LOCK m_table_lock;
  /** Table definition. */
  static Plugin_table m_table_def;

  /** Current row */
  st_row_rpd_columns m_row;
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

  int read_row_values(TABLE *table, unsigned char *buf, Field **fields,
                      bool read_all) override;

  table_rpd_columns();

 public:
  ~table_rpd_columns() override;

  /** Table share. */
  static PFS_engine_table_share m_share;
  static PFS_engine_table *create(PFS_engine_table_share *);
  static ha_rows get_row_count();
  int rnd_next() override;
  int rnd_pos(const void *pos) override;
  void reset_position() override;
};

/** @} */
#endif
