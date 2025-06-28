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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_DATA_TABLE_H__
#define __SHANNONBASE_DATA_TABLE_H__

#include <atomic>
#include <vector>

#include "storage/rapid_engine/imcs/index/iterator.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/iterators/iterator.h"
#include "storage/rapid_engine/trx/readview.h"

class TABLE;
namespace ShannonBase {
class Rapid_load_context;

namespace Imcs {
class Imcs;
class Cu;
class RapidTable;

class DataTable : public MemoryObject {
 public:
  DataTable(TABLE *source_table, RapidTable *rpd);
  virtual ~DataTable();

  // open a cursor on db_table to read/write.
  int open();

  // close a cursor.
  int close();

  // intitialize this data table object.
  int init();

  // to the next rows.
  int next(uchar *buf);

  // read the data in data in batch mode.
  int next_batch(size_t batch_size, std::vector<ShannonBase::Executor::ColumnChunk> &data, size_t &read_cnt);

  // end of scan.
  int end();

  // get the data pos.
  row_id_t find(uchar *buf);

  // for index scan initialization.
  int index_init(uint keynr, bool sorted);

  // for index scan end.
  int index_end();

  // index read.
  int index_read(uchar *buf, const uchar *key, uint key_len, ha_rkey_function find_flag);

  // index read next
  int index_next(uchar *buf);

  inline uchar *ensure_buffer_size(size_t needed_size) {
    if (needed_size > m_buffer_size) {
      m_row_buffer = std::make_unique<uchar[]>(needed_size);
      m_buffer_size = needed_size;
    }
    return m_row_buffer.get();
  }

 private:
  std::atomic<bool> m_initialized{false};

  // the data source, an IMCS.
  TABLE *m_data_source{nullptr};

  // rapid table.
  RapidTable *m_rapid_table;

  // start from where.
  std::atomic<row_id_t> m_rowid{0};

  // context
  std::unique_ptr<Rapid_load_context> m_context{nullptr};

  // index iterator.
  std::unique_ptr<Index::Iterator> m_index_iter{nullptr};

  // active index no.
  int8_t m_active_index{MAX_KEY};

  // key
  std::unique_ptr<uchar[]> m_key{nullptr};

  // Reusable buffer
  std::unique_ptr<uchar[]> m_row_buffer;
  size_t m_buffer_size = 0;
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_DATA_TABLE_H__