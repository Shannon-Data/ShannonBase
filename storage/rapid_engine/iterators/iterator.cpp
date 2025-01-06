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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
/** The basic iterator class for IMCS. All specific iterators are all inherited
 * from this.
 */
#include "storage/rapid_engine/iterators/iterator.h"

#include "sql/field.h"
#include "sql/sql_class.h"
#include "sql/table.h"  // TABLE

namespace ShannonBase {
namespace Executor {

BatchTableScanIterator::BatchTableScanIterator(THD *thd, TABLE *table, double expected_rows, ha_rows *examined_rows)
    : TableScanIterator(thd, table, expected_rows, examined_rows) {}

int BatchTableScanIterator::Read() {
  int tmp;
  if (table()->is_union_or_table()) {
    while ((tmp = table()->file->ha_rnd_next(m_record))) {
      /*
       ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is
       reading and another deleting without locks.
       */
      if (tmp == HA_ERR_RECORD_DELETED && !thd()->killed) continue;
      return HandleError(tmp);
    }
    if (m_examined_rows != nullptr) {
      ++*m_examined_rows;
    }
  } else {
    while (true) {
      if (m_remaining_dups == 0) {  // always initially
        while ((tmp = table()->file->ha_rnd_next(m_record))) {
          if (tmp == HA_ERR_RECORD_DELETED && !thd()->killed) continue;
          return HandleError(tmp);
        }
        if (m_examined_rows != nullptr) {
          ++*m_examined_rows;
        }

        // Filter out rows not qualifying for INTERSECT, EXCEPT by reading
        // the counter.
        const ulonglong cnt = static_cast<ulonglong>(table()->set_counter()->val_int());
        if (table()->is_except()) {
          if (table()->is_distinct()) {
            // EXCEPT DISTINCT: any counter value larger than one yields
            // exactly one row
            if (cnt >= 1) break;
          } else {
            // EXCEPT ALL: we use m_remaining_dups to yield as many rows
            // as found in the counter.
            m_remaining_dups = cnt;
          }
        } else {
          // INTERSECT
          if (table()->is_distinct()) {
            if (cnt == 0) break;
          } else {
            HalfCounter c(cnt);
            // Use min(left side counter, right side counter)
            m_remaining_dups = std::min(c[0], c[1]);
          }
        }
      } else {
        --m_remaining_dups;  // return the same row once more.
        break;
      }
      // Skipping this row
    }
    if (++m_stored_rows > m_limit_rows) {
      return HandleError(HA_ERR_END_OF_FILE);
    }
  }
  return 0;
}

}  // namespace Executor
}  // namespace ShannonBase
