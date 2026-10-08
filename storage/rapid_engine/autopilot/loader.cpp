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
#ifdef SHANNON_POSIX_PLATFORM
#include <pthread.h>  // For pthread_setname_np
#else
#include <Windows.h>  // For SetThreadDescription
#endif

#include <limits.h>

#include <algorithm>
#include <limits>

#include <optional>
#include <queue>
#include <regex>
#include <string_view>
#include <tuple>

#include "include/my_bitmap.h"
#include "include/my_dbug.h"  // DBUG_EXECUTE_IF
#include "mysqld_error.h"     // ER_LOG_PRINTF_MSG
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/types/table.h"
#include "sql/log.h"  // LogErr
#include "sql/mysqld.h"
#include "sql/partition_info.h"
#include "sql/sql_base.h"
#include "sql/sql_table.h"
#include "sql/statement/ed_connection.h"
#include "sql/table.h"
#include "sql/transaction.h"

#include "storage/innobase/include/dict0dict.h"
#include "storage/innobase/include/os0thread-create.h"
#include "storage/innobase/include/srv0shutdown.h"
#include "storage/innobase/include/srv0srv.h"

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/memory_pool.h"
#include "storage/rapid_engine/utils/utils.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_self_load_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
extern std::shared_ptr<Utils::MemoryPool> shannon_rpd_memory_pool;

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

/**
 * Backing store of performance_schema.rpd_mirror / rpd_tables.
 *
 * Four subsystems write these rows (DDL hooks, the load/unload path, change
 * propagation, the self-loader) and three read them (perfschema, the optimizer,
 * recovery), so the storage cannot belong to any one of them.  See loader.h.
 */
