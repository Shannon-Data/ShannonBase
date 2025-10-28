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

   The fundmental code for imcs. for transaction.
*/
#ifndef __SHANNONBASE_TRANSACTION_H__
#define __SHANNONBASE_TRANSACTION_H__
#include <chrono>
#include <shared_mutex>
#include <vector>
#include "sql/current_thd.h"

#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/utils/utils.h"
class THD;
class trx_t;
class ReadView;
namespace ShannonBase {
namespace Imcs {
class Imcu;
}  // namespace Imcs
}  // namespace ShannonBase
namespace ShannonBase {
class TransactionCoordinator;
class Transaction : public MemoryObject {
 public:
  Transaction(THD *thd = current_thd);
  virtual ~Transaction();

  // here, we use innodb's trx_id_t as ours. the defined in innodb is: typedef ib_id_t trx_id_t;
  using ID = uint64_t;
  static constexpr ID MAX_ID = std::numeric_limits<uint64_t>::max();

  enum class ISOLATION_LEVEL : uint8 { READ_UNCOMMITTED, READ_COMMITTED, READ_REPEATABLE, SERIALIZABLE };

  enum class STATUS : uint8 { NOT_START, ACTIVE, PREPARED, COMMITTED_IN_MEMORY };

  static Transaction *get_or_create_trx(THD *thd);

  static Transaction *get_trx_from_thd(THD *const thd);

  static ISOLATION_LEVEL get_rpd_isolation_level(THD *thd);

  static void free_trx_from_thd(THD *const thd);

  void set_trx_on_thd(THD *const thd);

  void reset_trx_on_thd(THD *const thd);

  virtual void set_isolation_level(ISOLATION_LEVEL level) { m_iso_level = level; }
  virtual ISOLATION_LEVEL isolation_level() const { return m_iso_level; }

  virtual void set_read_only(bool read_only) { m_read_only = read_only; }
  virtual Transaction::ID get_id();

  virtual int begin(ISOLATION_LEVEL iso_level = ISOLATION_LEVEL::READ_REPEATABLE);
  virtual int commit();
  virtual int rollback();

  virtual int begin_stmt(ISOLATION_LEVEL iso_level = ISOLATION_LEVEL::READ_REPEATABLE);
  virtual int rollback_stmt();
  virtual int rollback_to_savepoint(void *const) { return SHANNON_SUCCESS; }

  virtual ::ReadView *acquire_snapshot();
  virtual int release_snapshot();
  virtual bool changes_visible(Transaction::ID trx_id, const char *table_name);
  virtual bool has_snapshot() const;
  virtual ::ReadView *get_snapshot() const;

  virtual bool is_auto_commit();
  virtual bool is_active();

  void register_imcu_modification(ShannonBase::Imcs::Imcu *imcu);

  uint64_t get_start_scn() const { return m_start_scn; }
  uint64_t get_commit_scn() const { return m_commit_scn; }

 private:
  friend class TransactionCoordinator;

  THD *m_thd{nullptr};
  bool m_read_only{false};
  trx_t *m_trx_impl{nullptr};
  ISOLATION_LEVEL m_iso_level{ISOLATION_LEVEL::READ_REPEATABLE};
  bool m_stmt_active{false};

  uint64_t m_start_scn{0};
  uint64_t m_commit_scn{0};
  bool m_registered_in_coordinator{false};
};

class TransactionCoordinator {
 public:
  struct TransactionInfo {
    Transaction::ID txn_id;
    Transaction *trx{nullptr};
    uint64_t start_scn;
    uint64_t commit_scn{0};
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point commit_time;

    enum Status { ACTIVE, PREPARING, COMMITTED, ABORTED } status;

    std::vector<ShannonBase::Imcs::Imcu *> modified_imcus;
  };

  static TransactionCoordinator &instance() {
    static TransactionCoordinator coordinator;
    return coordinator;
  }

  TransactionCoordinator(const TransactionCoordinator &) = delete;
  TransactionCoordinator &operator=(const TransactionCoordinator &) = delete;

  /**
   * Register a new transaction with the Coordinator
   * @param trx Transaction object pointer
   * @param iso_level Isolation level
   * @return Allocated SCN
   */
  uint64_t register_transaction(Transaction *trx, Transaction::ISOLATION_LEVEL iso_level);

  /**
   * Commit a transaction (called by Transaction::commit)
   * @param trx Transaction object pointer
   * @return Whether successful
   */
  bool commit_transaction(Transaction *trx);

  /**
   * Rollback a transaction (called by Transaction::rollback)
   * @param trx Transaction object pointer
   * @return Whether successful
   */
  bool rollback_transaction(Transaction *trx);

