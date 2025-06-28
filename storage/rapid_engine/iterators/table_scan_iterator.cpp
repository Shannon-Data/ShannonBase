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
/** The table scan iterator class for IMCS. All specific iterators are all inherited
 * from this.
 * vectorized/parallelized table scan iterator impl for rapid engine. In
 */
#include "storage/rapid_engine/iterators/table_scan_iterator.h"

#include "include/my_base.h"

#include "sql/dd/cache/dictionary_client.h"
#include "sql/sql_class.h"

#include "storage/innobase/include/dict0dd.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {
namespace Executor {

VectorizedTableScanIterator::VectorizedTableScanIterator(THD *thd, TABLE *table, double expected_rows,
                                                         ha_rows *examined_rows)
    : TableRowIterator(thd, table, RowIterator::Type::ROWITERATOR_VECTORIZED), m_table{table} {
  m_batch_size = SHANNON_VECTOR_WIDTH;

  std::string key(table->s->db.str);
  key.append(":").append(table->s->table_name.str);

  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  const dd::Table *table_def = nullptr;
  if (thd->dd_client()->acquire(m_table->s->db.str, m_table->s->table_name.str, &table_def)) {
    // Error is reported by the dictionary subsystem.
    return;
  }
  if (table_def == nullptr) return;

  if (dd_table_is_partitioned(*table_def)) {
    m_data_table.reset(
        new ShannonBase::Imcs::DataTable(table, ShannonBase::Imcs::Imcs::instance()->get_parttable(key)));
  } else {
    m_data_table.reset(new ShannonBase::Imcs::DataTable(table, Imcs::Imcs::instance()->get_table(key)));
  }
}

VectorizedTableScanIterator::~VectorizedTableScanIterator() {}

bool VectorizedTableScanIterator::Init() {
  // Initialize similar to ha_rapid::rnd_init()
  if (m_data_table->init()) {
    return true;
  }

  // Allocate row buffer for batch processing. to store data in mysql format in column format.
  for (auto ind = 0u; ind < m_table->s->fields; ind++) {
    auto field = *(m_table->field + ind);
    if (field->is_flag_set(NOT_SECONDARY_FLAG)) {  // not secondary flaged, then not loaded into.
      ColumnChunk placement(field, 0);
      m_col_chunks.push_back(std::move(placement));
      continue;
    }

    ColumnChunk colchk(field, ShannonBase::SHANNON_ROWS_IN_CHUNK / 8);
    m_col_chunks.push_back(std::move(colchk));
  }
  return false;
}

int VectorizedTableScanIterator::Read() {
  int result{ShannonBase::SHANNON_SUCCESS};
  m_read_cnt = 0;
  while ((result = m_data_table->next_batch(m_batch_size, m_col_chunks, m_read_cnt))) {
    /*
     next_batch can return RECORD_DELETED for MyISAM when one thread is
     reading and another deleting without locks.
     */
    if (result == HA_ERR_RECORD_DELETED && !thd()->killed)
      continue;
    else if (result && m_read_cnt == 0)  // no more rows to read.
      return HandleError(result);
    if (m_read_cnt) break;
  }

  m_total_read += m_read_cnt;
  return 0;
}

uchar *VectorizedTableScanIterator::GetData(size_t rowid) {
  if (rowid >= m_read_cnt) return nullptr;

  for (auto ind = 0u; ind < m_table->s->fields; ind++) {
    auto field = *(m_table->field + ind);
    if (!bitmap_is_set(m_table->read_set, ind) || field->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    auto col_chunk = m_col_chunks[ind];
    Utils::ColumnMapGuard guard(m_table);
    if (col_chunk.nullable(rowid)) {
      field->set_null();
    } else {
      if (Utils::Util::is_string(field->type()) || Utils::Util::is_blob(field->type())) {
        auto data_ptr = (const char *)col_chunk.data(rowid);
        field->store(data_ptr, strlen(data_ptr), field->charset());
      } else {
        field->pack(const_cast<uchar *>(field->data_ptr()), col_chunk.data(rowid), col_chunk.width());
      }
    }
  }
  return nullptr;
}

}  // namespace Executor
}  // namespace ShannonBase