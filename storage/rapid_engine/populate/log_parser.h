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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.
*/

#ifndef __SHANNONBASE_LOG_PARSER_H__
#define __SHANNONBASE_LOG_PARSER_H__

#include "storage/innobase/include/buf0buf.h"
#include "storage/innobase/include/log0types.h"
#include "storage/innobase/include/mtr0types.h"
#include "storage/innobase/include/trx0types.h"
#include "storage/innobase/include/univ.i"

class Field;
namespace ShannonBase {
namespace Populate {
/**
 * To parse the redo log file, it used to populate the changes from ionnodb
 * to rapid.
 */
class LogParser {
 public:
  bool parse_redo(log_t *log_ptr, lsn_t target_lsn, lsn_t rapid_lsn);
 private:
  int handle_multi_rec(byte *ptr, byte *end_ptr, size_t *len, lsn_t start_lsn);
  size_t compute_rapid_event_slot(lsn_t lsn) {
    return ((lsn - 1) / OS_FILE_LOG_BLOCK_SIZE) &
           (INNODB_LOG_EVENTS_DEFAULT - 1);
  }
  ulint parse_single_redo(mlog_id_t *type, byte *ptr, byte *end_ptr,
                          space_id_t *space_id, page_no_t *page_no, byte **body,
                          lsn_t start_lsn, bool apply_to_rapid);

  int handle_single_rec(byte *ptr, byte *end_ptr, size_t *len, lsn_t start_lsn);
  void log_allocate_rapid_events(log_t &log);
  lsn_t parse_redo_and_apply(log_t *log_ptr, lsn_t start_lsn, lsn_t target_lsn);
  byte *parse_log_body(mlog_id_t type, byte *ptr, byte *end_ptr,
                       space_id_t space_id, page_no_t page_no,
                       ulint parsed_bytes, lsn_t start_lsn,
                       bool apply_to_rapid);

  bool check_encryption(page_no_t page_no, space_id_t space_id,
                        const byte *start, const byte *end);

  byte *parse_insert_rec(bool is_short,       /*!< in: true if short inserts */
                         const byte *ptr,     /*!< in: buffer */
                         const byte *end_ptr, /*!< in: buffer end */
                         buf_block_t *block,  /*!< in: page or NULL */
                         dict_index_t *index, /*!< in: record descriptor */
                         bool apply_to_rapid, mtr_t &mtr);

  byte *parse_delete_rec(
      byte *ptr,                /*!< in: buffer */
      byte *end_ptr,            /*!< in: buffer end */
      page_t *page,             /*!< in/out: page or NULL */
      page_zip_des_t *page_zip, /*!< in/out: compressed page, or NULL */
      buf_block_t *block,
      dict_index_t *index, /*!< in: index corresponding to page */
      bool apply_to_rapid, mtr_t &mtr);

  void find_index(uint64 idx_id, const dict_index_t **index);
  int apply_insert(const byte *rec, dict_index_t *index, ulint *offsets,
                   const dict_index_t *real_index);
  int apply_delete(const byte *rec, const dict_index_t *index, ulint *offsets);

  uint64 get_trx_id(const rec_t *rec, const dict_index_t *rec_index,
                    const ulint *offsets);
  const byte *rec_get_nth_field(const rec_t *rec, const ulint *offsets, ulint n,
                                ulint *len);
  void innodb_rec_convert_to_mysql_rec(const rec_t *rec,
                                       const dict_index_t *rec_index,
                                       const ulint *offsets, TABLE *table,
                                       uint32 n_fields);
  ulint rec_get_nth_field_offs(const ulint *offsets, ulint n, ulint *len);
  uint32 find_col_in_index(const dict_index_t *index, const dict_col_t *col);

  void innodb_field_convert_to_mysql_field(byte *dest, const byte *data,
                                           ulint len, dict_col_t *col,
                                           Field *field);
};

}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_LOG_PARSER_H__