  /**
   * Unregister a transaction (called by Transaction::~Transaction)
   * @param trx Transaction object pointer
   */
  void unregister_transaction(Transaction *trx);

  /**
   * Register IMCU modification
   */
  void register_imcu_modification(Transaction::ID txn_id, ShannonBase::Imcs::Imcu *imcu);

  /**
   * Get current SCN
   */
  inline uint64_t get_current_scn() const { return m_global_scn.load(std::memory_order_acquire); }

  /**
   * Allocate next SCN
   */
  inline uint64_t allocate_scn() { return m_global_scn.fetch_add(1, std::memory_order_acq_rel); }

  /**
   * Get minimum active SCN (for garbage collection)
   */
  inline uint64_t get_min_active_scn() const { return m_min_active_scn.load(std::memory_order_acquire); }

  /**
   * Get active transaction count
   */
  inline size_t get_active_txn_count() const {
    std::shared_lock lock(m_txns_mutex);
    return m_active_txns.size();
  }

  /**
   * Get transaction information by transaction ID
   */
  std::optional<TransactionInfo> get_transaction_info(Transaction::ID txn_id) const;

  /**
   * Get snapshot of all active transactions
   */
  std::vector<TransactionInfo> get_active_transactions() const;

  /**
   * Check if transaction is active
   */
  bool is_transaction_active(Transaction::ID txn_id) const;

  /**
   * Print all currently active transactions
   */
  void dump_active_transactions(std::ostream &out) const;

  /**
   * Get statistics
   */
  struct Statistics {
    size_t active_count;
    size_t total_committed;
    size_t total_aborted;
    uint64_t current_scn;
    uint64_t min_active_scn;
  };

  Statistics get_statistics() const;

 private:
  TransactionCoordinator() = default;

  // Generate unique transaction ID
  inline Transaction::ID generate_txn_id() {
    static std::atomic<Transaction::ID> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
  }

  // Update minimum active SCN
  void update_min_active_scn();

  // Global SCN (System Change Number) - value of 0 indicates still in progress/not yet determined
  std::atomic<uint64_t> m_global_scn{1};

  // Active transactions map
  mutable std::shared_mutex m_txns_mutex;
  std::unordered_map<Transaction::ID, TransactionInfo> m_active_txns;

  // Minimum active SCN (for garbage collection)
  std::atomic<uint64_t> m_min_active_scn{UINT64_MAX};

  // Statistics
  std::atomic<size_t> m_total_committed{0};
  std::atomic<size_t> m_total_aborted{0};
};

class TransactionGuard {
 public:
  explicit TransactionGuard(Transaction *trx) : m_trx(trx) {}

  ~TransactionGuard() {
    if (m_trx && m_trx->is_active()) {
      m_trx->rollback();
    }
  }

  void commit() {
    if (m_trx) {
      m_trx->commit();
    }
  }

  Transaction *get() const { return m_trx; }

 private:
  Transaction *m_trx;
};

class TransactionJournal {
 public:
  // Status Enum
  enum Entry_Status {
    ACTIVE = 0,     // Active (uncommitted)
    COMMITTED = 1,  // Committed
    ABORTED = 2     // Rolled back
  };

  TransactionJournal(size_t capacity) : m_capacity(capacity) {}

  virtual ~TransactionJournal() { clear(); }

  TransactionJournal(TransactionJournal &&other) noexcept
      : m_capacity(other.m_capacity),
        m_entry_count(other.m_entry_count.load()),
        m_total_size(other.m_total_size.load()) {
    std::unique_lock lock1(m_mutex, std::defer_lock);
    std::unique_lock lock2(other.m_mutex, std::defer_lock);
    std::lock(lock1, lock2);

    m_entries = std::move(other.m_entries);
    m_txn_entries = std::move(other.m_txn_entries);
    m_active_txns = std::move(other.m_active_txns);

    other.m_entry_count.store(0);
    other.m_total_size.store(0);
  }

  TransactionJournal &operator=(TransactionJournal &&other) noexcept {
    if (this != &other) {
      std::unique_lock lock1(m_mutex, std::defer_lock);
      std::unique_lock lock2(other.m_mutex, std::defer_lock);
      std::lock(lock1, lock2);

      m_capacity = other.m_capacity;
      m_entries = std::move(other.m_entries);
      m_txn_entries = std::move(other.m_txn_entries);
      m_active_txns = std::move(other.m_active_txns);

      m_entry_count.store(other.m_entry_count.load());
      m_total_size.store(other.m_total_size.load());

      other.m_entry_count.store(0);
      other.m_total_size.store(0);
    }
    return *this;
  }

