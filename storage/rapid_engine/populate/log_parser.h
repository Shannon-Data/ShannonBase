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
  uint parse_redo(byte* ptr, byte* end_ptr);
private:
  uint parse_single_rec(byte *ptr, byte *end_ptr);
  bool parse_multi_rec(byte *ptr, byte *end_ptr);
  byte *parse_parse_or_apply_log_rec_body(
      mlog_id_t type, byte *ptr, byte *end_ptr, space_id_t space_id,
      page_no_t page_no, buf_block_t *block, mtr_t *mtr, lsn_t start_lsn);

  ulint parse_parse_log_rec(mlog_id_t *type, byte *ptr, byte *end_ptr,
                                space_id_t *space_id, page_no_t *page_no,
                                byte **body);

  inline bool get_record_type(const unsigned char *ptr, mlog_id_t *type, bool* single_flag) {  
    *single_flag = (*ptr & MLOG_SINGLE_REC_FLAG) ? true : false;
    *type = mlog_id_t(*ptr & ~MLOG_SINGLE_REC_FLAG);
    if (UNIV_UNLIKELY(*type > MLOG_BIGGEST_TYPE)) {
      // PRINT_ERR << "Found an invalid redo log record type at offset: " <<
      return true;
    }
    return false;
  }

  byte *parse_cur_parse_and_apply_insert_rec(
    bool is_short,       /*!< in: true if short inserts */
    const byte *ptr,     /*!< in: buffer */
    const byte *end_ptr, /*!< in: buffer end */
    buf_block_t *block,  /*!< in: page or NULL */
    dict_index_t *index, /*!< in: record descriptor */
    mtr_t *mtr);          /*!< in: mtr or NULL */

  //only user's index be retrieved from dd_table.
  const dict_index_t* find_index(uint64 idx_id);
};

}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_LOG_PARSER_H__