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
#include "sql/iterators/row_iterator.h"
#include "sql/mem_root_array.h"

#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "storage/rapid_engine/imcs/data_table.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"

class TABLE;
namespace ShannonBase {
namespace Executor {

class VectorizedTableScanIterator : public TableRowIterator {
 public:
  VectorizedTableScanIterator(THD *thd, TABLE *table, double expected_rows, ha_rows *examined_rows);
  virtual ~VectorizedTableScanIterator();

  bool Init() override;
  int Read() override;

  size_t ReadCount() override { return m_read_cnt; }

  uchar *GetData(size_t rowid) override;

  void set_filter(filter_func_t filter) { m_filter = filter; }

 private:
  TABLE *m_table{nullptr};

  std::unique_ptr<ShannonBase::Imcs::DataTable> m_data_table{nullptr};

  std::vector<ColumnChunk> m_col_chunks;

  // Optional filter for predicate pushdown
  filter_func_t m_filter;

  // the row read a chunk. chunk-a-time.
  size_t m_read_cnt{0};

  // total rows read since started.
  size_t m_total_read{0};

  size_t m_batch_size{1};
};

}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_TABLE_SCAN_ITERATOR_H__