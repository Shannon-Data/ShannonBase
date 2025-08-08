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

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "storage/innobase/include/os0thread-create.h"

#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"

class Field;
namespace ShannonBase {
namespace Autopilot {
constexpr uint64_t MAX_LOADER_TIMEOUT = 5000;  // timeout value.

enum class loader_state_t {
  LOADER_STATE_INIT,    /*!< self-loader thread instance created */
  LOADER_STATE_RUN,     /*!< self-loader thread should be running */
  LOADER_STATE_STOP,    /*!< self-loader thread should be stopped */
  LOADER_STATE_EXIT,    /*!< self-loader thread has been shutdown */
  LOADER_STATE_DISABLED /*!< self-loader thread was never started */
};

struct TableAccessStats {
  std::atomic<uint64_t> mysql_access_count{0};
  std::atomic<uint64_t> heatwave_access_count{0};
  std::atomic<double> importance{0.0};
  std::chrono::system_clock::time_point last_queried_time;

  enum State { NOT_LOADED, LOADED, INSUFFICIENT_MEMORY } state{NOT_LOADED};

  enum LoadType { SELF, USER } load_type{SELF};

  std::mutex stats_mutex;
  TableAccessStats() : last_queried_time(std::chrono::system_clock::now()) {}
};

// RPD Mirror Table Info.
struct TableInfo {
  std::string schema_name;
  std::string table_name;
  std::string secondary_engine;
  uint64_t estimated_size{0};
  TableAccessStats stats;
  bool excluded_from_self_load{false};

  std::string full_name() const { return schema_name + ":" + table_name; }
};

class SelfLoadManager {
 public:
  static SelfLoadManager &instance() {
    static SelfLoadManager instance;
    return instance;
  }

  int initialize();
  int deinitialize();

  // RPD Mirror management.
  int add_table(const std::string &schema, const std::string &table,
                const std::string &secondary_engine = ShannonBase::rapid_hton_name);
  int remove_table(const std::string &schema, const std::string &table);
  int update_table_state(const std::string &schema, const std::string &table, TableAccessStats::State state,
                         TableAccessStats::LoadType load_type);

  void update_access_stats(const std::string &schema, const std::string &table, bool executed_in_rpd,
                           double execution_time, uint64_t table_size, uint64_t total_query_size);

  // Self-Load thread management.
  void start_self_load_worker();
  void stop_self_load_worker();

  std::shared_ptr<TableInfo> get_table_info(const std::string &schema, const std::string &table);
  std::vector<std::shared_ptr<TableInfo>> get_all_tables();

  void set_enabled(bool enabled) { self_load_enabled.store(enabled); }
  void set_interval_seconds(uint32_t seconds) { interval_seconds.store(seconds); }
  void set_skip_quiet_check(bool skip) { skip_quiet_check.store(skip); }
  void set_memory_fill_percentage(uint32_t percentage) { memory_fill_percentage.store(percentage); }

  bool is_enabled() const { return self_load_enabled.load(); }
  uint32_t get_interval_seconds() const { return interval_seconds.load(); }
  bool should_skip_quiet_check() const { return skip_quiet_check.load(); }
  uint32_t get_memory_fill_percentage() const { return memory_fill_percentage.load(); }

 private:
  SelfLoadManager() = default;
  ~SelfLoadManager() = default;
  SelfLoadManager(const SelfLoadManager &) = delete;
  SelfLoadManager &operator=(const SelfLoadManager &) = delete;

  // Self-Load jobs.
  void self_load_worker_thread();
  bool is_system_quiet();
  void run_self_load_algorithm();
  void decay_importance();
  void unload_cold_tables();
  void prepare_load_unload_queues();
  void run_load_unload_algorithm();

  uint64_t get_current_memory_usage();
  uint64_t get_memory_threshold();
  bool can_load_table(uint64_t table_size);

  int perform_self_load(const std::string &schema, const std::string &table);
  int perform_self_unload(const std::string &schema, const std::string &table);
  TABLE *get_mysql_table(const std::string &schema, const std::string &table);

 private:
  std::atomic<bool> self_load_enabled{false};
  std::atomic<uint32_t> interval_seconds{86400};  // 24hurs
  std::atomic<bool> skip_quiet_check{false};
  std::atomic<uint32_t> memory_fill_percentage{70};

  // thread management.
  std::atomic<bool> worker_running{false};
  std::unique_ptr<std::thread> worker_thread;
  std::condition_variable worker_cv;
  std::mutex worker_mutex;

  // (RPD Mirror)
  std::shared_mutex tables_mutex;
  std::unordered_map<std::string, std::shared_ptr<TableInfo>> tables;

  // load/unload strategies.
  struct LoadCandidate {
    std::string full_name;
    double importance;
    uint64_t estimated_size;

    bool operator<(const LoadCandidate &other) const {
      return importance < other.importance;  // max heap
    }
  };

  struct UnloadCandidate {
    std::string full_name;
    double importance;

    bool operator<(const UnloadCandidate &other) const {
      return importance > other.importance;  // min heap.
    }
  };

  static constexpr int QUIET_WAIT_SECONDS = 300;
  static constexpr int MAX_QUIET_WAIT_ATTEMPTS = 10;
  static constexpr int QUERY_QUIET_MINUTES = 5;

  static constexpr double IMPORTANCE_DECAY_FACTOR = 0.9;  // decline 10% a dya.
  static constexpr double IMPORTANCE_THRESHOLD = 0.001;   // 99.9% threshold of decline.
  static constexpr int COLD_TABLE_DAYS = 3;
};

class AutoLoader : public MemoryObject {
 public:
  AutoLoader() = default;
  virtual ~AutoLoader() = default;

  // to launch log pop main thread.
  static void start();

  // to stop lop pop main thread.
  static void end();

  // whether the log pop main thread is active or not. true is alive, false dead.
  static bool active();

  // to print thread infos.
  static void print_info(FILE *file);

  // Enhanced status management
  static inline void set_status(loader_state_t stat) { m_state.store(stat, std::memory_order_release); }

  static inline loader_state_t get_status() { return m_state.load(std::memory_order_acquire); }

  static IB_thread m_rapid_loader_cordinator;
  static std::atomic<loader_state_t> m_state;
  static std::mutex m_notify_mutex;
  static std::condition_variable m_notify_cv;

  std::atomic<bool> m_running{false};
  std::thread m_worker;
  std::chrono::seconds m_interval{86400};  // 24hrs

 private:
  void unload_cold_tables() {}
  void execute_load_unload() {}
  void run() {}
};

}  // namespace Autopilot
}  // namespace ShannonBase
#endif  //__SHANNONBASE_AUTOPILOT_LOADER_H__