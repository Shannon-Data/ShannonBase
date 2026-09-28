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
/**
 * IMCS Auto-Load/Unload Management System
 *
 * Overview:
 * This subsystem implements intelligent automatic loading and unloading of tables
 * in the In-Memory Column Store (IMCS) based on usage patterns. The system
 * dynamically optimizes memory usage by keeping frequently accessed tables in memory
 * while evicting cold data.
 *
 * Key Components:
 * 1. Access Statistics Tracker:
 *    - Records table access frequency from both MySQL and IMCS
 *    - Calculates table importance scores based on:
 *      * Access frequency
 *      * Query execution time
 *      * Table size
 *    - Implements exponential decay for historical data
 *
 * 2. Decision Engine:
 *    - Periodically evaluates table importance (default: 24h interval)
 *    - Maintains two priority queues:
 *      * Load queue: Sorted by descending importance
 *      * Unload queue: Sorted by ascending importance
 *    - Implements memory threshold protection (default: 70% of allocated IMCS memory)
 *
 * 3. Execution Controller:
 *    - Performs actual load/unload operations during system quiet periods
 *    - Ensures user-loaded tables take precedence over auto-loaded ones
 *    - Maintains atomic operation state to prevent conflicts
 *
 * Operational Characteristics:
 * - Auto-loaded tables are clearly distinguished from user-loaded ones
 * - System automatically recovers after restarts using persisted statistics
 * - Minimal runtime overhead through:
 *   * Sampling-based statistics collection
 *   * Lock-free data structures for hot paths
 *   * Asynchronous background processing
 *
 * Configuration:
 * - rapid_auto_load_enabled: ON/OFF master switch
 * - rapid_auto_load_interval: Tuning frequency (seconds)
 * - rapid_auto_load_memory_threshold: Max memory utilization (%)
 *
 * Safety Mechanisms:
 * - Never unload tables actively involved in transactions
 * - Preserves user-loaded tables during memory pressure
 * - Graceful degradation under system stress
 *
 * Monitoring:
 * - Provides real-time visibility through:
 *   information_schema.rapid_auto_load_status
 *   performance_schema.rapid_auto_load_history
 *
 * Note: This feature requires the Autopilot component for accurate table size
 * estimation and optimal encoding selection.
 */

#ifndef __SHANNONBASE_AUTOPILOT_LOADER_H__
#define __SHANNONBASE_AUTOPILOT_LOADER_H__

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>  // once_flag
#include <shared_mutex>
#include <string>
#include <vector>

#include "sql/handler.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_table_info.h"

class Field;
class THD;
class Table_ref;
struct TABLE;
class IB_thread;

namespace ShannonBase {

/**
 * Backing store of performance_schema.rpd_mirror / performance_schema.rpd_tables:
 * one entry per tracked table.
 *
 * It is written by the DDL hooks, the load/unload path, the change-propagation
 * layer and the self-loader, and read by perfschema, the optimizer and the
 * recovery path.  No single one of those subsystems owns it, so it lives here
 * instead of inside any one of them: a private cache of the self-loader cannot
 * be the public data source of a perfschema table whose rows must survive
 * `rapid_self_load_enabled=OFF` and outlive the self-loader's own lifecycle.
 */
namespace RpdMirror {
class Registry final {
 public:
  Registry() = delete;

  /// DDL CREATE TABLE (or SECONDARY_LOAD of an existing table): insert the entry,
  /// or refresh the fields a re-creation can change.  The table becomes
  /// user-owned, so the self-loader will not evict it.
  static int upsert(uint tid, const std::string &schema, const std::string &table, const std::string &secondary_engine,
                    bool is_partition);

  /// Startup seeding from the data dictionary: insert the entry when the table is
  /// not tracked yet, leaving an existing entry alone.
  static void seed(uint tid, const std::string &schema, const std::string &table, const std::string &secondary_engine,
                   bool is_partition, uint64_t estimated_size);

  /// DROP TABLE: forget the table completely.
  static int erase(const std::string &schema, const std::string &table);

  /// SECONDARY_UNLOAD: keep the entry, but stop reporting it as loaded.
  static int mark_unloaded(const std::string &schema, const std::string &table);

  /// Record the lifecycle state and the ownership (user vs self loaded) of a
  /// tracked table.
  static int set_state(const std::string &schema, const std::string &table, table_access_stats_t::State state,
                       ShannonBase::load_type_t load_type);

  /// Quarantine the table carrying in-memory id @a tid: it stays visible in
  /// rpd_tables with a terminal state instead of reading as available.  A no-op
  /// when no entry carries that id.
  static void mark_stale(uint tid, stale_reason_t reason);

