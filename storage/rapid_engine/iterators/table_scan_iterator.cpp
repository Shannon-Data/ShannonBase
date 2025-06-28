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

  // Allocate row buffer for batch processing
  auto fields{m_table->s->fields};
  while (fields) {
    m_row_buffer.push_back(std::move(std::make_unique<uchar[]>(m_table->s->rec_buff_length * m_batch_size)));
    fields--;
  }

  return false;
}

int VectorizedTableScanIterator::Read() {
  int result{ShannonBase::SHANNON_SUCCESS};
  for (size_t i = 0; i < m_batch_size; i++) {
#if 0
    result = m_data_table->next(&m_row_buffer[i * m_table->s->rec_buff_length]);
    if (result == HA_ERR_END_OF_FILE) {
      break;
    }

    // Apply filter if present
    if (m_filter && !m_filter(&m_row_buffer[i])) {
      continue;  // Skip this row
    }
#endif
  }
  return (result != 0) ? HandleError(result) : 0;
}

}  // namespace Executor
}  // namespace ShannonBase