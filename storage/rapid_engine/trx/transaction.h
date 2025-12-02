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
#include <future>
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
  // here, we use innodb's trx_id_t as ours. the defined in innodb is: typedef ib_id_t trx_id_t;
  using ID = uint64_t;
  static constexpr ID MAX_ID = std::numeric_limits<uint64_t>::max();

  class VersionManager {
   public:
    // Snapshot structure
    struct Snapshot {
      uint64_t scn;                              // Snapshot SCN
      std::vector<Transaction::ID> active_txns;  // Active transactions at snapshot time
      std::chrono::steady_clock::time_point created_at;

      Snapshot() : scn(0) {}

      /**
       * Check if a version is visible
       * @param version_scn SCN when version was created
       * @param creator_txn Transaction that created the version
       * @param reader_txn Current transaction doing the read
       */
      bool is_visible(uint64_t version_scn, Transaction::ID creator_txn, Transaction::ID reader_txn) const {
        // Rule 1: Own modifications are always visible
        if (creator_txn == reader_txn) return true;
        // Rule 2: Version created after snapshot is not visible
        if (version_scn > scn) return false;
        // Rule 3: Version created by uncommitted transaction is not visible
        for (auto txn_id : active_txns) {
          if (txn_id == creator_txn) return false;
        }
        // Rule 4: Committed version before snapshot is visible
        return true;
      }
    };

    static VersionManager &instance() {
      static VersionManager vm;
      return vm;
    }

    VersionManager(const VersionManager &) = delete;
    VersionManager &operator=(const VersionManager &) = delete;

    /**
     * Get current SCN
     */
    inline uint64_t get_current_scn() const { return m_global_scn.load(std::memory_order_acquire); }

    /**
     * Allocate next SCN
     * This is the ONLY place where SCN is allocated
     */
    inline uint64_t allocate_scn() { return m_global_scn.fetch_add(1, std::memory_order_acq_rel); }

    /**
     * Batch allocate SCNs (for batch commit optimization)
     * @param count Number of SCNs to allocate
     * @return Starting SCN of the batch
     */
    inline uint64_t allocate_scn_batch(size_t count) {
      return m_global_scn.fetch_add(count, std::memory_order_acq_rel);
    }

    /**
     * Create snapshot for transaction
     * @param active_txns Current active transaction list
     * @return Snapshot object
     */
    Snapshot create_snapshot(const std::vector<Transaction::ID> &active_txns) {
      Snapshot snapshot;
      snapshot.scn = get_current_scn();
      snapshot.active_txns = active_txns;
      snapshot.created_at = std::chrono::steady_clock::now();

      // Update statistics
      m_snapshot_count.fetch_add(1, std::memory_order_relaxed);
      return snapshot;
    }

    /**
     * Check if a version is visible to a snapshot
     * This is the central visibility checking logic
     */
    bool is_visible(const Snapshot &snapshot, uint64_t version_scn, Transaction::ID creator_txn,
                    Transaction::ID reader_txn) const {
      return snapshot.is_visible(version_scn, creator_txn, reader_txn);
    }

    /**
     * Batch visibility check (SIMD-optimizable)
     * @param snapshot Reader's snapshot
     * @param version_scns Array of version SCNs
     * @param creator_txns Array of creator transaction IDs
     * @param count Number of versions to check
     * @param results Output bitmap (1 = visible)
     */
    void check_visibility_batch(const Snapshot &snapshot, const uint64_t *version_scns,
                                const Transaction::ID *creator_txns, Transaction::ID reader_txn, size_t count,
                                bit_array_t &results) const {
      for (size_t i = 0; i < count; ++i) {
        bool visible = snapshot.is_visible(version_scns[i], creator_txns[i], reader_txn);
        if (visible)
          Utils::Util::bit_array_set(&results, i);
        else
          Utils::Util::bit_array_reset(&results, i);
      }
    }

    /**
     * Update minimum active SCN (called when transaction commits/aborts)
     * @param active_txns Current active transaction list with their start SCNs
     */
    void update_min_active_scn(const std::unordered_map<Transaction::ID, uint64_t> &active_txns) {
      if (active_txns.empty()) {
        m_min_active_scn.store(get_current_scn(), std::memory_order_release);
        return;
      }

      uint64_t min_scn = UINT64_MAX;
      for (const auto &[txn_id, start_scn] : active_txns) {
        if (start_scn < min_scn) min_scn = start_scn;
      }

      m_min_active_scn.store(min_scn, std::memory_order_release);
    }

    /**
     * Get minimum active SCN (for garbage collection)
     * Any version older than this SCN can be safely garbage collected
     */
    inline uint64_t get_min_active_scn() const { return m_min_active_scn.load(std::memory_order_acquire); }

    /**
     * Get GC watermark with safety margin
     * @param safety_margin_scn Keep versions within this many SCNs
     */
    inline uint64_t get_gc_watermark(uint64_t safety_margin_scn = 1000) const {
      uint64_t min_scn = m_min_active_scn.load(std::memory_order_acquire);
      return (min_scn > safety_margin_scn) ? (min_scn - safety_margin_scn) : 0;
    }

    struct Statistics {
      uint64_t current_scn;
      uint64_t min_active_scn;
      uint64_t scn_range;  // current_scn - min_active_scn
      size_t snapshot_count;
    };

    Statistics get_statistics() const {
      Statistics stats;
      stats.current_scn = get_current_scn();
      stats.min_active_scn = get_min_active_scn();
      stats.scn_range = stats.current_scn - stats.min_active_scn;
      stats.snapshot_count = m_snapshot_count.load();
      return stats;
    }

   private:
    VersionManager() : m_global_scn(1), m_min_active_scn(UINT64_MAX), m_snapshot_count(0) {}

    // Global SCN counter (monotonically increasing)
    // This is the SINGLE SOURCE OF TRUTH for SCN allocation
    std::atomic<uint64_t> m_global_scn;

    // Minimum active SCN (for garbage collection watermark)
    std::atomic<uint64_t> m_min_active_scn;

    // Statistics
    std::atomic<size_t> m_snapshot_count;
  };

  Transaction(THD *thd = current_thd);
  virtual ~Transaction();

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

  // using innodb transaction impl for us.
  trx_t *m_trx_impl{nullptr};

  ISOLATION_LEVEL m_iso_level{ISOLATION_LEVEL::READ_REPEATABLE};

  bool m_stmt_active{false};
  bool m_read_only{true};

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

  struct Statistics {
    size_t active_count;
    size_t total_committed;
    size_t total_aborted;
    uint64_t current_scn;
    uint64_t min_active_scn;

    // Cache statistics
    size_t snapshot_cache_hits;
    size_t snapshot_cache_misses;
    size_t visibility_cache_hits;
    size_t visibility_cache_misses;
  };

  // Batch commit
  struct BatchCommitRequest {
    Transaction *trx;
    std::promise<uint64_t> commit_scn_promise;
  };

  struct CachedVisibility {
    std::unique_ptr<bit_array_t> bitmap;
    std::chrono::steady_clock::time_point created_at;
    std::atomic<size_t> access_count{0};

    CachedVisibility() = default;
    explicit CachedVisibility(std::unique_ptr<bit_array_t> bm)
        : bitmap(std::move(bm)), created_at(std::chrono::steady_clock::now()), access_count(0) {}

    CachedVisibility(CachedVisibility &&other) noexcept
        : bitmap(std::move(other.bitmap)),
          created_at(std::move(other.created_at)),
          access_count(other.access_count.load()) {}

    CachedVisibility &operator=(CachedVisibility &&other) noexcept {
      if (this != &other) {
        bitmap = std::move(other.bitmap);
        created_at = std::move(other.created_at);
        access_count.store(other.access_count.load());
      }
      return *this;
    }

    CachedVisibility(const CachedVisibility &) = delete;
    CachedVisibility &operator=(const CachedVisibility &) = delete;
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

  Transaction::VersionManager::Snapshot create_snapshot();

  const bit_array_t *get_cached_visibility(void *imcu, uint64_t scn);

  void cache_visibility(void *imcu, uint64_t scn, std::unique_ptr<bit_array_t> bitmap);

  void invalidate_visibility_cache(void *imcu);

  std::future<uint64_t> commit_transaction_async(Transaction *trx);

  /**
   * Get current SCN
   */
  inline uint64_t get_current_scn() const { return Transaction::VersionManager::instance().get_current_scn(); }

  /**
   * Allocate next SCN
   */
  inline uint64_t allocate_scn() { return Transaction::VersionManager::instance().allocate_scn(); }

  /**
   * Get minimum active SCN (for garbage collection)
   */
  inline uint64_t get_min_active_scn() const { return Transaction::VersionManager::instance().get_min_active_scn(); }

  inline uint64_t get_gc_watermark(uint64_t safety_margin = 1000) const {
    return Transaction::VersionManager::instance().get_gc_watermark(safety_margin);
  }

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

  Statistics get_statistics() const;

 private:
  TransactionCoordinator()
      : m_max_snapshot_cache_size(128),
        m_max_visibility_cache_entries(1024),
        m_batch_commit_size(32),
        m_batch_running(true) {
    // Start batch commit worker thread
    m_batch_commit_worker = std::thread(&TransactionCoordinator::batch_commit_worker_loop, this);
  }

  ~TransactionCoordinator() {
    m_batch_running = false;
    m_batch_commit_cv.notify_all();
    if (m_batch_commit_worker.joinable()) {
      m_batch_commit_worker.join();
    }
  }

  // Update minimum active SCN
  void update_min_active_scn();

  // Snapshot cache
  std::optional<Transaction::VersionManager::Snapshot> get_cached_snapshot(
      uint64_t scn, const std::vector<Transaction::ID> &active_txns) {
    std::shared_lock lock(m_snapshot_cache_mutex);

    for (const auto &cached : m_snapshot_cache) {
      if (cached.scn == scn && cached.active_txns == active_txns) {
        return cached;
      }
    }
    return std::nullopt;
  }

  void cache_snapshot(const Transaction::VersionManager::Snapshot &snapshot) {
    std::unique_lock lock(m_snapshot_cache_mutex);

    if (m_snapshot_cache.size() >= m_max_snapshot_cache_size) {
      m_snapshot_cache.erase(m_snapshot_cache.begin());
    }
    m_snapshot_cache.push_back(snapshot);
  }

  // Visibility cache eviction
  void evict_visibility_cache_lfu() {
    auto to_remove = std::min_element(
        m_visibility_cache.begin(), m_visibility_cache.end(),
        [](const auto &a, const auto &b) { return a.second.access_count.load() < b.second.access_count.load(); });

    if (to_remove != m_visibility_cache.end()) {
      m_visibility_cache.erase(to_remove);
    }
  }

  // Batch commit worker
  void batch_commit_worker_loop() {
    while (m_batch_running) {
      std::vector<BatchCommitRequest> batch;

      {
        std::unique_lock lock(m_batch_commit_mutex);

        m_batch_commit_cv.wait_for(lock, std::chrono::milliseconds(1), [this] {
          return m_pending_commits.size() >= m_batch_commit_size || !m_batch_running;
        });

        if (!m_pending_commits.empty()) {
          batch = std::move(m_pending_commits);
          m_pending_commits.clear();
        }
      }

      if (!batch.empty()) {
        process_batch_commits(batch);
      }
    }
  }

  void process_batch_commits(std::vector<BatchCommitRequest> &batch) {
    uint64_t base_scn = Transaction::VersionManager::instance().allocate_scn_batch(batch.size());

    for (size_t i = 0; i < batch.size(); ++i) {
      auto &req = batch[i];
      uint64_t commit_scn = base_scn + i;

      commit_transaction(req.trx);
      req.commit_scn_promise.set_value(commit_scn);
    }
  }

  // Transaction tracking
  mutable std::shared_mutex m_txns_mutex;
  std::unordered_map<Transaction::ID, TransactionInfo> m_active_txns;

  std::atomic<size_t> m_total_committed{0};
  std::atomic<size_t> m_total_aborted{0};

  // Snapshot cache
  mutable std::shared_mutex m_snapshot_cache_mutex;
  std::vector<Transaction::VersionManager::Snapshot> m_snapshot_cache;
  size_t m_max_snapshot_cache_size;
  std::atomic<size_t> m_snapshot_cache_hits{0};
  std::atomic<size_t> m_snapshot_cache_misses{0};

  // Visibility cache
  struct VisibilityCacheKey {
    void *imcu_ptr;
    uint64_t scn;

    bool operator==(const VisibilityCacheKey &other) const { return imcu_ptr == other.imcu_ptr && scn == other.scn; }
  };

  struct VisibilityCacheKeyHash {
    size_t operator()(const VisibilityCacheKey &key) const {
      return std::hash<void *>{}(key.imcu_ptr) ^ (std::hash<uint64_t>{}(key.scn) << 1);
    }
  };

  mutable std::shared_mutex m_visibility_cache_mutex;
  std::unordered_map<VisibilityCacheKey, CachedVisibility, VisibilityCacheKeyHash> m_visibility_cache;
  size_t m_max_visibility_cache_entries;
  std::atomic<size_t> m_visibility_cache_hits{0};
  std::atomic<size_t> m_visibility_cache_misses{0};

  std::mutex m_batch_commit_mutex;
  std::condition_variable m_batch_commit_cv;
  std::vector<BatchCommitRequest> m_pending_commits;
  size_t m_batch_commit_size;
  std::atomic<bool> m_batch_running;
  std::thread m_batch_commit_worker;
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
  bool is_visible(row_id_t row_id, Transaction::ID reader_txn_id, uint64_t reader_scn) const;

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