  /// Consistent value copy of every entry, sorted by (schema, table).  Callers
  /// iterate the result after the lock is released, so a concurrent unload
  /// cannot invalidate a scan in progress.
  static std::vector<TableInfoSnapshot> snapshot();

  /// Locked entry count, for the perfschema shares' static row-count estimate.
  static size_t count();

  /// Shared handle on one entry, looked up by fully qualified name
  /// ("schema.table"); nullptr when the table is not tracked.
  static std::shared_ptr<TableInfo> find(const std::string &full_name);

  /// Visit every entry under the registry lock.  The callback runs with that lock
  /// held, so it must not call back into the registry.
  template <typename Fn>
  static void for_each(Fn &&fn) {
    std::shared_lock lock(m_mutex);
    for (auto &entry : m_tables) {
      if (entry.second) fn(entry.first, *entry.second);
    }
  }

  /// Like for_each(), but stops at the first entry the callback reports true for.
  template <typename Fn>
  static bool any_of(Fn &&fn) {
    std::shared_lock lock(m_mutex);
    for (auto &entry : m_tables) {
      if (entry.second && fn(entry.first, *entry.second)) return true;
    }
    return false;
  }

  /// Forget every entry.  Called on plugin shutdown so a re-install does not
  /// inherit the rows of the previous lifetime.
  static void reset();

 private:
  static std::shared_mutex m_mutex;
  static std::unordered_map<std::string, std::shared_ptr<TableInfo>> m_tables;
};

/// Bring every loaded entry's load_status / stale_reason / pool_type back in step
/// with the health of its change-propagation buffer.  When @a self_loaded_stale
/// is non-null, self-loaded tables that went stale are appended to it for the
/// self-loader to unload; pass nullptr to only refresh the reported state.
void refresh_propagation_health(std::vector<std::pair<std::string, std::string>> *self_loaded_stale);

}  // namespace RpdMirror

namespace Autopilot {
namespace detail {

inline uint64_t memory_threshold_bytes(uint64_t max_memory, int fill_percentage) {
  const uint64_t percentage = static_cast<uint64_t>(std::clamp(fill_percentage, 0, 100));
  // Split the calculation so even a UINT64_MAX memory limit cannot overflow.
  return (max_memory / 100) * percentage + ((max_memory % 100) * percentage) / 100;
}

inline bool fits_memory_budget(uint64_t used, uint64_t requested, uint64_t threshold) {
  return used <= threshold && requested <= threshold - used;
}

inline bool is_system_schema_name(const char *schema_name) {
  if (schema_name == nullptr) return false;
  const auto equals_ascii_case_insensitive = [schema_name](const char *expected) {
    const char *actual = schema_name;
    while (*actual != '\0' && *expected != '\0') {
      if (std::tolower(static_cast<unsigned char>(*actual)) != std::tolower(static_cast<unsigned char>(*expected)))
        return false;
      ++actual;
      ++expected;
    }
    return *actual == '\0' && *expected == '\0';
  };

  return equals_ascii_case_insensitive("mysql") || equals_ascii_case_insensitive("information_schema") ||
         equals_ascii_case_insensitive("performance_schema") || equals_ascii_case_insensitive("sys");
}

}  // namespace detail

enum class loader_state_t {
  LOADER_STATE_INIT = 0, /*!< self-loader thread instance created */
  LOADER_STATE_RUN,      /*!< self-loader thread should be running */
  LOADER_STATE_STOP,     /*!< self-loader thread should be stopped */
  LOADER_STATE_EXIT,     /*!< self-loader thread has been shutdown */
  LOADER_STATE_DISABLED  /*!< self-loader thread was never started */
};

class SelfLoadManager {
 public:
  static SelfLoadManager *instance() {
    std::call_once(one, [] { m_instance = std::make_unique<SelfLoadManager>(); });
    return m_instance.get();
  }

  void start() {
    if (!m_instance.get() || !m_intialized) return;
    start_self_load_worker();
  }

  void shutdown() {
    if (!m_instance.get() || !m_intialized) return;
    stop_self_load_worker();
  }

  inline bool initialized() { return m_intialized.load(); }

  /// Record the per-query access statistics (counts, last-query timestamps,
  /// importance, partitions touched) into the RPD Mirror rows.
  void update_table_stats(THD *thd, Table_ref *table_lists, SelectExecutedIn executed_in);

  bool is_system_quiet();

 public:
  // Self-Load thread management.
  void start_self_load_worker();
  void stop_self_load_worker();

  void run_self_load_algorithm();

  static std::atomic<loader_state_t> m_worker_state;
  static std::condition_variable m_worker_cv;
  static std::mutex m_worker_mutex;

