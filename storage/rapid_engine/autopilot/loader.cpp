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

#include <optional>
#include <queue>
#include <regex>

#include "sql/sql_base.h"
#include "sql/sql_table.h"
#include "sql/table.h"

#include "storage/innobase/include/os0thread-create.h"
#include "storage/innobase/include/srv0shutdown.h"
#include "storage/innobase/include/srv0srv.h"

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_self_load_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
extern bool shannon_rpd_self_load_enabled;
extern ulonglong shannon_rpd_self_load_interval_sec;  // default 24hurs
extern bool shannon_rpd_self_load_skip_quiet_check;
extern int shannon_rpd_self_load_base_relation_fill_percentage;  // default percentage 70%.
extern ulonglong shannon_rpd_purge_batch_size;

namespace Populate {
extern std::shared_mutex shannon_pop_table_mutex;
extern std::multiset<std::string> shannon_pop_tables;

// how many data was in shannon_pop_buff?
extern std::atomic<uint64> shannon_pop_data_sz;
}  // namespace Populate

namespace Autopilot {
// static members initialization.
std::once_flag SelfLoadManager::one;
SelfLoadManager *SelfLoadManager::m_instance{nullptr};
std::atomic<loader_state_t> SelfLoadManager::m_worker_state{loader_state_t::LOADER_STATE_EXIT};
std::condition_variable SelfLoadManager::m_worker_cv;
std::mutex SelfLoadManager::m_worker_mutex;

std::shared_mutex SelfLoadManager::m_tables_mutex;
std::unordered_map<std::string, std::unique_ptr<TableInfo>> SelfLoadManager::m_rpd_mirror_tables;

class HandlerGuard {
 public:
  HandlerGuard(THD *thd, TABLE *tb) : m_thd(thd), m_table_ptr(tb) {}
  ~HandlerGuard() {}

