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

   Copyright (c) 2023, 2024, 2025,  Shannon Data AI and/or its affiliates.

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.
*/
#ifndef __SHANNONBASE_LOG_COPY_INFO_PARSER_H__
#define __SHANNONBASE_LOG_COPY_INFO_PARSER_H__

#include "sql/sql_base.h"
#include "storage/rapid_engine/include/rapid_context.h"  //Rapid_load_context
#include "storage/rapid_engine/populate/log_commons.h"   //change_record_buff_t::OperType
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Populate {
/**
 * To parse the copy_info, it used to populate the changes from ionnodb
 * to rapid.
 */
class CopyInfoParser {
 public:
  CopyInfoParser() = default;
  ~CopyInfoParser() = default;

  /**
   * @brief RAII wrapper for TABLE resource management
   *
   * Automatically closes table when going out of scope.
   */
  class TableGuard {
   public:
    explicit TableGuard(THD *thd, TABLE *table) noexcept : m_thd(thd), m_table(table) {}

    ~TableGuard() noexcept {
      if (m_table && m_thd) {
        ShannonBase::Utils::Util::close_table(m_thd, m_table);
        // Remove the table from table definition cache to force reopening on next access
        // Reason: In multi-threaded environments, other threads may use cached table instances,
        // but the field pointers (field->ptr) within these table instances might have been
        // reset during previous operations, leading to incorrect field offset values.
        // Forcing a reopen ensures that field information is properly reinitialized.
        tdc_remove_table(m_thd, TDC_RT_MARK_FOR_REOPEN, m_table->s->db.str, m_table->s->table_name.str, false);
      }
    }

    // Disable copy
    TableGuard(const TableGuard &) = delete;
    TableGuard &operator=(const TableGuard &) = delete;

    TableGuard(TableGuard &&other) = delete;
    TableGuard &operator=(TableGuard &&other) = delete;

   private:
    THD *m_thd;
    TABLE *m_table;
  };

  /**
   * @brief Parse and apply change records from a binary copy buffer.
   *
   * This function iterates through a binary stream containing encoded change
   * records (INSERT, UPDATE, DELETE) generated during a population or replication
   * process. Each record is decoded into a `change_record_buff_t` structure, and
   * applied to the corresponding table.
   *
   * The function uses the provided `Rapid_load_context` to either retrieve an
   * already opened TABLE object or open one on-demand. Each record is then
   * dispatched to the corresponding handler:
   * - `parse_and_apply_insert()` for insert operations.
   * - `parse_and_apply_update()` for update operations.
   * - `parse_and_apply_delete()` for delete operations.
   *
   * Typical usage:
   * - During system startup or incremental load to replay change logs.
   * - When synchronizing data between primary storage and secondary population buffers.
   *
   * Error handling:
   * - If a table cannot be opened, the function logs an error via `sql_print_error`
   *   and skips the current record.
   * - The parser continues until all records in the buffer are processed or the
   *   end of buffer (`end_ptr`) is reached.
   *
   * @param[in]  context     Pointer to the current rapid load execution context,
   *                         which provides access to open tables and schema metadata.
   * @param[in]  oper_type   The type of operation (INSERT, UPDATE, DELETE) being parsed.
   * @param[in]  start         Pointer to the start of the binary buffer containing change records.
   * @param[in]  end_ptr     Pointer to the end of the binary buffer.
   * @param[in]  new_start   Pointer to the start of the binary buffer containing change records[table->record[0]].
   * @param[in]  new_end_ptr Pointer to the end of the binary buffer [table->record[1]].
   *
   * @return returns parsed bytes; or return 0 on error.
   *
   * @note The function assumes `parse_record_header()` advances `start` safely
   *       within bounds and that each parsed record includes valid schema/table
   *       identifiers.
   * @threadsafe Not thread-safe; caller must ensure single-threaded access to the
   *              provided `Rapid_load_context` instance.
   */
  uint parse_copy_info(Rapid_load_context *context, change_record_buff_t::OperType oper_type, byte *start,
                       byte *end_ptr, byte *new_start, byte *new_end_ptr);

 private:
  /**
   * @brief Apply an UPDATE operation to a RAPID table using COPY_INFO data.
   *
   * This function is invoked when a user executes an UPDATE statement on a primary
   * (InnoDB) table while RAPID population is active. The SQL layer captures both
   * the old and new row images (`table->record[1]` and `table->record[0]`) and
   * delivers them to this function for decoding and synchronization.
   *
   * Internally, the function extracts field values from the in-memory record
   * buffer (MySQL row format) and applies corresponding updates to the RAPID
   * table’s in-memory storage engine representation.
   *
   * @param[in] context   Rapid population execution context.
   * @param[in] table     The MySQL TABLE object (from COPY_INFO).
   * @param[in] old_start     Pointer to the row buffer , old row buffer (`table->record[0]`).
   * @param[in] old_end_ptr   Pointer to the end of row buffer, old row buffer.
   * @param[in] new_start     Pointer to the row buffer , new row buffer(`table->record[1]`).
   * @param[in] new_ end_ptr   Pointer to the end of row buffer, new row buffer.
   *
   * @return
   *   - `The parsed bytes` if update applied successfully.
   *   - 0 if schema mismatch, parsing, or update fails.
   *
   * @note
   *   - This function is only used when `g_sync_mode == COPY_INFO`.
   *   - It does not perform any physical logging; the LSN was already assigned
   *     in `NotifyAfterUpdate()` before enqueuing the record.
   */
  int parse_and_apply_update(Rapid_load_context *context, TABLE *table, const byte *old_start, const byte *old_end_ptr,
                             const byte *new_start, const byte *new_end_ptr);

  /**
   * @brief Apply an INSERT operation to a RAPID table using COPY_INFO data.
   *
   * This function handles INSERT propagation during COPY_INFO-based synchronization.
   * When a new row is inserted into the primary InnoDB table, the SQL layer
   * (via `NotifyAfterInsert`) provides the new row image (`table->record[0]`)
   * directly to this function for insertion into RAPID.
   *
   * @param[in] context   Rapid population execution context.
   * @param[in] table     The MySQL TABLE object corresponding to the RAPID table.
   * @param[in] start     Pointer to the new row buffer (`table->record[0]`).
   * @param[in] end_ptr   Pointer to the end of buffer (unused for COPY_INFO).
   *
   * @return
   *   - `The parsed bytes` if insert applied successfully.
   *   - 0 otherwise.
   *
   * @note
   *   - The record buffer is already laid out in MySQL’s internal row format.
   *   - No binary decoding is required, only field extraction and mapping.
   */
  int parse_and_apply_insert(Rapid_load_context *context, TABLE *table, const byte *start, const byte *end_ptr);

  /**
   * @brief Apply a DELETE operation to a RAPID table using COPY_INFO data.
   *
   * This function is triggered when a row is deleted in the primary engine.
   * The SQL layer (via `NotifyAfterDelete`) provides the deleted row image
   * (`table->record[1]` or `old_row`) directly, and this function removes
   * the corresponding entry from the RAPID table’s in-memory storage.
   *
   * @param[in] context   Rapid population execution context.
   * @param[in] table     Target table to delete the row from.
   * @param[in] start     Pointer to the deleted row buffer (`table->record[1]`).
   * @param[in] end_ptr   Unused (may be nullptr).
   *
   * @return
   *   - `The parsed bytes` if delete applied successfully.
   *   - 0 otherwise.
   *
   * @note
   *   - This function does not read or modify redo logs.
   *   - It directly interprets MySQL’s in-memory row layout.
   */
  int parse_and_apply_delete(Rapid_load_context *context, TABLE *table, const byte *start, const byte *end_ptr);

  /**
   * @brief Initialize column metadata information from a given TABLE object.
   *
   * This function extracts essential per-column metadata (offsets, null byte locations,
   * and null bitmasks) from the provided `TABLE` structure, and caches them internally
   * in the `CopyInfoParser` instance for later use during record parsing and population.
   *
   * Specifically, it computes and stores:
   *   - `m_col_offsets[i]`      → byte offset of column `i` within a record buffer.
   *   - `m_null_byte_offsets[i]` → byte offset of the null-byte that contains the null bit for column `i`.
   *   - `m_null_bitmasks[i]`     → bitmask used to test or set the null flag of column `i`.
   *
   * These values are derived from the MySQL `Field` metadata and are used to efficiently
   * locate and interpret each field's data during row decoding or replay from COPY_INFO logs.
   *
   * @param[in] context   The rapid load execution context (may contain shared buffers or state).
   * @param[in] table     The MySQL TABLE object whose field metadata will be parsed.
   *
   * @return
   *   - `ShannonBase::SHANNON_SUCCESS` on success.
   *   - (In future) error code if metadata extraction fails or table is invalid.
   *
   * @note
   *   - The function assumes that `table` is already opened and valid.
   *   - This function does not read or modify record contents — it only prepares metadata.
   *   - The extracted offsets are based on `table->record[0]` layout and are reused
   *     for parsing binary row data efficiently.
   */
  int parse_table_meta(Rapid_load_context *context, const TABLE *table);

 private:
  size_t m_n_fields{0};
  std::map<std::string, std::vector<ulong>> m_col_offsets;
  std::map<std::string, std::vector<ulong>> m_null_byte_offsets;
  std::map<std::string, std::vector<ulong>> m_null_bitmasks;
};
}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_LOG_COPY_INFO_PARSER_H__