  static constexpr int QUIET_WAIT_SECONDS = 300;
  static constexpr int MAX_QUIET_WAIT_ATTEMPTS = 10;
  static constexpr int QUERY_QUIET_MINUTES = 5;
  static constexpr double IMPORTANCE_DECAY_FACTOR = 0.464;  // 0.464^3 ≈ 0.1 (3 days decline 90%)
  static constexpr double IMPORTANCE_THRESHOLD = 0.001;     // 99.9% threshold of decline.
  static constexpr int COLD_TABLE_DAYS = 3;
  static constexpr double UPDATE_WEIGHT = 0.2;  // A smaller weight makes importance changes smoother

 public:
  SelfLoadManager();
  ~SelfLoadManager();

 private:
  SelfLoadManager(const SelfLoadManager &) = delete;
  SelfLoadManager &operator=(const SelfLoadManager &) = delete;

  int initialize();
  int deinitialize();

  // Self-Load jobs.
  void reconcile_propagation_state();

  void decay_importance();
  void unload_cold_tables();
  void prepare_load_unload_queues();
  void run_load_unload_algorithm();

  uint64_t get_current_memory_usage();
  uint64_t get_memory_threshold();
  bool can_load_table(uint64_t table_size);

  int perform_self_load(const std::string &schema, const std::string &table);
  int perform_self_unload(const std::string &schema, const std::string &table);

  int load_mysql_table_ids();
  int load_mysql_schema_info();
  int load_mysql_table_stats();
  int load_mysql_tables_info();

  /// Fully qualified "schema.table" name of @a table, or an empty string.
  static std::string full_name_of(TABLE *table);

  void update_table_importance(TableInfo *table_info, uint64_t total_query_size, double query_execution_time,
                               SelectExecutedIn executed_in);

  std::optional<std::string> extract_secondary_engine(const std::string &input);

  bool worker_active();

  bool is_system_schema(const char *schema_name) { return detail::is_system_schema_name(schema_name); }

 private:
  // load/unload strategies.
  struct SHANNON_ALIGNAS LoadCandidate {
    std::string full_name;
    double importance;
    uint64_t estimated_size;

    bool operator<(const LoadCandidate &other) const {
      return importance < other.importance;  // max heap
    }
  };

  struct SHANNON_ALIGNAS UnloadCandidate {
    std::string full_name;
    double importance;

    bool operator<(const UnloadCandidate &other) const {
      return importance > other.importance;  // min heap.
    }
  };

  static std::once_flag one;
  static std::unique_ptr<SelfLoadManager> m_instance;
  std::atomic<bool> m_intialized{false};

  // format: <schema_id, schema_name>
  std::unordered_map<int, std::string> m_schema_tables;

  // format: <schema_name+"."+table_name, estimated_size>
  std::unordered_map<std::string, uint64_t> m_table_stats;

  // format: <schema_name+"/"+table_name, table_id>
  std::unordered_map<std::string, uint64_t> m_table_ids;

  // mysql.tables.
  // schema_id
  static constexpr uint FIELD_SCH_ID_OFFSET_TABLES = 1;
  // schema_name
  static constexpr uint FIELD_NAME_OFFSET_TABLES = 2;
  // engine
  static constexpr uint FIELD_ENGINE_OFFSET_TABLES = 4;
  // comment
  static constexpr uint FIELD_COMMENT_OFFSET_TABLES = 8;
  // options
  static constexpr uint FIELD_OPTIONS_OFFSET_TABLES = 10;

  // mysql.schemata.
  // schema id
  static constexpr uint FIELD_CAT_ID_OFFSET_SCHEMA = 0;
  // schema name
  static constexpr uint FIELD_CAT_NAME_OFFSET_SCHEMA = 2;

  // mysql.table_stats
  // schema name
  static constexpr uint FIELD_SCH_NAME_OFFSET_STATS = 0;
  // table name
  static constexpr uint FIELD_TABLE_NAME_OFFSET_STATS = 1;
  // table # of rows
  static constexpr uint FIELD_TABLE_ROWS_OFFSET_STATS = 2;
  // data length
  static constexpr uint FIELD_DATA_LEN_OFFSET_STATS = 4;
  // index length
  static constexpr uint FIELD_INDEX_LEN_OFFSET_STATS = 6;

  // information_schema.INNODB_TABLES
  // schema id
  static constexpr uint FIELD_CAT_ID_OFFSET_INNODB_TABLES = 0;
  // schema name
  static constexpr uint FIELD_CAT_NAME_OFFSET_INNODB_TABLES = 1;
};
}  // namespace Autopilot
}  // namespace ShannonBase
#endif  //__SHANNONBASE_AUTOPILOT_LOADER_H__