namespace RpdMirror {

std::shared_mutex Registry::m_mutex;
std::unordered_map<std::string, std::shared_ptr<TableInfo>> Registry::m_tables;

int Registry::upsert(uint tid, const std::string &schema, const std::string &table, const std::string &secondary_engine,
                     bool is_partition) {
  std::unique_lock lock(m_mutex);
  const std::string full_name = schema + "." + table;
  auto it = m_tables.find(full_name);
  if (it == m_tables.end()) {
    auto info = std::make_shared<TableInfo>();
    info->tid = tid;
    info->schema_name = schema;
    info->table_name = table;
    info->secondary_engine = secondary_engine;
    info->partitioned = is_partition;
    info->excluded_from_self_load = false;  // freshly created: the self-loader may pick it up
    info->stats.state = ShannonBase::shannon_loaded_tables->get(schema, table)
                            ? table_access_stats_t::State::LOADED
                            : table_access_stats_t::State::NOT_LOADED;
    info->meta_info.load_type = ShannonBase::load_type_t::USER;
    m_tables.emplace(full_name, std::move(info));
    return SHANNON_SUCCESS;
  }

  // An entry already exists (re-created or newly loaded table): it is user-owned now.
  if (!it->second) return SHANNON_SUCCESS;
  it->second->tid = tid;
  it->second->stats.state = table_access_stats_t::State::LOADED;
  it->second->with_meta([](rpd_table_meta_info_t &meta) { meta.load_type = ShannonBase::load_type_t::USER; });
  it->second->excluded_from_self_load = true;
  return SHANNON_SUCCESS;
}

void Registry::seed(uint tid, const std::string &schema, const std::string &table, const std::string &secondary_engine,
                    bool is_partition, uint64_t estimated_size) {
  std::unique_lock lock(m_mutex);
  const std::string full_name = schema + "." + table;
  if (m_tables.find(full_name) != m_tables.end()) return;  // already tracked

  auto info = std::make_shared<TableInfo>();
  info->tid = tid;
  info->schema_name = schema;
  info->table_name = table;
  info->secondary_engine = secondary_engine;
  info->partitioned = is_partition;
  info->estimated_size = estimated_size;
  info->excluded_from_self_load = false;
  info->stats.state = ShannonBase::shannon_loaded_tables->get(schema, table) ? table_access_stats_t::State::LOADED
                                                                             : table_access_stats_t::State::NOT_LOADED;
  info->meta_info.load_type = ShannonBase::load_type_t::USER;
  m_tables.emplace(full_name, std::move(info));
}

int Registry::erase(const std::string &schema, const std::string &table) {
  std::unique_lock lock(m_mutex);
  m_tables.erase(schema + "." + table);
  return SHANNON_SUCCESS;
}

int Registry::mark_unloaded(const std::string &schema, const std::string &table) {
  std::unique_lock lock(m_mutex);
  auto it = m_tables.find(schema + "." + table);
  if (it == m_tables.end() || !it->second) return SHANNON_SUCCESS;

  auto &info = it->second;
  info->stats.state = table_access_stats_t::State::NOT_LOADED;
  // stats.state and meta_info.load_status record the same fact and must not
  // drift: leaving load_status at AVAIL made an unloaded table keep reporting
  // itself as loaded in performance_schema.rpd_tables.
  info->with_meta([](rpd_table_meta_info_t &meta) {
    meta.load_status = load_status_t::NOLOAD_RPDGSTABSTATE;
    meta.stale_reason = stale_reason_t::OK;
  });
  return SHANNON_SUCCESS;
}

int Registry::set_state(const std::string &schema, const std::string &table, table_access_stats_t::State state,
                        ShannonBase::load_type_t load_type) {
  std::unique_lock lock(m_mutex);
  auto it = m_tables.find(schema + "." + table);
  if (it == m_tables.end() || !it->second) return SHANNON_SUCCESS;

  it->second->stats.state = state;
  it->second->with_meta([load_type](rpd_table_meta_info_t &meta) { meta.load_type = load_type; });
  return SHANNON_SUCCESS;
}

void Registry::mark_stale(uint tid, stale_reason_t reason) {
  std::shared_lock lock(m_mutex);
  for (auto &entry : m_tables) {
    auto &info = entry.second;
    if (!info || info->tid != tid) continue;
    info->with_meta([reason](rpd_table_meta_info_t &meta) {
      meta.load_status = load_status_t::STALE_RPDGSTABSTATE;
      // Repeated quarantine must preserve a more specific failure category.
      if (reason != stale_reason_t::RELOAD_REQUIRED || meta.stale_reason == stale_reason_t::OK)
        meta.stale_reason = reason;
      meta.pool_type = pool_type_t::SNAPSHOT;
    });
    return;
  }
}

std::vector<TableInfoSnapshot> Registry::snapshot() {
  std::shared_lock lock(m_mutex);
  std::vector<TableInfoSnapshot> result;
  result.reserve(m_tables.size());
  for (const auto &[full_name, info] : m_tables) {
    if (!info) continue;
    TableInfoSnapshot row;
    row.tid = info->tid;
    row.schema_name = info->schema_name;
    row.table_name = info->table_name;
    row.mysql_access_count = info->stats.mysql_access_count.load(std::memory_order_relaxed);
    row.heatwave_access_count = info->stats.heatwave_access_count.load(std::memory_order_relaxed);
    row.importance = info->stats.importance.load(std::memory_order_relaxed);
    row.last_queried_time = info->stats.last_queried_time;
    row.last_queried_time_in_rpd = info->stats.last_queried_time_in_rpd;
    row.state = info->stats.state;
    {
      std::shared_lock stats_lock(info->stats.stats_mutex);
      row.queried_partitions = info->queried_partitions;
    }
    row.meta_info = info->meta_copy();
    result.push_back(std::move(row));
  }
  // Stable output ordering: perfschema scans must not reshuffle between
  // SELECTs just because the underlying hash map rehashed.
  std::sort(result.begin(), result.end(), [](const TableInfoSnapshot &l, const TableInfoSnapshot &r) {
    return std::tie(l.schema_name, l.table_name) < std::tie(r.schema_name, r.table_name);
  });
  return result;
}

size_t Registry::count() {
  std::shared_lock lock(m_mutex);
  return m_tables.size();
}

std::shared_ptr<TableInfo> Registry::find(const std::string &full_name) {
  std::shared_lock lock(m_mutex);
  auto it = m_tables.find(full_name);
  return (it != m_tables.end()) ? it->second : nullptr;
}

void Registry::reset() {
  std::unique_lock lock(m_mutex);
  m_tables.clear();
}

void refresh_propagation_health(std::vector<std::pair<std::string, std::string>> *self_loaded_stale) {
  struct Probe {
    std::string full_name;
    uint tid;
  };
  std::vector<Probe> probes;
  Registry::for_each([&probes](const std::string &full_name, TableInfo &info) {
    if (info.stats.state != table_access_stats_t::LOADED) return;
    probes.push_back({full_name, info.tid});
  });

  for (const auto &probe : probes) {
    const auto barrier = ShannonBase::Populate::Populator::request_table_barrier(probe.tid);
    const bool broken = (barrier.state == ShannonBase::Populate::TablePropagationState::BROKEN);

    // The table may have been unloaded while the barrier was being taken.
    auto table_info = Registry::find(probe.full_name);
    if (!table_info) continue;

    bool report_stale = false;
    table_info->with_meta([&](rpd_table_meta_info_t &meta) {
      if (broken) {
        meta.load_status = load_status_t::STALE_RPDGSTABSTATE;
        meta.pool_type = pool_type_t::SNAPSHOT;
        report_stale = (meta.stale_reason == stale_reason_t::ERROR_CLUSTER_OOM);
      } else if (meta.load_status == load_status_t::STALE_RPDGSTABSTATE) {
        // Change Propagation recovered; the table is loaded and healthy again.
        meta.load_status = load_status_t::AVAIL_RPDGSTABSTATE;
        meta.stale_reason = stale_reason_t::OK;
        meta.pool_type = pool_type_t::TRANSACTIONAL;
      }
    });

    if (report_stale && self_loaded_stale != nullptr) {
      const size_t pos = probe.full_name.find('.');
      if (pos != std::string::npos)
        self_loaded_stale->emplace_back(probe.full_name.substr(0, pos), probe.full_name.substr(pos + 1));
    }
  }
}
}  // namespace RpdMirror