  TransactionJournal(const TransactionJournal &) = delete;
  TransactionJournal &operator=(const TransactionJournal &) = delete;

  // Log Entry
  struct Entry {
    // Basic Information
    row_id_t row_id : 20;   // Local row ID (supports 1M rows)
    uint8_t operation : 2;  // INSERT/UPDATE/DELETE
    uint8_t status : 2;     // ACTIVE/COMMITTED/ABORTED
    uint32_t reserved : 8;

    // Transaction Information
    Transaction::ID txn_id;                           // Transaction ID
    uint64_t scn;                                     // System Change Number (assigned at commit)
    std::chrono::system_clock::time_point timestamp;  // Timestamp

    // UPDATE Specific
    // Bitmap marking modified columns (256 columns, 32 bytes)
    std::bitset<SHANNON_MAX_COLUMNS> modified_columns;

    // Linked List Pointer
    Entry *prev;  // Points to previous version of the same row

    Entry() : row_id(0), operation(0), status(0), reserved(0), txn_id(0), scn(0), prev(nullptr) {}

    ~Entry() = default;
  };

  // Log Operations
  /**
   * Add log entry
   * @param entry: Log entry (move semantics)
   */
  void add_entry(Entry &&entry);

  /**
   * Commit transaction
   * @param txn_id: Transaction ID
   * @param commit_scn: Commit SCN
   */
  void commit_transaction(Transaction::ID txn_id, uint64_t commit_scn);

  /**
   * Abort transaction
   * @param txn_id: Transaction ID
   */
  void abort_transaction(Transaction::ID txn_id);

  // Visibility Checking
  /**
   * Check if row is visible to reader
   * @param row_id: Local row ID
   * @param reader_txn_id: Reader transaction ID
   * @param reader_scn: Reader snapshot SCN
   * @return: Returns true if visible
   */
  bool is_row_visible(row_id_t row_id, Transaction::ID reader_txn_id, uint64_t reader_scn) const;

  /**
   * Batch visibility check (vectorized)
   * @param start_row: Starting row
   * @param count: Number of rows
   * @param reader_txn_id: Reader transaction ID
   * @param reader_scn: Reader SCN
   * @param visibility_mask: Output bitmap (1 indicates visible)
   */
  void check_visibility_batch(row_id_t start_row, size_t count, Transaction::ID reader_txn_id, uint64_t reader_scn,
                              bit_array_t &visibility_mask) const;

  /**
   * Get row state at specified SCN
   * @param row_id: Local row ID
   * @param target_scn: Target SCN
   * @param modified_columns: Output modified columns (UPDATE operation)
   * @return: Operation type
   */
  ShannonBase::OPER_TYPE get_row_state_at_scn(row_id_t row_id, uint64_t target_scn,
                                              std::bitset<SHANNON_MAX_COLUMNS> *modified_columns = nullptr) const;

  /**
   * Clean up old versions
   * @param min_active_scn: Minimum active SCN (versions before this SCN can be cleaned)
   * @return: Number of entries cleaned
   */
  size_t purge(uint64_t min_active_scn);

  /**
   * Clean up aborted transactions
   */
  size_t purge_aborted();

  /**
   * Clear all logs
   */
  inline void clear() {
    std::unique_lock lock(m_mutex);

    m_entries.clear();
    m_txn_entries.clear();
    m_active_txns.clear();

    m_entry_count.store(0);
    m_total_size.store(0);
  }

  inline size_t get_entry_count() const { return m_entry_count.load(); }

  inline size_t get_total_size() const { return m_total_size.load(); }

  inline size_t get_active_txn_count() const {
    std::shared_lock lock(m_mutex);
    return m_active_txns.size();
  }

  /**
   * Print log content (for debugging)
   */
  void dump(std::ostream &out) const;

 private:
  // Configuration
  size_t m_capacity;  // IMCU capacity

  // Log entries indexed by row
  // key: local_row_id, value: version chain head (newest -> oldest)
  std::unordered_map<row_id_t, std::unique_ptr<Entry>> m_entries;

  // Indexed by transaction ID (for rollback)
  std::unordered_map<Transaction::ID, std::vector<Entry *>> m_txn_entries;

  // Active transaction set
  std::unordered_set<Transaction::ID> m_active_txns;

  // Concurrency Control
  mutable std::shared_mutex m_mutex;

  // Statistics
  std::atomic<size_t> m_entry_count{0};
  std::atomic<size_t> m_total_size{0};
};

}  // namespace ShannonBase
#endif  //__SHANNONBASE_TRANSACTION_H__