 private:
  THD *m_thd{nullptr};
  TABLE *m_table_ptr{nullptr};
};

std::optional<std::string> SelfLoadManager::extract_secondary_engine(const std::string &input) {
  auto eng_str = input;
  std::transform(eng_str.begin(), eng_str.end(), eng_str.begin(), [](unsigned char c) { return std::toupper(c); });
  const std::string key = "SECONDARY_ENGINE=";
  size_t pos = eng_str.find(key);
  if (pos == std::string::npos) return eng_str;

  pos += key.length();
  size_t end_pos = eng_str.find_first_of(";", pos);
  if (end_pos == std::string::npos) end_pos = eng_str.length();
  return eng_str.substr(pos, end_pos - pos);
}

// to scan mysq.schema, to get all schem information. such as schema_id, schema_name, etc.
int SelfLoadManager::load_mysql_schema_info() {
  auto cat_tables_ptr = Utils::Util::open_table_by_name(current_thd, "mysql", "schemata", TL_READ_WITH_SHARED_LOCKS);
  if (!cat_tables_ptr) {
    Utils::Util::close_table(current_thd, cat_tables_ptr);
    return HA_ERR_GENERIC;
  }

  // must read from secondary engine.
  /* Read the traning data into train_data vector from rapid engine. here, we use training data
  as lablels too */
  HandlerGuard garud(current_thd, cat_tables_ptr);
  if (cat_tables_ptr->file->inited == handler::NONE && cat_tables_ptr->file->ha_rnd_init(true)) {
    Utils::Util::close_table(current_thd, cat_tables_ptr);
    return HA_ERR_GENERIC;
  }

  int tmp{HA_ERR_GENERIC};
  ShannonBase::Utils::ColumnMapGuard guard(cat_tables_ptr);

  while ((tmp = cat_tables_ptr->file->ha_rnd_next(cat_tables_ptr->record[0])) != HA_ERR_END_OF_FILE) {
    /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
     without locks. Now, do full scan, but multi-thread scan will impl in future. */
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;
    auto sch_id_fld = *(cat_tables_ptr->field + FIELD_CAT_ID_OFFSET_SCHEMA);
    auto sch_id = sch_id_fld->val_int();

    auto sch_name_fld = *(cat_tables_ptr->field + FIELD_CAT_NAME_OFFSET_SCHEMA);
    String sch_name_str;
    auto sch_name = std::string(sch_name_fld->val_str(&sch_name_str)->c_ptr());
    m_schema_tables.emplace(sch_id, sch_name);
  }
  cat_tables_ptr->file->ha_rnd_end();

  Utils::Util::close_table(current_thd, cat_tables_ptr);
  return SHANNON_SUCCESS;
}

// to scan mysq.table_stats, to get all statistics information. such as row count, data size, index size, etc.
int SelfLoadManager::load_mysql_table_stats() {
  auto cat_tables_ptr = Utils::Util::open_table_by_name(current_thd, "mysql", "table_stats", TL_READ_WITH_SHARED_LOCKS);
  if (!cat_tables_ptr) {
    Utils::Util::close_table(current_thd, cat_tables_ptr);
    return HA_ERR_GENERIC;
  }

  // must read from secondary engine.
  /* Read the traning data into train_data vector from rapid engine. here, we use training data
  as lablels too */
  HandlerGuard garud(current_thd, cat_tables_ptr);
  if (cat_tables_ptr->file->inited == handler::NONE && cat_tables_ptr->file->ha_rnd_init(true)) {
    Utils::Util::close_table(current_thd, cat_tables_ptr);
    return HA_ERR_GENERIC;
  }

  int tmp{HA_ERR_GENERIC};
  ShannonBase::Utils::ColumnMapGuard guard(cat_tables_ptr);

  while ((tmp = cat_tables_ptr->file->ha_rnd_next(cat_tables_ptr->record[0])) != HA_ERR_END_OF_FILE) {
    /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
     without locks. Now, do full scan, but multi-thread scan will impl in future. */
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    auto sch_name_fld = *(cat_tables_ptr->field + FIELD_SCH_NAME_OFFSET_STATS);
    String sch_strstr;
    auto sch_str = std::string(sch_name_fld->val_str(&sch_strstr)->c_ptr());

    auto tb_name_fld = *(cat_tables_ptr->field + FIELD_TABLE_NAME_OFFSET_STATS);
    String tb_name_strstr;
    auto tb_name_str = std::string(tb_name_fld->val_str(&tb_name_strstr)->c_ptr());

    auto row_cnt_fld = *(cat_tables_ptr->field + FIELD_TABLE_ROWS_OFFSET_STATS);
    auto row_cnt = row_cnt_fld->val_real() ? row_cnt_fld->val_real() : 1;

    auto data_len_fld = *(cat_tables_ptr->field + FIELD_DATA_LEN_OFFSET_STATS);
    auto data_len = data_len_fld->val_real();

    auto index_data_len_fld = *(cat_tables_ptr->field + FIELD_DATA_LEN_OFFSET_STATS);
    auto index_data_len = index_data_len_fld->val_real();

    auto size_mb = ((data_len + index_data_len) * row_cnt) / (1024 * 1024);
    if (size_mb <= SHANNON_TABLE_MEMRORY_SIZE) size_mb = SHANNON_TABLE_MEMRORY_SIZE;
    m_table_stats.emplace(sch_str + ":" + tb_name_str, size_mb ? size_mb : 1);
  }
  cat_tables_ptr->file->ha_rnd_end();

  Utils::Util::close_table(current_thd, cat_tables_ptr);
  m_intialized.store(true);
  return SHANNON_SUCCESS;
}

// to scan mysq.tables, to get all schem information. such as table_name, secondary_engine info, etc.
int SelfLoadManager::load_mysql_tables_info() {
  auto cat_tables_ptr = Utils::Util::open_table_by_name(current_thd, "mysql", "tables", TL_READ_WITH_SHARED_LOCKS);
  if (!cat_tables_ptr) {
    Utils::Util::close_table(current_thd, cat_tables_ptr);
    return HA_ERR_GENERIC;
  }

  // must read from secondary engine.
  /* Read the traning data into train_data vector from rapid engine. here, we use training data
  as lablels too */
  HandlerGuard garud(current_thd, cat_tables_ptr);
  if (cat_tables_ptr->file->inited == handler::NONE && cat_tables_ptr->file->ha_rnd_init(true)) {
    Utils::Util::close_table(current_thd, cat_tables_ptr);
    return HA_ERR_GENERIC;
  }

  int tmp{HA_ERR_GENERIC};
  ShannonBase::Utils::ColumnMapGuard guard(cat_tables_ptr);

  while ((tmp = cat_tables_ptr->file->ha_rnd_next(cat_tables_ptr->record[0])) != HA_ERR_END_OF_FILE) {
    /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
     without locks. Now, do full scan, but multi-thread scan will impl in future. */
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    auto sch_id_fld = *(cat_tables_ptr->field + FIELD_SCH_ID_OFFSET_TABLES);
    auto sch_id = sch_id_fld->val_int();
    auto name_fld = *(cat_tables_ptr->field + FIELD_NAME_OFFSET_TABLES);
    String name_strstr;
    auto name_str = std::string(name_fld->val_str(&name_strstr)->c_ptr());

    auto eng_name_fld = *(cat_tables_ptr->field + FIELD_ENGINE_OFFSET_TABLES);
    String eng_strstr;
    auto eng_str = std::string(eng_name_fld->val_str(&eng_strstr)->c_ptr());
    std::transform(eng_str.begin(), eng_str.end(), eng_str.begin(), [](unsigned char c) { return std::toupper(c); });
    if (eng_str.find("INNODB") == std::string::npos) continue;

    auto option_txt_fld = *(cat_tables_ptr->field + FIELD_OPTIONS_OFFSET_TABLES);
    String opt_strstr;
    auto opt_str = std::string(option_txt_fld->val_str(&opt_strstr)->c_ptr());
    // valid option: `secondary_engine=rapid` or `secondary_engine=` or 'rapid'
    // invalid option will be skipped. such as `secondary_engine=asdfasd`
    auto res = extract_secondary_engine(opt_str);
    if (res) {
      auto val = res.value();
      if (val.find("RAPID") == std::string::npos && val.find("NULL") == std::string::npos && !val.empty()) continue;
    }

    ut_a(m_schema_tables.find(sch_id) != m_schema_tables.end());
    auto tb_info = std::make_unique<TableInfo>();
    tb_info.get()->schema_name = m_schema_tables[sch_id];
    tb_info.get()->table_name = name_str;
    tb_info.get()->secondary_engine = std::string("SECONDARY_ENGINE=RAPID");
    tb_info.get()->excluded_from_self_load = false;

    bool is_partitioned = (opt_str.find("PARTITIONED") != std::string::npos);
    if (is_partitioned) tb_info.get()->partitioned = true;

    auto key_str = tb_info.get()->schema_name + ":" + tb_info.get()->table_name;
    // ut_a(m_table_stats.find(key_str) != m_table_stats.end());
    if (m_table_stats.find(key_str) != m_table_stats.end())
      tb_info.get()->estimated_size = m_table_stats[key_str];
    else
      tb_info.get()->estimated_size = 0;

    if (ShannonBase::shannon_loaded_tables->get(tb_info.get()->schema_name, tb_info.get()->table_name))
      tb_info.get()->stats.state = table_access_stats_t::State::LOADED;
    else
      tb_info.get()->stats.state = table_access_stats_t::State::NOT_LOADED;

    tb_info.get()->meta_info.load_type = ShannonBase::load_type_t::USER;
    tb_info.get()->stats.last_queried_time = std::chrono::system_clock::now();
    tb_info.get()->stats.last_queried_time_in_rpd = std::chrono::system_clock::now();

    if (m_rpd_mirror_tables.find(key_str) == m_rpd_mirror_tables.end())
      m_rpd_mirror_tables.emplace(key_str, std::move(tb_info));
  }
  cat_tables_ptr->file->ha_rnd_end();

  Utils::Util::close_table(current_thd, cat_tables_ptr);
  m_intialized.store(true);

  return SHANNON_SUCCESS;
}

SelfLoadManager::SelfLoadManager() { initialize(); }
SelfLoadManager::~SelfLoadManager() { deinitialize(); }

int SelfLoadManager::initialize() {
  if (m_intialized.load(std::memory_order_relaxed)) return SHANNON_SUCCESS;

  auto ret{SHANNON_SUCCESS};
  ret = load_mysql_schema_info() || load_mysql_table_stats() || load_mysql_tables_info();
  if (ret == SHANNON_SUCCESS) m_intialized.store(true);

  return SHANNON_SUCCESS;
}

int SelfLoadManager::deinitialize() {
  if (!m_intialized.load(std::memory_order_relaxed)) return SHANNON_SUCCESS;

  std::unique_lock lock(m_tables_mutex);
  m_rpd_mirror_tables.clear();
  m_schema_tables.clear();
  m_table_stats.clear();
  m_intialized.store(false);

  delete m_instance;
  m_instance = nullptr;
  return SHANNON_SUCCESS;
}

int SelfLoadManager::get_innodb_thread_num() { return thd_parallel_read_threads(current_thd); }

TableInfo *SelfLoadManager::get_table_info(const std::string &schema, const std::string &table) {
  std::shared_lock lock(m_tables_mutex);
  std::string full_name = schema + ":" + table;

  auto it = m_rpd_mirror_tables.find(full_name);
  return (it != m_rpd_mirror_tables.end()) ? it->second.get() : nullptr;
}

std::unordered_map<std::string, std::unique_ptr<TableInfo>> &SelfLoadManager::tables() {
  std::shared_lock lock(m_tables_mutex);
  return m_rpd_mirror_tables;
}

int SelfLoadManager::add_table(const std::string &schema, const std::string &table, const std::string &secondary_engine,
                               bool is_partition) {
  std::unique_lock lock(m_tables_mutex);
  auto sch_tb = schema + ":" + table;
  if (m_rpd_mirror_tables.find(sch_tb) == m_rpd_mirror_tables.end()) {
    auto table_info = std::make_unique<TableInfo>();
    table_info->schema_name = schema;
    table_info->table_name = table;
    table_info->secondary_engine = secondary_engine;
    table_info->partitioned = is_partition;
    table_info->excluded_from_self_load = false;  // means new table, `create table`

    if (ShannonBase::shannon_loaded_tables->get(schema, table))
      table_info->stats.state = table_access_stats_t::State::LOADED;
    else
      table_info->stats.state = table_access_stats_t::State::NOT_LOADED;

    table_info->meta_info.load_type = ShannonBase::load_type_t::USER;
    m_rpd_mirror_tables.emplace(sch_tb, std::move(table_info));
  } else {
    m_rpd_mirror_tables[sch_tb]->stats.state = table_access_stats_t::State::LOADED;
    m_rpd_mirror_tables[sch_tb]->meta_info.load_type = ShannonBase::load_type_t::USER;
    m_rpd_mirror_tables[sch_tb]->excluded_from_self_load = true;  // mean user
  }

  return SHANNON_SUCCESS;
}

int SelfLoadManager::erase_table(const std::string &schema, const std::string &table) {
  auto sch_tb = schema + ":" + table;
  std::unique_lock lock(m_tables_mutex);
  m_rpd_mirror_tables.erase(sch_tb);

  return SHANNON_SUCCESS;
}

int SelfLoadManager::remove_table(const std::string &schema, const std::string &table) {
  std::unique_lock lock(m_tables_mutex);
  auto sch_tb = schema + ":" + table;
  if (m_rpd_mirror_tables.find(sch_tb) == m_rpd_mirror_tables.end()) return SHANNON_SUCCESS;

  m_rpd_mirror_tables[sch_tb]->stats.state = table_access_stats_t::State::NOT_LOADED;
  return SHANNON_SUCCESS;
}

TableInfo *SelfLoadManager::get_table_info(TABLE *table) {
  if (!table || !table->s) return nullptr;

  std::string schema_name(table->s->db.str, table->s->db.length);
  std::string table_name(table->s->table_name.str, table->s->table_name.length);
  std::string full_name = schema_name + ":" + table_name;

  std::shared_lock lock(m_tables_mutex);
  auto it = m_rpd_mirror_tables.find(full_name);
  if (it != m_rpd_mirror_tables.end()) return it->second.get();
  return nullptr;
}

void SelfLoadManager::update_table_importance(TableInfo *table_info, uint64_t total_query_size,
                                              double query_execution_time, SelectExecutedIn executed_in) {
  if (!table_info || total_query_size == 0 || query_execution_time <= 0) return;

  // calc: importance = |T1| / (|T1| + ... + |Tq|) * QET
  double size_ratio = static_cast<double>(table_info->estimated_size) / static_cast<double>(total_query_size);
  double base_importance = size_ratio * query_execution_time;

  // Adjust weights based on execution location
  // Queries executed by MySQL receive a higher importance increment (due to longer execution time)
  // Queries executed by HeatWave receive a smaller importance increment (due to shorter execution time)
  double weight_factor = 1.0;
  if (executed_in == SelectExecutedIn::kSecondaryEngine) {
    // HeatWave has shorter execution times, so reduce the importance increment to balance the difference
    weight_factor = 0.5;  // This coefficient can be adjusted based on actual performance differences
  }

  double adjusted_importance = base_importance * weight_factor;

  // Update the importance score using weighted averaging
  // For frequently accessed tables, use a smaller weight to smooth out fluctuations
  double current_importance = table_info->stats.importance.load();
  double updated_importance;

  do {
    current_importance = table_info->stats.importance.load();
    updated_importance = current_importance * (1.0 - UPDATE_WEIGHT) + adjusted_importance * UPDATE_WEIGHT;
  } while (!table_info->stats.importance.compare_exchange_weak(current_importance, updated_importance));

#ifndef NDEBUG
  sql_print_information(
      "Table %s importance updated: size_ratio=%.4f, QET=%.2fms, "
      "executed_in=%s, base=%.2f, adjusted=%.2f, final=%.2f",
      table_info->full_name().c_str(), size_ratio, query_execution_time,
      (executed_in == SelectExecutedIn::kPrimaryEngine) ? "MySQL" : "Rapid", base_importance, adjusted_importance,
      updated_importance);
#endif
  return;
}

void SelfLoadManager::update_table_stats(THD *thd, Table_ref *table_lists, SelectExecutedIn executed_in) {
  auto query_start_time = thd->start_utime;
  double query_execution_time = (my_micro_time() / 1000) - query_start_time;  // in ms.

  std::vector<TableInfo *> query_tables;
  uint64_t total_query_size = 0;

  // travers all the tables in the query statement.
  for (Table_ref *table = table_lists; table; table = table->next_global) {
    if (table->table && table->table->file) {
      auto table_info = get_table_info(table->table);
      if (table_info) {
        query_tables.push_back(table_info);
        total_query_size += table_info->estimated_size;
      }
    }
  }

  if (query_tables.empty()) return;

  auto current_time = std::chrono::system_clock::now();
  for (auto &table_info : query_tables) {
    {
      std::unique_lock lock(table_info->stats.stats_mutex);

      if (executed_in == SelectExecutedIn::kPrimaryEngine) {
        table_info->stats.last_queried_time = current_time;
      } else if (executed_in == SelectExecutedIn::kSecondaryEngine) {
        table_info->stats.last_queried_time_in_rpd = current_time;
      }
    }

    if (executed_in == SelectExecutedIn::kPrimaryEngine) {
      table_info->stats.mysql_access_count.fetch_add(1, std::memory_order_relaxed);
    } else if (executed_in == SelectExecutedIn::kSecondaryEngine) {
      table_info->stats.heatwave_access_count.fetch_add(1, std::memory_order_relaxed);
    }
    update_table_importance(table_info, total_query_size, query_execution_time, executed_in);
  }
  return;
}

static void self_load_coordinator_main() {
#if !defined(_WIN32)  // here we
  pthread_setname_np(pthread_self(), "self_load_coordinator");
#else
  SetThreadDescription(GetCurrentThread(), L"self_load_coordinator");
#endif

  THD *thd = create_internal_thd();
  if (!thd) return;
  thd->system_thread = SYSTEM_THREAD_BACKGROUND;
  thd->security_context()->skip_grants();
  thd->store_globals();
  struct ThdGuard {
    THD *m_thd;
    explicit ThdGuard(THD *thd) : m_thd(thd) {}
    ~ThdGuard() {
      if (m_thd) {
        close_thread_tables(m_thd);

        my_thread_end();
        destroy_internal_thd(m_thd);
        m_thd = nullptr;
      }
    }
  } thd_guard(thd);

  auto self_load_inst = SelfLoadManager::instance();
  while (SelfLoadManager::m_worker_state.load() == loader_state_t::LOADER_STATE_RUN) {
    std::unique_lock<std::mutex> lock(SelfLoadManager::m_worker_mutex);

    auto timeout = std::chrono::seconds(ShannonBase::shannon_rpd_engine_cfg.self_load_interval_sec);
    if (SelfLoadManager::m_worker_cv.wait_for(lock, timeout, []() {
          auto state = SelfLoadManager::m_worker_state.load();
          return state == loader_state_t::LOADER_STATE_STOP || state == loader_state_t::LOADER_STATE_EXIT;
        })) {
      break;
    }

    if (SelfLoadManager::m_worker_state.load() == loader_state_t::LOADER_STATE_STOP ||
        SelfLoadManager::m_worker_state.load() == loader_state_t::LOADER_STATE_EXIT)
      break;

    if (!ShannonBase::shannon_rpd_engine_cfg.self_load_enabled) continue;

    /** If the system is not quiet, self-load thread waits for 300 seconds for a maximum of 10 times before checking
      again. If the system is still busy, the current self-load invocation is skipped until the next wake-up interval,
      as determined by rapid_self_load_interval_seconds.*/
    if (!ShannonBase::shannon_rpd_engine_cfg.self_load_skip_quiet_check) {
      int attempts = 0;
      while (!self_load_inst->is_system_quiet() && attempts < SelfLoadManager::MAX_QUIET_WAIT_ATTEMPTS) {
        std::this_thread::sleep_for(std::chrono::seconds(SelfLoadManager::QUIET_WAIT_SECONDS));
        attempts++;
      }

      if (attempts >= SelfLoadManager::MAX_QUIET_WAIT_ATTEMPTS) continue;
    }
    self_load_inst->run_self_load_algorithm();
  }

  close_thread_tables(thd);
  return;
}

bool SelfLoadManager::worker_active() { return thread_is_active(srv_threads.m_rapid_self_load_cordinator); }

void SelfLoadManager::start_self_load_worker() {
  if (SelfLoadManager::m_worker_state.load() != loader_state_t::LOADER_STATE_RUN) {
    srv_threads.m_rapid_self_load_cordinator =
        os_thread_create(rapid_self_load_thread_key, 0, self_load_coordinator_main);
    SelfLoadManager::m_worker_state.store(loader_state_t::LOADER_STATE_RUN);
    srv_threads.m_rapid_self_load_cordinator.start();
  }
  ut_a(worker_active());
}

void SelfLoadManager::stop_self_load_worker() {
  m_worker_state.store(loader_state_t::LOADER_STATE_EXIT);
  {
    std::unique_lock<std::mutex> lock(m_worker_mutex);
    m_worker_cv.notify_all();
  }

  if (worker_active()) srv_threads.m_rapid_self_load_cordinator.wait();  // join

  m_worker_state.store(loader_state_t::LOADER_STATE_EXIT);
  ut_a(!worker_active());
}

bool SelfLoadManager::is_system_quiet() {
  auto now = std::chrono::system_clock::now();
  auto quiet_threshold = now - std::chrono::minutes(QUERY_QUIET_MINUTES);

  std::shared_lock lock(m_tables_mutex);
  for (const auto &[full_name, table_info] : m_rpd_mirror_tables) {
    std::shared_lock stats_lock(table_info->stats.stats_mutex);
    if (table_info->stats.last_queried_time > quiet_threshold) return false;
    // TODO: to detemine whether is being loaded into Rpd.

    // to check Change Propagation's delay.
    std::shared_lock lk(ShannonBase::Populate::shannon_pop_table_mutex);
    if (ShannonBase::Populate::shannon_pop_tables.find(full_name) != ShannonBase::Populate::shannon_pop_tables.end())
      return false;  // is still in change propagating.
  }
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
    std::unique_lock stats_lock(table_info->stats.stats_mutex);

    // Calculate the number of days since last accessed
    auto time_since_query = now - table_info->stats.last_queried_time;
    auto hours = std::chrono::duration_cast<std::chrono::hours>(time_since_query).count();
    double days = hours / 24.0;

    if (days > 0) {
      // Apply exponential decay: importance = importance * (decay_factor ^ days)
      double current_importance = table_info->stats.importance.load();
      double decayed_importance = current_importance * std::pow(IMPORTANCE_DECAY_FACTOR, days);

      // If importance decays below threshold, set to 0
      if (decayed_importance < IMPORTANCE_THRESHOLD) decayed_importance = 0.0;

      table_info->stats.importance.store(decayed_importance);
#ifndef NDEBUG
      sql_print_information(
          "Table %s importance decay: current=%.6f, days=%.2f, "
          "decayed=%.6f",
          full_name.c_str(), current_importance, days, decayed_importance);
#endif
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
      std::shared_lock stats_lock(table_info->stats.stats_mutex);

      // Check if it's a cold self-loaded table
      if (table_info->meta_info.load_type == ShannonBase::load_type_t::SELF &&
          table_info->stats.state == table_access_stats_t::LOADED && table_info->stats.importance.load() == 0.0 &&
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
  // in run_load_unload_algorithm[do nothing]
}

void SelfLoadManager::run_load_unload_algorithm() {
  std::priority_queue<LoadCandidate> load_queue;
  std::priority_queue<UnloadCandidate> unload_queue;

  {
    std::shared_lock lock(m_tables_mutex);
    for (const auto &[full_name, table_info] : m_rpd_mirror_tables) {
      if (table_info->excluded_from_self_load) continue;
      std::unique_lock stats_lock(table_info->stats.stats_mutex);
      if (table_info->stats.state == table_access_stats_t::NOT_LOADED && table_info->stats.importance.load() > 0.0) {
        LoadCandidate candidate;
        candidate.full_name = full_name;
        candidate.importance = table_info->stats.importance.load();
        candidate.estimated_size = table_info->estimated_size;
        load_queue.push(candidate);
      } else if (table_info->stats.state == table_access_stats_t::LOADED &&
                 table_info->meta_info.load_type == ShannonBase::load_type_t::SELF) {
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

        if (perform_self_unload(schema, table) == SHANNON_SUCCESS) current_memory = get_current_memory_usage();
      }
    }

    if (current_memory + load_candidate.estimated_size <= memory_threshold) {
      size_t pos = load_candidate.full_name.find(':');
      if (pos != std::string::npos) {
        std::string schema = load_candidate.full_name.substr(0, pos);
        std::string table = load_candidate.full_name.substr(pos + 1);
        if (perform_self_load(schema, table) == SHANNON_SUCCESS) current_memory = get_current_memory_usage();
      }
    }
  }
}

uint64_t SelfLoadManager::get_current_memory_usage() {
  auto loaded_sz = shannon_loaded_tables->size();
  auto total_sz = loaded_sz * SHANNON_TABLE_MEMRORY_SIZE;
#ifndef NDEBUG
  sql_print_information("Memory usage: %lu loaded tables * %lu MB per table = %lu MB", loaded_sz,
                        SHANNON_TABLE_MEMRORY_SIZE / (1024 * 1024), total_sz / (1024 * 1024));
#endif
  return total_sz;
}

uint64_t SelfLoadManager::get_memory_threshold() {
  uint64_t max_memory = ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_mb;
  uint32_t fill_percentage = ShannonBase::shannon_rpd_engine_cfg.self_load_base_relation_fill_percentage;
  return (max_memory * fill_percentage) / 100;
}

bool SelfLoadManager::can_load_table(uint64_t table_size) {
  uint64_t current_memory = get_current_memory_usage();
  uint64_t memory_threshold = get_memory_threshold();

  uint64_t projected_memory = current_memory + SHANNON_TABLE_MEMRORY_SIZE;
  bool can_load = projected_memory <= memory_threshold;

  if (!can_load) {
#ifndef NDEBUG
    sql_print_information(
        "Cannot load table: current_memory=%lu MB, table_memory=%lu MB, "
        "projected=%lu MB, threshold=%lu MB",
        current_memory / (1024 * 1024), SHANNON_TABLE_MEMRORY_SIZE / (1024 * 1024), projected_memory / (1024 * 1024),
        memory_threshold / (1024 * 1024));
#endif
  }
  return can_load;
}

int SelfLoadManager::perform_self_load(const std::string &schema, const std::string &table) {
  auto table_info = get_table_info(schema, table);
  if (!table_info) return HA_ERR_GENERIC;

  int result{SHANNON_SUCCESS};
  // Check if memory is sufficient
  if (!can_load_table(table_info->estimated_size)) {
    update_table_state(schema, table, table_access_stats_t::INSUFFICIENT_MEMORY, ShannonBase::load_type_t::SELF);
    return HA_ERR_GENERIC;
  }

  Rapid_load_context context;
  context.m_schema_name = schema;
  context.m_table_name = table;
  context.m_thd = current_thd;
  context.m_sch_tb_name = schema + ":" + table;

  TABLE *source_table = Utils::Util::open_table_by_name(current_thd, schema, table, TL_READ_WITH_SHARED_LOCKS);
  if (!source_table) return HA_ERR_GENERIC;

  context.m_table = source_table;
  context.m_table_id = source_table->file->get_table_id();
  ha_rows num_rows{0};
  source_table->file->ha_records(&num_rows);

  m_rpd_mirror_tables[context.m_sch_tb_name]->meta_info.load_start_stamp = std::chrono::system_clock::now();
  m_rpd_mirror_tables[context.m_sch_tb_name]->meta_info.load_status = load_status_t::LOADING_RPDGSTABSTATE;
  m_rpd_mirror_tables[context.m_sch_tb_name]->meta_info.nrows = num_rows;

  if (context.m_extra_info.m_partition_infos.size() > 0) {
    result = Imcs::Imcs::instance()->load_parttable(&context, source_table);
  } else {
    result = Imcs::Imcs::instance()->load_table(&context, source_table);
  }
  Utils::Util::close_table(current_thd, source_table);

  if (result == SHANNON_SUCCESS) {
    update_table_state(schema, table, table_access_stats_t::LOADED, ShannonBase::load_type_t::SELF);

    m_rpd_mirror_tables[context.m_sch_tb_name]->meta_info.load_type = load_type_t::SELF;
    m_rpd_mirror_tables[context.m_sch_tb_name]->meta_info.load_end_stamp = std::chrono::system_clock::now();
    m_rpd_mirror_tables[context.m_sch_tb_name]->meta_info.load_status = load_status_t::AVAIL_RPDGSTABSTATE;

  } else {
    // failed，set the state to INSUFFICIENT_MEMORY.
    update_table_state(schema, table, table_access_stats_t::INSUFFICIENT_MEMORY, ShannonBase::load_type_t::SELF);
  }
  return result;
}

int SelfLoadManager::perform_self_unload(const std::string &schema, const std::string &table) {
  // Checks if it's a user-loaded table
  auto table_info = get_table_info(schema, table);

  if (table_info && table_info->meta_info.load_type == ShannonBase::load_type_t::USER &&
      table_info->stats.state == table_access_stats_t::LOADED) {
    // User-loaded m_rpd_mirror_tables are downgraded to self-loaded but not actually unloaded
    update_table_state(schema, table, table_access_stats_t::LOADED, ShannonBase::load_type_t::SELF);

    sql_print_warning(
        "Self-Load feature is enabled: table `%s`.`%s` "
        "demoted to self-loaded. To unload it from the system "
        "completely run secondary unload again.",
        schema.c_str(), table.c_str());

    sql_print_information("User-loaded table %s.%s demoted to self-loaded", schema.c_str(), table.c_str());
    return SHANNON_SUCCESS;
  }

  Rapid_load_context context;
  context.m_schema_name = schema;
  context.m_table_name = table;

  int result = Imcs::Imcs::instance()->unload_table(&context, schema.c_str(), table.c_str(), table_info->partitioned);
  if (result == SHANNON_SUCCESS) {
    // update state to unloaded.
    update_table_state(schema, table, table_access_stats_t::NOT_LOADED, ShannonBase::load_type_t::SELF);
  }
  return result;
}
}  // namespace Autopilot
}  // namespace ShannonBase