namespace Autopilot {
// static members initialization.
std::once_flag SelfLoadManager::one;
std::unique_ptr<SelfLoadManager> SelfLoadManager::m_instance = nullptr;
std::atomic<loader_state_t> SelfLoadManager::m_worker_state{loader_state_t::LOADER_STATE_EXIT};
std::condition_variable SelfLoadManager::m_worker_cv;
std::mutex SelfLoadManager::m_worker_mutex;
std::atomic<bool> SelfLoadManager::m_reload_pending{false};
uint64_t SelfLoadManager::m_reload_generation{0};
std::mutex SelfLoadManager::m_worker_lifecycle_mutex;
bool SelfLoadManager::m_worker_started{false};
std::atomic<bool> SelfLoadManager::m_accept_requests{true};

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
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, "Self-Load: cannot open mysql.schemata; the RPD Mirror stays empty");
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
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, "Self-Load: cannot open mysql.table_stats; table sizes stay unknown");
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

    // mysql.table_stats.data_length / index_length are already the totals for
    // the whole table, in bytes. Three things were wrong here: index_length was
    // read from the data_length column, the totals were multiplied by the row
    // count again, and the result was called MB while being compared against a
    // byte constant. Keep it in bytes, like every other size in the engine.
    auto data_len_fld = *(cat_tables_ptr->field + FIELD_DATA_LEN_OFFSET_STATS);
    auto index_len_fld = *(cat_tables_ptr->field + FIELD_INDEX_LEN_OFFSET_STATS);

    const double total_bytes = data_len_fld->val_real() + index_len_fld->val_real();
    uint64_t estimated_bytes = (total_bytes > 0) ? static_cast<uint64_t>(total_bytes) : 0;
    // A table InnoDB has no statistics for still needs a sub-pool to load into.
    if (estimated_bytes < SHANNON_MIN_TABLE_MEMORY_SIZE) estimated_bytes = SHANNON_MIN_TABLE_MEMORY_SIZE;
    m_table_stats.emplace(sch_str + "." + tb_name_str, estimated_bytes);
  }
  cat_tables_ptr->file->ha_rnd_end();

  Utils::Util::close_table(current_thd, cat_tables_ptr);
  return SHANNON_SUCCESS;
}

// to scan information_schema.INNODB_TABLES, to get all table ids.
int SelfLoadManager::load_mysql_table_ids() {
  dd::cache::Dictionary_client *dd_client = current_thd->dd_client();

  std::vector<const dd::Schema *> schemas;
  if (dd_client->fetch_global_components(&schemas)) {
    return HA_ERR_GENERIC;
  }

  for (const dd::Schema *schema : schemas) {
    if (is_system_schema(schema->name().c_str())) continue;

    std::vector<const dd::Table *> tables;
    if (dd_client->fetch_schema_components(schema, &tables)) continue;

    for (const dd::Table *dd_table : tables) {
      std::string full_name = schema->name().c_str();
      full_name.append(".").append(dd_table->name().c_str());
      m_table_ids.emplace(full_name, dd_table->se_private_id());
    }
  }

  return SHANNON_SUCCESS;
}

// to scan mysq.tables, to get all schem information. such as table_name, secondary_engine info, etc.
int SelfLoadManager::load_mysql_tables_info() {
  auto cat_tables_ptr = Utils::Util::open_table_by_name(current_thd, "mysql", "tables", TL_READ_WITH_SHARED_LOCKS);
  if (!cat_tables_ptr) {
    Utils::Util::close_table(current_thd, cat_tables_ptr);
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, "Self-Load: cannot open mysql.tables; the RPD Mirror stays empty");
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
    const std::string &schema_name = m_schema_tables[sch_id];
    const std::string key_str = schema_name + "." + name_str;

    const uint tid = (m_table_ids.find(key_str) != m_table_ids.end()) ? static_cast<uint>(m_table_ids[key_str]) : 0;
    const uint64_t estimated_size = (m_table_stats.find(key_str) != m_table_stats.end()) ? m_table_stats[key_str] : 0;
    const bool is_partitioned = (opt_str.find("PARTITIONED") != std::string::npos);

    // The dictionary scan only knows the facts; the registry owns the row.
    RpdMirror::Registry::seed(tid, schema_name, name_str, std::string("SECONDARY_ENGINE=RAPID"), is_partitioned,
                              estimated_size);
  }
  cat_tables_ptr->file->ha_rnd_end();

  Utils::Util::close_table(current_thd, cat_tables_ptr);
  return SHANNON_SUCCESS;
}

SelfLoadManager::SelfLoadManager() { initialize(); }
SelfLoadManager::~SelfLoadManager() { deinitialize(); }

int SelfLoadManager::initialize() {
  if (m_intialized.load(std::memory_order_relaxed)) return SHANNON_SUCCESS;

  // `a() || b() || c()` short-circuits on the FIRST success (0), so any step
  // that worked stopped the ones after it, and the result was a bool that
  // could never be compared against a handler error code. Run them in order,
  // stop at the first real failure, and hand that code back.
  int ret = load_mysql_table_ids();
  if (ret == SHANNON_SUCCESS) ret = load_mysql_schema_info();
  if (ret == SHANNON_SUCCESS) ret = load_mysql_table_stats();
  if (ret == SHANNON_SUCCESS) ret = load_mysql_tables_info();

  if (ret != SHANNON_SUCCESS) {
    sql_print_warning("Self-Load: initialization failed (%d); auto load/unload stays off", ret);
    return ret;
  }
  m_intialized.store(true);
  return SHANNON_SUCCESS;
}

