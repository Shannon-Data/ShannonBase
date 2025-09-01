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
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/data_table.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_arch_inf.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
/**
 * Usage:
 *
 * // 1. create and initialization.
 * auto iterator = std::make_unique<VectorizedTableScanIterator>(thd, table, expected_rows, &examined_rows);
 * if (iterator->Init()) {
 *   return HA_ERR_INITIALIZATION;
 * }
 *
 * // 2. read the data
 * while (true) {
 *   int result = iterator->Read();
 *   if (result == HA_ERR_END_OF_FILE) break;
 *   if (result != 0) return result;
 *
 *   // data has been fill up into table->field.
 *   ProcessRow(table);
 * }
 *
 * // 3. option：the performance metrics
 * size_t total_rows, total_batches;
 * double avg_batch_time, throughput;
 * iterator->GetPerformanceStats(&total_rows, &total_batches, &avg_batch_time, &throughput);
 */

class TABLE;
namespace ShannonBase {
namespace Executor {

class VectorizedTableScanIterator : public TableRowIterator {
 public:
  VectorizedTableScanIterator(THD *thd, TABLE *table, double expected_rows, ha_rows *examined_rows);
  virtual ~VectorizedTableScanIterator() override {}

  bool Init() override;
  int Read() override;

  void set_filter(filter_func_t filter) { m_filter = filter; }

  size_t GetCurrentBatchSize() const { return m_batch_size; }

 private:
  size_t EstimateRowSize() const;

  // to calculate the optimial batch size. cache line, and row size, etc. are considered.
  size_t CalculateOptimalBatchSize(double expected_rows);

  void CacheActiveFields();

  void PreallocateColumnChunks();

  int ReadNextBatch();

  void ClearBatchData() {
    for (auto &chunk : m_col_chunks) {
      chunk.clear();
    }
  }

  void UpdatePerformanceMetrics(std::chrono::high_resolution_clock::time_point start_time);

  void AdaptBatchSize();

  int PopulateCurrentRow();

  inline void ProcessFieldData(Field *field, const ShannonBase::Executor::ColumnChunk &col_chunk, size_t rowid) {
    if (Utils::Util::is_string(field->type()) || Utils::Util::is_blob(field->type())) {
      ProcessStringField(field, col_chunk, rowid);
    } else {
      ProcessNumericField(field, col_chunk, rowid);
    }
  }

  inline void ProcessStringField(Field *field, const ShannonBase::Executor::ColumnChunk &col_chunk, size_t rowid) {
    if (field->real_type() == MYSQL_TYPE_ENUM) {
      field->pack(const_cast<uchar *>(field->data_ptr()), col_chunk.data(rowid), field->pack_length());
    } else {
      const char *data_ptr = reinterpret_cast<const char *>(col_chunk.data(rowid));
      auto str_id = *(uint32 *)data_ptr;

      std::string fld_name(field->field_name);
      auto rpd_field = m_data_table.get()->source()->get_field(fld_name);
      if (!rpd_field) return;

      auto str_ptr = rpd_field->header()->m_local_dict->get(str_id);
      field->store(str_ptr.c_str(), strlen(str_ptr.c_str()), field->charset());
    }
  }

  inline void ProcessNumericField(Field *field, const ShannonBase::Executor::ColumnChunk &col_chunk, size_t rowid) {
    field->pack(const_cast<uchar *>(field->data_ptr()), col_chunk.data(rowid), col_chunk.width());
  }

 private:
  TABLE *m_table;

  std::unique_ptr<ShannonBase::Imcs::DataTable> m_data_table;
  std::vector<ShannonBase::Executor::ColumnChunk> m_col_chunks;

  filter_func_t m_filter;

  size_t m_batch_size;               // batch size
  size_t m_optimal_batch_size;       // optimial batch size
  size_t m_current_batch_size{0};    // the current batch size
  size_t m_current_row_in_batch{0};  // pos in batch.
  bool m_batch_exhausted{true};      // current batch has been processed.
  bool m_eof_reached{false};         // EOF. end of file.

  std::vector<Field *> m_active_fields;  // store the field we read.
  std::vector<uint> m_field_indices;     // field index.
  bool m_fields_cached{false};

  struct PerformanceMetrics {
    std::chrono::high_resolution_clock::time_point start_time;
    std::chrono::high_resolution_clock::time_point last_batch_time;
    size_t total_rows{0};
    size_t total_batches{0};
    size_t error_count{0};
    double avg_batch_time{0.0};
    double total_read_time{0.0};

    void reset() {
      start_time = std::chrono::high_resolution_clock::now();
      last_batch_time = start_time;
      total_rows = 0;
      total_batches = 0;
      error_count = 0;
      avg_batch_time = 0.0;
      total_read_time = 0.0;
    }
  };

  mutable PerformanceMetrics m_metrics;
  int m_last_error{0};

  alignas(CACHE_LINE_SIZE) char m_padding[CACHE_LINE_SIZE];
};

}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_TABLE_SCAN_ITERATOR_H__