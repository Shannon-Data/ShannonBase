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
#ifndef __SHANNONBASE_TABLE_SCAN_ITERATOR_H__
#define __SHANNONBASE_TABLE_SCAN_ITERATOR_H__

#include "sql/iterators/basic_row_iterators.h"

#include "storage/rapid_engine/include/rapid_const.h"

#include "storage/rapid_engine/imcs/data_table.h"
#include "storage/rapid_engine/iterators/iterator.h"

namespace ShannonBase {
namespace Executor {

class BatchTableScanIterator final : public TableScanIterator {
 public:
  BatchTableScanIterator(THD *thd, TABLE *table, double expected_rows, ha_rows *examined_rows);
  // bool Init() override;
  int Read() override;
};

class VectorizedTableScanIterator : public RowIterator {
 private:
  ShannonBase::Imcs::DataTable *m_data_table;
  uchar *m_batch_buffer;
  size_t m_batch_size;
  size_t m_current_batch_pos;
  size_t m_rows_in_current_batch;

 public:
  VectorizedTableScanIterator(THD *thd, TABLE *table, size_t batch_size = SHANNON_VECTOR_WIDTH);

  bool Init() override;
  int Read() override;
  int ReadBatch(uchar **buffers, size_t max_rows, size_t *rows_read);
  void SetNullRowFlag(bool is_null_row) override {}
  void UnlockRow() override {}
};

}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_TABLE_SCAN_ITERATOR_H__