int SelfLoadManager::deinitialize() {
  if (!m_intialized.exchange(false, std::memory_order_acq_rel)) return SHANNON_SUCCESS;

  stop_self_load_worker();
  m_table_ids.clear();
  m_schema_tables.clear();
  m_table_stats.clear();

  // The mirror rows describe tables of this server lifetime; a re-installed
  // plugin must not inherit them.  (They used to stay in the static map forever.)
  RpdMirror::Registry::reset();

  return SHANNON_SUCCESS;
}

std::string SelfLoadManager::full_name_of(TABLE *table) {
  if (!table || !table->s) return std::string();
  return std::string(table->s->db.str, table->s->db.length) + "." +
         std::string(table->s->table_name.str, table->s->table_name.length);
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

  std::vector<std::shared_ptr<TableInfo>> query_tables;
  uint64_t total_query_size = 0;

  // travers all the tables in the query statement.
  for (Table_ref *table = table_lists; table; table = table->next_global) {
    // Guard clauses instead of nested ifs, matching the loop below: one table
    // of the statement having nothing to track is no reason not to walk the
    // rest of them.
    if (table->table == nullptr || table->table->file == nullptr) continue;

    auto table_info = RpdMirror::Registry::find(full_name_of(table->table));
    if (table_info == nullptr) continue;

    query_tables.push_back(table_info);
    total_query_size += table_info->estimated_size;

    auto *part_info = table->table->part_info;
    if (part_info == nullptr) continue;

    // Record which partitions this query actually touched (post partition-pruning), mirroring HeatWave's rpd_mirror
    // QUERIED_PARTITIONS column.
    std::unique_lock lock(table_info->stats.stats_mutex);
    for (uint index = 0; index < part_info->get_tot_partitions(); ++index) {
      if (!bitmap_is_set(&part_info->read_partitions, index)) continue;
      table_info->queried_partitions.insert(part_info->partitions[index]->partition_name);
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
    update_table_importance(table_info.get(), total_query_size, query_execution_time, executed_in);
  }
  return;
}

static void self_load_coordinator_main() {
#ifdef SHANNON_POSIX_PLATFORM  // here we
  pthread_setname_np(pthread_self(), "self_load_coordinator");
#else
  SetThreadDescription(GetCurrentThread(), L"self_load_coordinator");
#endif

  THD *thd = create_internal_thd();
  if (!thd) {
    SelfLoadManager::m_worker_state.store(loader_state_t::LOADER_STATE_EXIT);
    return;
  }
  thd->system_thread = SYSTEM_THREAD_BACKGROUND;
  // Secondary-image maintenance is local to this server, not a source DDL
  // transaction to replicate or assign an internal-thread binlog XID to.
  thd->variables.sql_log_bin = false;
  thd->variables.option_bits &= ~OPTION_BIN_LOG;
  thd->security_context()->skip_grants();
  thd->store_globals();
  struct ThdGuard {
    THD *m_thd;
    explicit ThdGuard(THD *thd) : m_thd(thd) {}
    ~ThdGuard() {
      if (!m_thd) return;

      trans_rollback_stmt(m_thd);
      trans_rollback(m_thd);

      close_thread_tables(m_thd);
      destroy_internal_thd(m_thd);
      my_thread_end();
      m_thd = nullptr;
    }
  } thd_guard(thd);

  auto self_load_inst = SelfLoadManager::instance();
  auto last_selection = std::chrono::steady_clock::now();
  uint64_t observed_generation{0};
  bool first_cycle{true};
  while (SelfLoadManager::m_worker_state.load() == loader_state_t::LOADER_STATE_RUN) {
    {
      std::unique_lock<std::mutex> lock(SelfLoadManager::m_worker_mutex);
      auto timeout =
          std::chrono::seconds(std::min<uint64_t>(60, ShannonBase::shannon_rpd_engine_cfg.self_load_interval_sec));
      DBUG_EXECUTE_IF("rapid_reload_test_tick", { timeout = std::chrono::seconds(1); });
      if (!(first_cycle && SelfLoadManager::m_reload_pending.load()))
        SelfLoadManager::m_worker_cv.wait_for(lock, timeout, [&]() {
          auto state = SelfLoadManager::m_worker_state.load();
          return state == loader_state_t::LOADER_STATE_STOP || state == loader_state_t::LOADER_STATE_EXIT ||
                 observed_generation != SelfLoadManager::m_reload_generation;
        });
      observed_generation = SelfLoadManager::m_reload_generation;
      first_cycle = false;
    }
    if (SelfLoadManager::m_worker_state.load() != loader_state_t::LOADER_STATE_RUN) break;
    // Every maintenance cycle must release transaction-duration MDL tickets.
    struct CycleGuard {
      THD *thd;
      ~CycleGuard() {
        trans_rollback_stmt(thd);
        trans_rollback(thd);
        close_thread_tables(thd);
        thd->mdl_context.release_transactional_locks();
        thd->clear_error();
      }
    } cycle_guard{thd};
    if (SelfLoadManager::m_reload_pending.exchange(false) && self_load_inst->reconcile_propagation_state())
      SelfLoadManager::m_reload_pending.store(true);
    if (!ShannonBase::shannon_rpd_engine_cfg.self_load_enabled) {
      if (SelfLoadManager::m_reload_pending.load()) continue;
      break;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_selection < std::chrono::seconds(ShannonBase::shannon_rpd_engine_cfg.self_load_interval_sec))
      continue;
    last_selection = now;
    if (!ShannonBase::shannon_rpd_engine_cfg.self_load_skip_quiet_check && !self_load_inst->is_system_quiet()) continue;
    self_load_inst->run_self_load_algorithm();
  }
  SelfLoadManager::m_worker_state.store(loader_state_t::LOADER_STATE_EXIT);
  close_thread_tables(thd);
}

bool SelfLoadManager::worker_active() { return thread_is_active(srv_threads.m_rapid_self_load_cordinator); }

void SelfLoadManager::notify_propagation_failure() {
  {
    std::lock_guard<std::mutex> lock(m_worker_mutex);
    m_reload_pending.store(true);
    ++m_reload_generation;
  }
  m_worker_cv.notify_all();
}

void SelfLoadManager::dispatch_propagation_reload() {
  if (m_accept_requests.load() && m_reload_pending.load() && m_worker_state.load() != loader_state_t::LOADER_STATE_RUN)
    start();
}

void SelfLoadManager::start_self_load_worker() {
  std::lock_guard<std::mutex> lifecycle(m_worker_lifecycle_mutex);
  if (!m_accept_requests.load() || m_worker_state.load() == loader_state_t::LOADER_STATE_RUN) return;
  if (m_worker_started) srv_threads.m_rapid_self_load_cordinator.wait();
  srv_threads.m_rapid_self_load_cordinator =
      os_thread_create(rapid_self_load_thread_key, 0, self_load_coordinator_main);
  m_worker_state.store(loader_state_t::LOADER_STATE_RUN);
  m_worker_started = true;
  srv_threads.m_rapid_self_load_cordinator.start();
}

void SelfLoadManager::stop_self_load_worker() {
  std::lock_guard<std::mutex> lifecycle(m_worker_lifecycle_mutex);
  {
    std::lock_guard<std::mutex> lock(m_worker_mutex);
    m_worker_state.store(loader_state_t::LOADER_STATE_EXIT);
  }
  m_worker_cv.notify_all();
  if (m_worker_started) srv_threads.m_rapid_self_load_cordinator.wait();
  m_worker_started = false;
}

bool SelfLoadManager::is_system_quiet() {
  DBUG_EXECUTE_IF("rapid_reload_test_idle", { return true; });
  auto now = std::chrono::system_clock::now();
  auto quiet_threshold = now - std::chrono::minutes(QUERY_QUIET_MINUTES);

  const bool busy = RpdMirror::Registry::any_of([&](const std::string &full_name, TableInfo &table_info) {
    std::shared_lock stats_lock(table_info.stats.stats_mutex);
    if (table_info.stats.last_queried_time > quiet_threshold) return true;

    // A table mid-transition is the system doing work, whatever the query
    // clock says: load_table() flips load_status to LOADING before the scan
    // and back to AVAIL after it, and unload/recovery mark themselves the same
    // way. Reporting "quiet" during one of those would let the self-load
    // worker start a second transition on top of the first.
    switch (table_info.load_status()) {
      case load_status_t::LOADING_RPDGSTABSTATE:
      case load_status_t::UNLOADING_RPDGSTABSTATE:
      case load_status_t::INRECOVERY_RPDGSTABSTATE:
        return true;
      default:
        break;
    }

    // to check Change Propagation's delay.
    std::shared_lock lk(ShannonBase::Populate::shannon_pop_table_mutex);
    return ShannonBase::Populate::shannon_pop_tables.find(full_name) !=
           ShannonBase::Populate::shannon_pop_tables.end();  // is still in change propagating.
  });

  return !busy;
}

bool SelfLoadManager::reconcile_propagation_state() {
  // Only resource exhaustion is eligible for automatic recovery, and only
  // while idle. Other failures remain fenced until explicit unload/load.
  const auto has_oom = [] {
    return RpdMirror::Registry::any_of([](const std::string &, TableInfo &info) {
      const auto meta = info.meta_copy();
      return meta.load_status == load_status_t::STALE_RPDGSTABSTATE &&
             meta.stale_reason == stale_reason_t::ERROR_CLUSTER_OOM;
    });
  };
  if (!has_oom()) return false;
  if (!is_system_quiet()) return true;
  RpdMirror::refresh_propagation_health(nullptr);
  for (const auto &entry : RpdMirror::Registry::snapshot()) {
    if (entry.meta_info.load_status != load_status_t::STALE_RPDGSTABSTATE ||
        entry.meta_info.stale_reason != stale_reason_t::ERROR_CLUSTER_OOM)
      continue;

    const auto quote_identifier = [](const std::string &name) {
      std::string quoted("`");
      for (char ch : name) {
        quoted += ch;
        if (ch == '`') quoted += '`';
      }
      quoted += '`';
      return quoted;
    };
    const auto prefix = "ALTER TABLE " + quote_identifier(entry.schema_name) + "." + quote_identifier(entry.table_name);
    const auto execute = [&](const char *operation) {
#ifndef DBUG_OFF
      if (std::string_view(operation) == " SECONDARY_LOAD") {
        DBUG_EXECUTE_IF("rapid_reload_test_fail_once", {
          DBUG_SET("-d,rapid_reload_test_fail_once");
          return false;
        });
      }
#endif
      auto statement = prefix + operation;
      // Internal statement execution does not run dispatch_command(), which
      // normally assigns the unique query ID used by atomic-DDL XIDs.
      current_thd->set_query_id(next_query_id());
      Ed_connection connection(current_thd);
      LEX_STRING sql{statement.data(), statement.size()};
      if (!connection.execute_direct(sql)) return true;
      sql_print_warning("Rapid automatic OOM reload failed for %s.%s: %s", entry.schema_name.c_str(),
                        entry.table_name.c_str(), connection.get_last_error());
      return false;
    };
    // Execute the normal SQL paths, including table locks and partition load
    // handling. A failed load remains eligible for the next idle check.
    if (entry.state == table_access_stats_t::LOADED && !execute(" SECONDARY_UNLOAD")) continue;
    if (!execute(" SECONDARY_LOAD")) {
      RpdMirror::Registry::mark_stale(entry.tid, stale_reason_t::ERROR_CLUSTER_OOM);
      continue;
    }
    RpdMirror::Registry::set_state(entry.schema_name, entry.table_name, table_access_stats_t::LOADED,
                                   entry.meta_info.load_type);
    if (auto info = RpdMirror::Registry::find(entry.schema_name + "." + entry.table_name); info) {
      info->with_meta([](rpd_table_meta_info_t &meta) {
        if (meta.load_status == load_status_t::AVAIL_RPDGSTABSTATE) meta.stale_reason = stale_reason_t::OK;
      });
    }
  }
  return has_oom();
}

void SelfLoadManager::run_self_load_algorithm() {
  // Optional table selection does not control the separate OOM recovery check.
  if (!ShannonBase::shannon_rpd_engine_cfg.self_load_enabled) return;

  // step 1: decline the importance.
  decay_importance();

  // step 2: unload the clod tables.
  unload_cold_tables();

  // step 3: perform load/unload queue.
  prepare_load_unload_queues();

  // step 4: execute load/unload oper.
  run_load_unload_algorithm();
}

void SelfLoadManager::decay_importance() {
  auto now = std::chrono::system_clock::now();

  RpdMirror::Registry::for_each([&](const std::string &full_name, TableInfo &table_info) {
    std::unique_lock stats_lock(table_info.stats.stats_mutex);

    // Calculate the number of days since last accessed
    auto time_since_query = now - table_info.stats.last_queried_time;
    auto hours = std::chrono::duration_cast<std::chrono::hours>(time_since_query).count();
    double days = hours / 24.0;

    if (days > 0) {
      // Apply exponential decay: importance = importance * (decay_factor ^ days)
      double current_importance = table_info.stats.importance.load();
      double decayed_importance = current_importance * std::pow(IMPORTANCE_DECAY_FACTOR, days);

      // If importance decays below threshold, set to 0
      if (decayed_importance < IMPORTANCE_THRESHOLD) decayed_importance = 0.0;

      table_info.stats.importance.store(decayed_importance);
#ifndef NDEBUG
      sql_print_information(
          "Table %s importance decay: current=%.6f, days=%.2f, "
          "decayed=%.6f",
          full_name.c_str(), current_importance, days, decayed_importance);
#endif
    }
  });
}

void SelfLoadManager::unload_cold_tables() {
  auto now = std::chrono::system_clock::now();
  auto cold_threshold = now - std::chrono::hours(COLD_TABLE_DAYS * 24);
  std::vector<std::string> tables_to_unload;
  RpdMirror::Registry::for_each([&](const std::string &full_name, TableInfo &table_info) {
    std::shared_lock stats_lock(table_info.stats.stats_mutex);
    if (table_info.load_status() == load_status_t::STALE_RPDGSTABSTATE) return;

    // Check if it's a cold self-loaded table
    if (table_info.load_type() == ShannonBase::load_type_t::SELF &&
        table_info.stats.state == table_access_stats_t::LOADED && table_info.stats.importance.load() == 0.0 &&
        table_info.stats.last_queried_time < cold_threshold) {
      tables_to_unload.push_back(full_name);
    }
  });

  // unload the cold table.
  for (const auto &full_name : tables_to_unload) {
    size_t pos = full_name.find('.');
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

  RpdMirror::Registry::for_each([&](const std::string &full_name, TableInfo &table_info) {
    if (table_info.excluded_from_self_load || table_info.load_status() == load_status_t::STALE_RPDGSTABSTATE) return;
    std::unique_lock stats_lock(table_info.stats.stats_mutex);
    if (table_info.stats.state == table_access_stats_t::NOT_LOADED && table_info.stats.importance.load() > 0.0) {
      LoadCandidate candidate;
      candidate.full_name = full_name;
      candidate.importance = table_info.stats.importance.load();
      candidate.estimated_size = table_info.estimated_size;
      load_queue.push(candidate);
    } else if (table_info.stats.state == table_access_stats_t::LOADED &&
               table_info.load_type() == ShannonBase::load_type_t::SELF) {
      UnloadCandidate candidate;
      candidate.full_name = full_name;
      candidate.importance = table_info.stats.importance.load();
      unload_queue.push(candidate);
    }
  });

  uint64_t memory_threshold = get_memory_threshold();
  uint64_t current_memory = get_current_memory_usage();

  while (!load_queue.empty() && current_memory < memory_threshold) {
    // Check for shutdown signal before processing each table
    auto state = SelfLoadManager::m_worker_state.load();
    if (state == loader_state_t::LOADER_STATE_STOP || state == loader_state_t::LOADER_STATE_EXIT) return;

    auto load_candidate = load_queue.top();
    load_queue.pop();
    const uint64_t candidate_bytes = std::max<uint64_t>(load_candidate.estimated_size, SHANNON_MIN_TABLE_MEMORY_SIZE);

    // If more memory is needed, first unload the least important tables
    while (!unload_queue.empty() && !detail::fits_memory_budget(current_memory, candidate_bytes, memory_threshold)) {
      auto unload_candidate = unload_queue.top();
      unload_queue.pop();

      size_t pos = unload_candidate.full_name.find('.');
      if (pos != std::string::npos) {
        std::string schema = unload_candidate.full_name.substr(0, pos);
        std::string table = unload_candidate.full_name.substr(pos + 1);

        if (perform_self_unload(schema, table) == SHANNON_SUCCESS) current_memory = get_current_memory_usage();
      }
    }

    if (detail::fits_memory_budget(current_memory, candidate_bytes, memory_threshold)) {
      size_t pos = load_candidate.full_name.find('.');
      if (pos != std::string::npos) {
        std::string schema = load_candidate.full_name.substr(0, pos);
        std::string table = load_candidate.full_name.substr(pos + 1);
        if (perform_self_load(schema, table) == SHANNON_SUCCESS) current_memory = get_current_memory_usage();
      }
    }
  }
}

uint64_t SelfLoadManager::get_current_memory_usage() {
  // "loaded table count * 128 MB" is not a memory reading, it is a guess that
  // happens to have the units of one: it is the same number whether the tables
  // hold four rows or forty million, so every load/evict decision below was
  // taken against a figure the allocator had never seen. Ask the pool.
  if (!ShannonBase::shannon_rpd_memory_pool) return 0;

  const auto stats = ShannonBase::shannon_rpd_memory_pool->stats();
  // allocated_bytes, not used_bytes: a table's sub-pool is reserved from the
  // parent up front, so that is what another table has to fit beside.
  const uint64_t total_sz = stats.allocated_bytes;
#ifndef NDEBUG
  sql_print_information("Memory usage: %llu MB allocated of %llu MB pool capacity",
                        (unsigned long long)(total_sz / (1024 * 1024)),
                        (unsigned long long)(stats.total_capacity / (1024 * 1024)));
#endif
  return total_sz;
}

uint64_t SelfLoadManager::get_memory_threshold() {
  uint64_t max_memory = ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_bytes;
  const int fill_percentage = ShannonBase::shannon_rpd_engine_cfg.self_load_base_relation_fill_percentage;
  return detail::memory_threshold_bytes(max_memory, fill_percentage);
}

bool SelfLoadManager::can_load_table(uint64_t table_size) {
  uint64_t current_memory = get_current_memory_usage();
  uint64_t memory_threshold = get_memory_threshold();

  // table_size is this table's estimate, in bytes, from mysql.table_stats. It
  // was accepted and then ignored in favour of a per-table constant, so a 4 KB
  // table and a 400 GB one were judged identically.
  const uint64_t wanted = std::max<uint64_t>(table_size, SHANNON_MIN_TABLE_MEMORY_SIZE);
  bool can_load = detail::fits_memory_budget(current_memory, wanted, memory_threshold);
  const uint64_t projected_memory = wanted > std::numeric_limits<uint64_t>::max() - current_memory
                                        ? std::numeric_limits<uint64_t>::max()
                                        : current_memory + wanted;

  if (!can_load) {
#ifndef NDEBUG
    sql_print_information(
        "Cannot load table: current_memory=%llu MB, table_memory=%llu MB, "
        "projected=%llu MB, threshold=%llu MB",
        (unsigned long long)(current_memory / (1024 * 1024)), (unsigned long long)(wanted / (1024 * 1024)),
        (unsigned long long)(projected_memory / (1024 * 1024)), (unsigned long long)(memory_threshold / (1024 * 1024)));
#endif
  }
  return can_load;
}

int SelfLoadManager::perform_self_load(const std::string &schema, const std::string &table) {
  auto table_info = RpdMirror::Registry::find(schema + "." + table);
  if (!table_info) return HA_ERR_GENERIC;

  int result{SHANNON_SUCCESS};
  // Check if memory is sufficient
  if (!can_load_table(table_info->estimated_size)) {
    RpdMirror::Registry::set_state(schema, table, table_access_stats_t::INSUFFICIENT_MEMORY,
                                   ShannonBase::load_type_t::SELF);
    return HA_ERR_GENERIC;
  }

  Rapid_load_context context;
  context.m_schema_name = schema;
  context.m_table_name = table;
  context.m_thd = current_thd;
  context.m_sch_tb_name = schema + "." + table;
  // Bulk load, not DML. Leaving m_oper at its PROPAGATION default sends every
  // row down Imcu::insert_row()'s DML branch: a WAL prepare/commit pair per row
  // for data being read out of InnoDB (which is already durable), and a journal
  // entry per row. With no SCN the entry is ACTIVE, so its txn id lands in
  // active_txns and nothing ever commits it -- has_uncommitted_changes() stays
  // true for the life of the process, the checkpoint gate refuses every
  // snapshot, the WAL is never truncated and fast recovery can never arm. Even
  // where an SCN is set the entries alone keep is_fully_visible() false, which
  // costs every scan the per-row visibility walk.
  context.m_extra_info.m_oper = ShannonBase::Rapid_context::extra_info_t::OperType::LOAD;

  TABLE *source_table = Utils::Util::open_table_by_name(current_thd, schema, table, TL_READ_WITH_SHARED_LOCKS);
  if (!source_table) return HA_ERR_GENERIC;

  context.m_table = source_table;
  context.m_table_id = source_table->file->get_table_id();
  ha_rows num_rows{0};
  source_table->file->ha_records(&num_rows);

  const int read_threads = thd_parallel_read_threads(context.m_thd);
  table_info->with_meta([&](rpd_table_meta_info_t &meta) {
    meta.load_start_stamp = std::chrono::system_clock::now();
    meta.load_status = load_status_t::LOADING_RPDGSTABSTATE;
    meta.nrows = num_rows;
    meta.recommended_read_threads = read_threads;
  });

#ifndef NDEBUG
  bool injected_load_failure = false;
  DBUG_EXECUTE_IF("secondary_engine_rapid_self_load_error", { injected_load_failure = true; });
  if (injected_load_failure) {
    result = HA_ERR_GENERIC;
  } else
#endif
  {
    result = (context.m_extra_info.m_partition_infos.size() > 0)
                 ? Imcs::Imcs::instance()->load_parttable(&context, source_table)
                 : Imcs::Imcs::instance()->load_table(&context, source_table);
  }
  Utils::Util::close_table(current_thd, source_table);

  if (result == SHANNON_SUCCESS) {
    RpdMirror::Registry::set_state(schema, table, table_access_stats_t::LOADED, ShannonBase::load_type_t::SELF);

    table_info->with_meta([](rpd_table_meta_info_t &meta) {
      meta.load_type = load_type_t::SELF;
      meta.load_end_stamp = std::chrono::system_clock::now();
      meta.load_status = load_status_t::AVAIL_RPDGSTABSTATE;
      meta.pool_type = pool_type_t::TRANSACTIONAL;
    });
  } else {
    // failed，set the state to INSUFFICIENT_MEMORY.
    RpdMirror::Registry::set_state(schema, table, table_access_stats_t::INSUFFICIENT_MEMORY,
                                   ShannonBase::load_type_t::SELF);

    // The load did not happen, so the table is not loaded. Leaving load_status at LOADING_RPDGSTABSTATE would make
    // rpd_tables report a load that never finishes and never fails.
    table_info->with_meta([](rpd_table_meta_info_t &meta) {
      meta.load_status = load_status_t::NOLOAD_RPDGSTABSTATE;
      meta.loading_progress = 0.0;
    });
  }
  return result;
}

int SelfLoadManager::perform_self_unload(const std::string &schema, const std::string &table) {
  // Checks if it's a user-loaded table
  auto table_info = RpdMirror::Registry::find(schema + "." + table);

  if (table_info && table_info->load_type() == ShannonBase::load_type_t::USER &&
      table_info->stats.state == table_access_stats_t::LOADED) {
    // User-loaded tables are downgraded to self-loaded but not actually unloaded
    RpdMirror::Registry::set_state(schema, table, table_access_stats_t::LOADED, ShannonBase::load_type_t::SELF);

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
  if (!table_info) {
    sql_print_warning("Self-Load: table_info not found for %s.%s, cannot unload", schema.c_str(), table.c_str());
    return HA_ERR_GENERIC;
  }
  context.m_table_id = table_info->tid;

  // TEST-ONLY: fail the unload before anything is torn down, so the table is
  // left exactly as it was: still loaded, still readable, and its access stats
  // must not be flipped to NOT_LOADED.
#ifndef NDEBUG
  bool injected_unload_failure = false;
  DBUG_EXECUTE_IF("secondary_engine_rapid_self_unload_error", { injected_unload_failure = true; });
  if (injected_unload_failure) return HA_ERR_GENERIC;
#endif

  ShannonBase::Populate::Populator::unload(context.m_table_id);

  // The fourth parameter is error_if_not_loaded, not is_partition. Passing the
  // partition flag there sent every partitioned self-loaded table down the
  // non-partitioned map, where it was never found, so it never unloaded.
  int result = Imcs::Imcs::instance()->unload_table(&context, schema.c_str(), table.c_str(),
                                                    /*error_if_not_loaded=*/false,
                                                    /*is_partition=*/table_info->partitioned);
  if (result == SHANNON_SUCCESS) {
    // update state to unloaded.
    RpdMirror::Registry::set_state(schema, table, table_access_stats_t::NOT_LOADED, ShannonBase::load_type_t::SELF);
  }
  return result;
}
}  // namespace Autopilot
}  // namespace ShannonBase
