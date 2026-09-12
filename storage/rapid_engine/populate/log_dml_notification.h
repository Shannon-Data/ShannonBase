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
#ifndef __SHANNONBASE_LOG_DML_NOTIFICATION_H__
#define __SHANNONBASE_LOG_DML_NOTIFICATION_H__

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sql/sql_base.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_context.h"  //Rapid_load_context
#include "storage/rapid_engine/populate/log_commons.h"   //change_record_buff_t::OperType
#include "storage/rapid_engine/trx/transaction.h"        //TransactionSubscriber
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Populate {

/**
 * Owns COPY_INFO transaction participation and the asynchronous transaction
 * outcome rendezvous used by the propagation workers.
 *
 * Synchronous state is keyed by THD while a source transaction is active.
 * Asynchronous state is keyed by the captured InnoDB writer transaction id and
 * may outlive the THD-local Transaction facade until every queued record is
 * applied.
 */
class TransactionManager final : public TransactionSubscriber {
 public:
  enum class Outcome : uint8_t { ACTIVE = 0, COMMITTED, ABORTED };

  struct Registration {
    Transaction::ID source_trx_id{0};

    explicit operator bool() const noexcept { return source_trx_id != 0; }
  };

  static TransactionManager &instance() {
    static TransactionManager manager;
    return manager;
  }

  TransactionManager(const TransactionManager &) = delete;
  TransactionManager &operator=(const TransactionManager &) = delete;

  Registration register_change(THD *thd, table_id_t table_id);

  // ROLLBACK TO SAVEPOINT is a partial transaction undo, not a normal
  // statement-rollback callback. Keep this explicit while COPY_INFO lacks
  // per-operation undo.
  void quarantine_partial_rollback(THD *thd, const char *reason);

  Outcome get_outcome(Transaction::ID txn_id, uint64_t *commit_scn = nullptr);
  void on_change_applied(Transaction::ID txn_id, table_id_t table_id);
  void forget_table(table_id_t table_id);

  void on_transaction_commit(THD *thd) override;
  void on_transaction_rollback(THD *thd) override;
  void on_statement_commit(THD *thd) override;
  void on_statement_rollback(THD *thd) override;
  void on_transaction_detach(THD *thd) override;

  void start();
  void shutdown();

 private:
  struct Participant {
    Transaction::ID source_trx_id{0};
    std::unordered_set<table_id_t> touched_tables;
    bool statement_has_changes{false};
    bool fail_closed{false};
  };

  struct TableProgress {
    uint64_t registered{0};
    uint64_t applied{0};
  };

  struct TxnProgress {
    Outcome outcome{Outcome::ACTIVE};
    uint64_t commit_scn{0};
    std::unordered_map<table_id_t, TableProgress> tables;
  };

  TransactionManager() = default;

  void ensure_subscribed();
  void clear();
  void publish_commit(Transaction::ID txn_id, uint64_t commit_scn);
  void publish_rollback(Transaction::ID txn_id);
  void quarantine_participant(THD *thd, bool require_statement_change, const char *reason);
  static void finalize_table(Transaction::ID txn_id, table_id_t table_id, Outcome outcome, uint64_t commit_scn);
  void erase_if_complete_locked(Transaction::ID txn_id);

  std::mutex m_subscription_mutex;
  std::atomic<bool> m_subscribed{false};

  std::mutex m_mutex;
  std::unordered_map<THD *, Participant> m_participants;
  std::unordered_map<Transaction::ID, TxnProgress> m_transactions;
};

namespace DML {
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
        // tdc_remove_table(m_thd, TDC_RT_MARK_FOR_REOPEN, m_table->s->db.str, m_table->s->table_name.str, false);
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
  uint parse_copy_info(Rapid_load_context *context, table_id_t &table_id, change_record_buff_t::OperType oper_type,
                       byte *start, byte *end_ptr, byte *new_start, byte *new_end_ptr);

  /**
   * Apply one buffered COPY_INFO change record.
   *
   * Owns everything specific to the row-image source: validating the primary
   * writer identity, resolving the asynchronous primary COMMIT/ROLLBACK race,
   * preparing the Rapid_load_context and driving parse_copy_info(). On success
   * it also reports the record to TransactionManager so the source transaction
   * can be finalized. The propagation worker only routes the record here and
   * acts on the returned status.
   */
  ChangeApplyResult apply_change(Rapid_load_context &context, change_record_buff_t &record);

  /**
   * Producer-side check: does this buffered record satisfy the COPY_INFO
   * format invariants? Row-image propagation must carry the real primary
   * InnoDB creator id (commit_scn == 0 is a valid ACTIVE version).
   */
  static bool validate_record(const change_record_buff_t &record);

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
   *   - This function only consumes records tagged `Source::COPY_INFO`.
   *   - `Source::REDO_LOG` is dispatched to `LogParser` by the population
   *     worker and must never be interpreted as a MySQL row image.
   */
  int parse_and_apply_update(Rapid_load_context *context, table_id_t &table_id, const byte *old_start,
                             const byte *old_end_ptr, const byte *new_start, const byte *new_end_ptr);

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
  int parse_and_apply_insert(Rapid_load_context *context, table_id_t &table_id, const byte *start, const byte *end_ptr);

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
  int parse_and_apply_delete(Rapid_load_context *context, table_id_t &table_id, const byte *start, const byte *end_ptr);
};

/**
 * @brief hton se_after_commit: publish the Rapid-side transaction.
 * @param[in] arg  the server's Trans_param.
 */
void rapid_after_commit(void *arg);

/**
 * @brief hton se_before_rollback: undo the Rapid-side transaction.
 * @param[in] arg  the server's Trans_param.
 */
void rapid_before_rollback(void *arg);
}  // namespace DML

