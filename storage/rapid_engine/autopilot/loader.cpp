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

   The fundmental code for imcs.

   Copyright (c) 2023, 2024, 2025 Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/autopilot/loader.h"
#if !defined(_WIN32)
#include <pthread.h>  // For pthread_setname_np
#else
#include <Windows.h>  // For SetThreadDescription
#endif

#include <limits.h>
#include <queue>

#include "sql/table.h"
#include "storage/innobase/include/srv0srv.h"

#include "storage/innobase/include/srv0shutdown.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/utils/utils.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_self_load_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
extern bool rpd_self_load_enabled;
extern ulonglong rpd_self_load_interval_seconds;  // default 24hurs
extern bool rpd_self_load_skip_quiet_check;
extern int rpd_self_load_base_relation_fill_percentage;  // default percentage 70%.

namespace Autopilot {
std::once_flag SelfLoadManager::one;
SelfLoadManager *SelfLoadManager::m_instance{nullptr};

int SelfLoadManager::initialize() {
  // to initialize RPD Mirror, scan information_schema.m_rpd_mirror_tables to get all table info.
  auto cat_tables_ptr = Utils::Util::open_table_by_name("information_schema", "m_rpd_mirror_tables", TL_READ);
  if (!cat_tables_ptr) {
    Utils::Util::close_table(cat_tables_ptr);
    return HA_ERR_GENERIC;
  }

  auto handler = cat_tables_ptr->file;
  if (handler->inited == handler::NONE && handler->ha_rnd_init(true)) {
    handler->ha_rnd_end();
    Utils::Util::close_table(cat_tables_ptr);
    return HA_ERR_GENERIC;
  }

  int tmp{HA_ERR_GENERIC};
  while ((tmp = handler->ha_rnd_next(cat_tables_ptr->record[0])) != HA_ERR_END_OF_FILE) {
    /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
     without locks. Now, do full scan, but multi-thread scan will impl in future. */
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    auto tb_info = std::make_unique<TableInfo>();
    std::string sch_tb_name;
    m_rpd_mirror_tables.emplace(sch_tb_name, std::move(tb_info));
  }
  Utils::Util::close_table(cat_tables_ptr);
  return SHANNON_SUCCESS;
}

int SelfLoadManager::deinitialize() {
  stop_self_load_worker();
  std::unique_lock lock(m_tables_mutex);
  m_rpd_mirror_tables.clear();
  return SHANNON_SUCCESS;
}

TableInfo *SelfLoadManager::get_table_info(const std::string &schema, const std::string &table) {
  std::shared_lock lock(m_tables_mutex);
  std::string full_name = schema + ":" + table;

  auto it = m_rpd_mirror_tables.find(full_name);
  return (it != m_rpd_mirror_tables.end()) ? it->second.get() : nullptr;
}

std::vector<TableInfo *> SelfLoadManager::get_all_tables() {
  std::shared_lock lock(m_tables_mutex);
  std::vector<TableInfo *> result;

  for (const auto &[full_name, table_info] : m_rpd_mirror_tables) {
    result.push_back(table_info.get());
  }

  return result;
}

int SelfLoadManager::add_table(const std::string &schema, const std::string &table,
                               const std::string &secondary_engine) {
  std::shared_lock lock(m_tables_mutex);

  auto table_info = std::make_unique<TableInfo>();
  table_info->schema_name = schema;
  table_info->table_name = table;
  table_info->secondary_engine = secondary_engine;

  // to check should we remove this table or not.
  if (!secondary_engine.empty() && secondary_engine != "RAPID" && secondary_engine != "NULL") {
    table_info->excluded_from_self_load = true;
  }

  m_rpd_mirror_tables.emplace(table_info->full_name(), std::move(table_info));
  return SHANNON_SUCCESS;
}

int SelfLoadManager::remove_table(const std::string &schema, const std::string &table) {
  std::unique_lock lock(m_tables_mutex);
  std::string full_name = schema + ":" + table;
  m_rpd_mirror_tables.erase(full_name);
  return SHANNON_SUCCESS;
}

int SelfLoadManager::update_table_state(const std::string &schema, const std::string &table,
                                        TableAccessStats::State state, TableAccessStats::LoadType load_type) {
  std::shared_lock lock(m_tables_mutex);
  std::string full_name = schema + ":" + table;

  auto it = m_rpd_mirror_tables.find(full_name);
  if (it != m_rpd_mirror_tables.end()) {
    std::lock_guard<std::mutex> stats_lock(it->second->stats.stats_mutex);
    it->second->stats.state = state;
    it->second->stats.load_type = load_type;
  }

  return SHANNON_SUCCESS;
}

void SelfLoadManager::update_access_stats(const std::string &schema, const std::string &table, bool executed_in_rpd,
                                          double execution_time, uint64_t table_size, uint64_t total_query_size) {
  std::shared_lock lock(m_tables_mutex);
  std::string full_name = schema + ":" + table;

  auto it = m_rpd_mirror_tables.find(full_name);
  if (it == m_rpd_mirror_tables.end() || it->second->excluded_from_self_load) {
    return;
  }

  auto &stats = it->second->stats;
  std::lock_guard<std::mutex> stats_lock(stats.stats_mutex);

  if (executed_in_rpd) {
    stats.heatwave_access_count++;
  } else {
    stats.mysql_access_count++;
  }

  stats.last_queried_time = std::chrono::system_clock::now();

  // compute importance: importance = |T1|/(|T1|+...+|Tq|) * QET
  if (total_query_size > 0) {
    double size_ratio = static_cast<double>(table_size) / total_query_size;
    double new_importance = size_ratio * execution_time;

    // accumlate the importance.
    double current_importance = stats.importance.load();
    stats.importance.store(current_importance + new_importance);
  }
}

static void self_load_coordinator_main() {}
void SelfLoadManager::start_self_load_worker() {
  if (m_worker_state.load() == loader_state_t::LOADER_STATE_EXIT) {
    m_worker_state.store(loader_state_t::LOADER_STATE_RUN);
    m_worker_thread = std::make_unique<std::thread>(&SelfLoadManager::self_load_worker_thread, this);
    srv_threads.m_rapid_self_load_cordinator =
        os_thread_create(rapid_self_load_thread_key, 0, self_load_coordinator_main);
  }
}

void SelfLoadManager::stop_self_load_worker() {
  if (m_worker_state.load() == loader_state_t::LOADER_STATE_RUN) {
    m_worker_state.store(loader_state_t::LOADER_STATE_EXIT);
    m_worker_cv.notify_all();
    if (m_worker_thread && m_worker_thread->joinable()) {
      m_worker_thread->join();
    }
    m_worker_thread.reset();
  }
}

void SelfLoadManager::self_load_worker_thread() {
  while (m_worker_state.load() == loader_state_t::LOADER_STATE_RUN) {
    std::unique_lock<std::mutex> lock(m_worker_mutex);

    auto timeout = std::chrono::seconds(ShannonBase::rpd_self_load_interval_seconds);
    if (m_worker_cv.wait_for(lock, timeout,
                             [this]() { return m_worker_state.load() != loader_state_t::LOADER_STATE_RUN; })) {
      break;
    }

    if (!ShannonBase::rpd_self_load_enabled) {
      continue;
    }

    // should be silient or not.
    if (!ShannonBase::rpd_self_load_skip_quiet_check) {
      int attempts = 0;
      while (!is_system_quiet() && attempts < MAX_QUIET_WAIT_ATTEMPTS) {
        std::this_thread::sleep_for(std::chrono::seconds(QUIET_WAIT_SECONDS));
        attempts++;
      }

      if (attempts >= MAX_QUIET_WAIT_ATTEMPTS) {
        continue;
      }
    }

    run_self_load_algorithm();
  }
}

bool SelfLoadManager::is_system_quiet() {
  auto now = std::chrono::system_clock::now();
  auto quiet_threshold = now - std::chrono::minutes(QUERY_QUIET_MINUTES);

  std::shared_lock lock(m_tables_mutex);
  for (const auto &[full_name, table_info] : m_rpd_mirror_tables) {
    std::lock_guard<std::mutex> stats_lock(table_info->stats.stats_mutex);
    if (table_info->stats.last_queried_time > quiet_threshold) {
      return false;
    }
  }

  // TODO: to check the table are under loading.
  // TODO: to check Change Propagation's delay.

  return true;
}

void SelfLoadManager::run_self_load_algorithm() {
  // step 1: decline the importance.
  decay_importance();

  // step 2: unload the clod m_rpd_mirror_tables.
  unload_cold_tables();

  // step 3: perform load/unload queue.
  prepare_load_unload_queues();

  // step 4: execute load/unload oper.
  run_load_unload_algorithm();
}

void SelfLoadManager::decay_importance() {
  auto now = std::chrono::system_clock::now();

  std::shared_lock lock(m_tables_mutex);
  for (auto &[full_name, table_info] : m_rpd_mirror_tables) {
    std::lock_guard<std::mutex> stats_lock(table_info->stats.stats_mutex);

    // Calculate the number of days since last accessed
    auto time_since_query = now - table_info->stats.last_queried_time;
    auto days = std::chrono::duration_cast<std::chrono::hours>(time_since_query).count() / 24.0;

    if (days > 0) {
      // Apply exponential decay: importance = importance * (decay_factor ^ days)
      double current_importance = table_info->stats.importance.load();
      double decayed_importance = current_importance * std::pow(IMPORTANCE_DECAY_FACTOR, days);

      // If importance decays below threshold, set to 0
      if (decayed_importance < IMPORTANCE_THRESHOLD) {
        decayed_importance = 0.0;
      }

      table_info->stats.importance.store(decayed_importance);
    }
  }
}

void SelfLoadManager::unload_cold_tables() {
  auto now = std::chrono::system_clock::now();
  auto cold_threshold = now - std::chrono::hours(COLD_TABLE_DAYS * 24);

  std::vector<std::string> tables_to_unload;

  {
    std::shared_lock lock(m_tables_mutex);
    for (const auto &[full_name, table_info] : m_rpd_mirror_tables) {
      std::lock_guard<std::mutex> stats_lock(table_info->stats.stats_mutex);

      // Check if it's a cold self-loaded table
      if (table_info->stats.load_type == TableAccessStats::SELF &&
          table_info->stats.state == TableAccessStats::LOADED && table_info->stats.importance.load() == 0.0 &&
          table_info->stats.last_queried_time < cold_threshold) {
        tables_to_unload.push_back(full_name);
      }
    }
  }

  // unload the cold table.
  for (const auto &full_name : tables_to_unload) {
    size_t pos = full_name.find(':');
    if (pos != std::string::npos) {
      std::string schema = full_name.substr(0, pos);
      std::string table = full_name.substr(pos + 1);
      perform_self_unload(schema, table);
    }
  }
}

void SelfLoadManager::prepare_load_unload_queues() {
  // in run_load_unload_algorithm
}

void SelfLoadManager::run_load_unload_algorithm() {
  std::priority_queue<LoadCandidate> load_queue;
  std::priority_queue<UnloadCandidate> unload_queue;

  {
    std::shared_lock lock(m_tables_mutex);
    for (const auto &[full_name, table_info] : m_rpd_mirror_tables) {
      if (table_info->excluded_from_self_load) {
        continue;
      }

      std::lock_guard<std::mutex> stats_lock(table_info->stats.stats_mutex);

      if (table_info->stats.state == TableAccessStats::NOT_LOADED && table_info->stats.importance.load() > 0.0) {
        LoadCandidate candidate;
        candidate.full_name = full_name;
        candidate.importance = table_info->stats.importance.load();
        candidate.estimated_size = table_info->estimated_size;
        load_queue.push(candidate);

      } else if (table_info->stats.state == TableAccessStats::LOADED &&
                 table_info->stats.load_type == TableAccessStats::SELF) {
        UnloadCandidate candidate;
        candidate.full_name = full_name;
        candidate.importance = table_info->stats.importance.load();
        unload_queue.push(candidate);
      }
    }
  }

  uint64_t memory_threshold = get_memory_threshold();
  uint64_t current_memory = get_current_memory_usage();

  while (!load_queue.empty() && current_memory < memory_threshold) {
    auto load_candidate = load_queue.top();
    load_queue.pop();

    // If more memory is needed, first unload the least important m_rpd_mirror_tables
    while (!unload_queue.empty() && current_memory + load_candidate.estimated_size > memory_threshold) {
      auto unload_candidate = unload_queue.top();
      unload_queue.pop();

      size_t pos = unload_candidate.full_name.find(':');
      if (pos != std::string::npos) {
        std::string schema = unload_candidate.full_name.substr(0, pos);
        std::string table = unload_candidate.full_name.substr(pos + 1);

        if (perform_self_unload(schema, table) == SHANNON_SUCCESS) {
          // TODO: Get actual freed memory size
          current_memory -= 1000000;
        }
      }
    }

    if (current_memory + load_candidate.estimated_size <= memory_threshold) {
      size_t pos = load_candidate.full_name.find(':');
      if (pos != std::string::npos) {
        std::string schema = load_candidate.full_name.substr(0, pos);
        std::string table = load_candidate.full_name.substr(pos + 1);

        if (perform_self_load(schema, table) == SHANNON_SUCCESS) {
          current_memory += load_candidate.estimated_size;
        }
      }
    }
  }
}

uint64_t SelfLoadManager::get_current_memory_usage() { return ShannonBase::rapid_allocated_mem_size; }

uint64_t SelfLoadManager::get_memory_threshold() {
  uint64_t max_memory = ShannonBase::rpd_mem_sz_max;
  uint32_t fill_percentage = ShannonBase::rpd_self_load_base_relation_fill_percentage;
  return (max_memory * fill_percentage) / 100;
}

bool SelfLoadManager::can_load_table(uint64_t table_size) {
  return get_current_memory_usage() + table_size <= get_memory_threshold();
}

int SelfLoadManager::perform_self_load(const std::string &schema, const std::string &table) {
  auto table_info = get_table_info(schema, table);
  if (!table_info) {
    return HA_ERR_GENERIC;
  }
  int result{SHANNON_SUCCESS};
#if 0
    //* Use Autopilot to estimate table size (if available)
    auto &autopilot = AutopilotIntegration::instance();
    uint64_t estimated_size = autopilot.estimate_table_size(schema, table);
    table_info->estimated_size = estimated_size;

    // Check if memory is sufficient
    if (!can_load_table(estimated_size)) {
      update_table_state(schema, table, TableAccessStats::INSUFFICIENT_MEMORY, TableAccessStats::SELF);
      return HA_ERR_GENERIC;
    }

    // Check for unsupported columns
    auto unsupported_columns = autopilot.get_unsupported_columns(schema, table);
    if (!unsupported_columns.empty()) {
      // Log warnings but continue loading
      // TODO: Record unsupported column information
    }

    Rapid_load_context context;
    context.m_schema_name = schema;
    context.m_table_name = table;
    context.m_thd = current_thd;

    TABLE *mysql_table = get_mysql_table(schema, table);
    if (!mysql_table) {
      return HA_ERR_GENERIC;
    }

    context.m_table = mysql_table;

    int optimal_threads [[maybe_unused]]= autopilot.get_optimal_load_threads(schema, table);
    // TODO: set the thread num.

    auto &imcs = *Imcs::Imcs::instance();
    int result = SHANNON_SUCCESS;

    if (context.m_extra_info.m_partition_infos.size() > 0) {
      result = imcs.load_parttable(&context, mysql_table);
    } else {
      result = imcs.load_table(&context, mysql_table);
    }

    if (result == SHANNON_SUCCESS) {
      // update the state to loaded.
      update_table_state(schema, table, TableAccessStats::LOADED, TableAccessStats::SELF);

      //  Updates the actually used memory (if different from estimate)
      // TODO: Get actual memory usage and update table_info->estimated_size

    } else {
      //failed，set the state to INSUFFICIENT_MEMORY.
      update_table_state(schema, table, TableAccessStats::INSUFFICIENT_MEMORY, TableAccessStats::SELF);
    }
#endif
  return result;
}

TABLE *SelfLoadManager::get_mysql_table(const std::string &schema, const std::string &table) {
  /**
   * TODO: Implement logic to fetch table definition from MySQL system
   * This requires integration with MySQL's table cache and definition system
   *
   * Temporarily returns nullptr. Actual implementation needs to:
   * 1. Open table definition
   * 2. Verify table exists and is accessible
   * 3. Check secondary_engine setting
   * 4. Return TABLE structure pointer
   */
  return nullptr;
}

int SelfLoadManager::perform_self_unload(const std::string &schema, const std::string &table) {
  // Checks if it's a user-loaded table
  auto table_info = get_table_info(schema, table);
  if (table_info && table_info->stats.load_type == TableAccessStats::USER) {
    // User-loaded m_rpd_mirror_tables are downgraded to self-loaded but not actually unloaded
    update_table_state(schema, table, TableAccessStats::LOADED, TableAccessStats::SELF);

    // my_printf_error(ER_SECONDARY_ENGINE_PLUGIN,
    //                 "Self-Load feature is enabled: table `%s`.`%s` demoted to self-loaded. "
    //                 "To unload it from the system completely run secondary unload again.",
    //                 MYF(ME_JUST_WARNING), schema.c_str(), table.c_str());

    return SHANNON_SUCCESS;
  }

  Rapid_load_context context;
  context.m_schema_name = schema;
  context.m_table_name = table;

  auto &imcs = *Imcs::Imcs::instance();
  int result = imcs.unload_table(&context, schema.c_str(), table.c_str(), false);

  if (result == SHANNON_SUCCESS) {
    // update state to unloaded.
    update_table_state(schema, table, TableAccessStats::NOT_LOADED, TableAccessStats::SELF);
  }

  return result;
}

}  // namespace Autopilot
}  // namespace ShannonBase
