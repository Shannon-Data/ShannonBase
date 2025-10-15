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

/**
 * VectorizedTableScanIterator - A vectorized table scan iterator that processes data in batches
 *
 * This iterator implements a vectorized execution model for table scanning, where data is
 * processed in batches rather than row-by-row. It leverages columnar storage and SIMD
 * optimizations to improve cache locality and CPU efficiency.
 */
class VectorizedTableScanIterator : public TableRowIterator {
 public:
  /**
   * Constructor for VectorizedTableScanIterator
   * @param thd Thread handler
   * @param table MySQL table structure
   * @param expected_rows Estimated number of rows for batch size optimization
   * @param examined_rows Counter for examined rows (output parameter)
   */
  VectorizedTableScanIterator(THD *thd, TABLE *table, double expected_rows, ha_rows *examined_rows);

  bool Init() override;
  int Read() override;

  /**
   * Set a filter function to be applied during scanning
   * @param filter Filter function that returns true for rows to keep
   */
  void set_filter(filter_func_t filter) { m_filter = filter; }

  /**
   * Get the current batch size being processed
   * @return Current batch size in number of rows
   */
  size_t GetCurrentBatchSize() const { return m_batch_size; }

 private:
  /**
   * Estimate the average row size for memory allocation optimization
   * @return Estimated row size in bytes
   */
  size_t EstimateRowSize() const;

  /**
   * Calculate the optimal batch size based on system characteristics
   * Considers cache line size, row size, and expected row count
   * @param expected_rows Estimated number of rows to process
   * @return Optimal batch size in number of rows
   */
  size_t CalculateOptimalBatchSize(double expected_rows);

  /**
   * Cache active field pointers to avoid repeated lookups
   * Improves performance by storing frequently accessed field information
   */
  void CacheActiveFields();

  /**
   * Preallocate memory for column chunks to avoid runtime allocations
   * Sets up the columnar storage buffers for batch processing
   */
  void PreallocateColumnChunks();

  /**
   * Read the next batch of rows from the underlying storage
   * @return 0 on success, error code on failure
   */
  int ReadNextBatch();

  /**
   * Clear all batch data to prepare for next batch
   * Resets column chunks while maintaining allocated capacity
   */
  void ClearBatchData() {
    for (auto &chunk : m_col_chunks) {
      chunk.clear();
    }
  }

  /**
   * Update performance metrics after processing a batch
   * @param start_time Timestamp when batch processing started
   */
  void UpdatePerformanceMetrics(std::chrono::high_resolution_clock::time_point start_time);

  /**
   * Adapt batch size based on runtime performance characteristics
   * Implements dynamic batch size adjustment for optimal performance
   */
  void AdaptBatchSize();

  /**
   * Populate the current row from batch data to MySQL row format
   * @return 0 on success, error code on failure
   */
  int PopulateCurrentRow();

  /**
   * Process field data from column chunk to MySQL field format
   * Dispatches to specialized handlers based on field type
   * @param field MySQL field structure
   * @param col_chunk Column chunk containing the data
   * @param rowid Row index within the current batch
   */
  inline void ProcessFieldData(Field *field, const ShannonBase::Executor::ColumnChunk &col_chunk, size_t rowid) {
    if (Utils::Util::is_string(field->type()) || Utils::Util::is_blob(field->type())) {
      ProcessStringField(field, col_chunk, rowid);
    } else {
      ProcessNumericField(field, col_chunk, rowid);
    }
  }

  /**
   * Process string field data with dictionary encoding support
   * Handles ENUM types and dictionary-encoded strings
   * @param field MySQL field structure
   * @param col_chunk Column chunk containing the data
   * @param rowid Row index within the current batch
   */
  inline void ProcessStringField(Field *field, const ShannonBase::Executor::ColumnChunk &col_chunk, size_t rowid) {
    if (field->real_type() == MYSQL_TYPE_ENUM) {
      field->pack(const_cast<uchar *>(field->data_ptr()), col_chunk.data(rowid), field->pack_length());
    } else {
      const char *data_ptr = reinterpret_cast<const char *>(col_chunk.data(rowid));
      auto str_id = *(uint32 *)data_ptr;

      std::string fld_name(field->field_name);
      auto rpd_field = m_data_table.get()->table()->get_field(fld_name);
      if (!rpd_field) return;

      auto str_ptr = rpd_field->header()->m_local_dict->get(str_id);
      field->store(str_ptr.c_str(), strlen(str_ptr.c_str()), field->charset());
    }
  }

  /**
   * Process numeric field data with direct memory copying
   * Handles integer, float, and other numeric types efficiently
   * @param field MySQL field structure
   * @param col_chunk Column chunk containing the data
   * @param rowid Row index within the current batch
   */
  inline void ProcessNumericField(Field *field, const ShannonBase::Executor::ColumnChunk &col_chunk, size_t rowid) {
    field->pack(const_cast<uchar *>(field->data_ptr()), col_chunk.data(rowid), col_chunk.width());
  }

 private:
  TABLE *m_table;  ///< MySQL table structure pointer

  std::unique_ptr<ShannonBase::Imcs::DataTable> m_data_table;    ///< Underlying columnar data table
  std::vector<ShannonBase::Executor::ColumnChunk> m_col_chunks;  ///< Column chunks for batch processing

  filter_func_t m_filter;  ///< Optional filter function for row-level filtering

  size_t m_batch_size;               ///< Current batch size being used
  size_t m_optimal_batch_size;       ///< Calculated optimal batch size
  size_t m_current_batch_size{0};    ///< Size of the current batch being processed
  size_t m_current_row_in_batch{0};  ///< Current row index within the batch
  bool m_batch_exhausted{true};      ///< Flag indicating if current batch is fully processed
  bool m_eof_reached{false};         ///< Flag indicating end of data has been reached

  std::vector<Field *> m_active_fields;  ///< Cached pointers to active fields for faster access
  std::vector<uint> m_field_indices;     ///< Field indices for column mapping
  bool m_fields_cached{false};           ///< Flag indicating if field caching is complete

  /**
   * Performance metrics structure for monitoring and adaptive optimization
   * Tracks timing, throughput, and error statistics for dynamic tuning
   */
  struct PerformanceMetrics {
    std::chrono::high_resolution_clock::time_point start_time;       ///< Start time of current operation
    std::chrono::high_resolution_clock::time_point last_batch_time;  ///< Time when last batch completed
    size_t total_rows{0};                                            ///< Total rows processed so far
    size_t total_batches{0};                                         ///< Total batches processed so far
    size_t error_count{0};                                           ///< Count of errors encountered
    double avg_batch_time{0.0};                                      ///< Average time per batch in milliseconds
    double total_read_time{0.0};                                     ///< Total time spent reading data

    /**
     * Reset all metrics to initial state
     * Typically called at the start of a new scanning operation
     */
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

  mutable PerformanceMetrics m_metrics;  ///< Instance of performance metrics for this iterator
};

}  // namespace Executor
}  // namespace ShannonBase
#endif  // __SHANNONBASE_TABLE_SCAN_ITERATOR_H__