/**
 * @brief Capture side of the change-propagation path.
 *
 * A change source builds a change record out of the row images it holds and
 * hands it over to EnqueueCopyInfo() below: today the primary engine's DML
 * notifier (ha_shannon_rapid.cc: NotifyAfterInsert / NotifyAfterUpdate /
 * NotifyAfterDelete), and in the future the redo-log parser as well. Everything
 * after that point - the transaction identity stamped on the record, the queue
 * it enters and the primary COMMIT/ROLLBACK outcome that publishes or undoes it
 * - is owned here, so a source is left with nothing but building row images.
 *
 * These two deliberately live in Populate rather than Populate::DML so that the
 * redo-log parsing path can reuse them without depending on the COPY_INFO
 * specific machinery.
 */

/**
 * @brief Register Rapid as a statement participant of the current THD, and for
 *        an explicit transaction also as a final transaction participant.
 *
 * Must run before the first change record of the statement is enqueued: the
 * registration is what makes the server call the se_after_commit /
 * se_before_rollback hooks (rapid_after_commit / rapid_before_rollback) once the
 * primary transaction ends, which is what publishes the captured changes.
 *
 * @param[in] thd  thread whose transaction takes part in the propagation.
 */
void RegisterCopyInfoParticipant(THD *thd);

/**
 * @brief Stamp the transaction identity on a change record and enqueue it for
 *        the population worker.
 *
 * The record leaves this call carrying the real primary InnoDB creator id and
 * commit_scn == 0, i.e. it enters Rapid MVCC as an ACTIVE notification version;
 * the outcome published on primary COMMIT/ROLLBACK is what finalizes it.
 *
 * @param[in]     thd     thread the DML statement runs on.
 * @param[in,out] record  change record to enqueue; consumed on success.
 * @return true if the record was enqueued, false if the thread has no usable
 *         transaction and the change could not be captured.
 */
bool EnqueueCopyInfo(THD *thd, change_record_buff_t &&record);
}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_LOG_DML_NOTIFICATION_H__
