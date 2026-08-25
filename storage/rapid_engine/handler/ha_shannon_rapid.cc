/* Copyright (c) 2018, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   Copyright (c) 2023, Shannon Data AI and/or its affiliates. */

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"

#include <stddef.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "current_thd.h"
#include "include/lock0lock.h"
#include "lex_string.h"
#include "my_alloc.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysql/plugin.h"
#include "mysqld_error.h"
#include "sql/debug_sync.h"
#include "sql/handler.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/finalize_plan.h"
#include "sql/join_optimizer/make_join_hypergraph.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/opt_trace.h"
#include "sql/replication.h"  // Trans_param, TRANS_IS_REAL_TRANS
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"

#include "log0log.h" /* log_get_lsn */

#include "ml/ml_retrieve_schema_metadata.h"  // shannon_ml_on_ddl_event

#include "storage/innobase/handler/ha_innodb.h"  //thd_to_trx
#include "storage/innobase/include/dict0dd.h"    //dd_table_is_partitioned
#include "storage/innobase/include/trx0trx.h"    // trx_t::id, trx_is_started

#include "storage/rapid_engine/autopilot/loader.h"
#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/handler/ha_shannon_rapidpart.h"
#include "storage/rapid_engine/imcs/imcs.h"  // IMCS
#include "storage/rapid_engine/imcs/index/index.h"
#include "storage/rapid_engine/imcs/table0view.h"  //RapidCursor
#include "storage/rapid_engine/imcs/worker.h"      // BkgWorkerPool
#include "storage/rapid_engine/include/rapid_column_info.h"
#include "storage/rapid_engine/include/rapid_config.h"  //RpdEngineConfig
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/ml/query_arbitrator.h"  // Query Arbitrator
#include "storage/rapid_engine/monitor/rapid_monitor.h"
#include "storage/rapid_engine/optimizer/optimizer.h"
#include "storage/rapid_engine/optimizer/path/access_path.h"
#include "storage/rapid_engine/optimizer/utils.h"
#include "storage/rapid_engine/populate/log_commons.h"
#include "storage/rapid_engine/populate/log_populate.h"
#include "storage/rapid_engine/recovery/recovery.h"  // rapid_recovery_startup, rapid_recovery_shutdown
#include "storage/rapid_engine/statistics/statistics.h"
#include "storage/rapid_engine/trx/transaction.h"  //transaction
#include "storage/rapid_engine/utils/concurrent.h"
#include "storage/rapid_engine/utils/memory_pool.h"
#include "storage/rapid_engine/utils/utils.h"
#include "template_utils.h"
#include "thr_lock.h"

namespace dd {
class Table;
}

static void rapid_register_tx(handlerton *const hton, THD *const thd, ShannonBase::Transaction *const trx);

namespace ShannonBase {
// ShannonBase Rapid Engine handlerton.
handlerton *shannon_rapid_hton_ptr{nullptr};

MEM_ROOT rapid_mem_root(PSI_NOT_INSTRUMENTED, 1024);

// shannon rapid engine configuration.
RpdEngineConfig shannon_rpd_engine_cfg = RpdEngineConfig::Configuration();

// Global rapid engine instances.
std::shared_ptr<Utils::MemoryPool> shannon_rpd_memory_pool{nullptr};

// Column information for tables loaded into Shannon Rapid.
rpd_columns_container shannon_rpd_columns_info;
std::mutex shannon_rpd_columns_mutex;

// Shannon Rapid Engine Cost estimator.
ShannonBase::Optimizer::CostEstimator *shannon_rpd_cost_est_instances{nullptr};

LoadedTables *shannon_loaded_tables = nullptr;

// Self-Load manager instance.
ShannonBase::Autopilot::SelfLoadManager *shannon_self_load_mgr_inst{nullptr};

bool Rapid_execution_context::BestPlanSoFar(const JOIN &join, double cost) {
  if (&join != m_current_join) {
    // No plan has been seen for this join. The current one is best so far.
    m_current_join = &join;
    m_best_cost = cost;
    return true;
  }

  // Check if the current plan is the best seen so far.
  const bool cheaper = cost < m_best_cost;
  m_best_cost = std::min(m_best_cost, cost);
  return cheaper;
}

void LoadedTables::add(std::string db, std::string table, SharePtr share) {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_tables.insert_or_assign(TableKey{std::move(db), std::move(table)}, std::move(share));
}

LoadedTables::SharePtr LoadedTables::get(const std::string &db, const std::string &table) const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  auto it = m_tables.find(TableKey{db, table});
  return it == m_tables.end() ? nullptr : it->second;
}

void LoadedTables::erase(const std::string &db, const std::string &table) {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  m_tables.erase(TableKey{db, table});
}

std::vector<LoadedTableInfo> LoadedTables::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<LoadedTableInfo> result;
  result.reserve(m_tables.size());
  for (const auto &[key, share] : m_tables) result.push_back({share->m_tableid, key.schema, key.table});
  return result;
}

namespace {
[[nodiscard]] int secondary_error(const std::string &msg, int err_code) {
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), msg.c_str());
  return err_code;
}
}  // namespace
ha_rapid::ha_rapid(handlerton *hton, TABLE_SHARE *table_share_arg)
    : handler(hton, table_share_arg), m_share(nullptr), m_thd(ha_thd()) {}

int ha_rapid::open(const char *name, int, uint open_flags, const dd::Table *table_def) {
  auto share = shannon_loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (share == nullptr) return secondary_error("Table has not been loaded", HA_ERR_GENERIC);

  thr_lock_data_init(&share->lock, &m_lock, nullptr);

  m_rpd_table = dd_table_is_partitioned(*table_def) ? Imcs::Imcs::instance()->get_rpd_parttable(share->m_tableid)
                                                    : Imcs::Imcs::instance()->get_rpd_table(share->m_tableid);
  m_cursor.reset(new Imcs::RapidCursor(table, m_rpd_table));

  if (auto ret = m_cursor->open(); ret) return ret;  // open failed.

  if (end_range) m_cursor->set_end_range(end_range);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::close() {
  if (auto ret = m_cursor->close(); ret) return ret;  // close failed.
  m_cursor.reset(nullptr);

  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::info(unsigned int flags) {
  ut_a(flags == (HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK));

  auto share = shannon_loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (share == nullptr) return secondary_error("Table has not been loaded", HA_ERR_GENERIC);

  auto rpd_tb = table->part_info ? Imcs::Imcs::instance()->get_rpd_parttable(share->m_tableid)
                                 : Imcs::Imcs::instance()->get_rpd_table(share->m_tableid);
  stats.records = rpd_tb ? rpd_tb->count_total_rows() : 0;
  return ShannonBase::SHANNON_SUCCESS;
}

void ha_rapid::set_predicate(std::unique_ptr<Imcs::Predicate> predicate) {
  ut_a(m_cursor);
  m_cursor->set_scan_predicates(predicate.get() ? std::move(predicate) : nullptr);
}

void ha_rapid::set_projection(const std::vector<uint32_t> &columns) {
  ut_a(m_cursor);
  m_cursor->set_projection_columns(columns);
}

void ha_rapid::set_scan_limit(ha_rows limit, ha_rows offset) {
  ut_a(m_cursor);
  m_cursor->set_scan_limit(limit, offset);
}

void ha_rapid::set_storage_index(bool use_storage_index) {
  ut_a(m_cursor);
  m_cursor->set_storage_index(use_storage_index);
}

handler::Table_flags ha_rapid::table_flags() const {
  ulong flags = HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE | HA_KEYREAD_ONLY;
  return flags;
}

const char *ha_rapid::table_type() const { return rapid_hton_name; }

unsigned long ha_rapid::index_flags(unsigned int idx, unsigned int part, bool all_parts) const {
  if (table == nullptr) return 0;
  // Partitioned Rapid index execution is not implemented yet. Never advertise
  // an access path whose per-partition callbacks would have to fail at runtime.
  if (table->part_info != nullptr) return 0;

  auto share = shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
  if (share == nullptr) return 0;

  auto *rpd_tb = Imcs::Imcs::instance()->get_rpd_table(share->m_tableid);
  if (rpd_tb == nullptr) return 0;

  const auto *index_desc = rpd_tb->get_art_index_descriptor(idx);
  if (index_desc == nullptr || rpd_tb->get_index(idx) == nullptr) return 0;

  unsigned long rapid_flags = HA_READ_NEXT | HA_KEYREAD_ONLY | HA_KEY_SCAN_NOT_ROR;
  if (index_desc->supports_ordered_access()) {
    rapid_flags |= HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE;
  }

  const handler *primary = ha_get_primary_handler();
  const unsigned long primary_flags = primary == nullptr ? 0 : primary->index_flags(idx, part, all_parts);

  return rapid_flags & primary_flags;
}

int ha_rapid::records(ha_rows *num_rows) {
  auto share = shannon_loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (share == nullptr) {
    *num_rows = 0;
    return secondary_error("Table has not been loaded", HA_ERR_GENERIC);
  }

  auto rpd_tb = table->part_info ? Imcs::Imcs::instance()->get_rpd_parttable(share->m_tableid)
                                 : Imcs::Imcs::instance()->get_rpd_table(share->m_tableid);
  if (rpd_tb == nullptr) {
    *num_rows = 0;
    return HA_ERR_GENERIC;
  }

  auto *trx = ShannonBase::Transaction::get_or_create_trx(current_thd);
  if (trx == nullptr) {
    *num_rows = 0;
    return HA_ERR_GENERIC;
  }
  if (trx->begin() != ShannonBase::SHANNON_SUCCESS) {
    *num_rows = 0;
    return HA_ERR_GENERIC;
  }

  ShannonBase::Rapid_scan_context scan_context;
  scan_context.m_thd = current_thd;
  scan_context.m_trx = trx;
  scan_context.m_extra_info.m_trxid = trx->get_id();
  scan_context.m_extra_info.m_scn = ShannonBase::TransactionCoordinator::instance().get_current_scn();

  ::ReadView *read_view = trx->acquire_snapshot();
  if (trx->isolation_level() > ShannonBase::Transaction::ISOLATION_LEVEL::READ_UNCOMMITTED && read_view == nullptr) {
    *num_rows = 0;
    return HA_ERR_GENERIC;
  }

  const table_id_t table_id = rpd_tb->meta().table_id;
  const auto barrier = ShannonBase::Populate::Populator::request_table_barrier(table_id);
  if (barrier.state == ShannonBase::Populate::TablePropagationState::BROKEN) {
    *num_rows = 0;
    return secondary_error("Rapid table has failed DML propagation and must be reloaded before secondary-engine reads",
                           HA_ERR_GENERIC);
  }
  if (barrier.needs_wait()) {
    for (;;) {
      if (current_thd != nullptr && current_thd->killed) {
        *num_rows = 0;
        return HA_ERR_GENERIC;
      }
      const auto wait_result = ShannonBase::Populate::Populator::wait_table_applied_for(
          table_id, barrier.required_change_id, ShannonBase::Populate::QUERY_PROPAGATION_WAIT_SLICE_MS);
      if (wait_result == ShannonBase::Populate::TablePropagationWaitResult::APPLIED) break;
      if (wait_result == ShannonBase::Populate::TablePropagationWaitResult::BROKEN) {
        *num_rows = 0;
        return secondary_error(
            "Rapid table DML propagation failed while waiting for the query watermark; reload required",
            HA_ERR_GENERIC);
      }
      // PENDING/GONE: keep waiting -- GONE only means this particular wait
      // slice raced a buffer swap, not that the table is unrecoverable.
    }
  }

  *num_rows = static_cast<ha_rows>(rpd_tb->count_visible_rows(&scan_context));
  return ShannonBase::SHANNON_SUCCESS;
}

ha_rows ha_rapid::records_in_range(unsigned int index, key_range *min_key, key_range *max_key) {
  // Get the number of records in the range from the primary storage engine.
  return ha_get_primary_handler()->records_in_range(index, min_key, max_key);
}

double ha_rapid::scan_time() {
  DBUG_TRACE;

  const double t = (stats.records + stats.deleted) * ShannonBase::shannon_rpd_cost_est_instances->io_factor();
  return t;
}

THR_LOCK_DATA **ha_rapid::store_lock(THD *, THR_LOCK_DATA **to, thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && m_lock.type == TL_UNLOCK) m_lock.type = lock_type;
  *to++ = &m_lock;
  return to;
}

int ha_rapid::load_table(const TABLE &table_arg, bool *skip_metadata_update [[maybe_unused]]) {
  ut_ad(table_arg.file != nullptr && table_arg.s != nullptr);

  // between the tables loaded into rapid engine for log parser thread. perhaps, some new indexes are added into,
  // therefore, we reload the indexes caches at each table loaded into to refresh the global indexes cache.
  ShannonBase::Populate::Populator::load_indexes_caches();

  std::ostringstream oss;
  if (shannon_loaded_tables->get(table_arg.s->db.str, table_arg.s->table_name.str) != nullptr) {
    oss << table_arg.s->db.str << "." << table_arg.s->table_name.str << " already loaded";
    auto err = oss.str();
    return secondary_error(err, HA_ERR_KEY_NOT_FOUND);
  }

  if (table_arg.s->is_missing_primary_key()) {
    oss << table_arg.s->db.str << "." << table_arg.s->table_name.str << " requires PK for loading into rapid";
    auto err = oss.str();
    return secondary_error(err, HA_ERR_KEY_NOT_FOUND);
  }

  for (auto idx = 0u; idx < table_arg.s->fields; idx++) {
    auto fld = *(table_arg.field + idx);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    if (!ShannonBase::Utils::Util::is_support_type(fld->type())) {
      oss << table_arg.s->table_name.str << "." << fld->field_name << " type not allowed";
      auto err = oss.str();
      return secondary_error(err, HA_ERR_KEY_NOT_FOUND);
    }
  }

  m_thd->set_sent_row_count(0);

  // start to read data from innodb and load to rapid.
  ShannonBase::Rapid_load_context context;
  context.m_thd = m_thd;
  context.m_table = const_cast<TABLE *>(&table_arg);
  context.m_table_id = table_arg.file->get_table_id();
  context.m_schema_name = table_arg.s->db.str;
  context.m_table_name = table_arg.s->table_name.str;
  context.m_sch_tb_name = context.m_schema_name + "." + context.m_table_name;
  context.m_extra_info.m_oper = ShannonBase::Rapid_context::extra_info_t::OperType::LOAD;
  context.m_extra_info.m_keynr = active_index;
  context.m_extra_info.m_key_len = table_arg.file->ref_length;
  context.m_trx = Transaction::get_or_create_trx(m_thd);
  if (context.m_trx == nullptr)
    return secondary_error("Rapid: cannot get the primary InnoDB transaction information", HA_ERR_GENERIC);
  ShannonBase::TransactionGuard guard(context.m_trx);
  context.m_extra_info.m_trxid = context.m_trx->get_id();

  // at loading step, to set SCN to non-zero, it means it committed after inserted with explicit begin/commit.
  context.m_extra_info.m_scn = TransactionCoordinator::instance().allocate_scn();

  Utils::Util::update_rpd_meta_info(&context, &table_arg, Utils::Util::STAGE::BEGIN);
  if (Imcs::Imcs::instance()->load_table(&context, const_cast<TABLE *>(&table_arg))) {
    oss << table_arg.s->db.str << "." << table_arg.s->table_name.str << " load failed";
    auto err = oss.str();
    return secondary_error(err, HA_ERR_GENERIC);
  }
  Utils::Util::update_rpd_meta_info(&context, &table_arg, Utils::Util::STAGE::END);

  guard.commit();
  m_share = std::make_shared<RapidShare>(table_arg);
  m_share->m_source_table = &table_arg;
  m_share->is_partitioned = false;
  m_share->file = this;
  m_share->m_tableid = context.m_table_id;

  shannon_loaded_tables->add(table_arg.s->db.str, table_arg.s->table_name.str, m_share);
  if (shannon_loaded_tables->get(table_arg.s->db.str, table_arg.s->table_name.str) == nullptr)
    return secondary_error("Failed to load table", HA_ERR_KEY_NOT_FOUND);

  // start population thread if table loaded successfully.
  ShannonBase::Populate::Populator::start();
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::unload_table(const char *db_name, const char *table_name, bool error_if_not_loaded) {
  // stop the table worker thread.
  auto share = shannon_loaded_tables->get(db_name, table_name);
  if (error_if_not_loaded && !share) {
    std::string msg = std::string(db_name) + "." + table_name + " table is not loaded into rapid yet";
    return secondary_error(msg, HA_ERR_GENERIC);
  }

  const auto table_id = share ? share->m_tableid : 0;
  ShannonBase::Populate::Populator::unload(table_id);

  ShannonBase::Rapid_load_context context;
  context.m_table = share ? const_cast<TABLE *>(share->m_source_table) : nullptr;
  context.m_table_id = table_id;
  context.m_thd = m_thd;
  context.m_extra_info.m_keynr = active_index;
  context.m_schema_name = db_name;
  context.m_table_name = table_name;

  Imcs::Imcs::instance()->unload_table(&context, table_id, false);

  {
    std::lock_guard<std::mutex> lock(ShannonBase::shannon_rpd_columns_mutex);
    std::erase_if(ShannonBase::shannon_rpd_columns_info, [&](const auto &col) {
      return std::strcmp(db_name, col.schema_name) == 0 && std::strcmp(table_name, col.table_name) == 0;
    });
  }

  shannon_loaded_tables->erase(db_name, table_name);

  if (ShannonBase::shannon_self_load_mgr_inst) {
    ShannonBase::shannon_self_load_mgr_inst->remove_table(db_name, table_name);
  }

  if (shannon_loaded_tables->size() == 0) {
    ShannonBase::Populate::Populator::shutdown();
  }

  return ShannonBase::SHANNON_SUCCESS;
}

/**
  @note
  A quote from handler::start_stmt():
  <quote>
  MySQL calls this function at the start of each SQL statement inside LOCK
  TABLES. Inside LOCK TABLES the ::external_lock method does not work to
  mark SQL statement borders.
  </quote>

  @return
    HA_EXIT_SUCCESS  OK
*/
int ha_rapid::start_stmt(THD *const thd, thr_lock_type lock_type) {
  ut_a(thd != nullptr);

  auto *trx = ShannonBase::Transaction::get_or_create_trx(thd);
  if (trx == nullptr) return HA_ERR_GENERIC;
  rapid_register_tx(ShannonBase::shannon_rapid_hton_ptr, thd, trx);

  return ShannonBase::SHANNON_SUCCESS;
}

/** Initialize a table scan.
@param[in]      scan    whether this is a second call to rnd_init()
                        without rnd_end() in between
@return 0 or error number */
int ha_rapid::rnd_init(bool scan) {
  // For LATERAL / correlated re-scans, MySQL calls rnd_init(scan=true)
  // without an intervening rnd_end().  Rewind the scan position so each
  // outer row sees a fresh inner scan, but keep the transaction and
  // snapshot alive.
  if (scan) m_cursor->reset_scan();
  if (auto ret = m_cursor->init(); ret) return ret;

  m_extra_description.clear();
  inited = handler::RND;
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::rnd_end(void) {
  if (auto ret = m_cursor->end(); ret) return ret;

  inited = handler::NONE;
  return ShannonBase::SHANNON_SUCCESS;
}

void ha_rapid::position(const unsigned char *record) {
  // Here, table should has a PK, otherwise, it cannot be loaded. Therefore, ref stores the rowid of `record`.
  if (m_cursor) {
    auto pos = m_cursor->position(record);
    memcpy(ref, &pos, sizeof(row_id_t));
  }
}

int ha_rapid::rnd_pos(unsigned char *buff, unsigned char *pos) {
  int error{HA_ERR_KEY_NOT_FOUND};
  if (inited == handler::RND && m_cursor) error = m_cursor->rnd_pos(buff, pos);
  return error;
}

/** Reads the next row in a table scan (also used to read the FIRST row
 in a table scan).
 @return 0, HA_ERR_END_OF_FILE, or error number */
int ha_rapid::rnd_next(uchar *buf) {
  int error{HA_ERR_END_OF_FILE};

  if (inited == handler::RND) {
    auto reader_pool = ShannonBase::Imcs::Imcs::pool();
    if (table_share->fields <= static_cast<uint>(ShannonBase::shannon_rpd_engine_cfg.async_column_threshold) ||
        reader_pool == nullptr) {
      error = m_cursor->next(buf);
    } else {
      std::future<int> fut = boost::asio::co_spawn(*reader_pool, m_cursor->next_async(buf), boost::asio::use_future);
      error = fut.get();
    }
    // Normalise HA_ERR_KEY_NOT_FOUND → HA_ERR_END_OF_FILE for both paths.
    if (error == HA_ERR_KEY_NOT_FOUND) {
      error = HA_ERR_END_OF_FILE;
    }
  }

  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return error;
}

int ha_rapid::rnd_next_batch(size_t batch_size, std::vector<ShannonBase::Executor::ColumnChunk> &data,
                             size_t &read_cnt) {
  int error{HA_ERR_END_OF_FILE};

  if (inited == handler::RND) error = m_cursor->next(batch_size, data, read_cnt);

  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return error;
}

const std::vector<row_id_t> &ha_rapid::last_batch_row_ids() const { return m_cursor->last_batch_row_ids(); }

void ha_rapid::set_last_returned_rowid(row_id_t rid) { m_cursor->set_last_returned_rowid(rid); }

int ha_rapid::index_init(uint keynr, bool sorted) {
  DBUG_TRACE;

  if (auto ret = m_cursor->index_init(keynr, sorted); ret) return ret;

  active_index = keynr;
  inited = handler::INDEX;
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::index_end() {
  DBUG_TRACE;

  if (auto ret = m_cursor->index_end(); ret) return ret;

  active_index = MAX_KEY;
  inited = handler::NONE;
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::index_read(uchar *buf, const uchar *key, uint key_len, ha_rkey_function find_flag) {
  DBUG_TRACE;
  int err{HA_ERR_END_OF_FILE};
  ut_ad(inited == handler::INDEX);

  m_cursor->set_end_range(end_range);
  err = m_cursor->index_read(buf, key, key_len, find_flag);
  if (err == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return err;
}

int ha_rapid::index_read_last(uchar *buf, const uchar *key, uint key_len) {
  m_cursor->set_end_range(end_range);
  return (m_cursor->index_read(buf, key, key_len, HA_READ_PREFIX_LAST));
}

int ha_rapid::index_next(uchar *buf) {
  ut_ad(inited == handler::INDEX);

  auto error = m_cursor->index_next(buf);
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return error;
}

int ha_rapid::index_next_same(uchar *buf, const uchar *, uint) {
  ut_ad(inited == handler::INDEX);

  auto error = m_cursor->index_next(buf);
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return error;
}

int ha_rapid::index_first(uchar *buf) {
  DBUG_TRACE;
  ut_ad(inited == handler::INDEX);

  // Always start from the true beginning of the index.  end_range (if set)
  // only constrains how far index_next() may go; it must not be used as the
  // search key to locate the starting position.
  if (end_range) m_cursor->set_end_range(end_range);
  int error = m_cursor->index_read(buf, nullptr, 0, HA_READ_KEY_OR_NEXT);
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_first_count);
  return error;
}

int ha_rapid::index_prev(uchar *buf) {
  ut_ad(inited == handler::INDEX);

  auto error = m_cursor->index_prev(buf);
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_prev_count);
  return error;
}

int ha_rapid::index_last(uchar *buf) {
  DBUG_TRACE;

  m_cursor->set_end_range(end_range);
  int error = m_cursor->index_read(buf, nullptr, 0, HA_READ_BEFORE_KEY);

  /* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND */

  if (error == HA_ERR_KEY_NOT_FOUND) {
    error = HA_ERR_END_OF_FILE;
  }
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_last_count);
  return error;
}

int ha_rapid::read_range_first(const key_range *start_key, const key_range *end_key, bool eq_range_arg, bool sorted) {
  m_cursor->set_start_range(start_key);

  const int error = handler::read_range_first(start_key, end_key, eq_range_arg, sorted);

  m_cursor->set_start_range(nullptr);
  return error;
}

int ha_rapid::read_range_next() { return (handler::read_range_next()); }
}  // namespace ShannonBase

static bool rpd_thd_trx_is_auto_commit(THD *thd);

namespace {

/**
 * Register Rapid for statement/final transaction callbacks when COPY_INFO is
 * emitted from a primary-engine DML statement.
 */
inline void RegisterCopyInfoParticipant(THD *thd) {
  trans_register_ha(thd, false, ShannonBase::shannon_rapid_hton_ptr, nullptr);
  if (!rpd_thd_trx_is_auto_commit(thd)) {
    trans_register_ha(thd, true, ShannonBase::shannon_rapid_hton_ptr, nullptr);
  }
}

bool EnqueueCopyInfo(THD *thd, ShannonBase::Populate::change_record_buff_t &&record) {
  if (thd == nullptr) return false;

  if (ShannonBase::Transaction::get_or_create_trx(thd) == nullptr) return false;

  auto registration = ShannonBase::Populate::TransactionManager::instance().register_change(thd, record.m_table_id);
  if (!registration) return false;

  record.m_source_trx_id = registration.source_trx_id;
  record.m_commit_scn = 0;

  const uint64_t capture_lsn = log_get_lsn(*log_sys);
  ShannonBase::Populate::Populator::write(nullptr, capture_lsn, &record);
  return true;
}

void rapid_after_commit(void *arg) {
  const auto *param = static_cast<const Trans_param *>(arg);
  if (param == nullptr || (param->flags & TRANS_IS_REAL_TRANS) == 0) return;

  THD *thd = current_thd;
  auto *trx = thd ? ShannonBase::Transaction::find_trx(thd) : nullptr;
  if (trx != nullptr) trx->commit();
}

void rapid_before_rollback(void *arg) {
  const auto *param = static_cast<const Trans_param *>(arg);
  if (param == nullptr || (param->flags & TRANS_IS_REAL_TRANS) == 0) return;

  THD *thd = current_thd;
  auto *trx = thd ? ShannonBase::Transaction::find_trx(thd) : nullptr;
  if (trx != nullptr) trx->rollback();
}
}  // namespace

static bool rpd_thd_trx_is_auto_commit(THD *thd) { /*!< in: thread handle, can be NULL */
  return (thd != nullptr && !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN));
}

static void rapid_register_tx(handlerton *const hton, THD *const thd, ShannonBase::Transaction *const trx) {
  ut_a(trx != nullptr);

  trans_register_ha(thd, false, ShannonBase::shannon_rapid_hton_ptr, nullptr);

  if (!rpd_thd_trx_is_auto_commit(thd)) {
    trx->begin_stmt();  // tx->stat_stmt()
    trans_register_ha(thd, true, ShannonBase::shannon_rapid_hton_ptr, nullptr);
  }
}

/** Commits a transaction in an database or marks an SQL statement
 ended.
 @return 0 or deadlock error if the transaction was aborted by another
         higher priority transaction. */
static int rapid_commit(handlerton *hton,  /*!< in: handlerton */
                        THD *thd,          /*!< in: MySQL thread handle of the
                                             user for whom the transaction should
                                             be committed */
                        bool commit_trx) { /*!< in: true - commit transaction
                                            false - the current SQL statement
                                            ended */
  const bool final_commit = commit_trx || rpd_thd_trx_is_auto_commit(thd);
  auto *trx = ShannonBase::Transaction::find_trx(thd);

  if (trx != nullptr) {
    if (final_commit) {
      /*
        We get here
         - For a COMMIT statement that finishes a multi-statement transaction
         - For a statement that has its own transaction
      */
      if (trx->commit()) return HA_ERR_ERRORS;
    }

    if (!final_commit && trx->commit_stmt() != ShannonBase::SHANNON_SUCCESS) return HA_ERR_ERRORS;

    if (trx->isolation_level() <= ShannonBase::Transaction::ISOLATION_LEVEL::READ_COMMITTED) {
      // Drop only Rapid's before-image retention fence at the statement
      // boundary. InnoDB owns opening/closing/replacing its SQL ReadView.
      trx->release_snapshot();
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

/** Rolls back a transaction or the latest SQL statement.
 @return 0 or error number */
static int rapid_rollback(handlerton *hton,    /*!< in: handlerton */
                          THD *thd,            /*!< in: handle to the MySQL thread
                                                 of the user whose transaction should
                                                 be rolled back */
                          bool rollback_trx) { /*!< in: true - rollback entire
                                              transaction false - rollback the
                                              current statement only */
  const bool final_rollback = rollback_trx || rpd_thd_trx_is_auto_commit(thd);

  auto *trx = ShannonBase::Transaction::find_trx(thd);
  if (trx != nullptr) {
    final_rollback ? trx->rollback() : trx->rollback_stmt();
    if (trx->isolation_level() <= ShannonBase::Transaction::ISOLATION_LEVEL::READ_COMMITTED) {
      // Drop only Rapid's before-image retention fence; primary SQL snapshot
      // lifecycle remains owned by InnoDB/server.
      trx->release_snapshot();
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

/** Creates the Rapid transaction facade for the THD if needed. The facade is
 bound at construction to the THD's real primary InnoDB trx_t and uses that
 transaction's ReadView as the sole SQL visibility oracle. Rapid owns neither
 the trx_t nor its commit/rollback lifecycle.
 @return 0 */
static int rapid_start_trx_and_assign_read_view(handlerton *hton, /* in: Rapid handlerton */
                                                THD *thd) {       /* in: MySQL thread handle of the user for whom the
                                                                     transaction should be committed */

  ut_a(hton == ShannonBase::shannon_rapid_hton_ptr);

  ShannonBase::Transaction *trx = ShannonBase::Transaction::get_or_create_trx(thd);
  if (!trx) {
    push_warning_printf(thd, Sql_condition::SL_WARNING, HA_ERR_UNSUPPORTED,
                        "Rapid: Can not get transaction from innodb. "
                        "A transaction should be created in innodb Storage Engine firstly.");
    return HA_ERR_ERRORS;
  }

  // here, the trx should be regiestered in innodb.
  rapid_register_tx(hton, thd, trx);

  // Register the same primary InnoDB transaction for this SQL statement even
  // when it is already active (e.g. the second statement of an RR transaction).
  if (trx->begin() != ShannonBase::SHANNON_SUCCESS) return HA_ERR_ERRORS;

  // Transaction construction already bound the facade to check_trx_exists(thd),
  // i.e. the exact InnoDB-owned transaction for this THD.
  if (trx->isolation_level() == ShannonBase::Transaction::ISOLATION_LEVEL::READ_REPEATABLE) {
    if (trx->acquire_snapshot() == nullptr) return HA_ERR_ERRORS;
  } else {
    push_warning_printf(thd, Sql_condition::SL_WARNING, HA_ERR_UNSUPPORTED,
                        "Only REPEATABLE READ isolation level is "
                        "supported for START TRANSACTION WITH CONSISTENT "
                        "SNAPSHOT in Rapid Storage Engine. Snapshot has not "
                        "been taken.");
  }

  return ShannonBase::SHANNON_SUCCESS;
}

/* Dummy SAVEPOINT support. This is needed for long running transactions
 * like mysqldump (https://bugs.mysql.com/bug.php?id=71017).
 * Current SAVEPOINT does not correctly handle ROLLBACK and does not return
 * errors. This needs to be addressed in future versions (Issue#96).
 */
static int rapid_savepoint(handlerton *const, THD *const, void *const) { return 0; }

static int rapid_rollback_to_savepoint(handlerton *const hton, THD *const thd, void *const savepoint) {
  ShannonBase::Populate::TransactionManager::instance().quarantine_partial_rollback(
      thd, "ROLLBACK TO SAVEPOINT after DML was already propagated");
  auto *trx = ShannonBase::Transaction::find_trx(thd);
  return trx ? trx->rollback_to_savepoint(savepoint) : ShannonBase::SHANNON_SUCCESS;
}

static bool rapid_rollback_to_savepoint_can_release_mdl(handlerton *const hton, THD *const thd) { return true; }

/** Frees a possible trx object associated with the current THD.
 @return 0 or error number */
static int rapid_close_connection(handlerton *hton, /*!< in: handlerton */
                                  THD *thd) {       /*!< in: handle to the MySQL thread of the user
                                                   whose resources should be free'd */
  DBUG_TRACE;
  ut_a(hton == ShannonBase::shannon_rapid_hton_ptr);
  // Defensive cleanup only. Normal transaction rollback publishes from
  // se_before_rollback; this covers connection teardown with unresolved
  // propagation participation and is idempotent after a terminal callback.
  if (auto *trx = ShannonBase::Transaction::find_trx(thd); trx != nullptr) {
    trx->rollback();
  }
  ShannonBase::Transaction::free_trx_from_thd(thd);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Cancel any pending lock request associated with the current THD. */
static void rapid_kill_connection(handlerton *hton, /*!< in:  innobase handlerton */
                                  THD *thd) {       /*!< in: handle to the MySQL thread being
                                                   killed */
  DBUG_TRACE;
  ut_a(hton == ShannonBase::shannon_rapid_hton_ptr);

  trx_t *trx = thd_to_trx(thd);
  if (trx != nullptr) {
    /* Cancel a pending lock request if there are any */
    lock_cancel_if_waiting_and_release({trx});
  }
}

/** Return partitioning flags. */
static uint rapid_partition_flags() {
  return (HA_CAN_EXCHANGE_PARTITION | HA_CANNOT_PARTITION_FK | HA_TRUNCATE_PARTITION_PRECLOSE);
}

bool SetSecondaryEngineOffloadFailedReason(const THD *thd, std::string_view msg, bool raise_error) {
  ut_a(thd);
  thd->lex->m_secondary_engine_offload_or_exec_failed_reason = std::string(msg);

  if (raise_error) my_error(ER_SECONDARY_ENGINE, MYF(0), msg.data());
  return ShannonBase::SHANNON_SUCCESS;
}

static bool SetSecondaryEngineOffloadFailedReasonWrapper(const THD *thd, std::string_view msg) {
  return SetSecondaryEngineOffloadFailedReason(thd, msg, /*raise_error=*/true);
}

std::string_view GetSecondaryEngineOffloadorExecFailedReason(const THD *thd) {
  ut_a(thd);
  return thd->lex->m_secondary_engine_offload_or_exec_failed_reason.c_str();
}

SecondaryEngineGraphSimplificationRequestParameters SecondaryEngineCheckOptimizerRequest(
    THD *thd, const JoinHypergraph &hypergraph, const AccessPath *access_path, int current_subgraph_pairs,
    int current_subgraph_pairs_limit, bool is_root_access_path, std::string *trace) {
  SecondaryEngineGraphSimplificationRequestParameters params;
  params.secondary_engine_optimizer_request = SecondaryEngineGraphSimplificationRequest::kContinue;
  params.subgraph_pair_limit = 0;
  return params;
}

void NotifyCreateTable(struct HA_CREATE_INFO *create_info, const char *db, const char *table_name) {
  if (dd::get_dictionary()->is_dd_schema_name(db) || dd::get_dictionary()->is_system_table_name(db, table_name)) return;

  auto is_partitioned{false};
  dd::cache::Dictionary_client *dc = current_thd->dd_client();
  const dd::cache::Dictionary_client::Auto_releaser releaser(dc);
  const dd::Table *table_obj = nullptr;
  if (dc && !dc->acquire(db, table_name, &table_obj) && table_obj)
    is_partitioned = (table_obj->partition_type() != dd::Table::PT_NONE);

  std::string eng_str;
  if (create_info->secondary_engine.str) eng_str = create_info->secondary_engine.str;

  if (ShannonBase::shannon_self_load_mgr_inst) {
    auto tid = table_obj ? table_obj->se_private_id() : 0;
    ShannonBase::shannon_self_load_mgr_inst->add_table(tid, db, table_name, eng_str, is_partitioned);
  }

  // schema meta data embedding
  if (ShannonBase::shannon_rpd_engine_cfg.enable_schema_embedding) {
    std::string doc = ShannonBase::ML::serialize_from_dd_table(db, table_name, table_obj,
                                                               ShannonBase::ML::SerializeMode::WITH_COMMENTS);
    ShannonBase::ML::DDLEvent ev{ShannonBase::ML::DDLEventType::CREATE, db, table_name, std::move(doc)};
    ShannonBase::ML::shannon_ml_on_ddl_event(ev);
  }
}

void NotifyDropTable(Table_ref *tab) {
  if (!tab) return;

  if (dd::get_dictionary()->is_dd_schema_name(tab->get_db_name()) ||
      dd::get_dictionary()->is_system_table_name(tab->get_db_name(), tab->get_table_name()))
    return;

  if (ShannonBase::shannon_self_load_mgr_inst)
    ShannonBase::shannon_self_load_mgr_inst->erase_table(tab->get_db_name(), tab->get_table_name());

  if (ShannonBase::shannon_rpd_engine_cfg.enable_schema_embedding) {
    ShannonBase::ML::DDLEvent ev{ShannonBase::ML::DDLEventType::DROP, tab->get_db_name(), tab->get_table_name(), ""};
    ShannonBase::ML::shannon_ml_on_ddl_event(ev);
  }
}

bool NotifyAlterTable(THD *thd, const MDL_key *mdl_key, ha_notification_type notification_type) {
  auto schema = mdl_key->db_name();
  auto table = mdl_key->name();
  if (dd::get_dictionary()->is_dd_schema_name(schema) || dd::get_dictionary()->is_system_table_name(schema, table))
    return false;

  if (notification_type != HA_NOTIFY_POST_EVENT) return false;

  if (ShannonBase::shannon_rpd_engine_cfg.enable_schema_embedding) {
    ShannonBase::ML::DDLEvent ev{ShannonBase::ML::DDLEventType::ALTER, schema, table, "" /**refill later*/};
    ShannonBase::ML::shannon_ml_on_ddl_event(ev);
  }
  return false;
}

/**
 * @brief Read and copy BLOB-type off-page data from table
 *
 * The main purpose of this function is to address the storage characteristics of BLOB data types in MySQL:
 * - BLOB fields only store pointers and length information in the row record, actual data is stored in off-page areas
 * - When inserting multiple records consecutively, MySQL may reuse the same memory area for BLOB data
 * - If we only copy pointers from record[0], subsequent operations will overwrite the actual data pointed to
 *
 * The function ensures data integrity through the following steps:
 * 1. Use pre-cached BLOB field indices to avoid traversing all fields
 * 2. Parse BLOB field length prefixes and pointer information
 * 3. Create independent memory copies for each BLOB data
 * 4. Store copied data in off_page_data structure for later use
 *
 * @param table MySQL table structure pointer
 * @param off_page_data Output parameter, stores field indices and corresponding BLOB data copies
 */
static void read_off_page_data(TABLE *table,
                               ShannonBase::Populate::change_record_buff_t::off_page_data_t &off_page_data) {
  for (uint idx = 0; idx < table->s->fields; idx++) {
    Field *fld = *(table->field + idx);
    if (!bitmap_is_set(table->read_set, idx) || fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    if (likely(((fld->type() != MYSQL_TYPE_BLOB) && (fld->type() != MYSQL_TYPE_TINY_BLOB) &&
                (fld->type() != MYSQL_TYPE_MEDIUM_BLOB) && (fld->type() != MYSQL_TYPE_LONG_BLOB))))
      continue;
    if (fld->is_null()) continue;

    auto bfld = down_cast<Field_blob *>(fld);
    const size_t data_len = bfld->get_length();
    const uchar *actual_blob_data = bfld->get_blob_data();
    if (actual_blob_data == nullptr) continue;

    std::shared_ptr<uchar[]> blob_copy(new uchar[data_len ? data_len : 1]);
    std::memcpy(blob_copy.get(), actual_blob_data, data_len);
    off_page_data.emplace(idx, std::make_pair(data_len, std::move(blob_copy)));
  }
}

void NotifyAfterInsert(THD *thd, void *args) {
  if (!thd || !args) return;
  struct comb_args {
    TABLE *arg1;
    COPY_INFO *arg2;
    COPY_INFO *arg3;
  };

  auto params = static_cast<comb_args *>(args);
  if (!params) return;

  auto table = params->arg1;
  auto info = params->arg2;
  auto update = params->arg3;

  if (!table || !info || !update) return;

  auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
  if (share) {
    ShannonBase::Populate::change_record_buff_t copy_info_rec(ShannonBase::Populate::Source::COPY_INFO,
                                                              table->s->rec_buff_length);
    copy_info_rec.m_oper = ShannonBase::Populate::change_record_buff_t::OperType::INSERT;
    copy_info_rec.m_table_id = share->m_tableid;
#ifndef NDEBUG
    copy_info_rec.m_schema_name = table->s->db.str;
    copy_info_rec.m_table_name = table->s->table_name.str;
#endif
    std::memcpy(copy_info_rec.m_buff0.get(), table->record[0], table->s->rec_buff_length);
    // read and store off-page data.
    read_off_page_data(table, copy_info_rec.m_offpage_data0);

    RegisterCopyInfoParticipant(thd);
    if (!EnqueueCopyInfo(thd, std::move(copy_info_rec))) {
      ShannonBase::Populate::QuarantinePropagationTables({share->m_tableid});
      sql_print_warning("Rapid COPY_INFO could not register COPY_INFO transaction participation for table %llu",
                        static_cast<unsigned long long>(share->m_tableid));
    }
  }
}

// old_row = table->record[1], new_row = table->record[0]
void NotifyAfterUpdate(THD *thd, void *args) {
  if (!thd || !args) return;
  struct comb_args {
    TABLE *arg1;
    const uchar *arg2;
    const uchar *arg3;
  };

  auto params = static_cast<comb_args *>(args);
  if (!params) return;

  auto table = params->arg1;
  auto old_row = params->arg2;
  auto new_row = params->arg3;

  if (!table || !old_row || !new_row) return;

  auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
  if (share) {
    ShannonBase::Populate::change_record_buff_t copy_info_rec(ShannonBase::Populate::Source::COPY_INFO,
                                                              table->s->rec_buff_length);
    copy_info_rec.m_oper = ShannonBase::Populate::change_record_buff_t::OperType::UPDATE;
    copy_info_rec.m_table_id = share->m_tableid;
#ifndef NDEBUG
    copy_info_rec.m_schema_name = table->s->db.str;
    copy_info_rec.m_table_name = table->s->table_name.str;
#endif
    std::memcpy(copy_info_rec.m_buff0.get(), old_row, table->s->rec_buff_length);
    if (new_row) {
      std::memcpy(copy_info_rec.m_buff1.get(), new_row, table->s->rec_buff_length);
      read_off_page_data(table, copy_info_rec.m_offpage_data1);
    }

    RegisterCopyInfoParticipant(thd);
    if (!EnqueueCopyInfo(thd, std::move(copy_info_rec))) {
      ShannonBase::Populate::QuarantinePropagationTables({share->m_tableid});
      sql_print_warning("Rapid COPY_INFO could not register COPY_INFO transaction participation for table %llu",
                        static_cast<unsigned long long>(share->m_tableid));
    }
  }
}

void NotifyAfterDelete(THD *thd, void *args) {
  if (!thd || !args) return;
  struct comb_args {
    TABLE *arg1;
    const uchar *old_rec;
  };

  auto params = static_cast<comb_args *>(args);
  if (!params) return;

  auto table = params->arg1;
  auto old_row = params->old_rec;

  if (!table || !old_row) return;

  auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
  if (share) {
    ShannonBase::Populate::change_record_buff_t copy_info_rec(ShannonBase::Populate::Source::COPY_INFO,
                                                              table->s->rec_buff_length);
    copy_info_rec.m_oper = ShannonBase::Populate::change_record_buff_t::OperType::DELETE;
    copy_info_rec.m_table_id = share->m_tableid;
#ifndef NDEBUG
    copy_info_rec.m_schema_name = table->s->db.str;
    copy_info_rec.m_table_name = table->s->table_name.str;
#endif
    std::memcpy(copy_info_rec.m_buff0.get(), old_row, table->s->rec_buff_length);

    read_off_page_data(table, copy_info_rec.m_offpage_data0);
    RegisterCopyInfoParticipant(thd);
    if (!EnqueueCopyInfo(thd, std::move(copy_info_rec))) {
      ShannonBase::Populate::QuarantinePropagationTables({share->m_tableid});
      sql_print_warning("Rapid COPY_INFO could not register COPY_INFO transaction participation for table %llu",
                        static_cast<unsigned long long>(share->m_tableid));
    }
  }
}

void NotifyAfterSelect(THD *thd, SelectExecutedIn executed_in) {
  if (executed_in == SelectExecutedIn::kPrimaryEngine) return;

  if (!thd || !thd->lex) return;

  double query_cost = 0.0f;
  if (thd->lex->query_block && thd->lex->query_block->join && thd->lex->query_block->join->best_read) {
    query_cost = thd->lex->query_block->join->best_read *
                 (ShannonBase::SHANNON_HD_READ_FACTOR + ShannonBase::SHANNON_RAM_READ_FACTOR);
  }

  double cost_threshold = thd->variables.secondary_engine_cost_threshold;
  if (query_cost <= cost_threshold)  // update only if query coast is higher than threshold.
    return;

  if (ShannonBase::shannon_self_load_mgr_inst)
    ShannonBase::shannon_self_load_mgr_inst->update_table_stats(thd, thd->lex->query_tables, executed_in);
}

// In this function, Dynamic offload combines mysql plan features retrieved from rapid_statement_context and RAPID info
// such as rapid base table cardinality, dict encoding projection, varlen projection size, rapid queue size in to
// decide if query should be offloaded to RAPID. returns true, goes to innodb for execution. returns false, goes to
// next phase for secondary engine execution.
static bool RapidPrepareEstimateQueryCosts(THD *thd, LEX *lex) {
  if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_OFF) {
    SetSecondaryEngineOffloadFailedReason(thd, "use_secondary_engine set to off");
    return true;
  }

  const auto tx_isolation = thd_tx_isolation(thd);
  if (tx_isolation == ISO_READ_UNCOMMITTED || tx_isolation == ISO_SERIALIZABLE) {
    SetSecondaryEngineOffloadFailedReason(thd, "Rapid MVCC offload supports READ COMMITTED and REPEATABLE READ");
    return true;
  }

  for (Table_ref *table_ref = lex != nullptr ? lex->query_tables : nullptr; table_ref != nullptr;
       table_ref = table_ref->next_global) {
    if (table_ref->is_placeholder()) continue;

    auto share = ShannonBase::shannon_loaded_tables->get(table_ref->db, table_ref->table_name);
    if (!share) {
      SetSecondaryEngineOffloadFailedReason(thd, "table is not loaded in Rapid");
      return true;
    }

    const auto propagation = ShannonBase::Populate::Populator::request_table_barrier(share->m_tableid);
    if (propagation.state == ShannonBase::Populate::TablePropagationState::BROKEN) {
      SetSecondaryEngineOffloadFailedReason(thd, "table has failed DML propagation and must be reloaded1");
      return true;
    }
  }

  if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_FORCED) return false;
  // Only non-FORCED cost arbitration needs the primary-plan cache populated in the PRIMARY_TENTATIVELY phase.
  auto shannon_statement_context = thd->secondary_engine_statement_context();
  if (shannon_statement_context == nullptr) {
    SetSecondaryEngineOffloadFailedReason(thd, "missing Rapid statement context");
    return true;
  }

  auto primary_plan_info = shannon_statement_context->get_cached_primary_plan_info();
  ut_a(primary_plan_info);

  // 2: to check whether the shannon_pop_data_sz has too many data to populate.
  uint64 too_much_pop_threshold = static_cast<uint64_t>(ShannonBase::SHANNON_TO_MUCH_POP_THRESHOLD_RATIO *
                                                        ShannonBase::shannon_rpd_engine_cfg.pop_buff_sz_max);
  if (ShannonBase::Populate::shannon_pop_data_sz > too_much_pop_threshold) {
    SetSecondaryEngineOffloadFailedReason(thd, "too much changes need to populate");
    return true;
  }

  // 3: checks dict encoding projection, and varlen project size, etc.
  if (ShannonBase::ML::Query_arbitrator::check_dict_encoding_projection(thd)) {
    SetSecondaryEngineOffloadFailedReason(thd, "dict encoding, varlen pj size, etc. not supported");
    return true;
  }
  return false;
}

static bool PrepareSecondaryEngine(THD *thd, LEX *lex) {
  DBUG_EXECUTE_IF("secondary_engine_rapid_prepare_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
    return true;
  });

  auto context = new (thd->mem_root) ShannonBase::Rapid_execution_context;
  if (context == nullptr) return true;
  lex->set_secondary_engine_execution_context(context);

  // The hypergraph optimizer does not do const tables, nor does it evaluate subqueries during optimization.
  auto options = (thd->lex->using_hypergraph_optimizer())
                     ? OPTION_NO_CONST_TABLES | OPTION_NO_SUBQUERY_DURING_OPTIMIZATION
                     : OPTION_NO_SUBQUERY_DURING_OPTIMIZATION;
  lex->add_statement_options(options);
  return RapidPrepareEstimateQueryCosts(thd, lex);
}

static bool RapidCachePrimaryInfoAtPrimaryTentativelyStep(THD *thd) {
  ut_a(thd->secondary_engine_optimization() == Secondary_engine_optimization::PRIMARY_TENTATIVELY);
  if (unlikely(thd->secondary_engine_statement_context() == nullptr)) {
    /* Prepare this query's specific statment context */
    std::unique_ptr<Secondary_engine_statement_context> ctx = std::make_unique<ShannonBase::Rapid_statement_context>();
    thd->set_secondary_engine_statement_context(std::move(ctx));
  }

  auto shannon_statement_context = thd->secondary_engine_statement_context();
  Query_expression *const unit = thd->lex->unit;
  shannon_statement_context->cache_primary_plan_info(thd, unit->first_query_block()->join);
  return false;
}

bool SecondaryEnginePrePrepareHook(THD *thd) {
  RapidCachePrimaryInfoAtPrimaryTentativelyStep(thd);

  DBUG_EXECUTE_IF("secondary_engine_prepare_to_rpd", { return true; });

  if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_FORCED) return true;

  // If dynamic offload is disabled or query is too fast, use standard cost threshold classifier
  if (unlikely(!ShannonBase::shannon_rpd_engine_cfg.dynamic_offloads || is_very_fast_query(thd)))
    return ShannonBase::ML::Query_arbitrator::standard_cost_threshold_classifier(thd);

  // dynamic_offloads is enabled and query is not very fast Determine which classifier to use based on populator state
  bool use_decision_tree = !ShannonBase::Populate::Populator::active() ||
                           (ShannonBase::Populate::Populator::active() && ShannonBase::Populate::pop_buff_empty());
  return use_decision_tree ? ShannonBase::ML::Query_arbitrator::decision_tree_classifier(thd)
                           : ShannonBase::ML::Query_arbitrator::dynamic_feature_normalization(thd);
}

static bool RapidOptimize(ShannonBase::Optimizer::OptimizeContext *context, THD *thd, LEX *lex) {
  if (likely(thd->variables.use_secondary_engine == SECONDARY_ENGINE_OFF)) {
    SetSecondaryEngineOffloadFailedReason(thd, "RapidOptimize, set use_secondary_engine to false");
    return true;
  }

  const auto too_much_pop_threshold = static_cast<ulonglong>(ShannonBase::SHANNON_TO_MUCH_POP_THRESHOLD_RATIO *
                                                             ShannonBase::shannon_rpd_engine_cfg.pop_buff_sz_max);
  const bool too_much_change_lag =
      ShannonBase::Populate::pop_buff_table_count() > ShannonBase::SHANNON_POP_BUFF_THRESHOLD_COUNT ||
      ShannonBase::Populate::shannon_pop_data_sz > too_much_pop_threshold;
  if (unlikely(too_much_change_lag)) {
    SetSecondaryEngineOffloadFailedReason(thd, "RapidOptimize, the change propagation lag is too much");
    return true;
  }

  auto *unit = lex->unit;
  if (unit && !unit->is_optimized() && unit->optimize(thd, nullptr, true, true)) return true;

  JOIN *join = unit->first_query_block()->join;
  if (!join) return false;

  ShannonBase::Optimizer::Optimizer rpd_optimizer;
  auto plan = rpd_optimizer.Optimize(context, thd, join);
  if (!plan) return false;

  AccessPath *candidate_root_path = plan->ToAccessPath(thd);
  if (thd->is_error()) return true;

  if (candidate_root_path == nullptr) {
    DBUG_PRINT("rapid_optimizer", ("Rapid ToAccessPath failed; keeping original plan"));
    return false;
  }

  if (candidate_root_path == unit->root_access_path()) return false;

  auto candidate_root_iter = ShannonBase::Optimizer::PathGenerator::PathGenerator::CreateIteratorFromAccessPath(
      thd, context, candidate_root_path, join,
      /*eligible_for_batch_mode=*/true);

  if (!candidate_root_iter) {
    if (thd->is_error()) return true;

    DBUG_PRINT("rapid_optimizer", ("Rapid iterator construction failed; keeping original plan"));
    return false;
  }

  auto old_root_iter = unit->release_root_iterator();
  unit->root_access_path() = candidate_root_path;
  unit->set_root_iterator(candidate_root_iter);

  old_root_iter.reset();
  return false;
}

static bool OptimizeSecondaryEngine(THD *thd [[maybe_unused]], LEX *lex) {
  // The context should have been set by PrepareSecondaryEngine.
  ut_a(lex->secondary_engine_execution_context() != nullptr);

  DBUG_EXECUTE_IF("secondary_engine_rapid_optimize_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
    return true;
  });

  DEBUG_SYNC(thd, "before_rapid_optimize");

  auto optimizer_context = std::make_unique<ShannonBase::Optimizer::OptimizeContext>();
  optimizer_context->Rpd_statistics = ShannonBase::Optimizer::StatisticsFactory::get_statistics();
  return RapidOptimize(optimizer_context.get(), thd, lex);
}

static bool CompareJoinCost(THD *thd, const JOIN &join, double optimizer_cost, bool *use_best_so_far, bool *cheaper,
                            double *secondary_engine_cost) {
  *use_best_so_far = false;

  DBUG_EXECUTE_IF("secondary_engine_rapid_compare_cost_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
    return true;
  });

  DBUG_EXECUTE_IF("secondary_engine_rapid_choose_first_plan", {
    *use_best_so_far = true;
    *cheaper = true;
    *secondary_engine_cost = optimizer_cost;
  });

  ShannonBase::Rapid_execution_context *rapid_ctx =
      down_cast<ShannonBase::Rapid_execution_context *>(thd->lex->secondary_engine_execution_context());

  // Just use the cost calculated by the optimizer by default.
  *secondary_engine_cost = optimizer_cost;

  // This debug flag makes the cost function prefer orders where a table with
  // the alias "X" is closer to the beginning.
  DBUG_EXECUTE_IF("secondary_engine_rapid_change_join_order", {
    double cost = join.tables;
    for (size_t i = 0; i < join.tables; ++i) {
      const Table_ref *ref = join.positions[i].table->table_ref;
      if (std::string(ref->alias) == "X") {
        cost += i;
      }
    }
    *secondary_engine_cost = cost;
  });

  // Hypergraph mode: Cost has already been set by ModifyAccessPathCost during enumeration
  if (thd->lex->using_hypergraph_optimizer()) {
    AccessPath *root = join.query_block->join->root_access_path();
    *secondary_engine_cost = (root && root->cost() > 0.0) ? root->cost() : optimizer_cost;
  } else {  // Greedy mode.
    *secondary_engine_cost = optimizer_cost;
    bool estimation_error =
        ShannonBase::Optimizer::Optimizer::RapidEstimateJoinCostHGO(thd, join, secondary_engine_cost);
    if (estimation_error) {
      SetSecondaryEngineOffloadFailedReason(thd, "Calc Rapid Estimated Join Cost failed");
      return true;
    }
  }

  double primary_best = (join.best_read > 0.0) ? join.best_read : optimizer_cost;
  *cheaper = rapid_ctx->BestPlanSoFar(join, *secondary_engine_cost);
  *use_best_so_far = (*secondary_engine_cost < primary_best);
  return false;
}

/**
 * Hook for modifying the cost of partial plans in the Hypergraph optimizer
 * Invocation timing: When Hypergraph enumerates each AccessPath (including partial plans)
 * Goal: Replace MySQL's cost based on InnoDB statistics with IMCS precise cost
 *
 * Hypergraph invocation timing (MySQL internal inference): Called each time when costing an AccessPath node
 * path->cost has been set by MySQL, hook can modify it
 *
 * false = Accept path (can modify path->cost/num_output_rows)
 * true = Reject path (permanently remove from candidate set, may eventually lead to offload failure)
 *
 * @param thd Current thread
 * @param hypergraph Hypergraph structure (contains predicates, nodes, etc.)
 * @param path Currently proposed AccessPath (cost can be modified)
 * @return false=accept, true=reject
 */
static bool ModifyAccessPathCost(THD *thd, const JoinHypergraph &hypergraph, AccessPath *path) {
  ut_a(thd->lex->using_hypergraph_optimizer());
  ut_a(!thd->is_error());
  ut_a(hypergraph.query_block()->join == hypergraph.join());
  ut_a(path != nullptr);

  // fast check
  switch (path->type) {
    case AccessPath::ZERO_ROWS:
    case AccessPath::ZERO_ROWS_AGGREGATED:
    case AccessPath::FAKE_SINGLE_ROW:
    case AccessPath::TABLE_VALUE_CONSTRUCTOR:
      path->set_cost(0.0);
      path->set_cost_before_filter(0.0);
      path->set_init_cost(0.0);
      path->set_init_once_cost(0.0);
      return false;
    default:
      break;
  }

  auto *rapid_ctx = down_cast<ShannonBase::Rapid_execution_context *>(thd->lex->secondary_engine_execution_context());
  if (!rapid_ctx) return false;

  bool rejected = false;
  switch (path->type) {
    case AccessPath::TABLE_SCAN:
      rejected = ShannonBase::Optimizer::ModifyTableScanCost(thd, hypergraph, path, rapid_ctx);
      break;
    case AccessPath::INDEX_SCAN:
    case AccessPath::REF:
    case AccessPath::EQ_REF:
    case AccessPath::INDEX_RANGE_SCAN:
      rejected = ShannonBase::Optimizer::ModifyIndexScanCost(thd, hypergraph, path, rapid_ctx);
      break;
    case AccessPath::FILTER:
      rejected = ShannonBase::Optimizer::ModifyFilterCost(thd, hypergraph, path, rapid_ctx);
      break;
    case AccessPath::HASH_JOIN:
      rejected = ShannonBase::Optimizer::ModifyHashJoinCost(thd, hypergraph, path, rapid_ctx);
      break;
    case AccessPath::NESTED_LOOP_JOIN:
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
      rejected = ShannonBase::Optimizer::ModifyNestedLoopJoinCost(thd, hypergraph, path, rapid_ctx);
      break;
    case AccessPath::AGGREGATE:
      rejected = ShannonBase::Optimizer::ModifyAggregateCost(thd, hypergraph, path, rapid_ctx);
      break;
    case AccessPath::SORT:
      rejected = ShannonBase::Optimizer::ModifySortCost(thd, hypergraph, path, rapid_ctx);
      break;
    case AccessPath::LIMIT_OFFSET:
      rejected = ShannonBase::Optimizer::ModifyLimitCost(thd, hypergraph, path, rapid_ctx);
      break;
    case AccessPath::MATERIALIZE:
      rejected = ShannonBase::Optimizer::ModifyMaterializeCost(thd, hypergraph, path, rapid_ctx);
      break;
    case AccessPath::SAMPLE_SCAN:
      ::SetSecondaryEngineOffloadFailedReason(thd, "TABLESAMPLE is not supported in the secondary engine", false);
      return true;
    default:
      return false;  // keep MySQL cost
  }
  if (rejected) return true;

  // Every secondary-engine cost callback must preserve the AccessPath invariants checked by the hypergraph optimizer.
  if (path->cost_before_filter() == kUnknownCost || path->cost_before_filter() > path->cost())
    path->set_cost_before_filter(path->cost());
  if (path->init_cost() == kUnknownCost || path->init_cost() > path->cost()) path->set_init_cost(path->cost());
  if (!IsEmpty(path->filter_predicates) && (path->num_output_rows_before_filter == kUnknownRowCount ||
                                            path->num_output_rows_before_filter < path->num_output_rows()))
    path->num_output_rows_before_filter = path->num_output_rows();
  return false;
}

static handler *rapid_create_handler(handlerton *hton, TABLE_SHARE *table_share, bool partition, MEM_ROOT *mem_root) {
  if (partition) {
    ShannonBase::ha_rapidpart *file = new (mem_root) ShannonBase::ha_rapidpart(hton, table_share);
    if (file && file->init_partitioning(mem_root)) {
      ::destroy_at(file);
      return (nullptr);
    }
    return (file);
  }
  return new (mem_root) ShannonBase::ha_rapid(hton, table_share);
}

static void rapid_pre_dd_shutdown(handlerton *) {
  // Release ONNX Runtime resources (thread pool, session, environment).
  ShannonBase::ML::Query_arbitrator::shutdown();

  auto *mgr = ShannonBase::ML::EmbeddingManager::instance();
  if ((!mgr || !mgr->initialized())) return;

  ShannonBase::ML::EmbeddingManager::shutdown();
  DBUG_PRINT("ml", ("ML EmbeddingManager: shannon_ml_pre_dd_shutdown — all threads stopped."));
}

/** Shut down rapid  before the InnoDB has been shut down.
@see innodb_pre_dd_shutdown()
@retval 0 always */
static int rapid_shutdown(handlerton *, ha_panic_function) {
  DBUG_TRACE;

  // Release ONNX Runtime resources (thread pool, session, environment).
  ShannonBase::ML::Query_arbitrator::shutdown();

  // embedding worker thread shut down. Idempotent operation.
  ShannonBase::ML::EmbeddingManager::shutdown();

  // background worker pool (GC, compaction, stats).
  ShannonBase::Imcs::BkgWorkerPool::shutdown_all(true);

  // recovery worker
  ShannonBase::Recovery::rapid_recovery_shutdown();

  // self-loader worker
  if (ShannonBase::shannon_self_load_mgr_inst && ShannonBase::shannon_self_load_mgr_inst->initialized())
    ShannonBase::shannon_self_load_mgr_inst->shutdown();

  // change populator
  ShannonBase::Populate::Populator::shutdown();
  return ShannonBase::SHANNON_SUCCESS;
}
/**
 * Rapid engine system variables to control the behavior of Rapid Engine, such as the max memory used, etc.
 */
static SHOW_VAR rapid_status_variables[] = {
    /*the max memory used for rapid.*/
    {"rapid_memory_size_max", (char *)&ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_mb, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    /*the max size of pop buffer size.*/
    {"rapid_pop_buffer_size_max", (char *)&ShannonBase::shannon_rpd_engine_cfg.pop_buff_sz_max, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    /*the max row number of used to enable parallel load for secondary_load*/
    {"rapid_parallel_load_max", (char *)&ShannonBase::shannon_rpd_engine_cfg.para_load_threshold, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    /*the max part table number of used to enable parallel load for secondary_load*/
    {"rapid_parallel_part_load_threshold", (char *)&ShannonBase::shannon_rpd_engine_cfg.para_parttb_load_threshold,
     SHOW_LONG, SHOW_SCOPE_GLOBAL},
    /*the mode to aysnc the changes to rapid*/
    {"rapid_propagation_mode", (char *)&ShannonBase::shannon_rpd_engine_cfg.propagate_mode, SHOW_CHAR,
     SHOW_SCOPE_GLOBAL},
    /*the max column number of used to aysnc reading or parsing log*/
    {"rapid_async_column_threshold", (char *)&ShannonBase::shannon_rpd_engine_cfg.async_column_threshold, SHOW_INT,
     SHOW_SCOPE_GLOBAL},
    /*to enable dynamic off load or disable*/
    {"rapid_use_dynamic_offload", (char *)&ShannonBase::shannon_rpd_engine_cfg.dynamic_offloads, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    /*to enable self load or disable*/
    {"rapid_self_load_enabled", (char *)&ShannonBase::shannon_rpd_engine_cfg.self_load_enabled, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    /*the interval value of self load in second*/
    {"rapid_self_load_interval_seconds", (char *)&ShannonBase::shannon_rpd_engine_cfg.self_load_interval_sec, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    /*to skip the quiet check or not*/
    {"rapid_self_load_skip_quiet_check", (char *)&ShannonBase::shannon_rpd_engine_cfg.self_load_skip_quiet_check,
     SHOW_BOOL, SHOW_SCOPE_GLOBAL},
    /*the value of fill percentage of main memory*/
    {"rapid_self_load_base_relation_fill_percentage",
     (char *)&ShannonBase::shannon_rpd_engine_cfg.self_load_base_relation_fill_percentage, SHOW_INT, SHOW_SCOPE_GLOBAL},
    {"rapid_max_purger_timeout", (char *)&ShannonBase::shannon_rpd_engine_cfg.gc_interval_seconds, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"rapid_purge_batch_size", (char *)&ShannonBase::shannon_rpd_engine_cfg.gc_batch_size, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"rapid_min_versions_for_purge", (char *)&ShannonBase::shannon_rpd_engine_cfg.gc_min_version, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"rapid_purge_efficiency_threshold", (char *)&ShannonBase::shannon_rpd_engine_cfg.gc_version_ratio_threshold,
     SHOW_DOUBLE, SHOW_SCOPE_GLOBAL},
    /*the interval scn of GC*/
    {"rapid_gc_interval_scn", (char *)&ShannonBase::shannon_rpd_engine_cfg.gc_interval_scn, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"rapid_reload_on_restart", (char *)&ShannonBase::shannon_rpd_engine_cfg.reload_on_restart, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {"rapid_schema_embedding", (char *)&ShannonBase::shannon_rpd_engine_cfg.enable_schema_embedding, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

/** Callback function for accessing the Rapid variables from MySQL:  SHOW
 * VARIABLES. */
static int show_rapid_vars(THD *, SHOW_VAR *var, char *) {
  // gets the latest variables of shannonbase rapid.
  var->type = SHOW_ARRAY;
  var->value = (char *)&rapid_status_variables;
  var->scope = SHOW_SCOPE_GLOBAL;

  return (ShannonBase::SHANNON_SUCCESS);
}

// These globals are refreshed by refresh_rapid_export_vars() and exposed
// as SHOW STATUS variables so that Prometheus / mysqld_exporter can scrape
// them.  All names are prefixed with "rapid_" for easy identification.
struct RapidExportVars {
  /* Memory Pool */
  ulonglong mempool_capacity_bytes{0};
  ulonglong mempool_allocated_bytes{0};
  ulonglong mempool_used_bytes{0};
  ulonglong mempool_peak_usage_bytes{0};
  double mempool_usage_percentage{0.0};
  ulonglong mempool_alloc_count{0};
  ulonglong mempool_dealloc_count{0};
  ulonglong mempool_failed_allocs{0};
  ulonglong mempool_expansion_count{0};
  ulonglong mempool_defrag_count{0};

  /* IMCS */
  ulonglong loaded_tables{0};
  ulonglong loaded_part_tables{0};
  ulonglong total_imcus{0};
  ulonglong total_cus{0};
  ulonglong total_rows{0};
  ulonglong total_physical_rows{0};
  ulonglong estimated_data_size_bytes{0};
  ulonglong estimated_compressed_size_bytes{0};

  /* Population / Propagation */
  ulonglong pop_thread_running{0};
  ulonglong pop_loop_counter{0};
  ulonglong pop_data_remaining_bytes{0};
  ulonglong pop_buffer_tables{0};
  ulonglong pop_tables_in_progress{0};
  ulonglong pop_worker_threads{0};
  ulonglong pop_worker_pending_bytes{0};

  /* Background Worker Pool */
  ulonglong bg_queue_size{0};
  ulonglong bg_active_workers{0};
  ulonglong bg_total_workers{0};
  ulonglong bg_concurrent_gc{0};
  ulonglong bg_concurrent_compact{0};
  ulonglong bg_concurrent_stats{0};
  ulonglong bg_tasks_submitted{0};
  ulonglong bg_tasks_completed{0};
  ulonglong bg_tasks_failed{0};
  ulonglong bg_tasks_cancelled{0};
  ulonglong bg_tasks_retried{0};

  /* GC */
  ulonglong gc_total_runs{0};
  ulonglong gc_total_purged_rows{0};
  ulonglong gc_total_purged_versions{0};
  ulonglong gc_last_run_scn{0};
  ulonglong gc_last_run_duration_us{0};

  /* Compaction */
  ulonglong compact_total_runs{0};
  ulonglong compact_total_merged_rows{0};
  ulonglong compact_last_run_duration_us{0};

  /* Query Execution */
  ulonglong query_scans_total{0};
  ulonglong query_index_lookups_total{0};
  ulonglong query_rows_read_total{0};
  ulonglong query_offload_total{0};
  ulonglong query_offload_fallback_total{0};

  /* Transactions */
  ulonglong active_transactions{0};
  ulonglong transaction_commits_total{0};
  ulonglong transaction_rollbacks_total{0};
};

static RapidExportVars rapid_export_vars;

/** Refresh all rapid_export_vars from the live RapidMonitor::Metrics. */
static void refresh_rapid_export_vars() {
  ShannonBase::RapidMonitor::Metrics m;
  ShannonBase::RapidMonitor::collect_rapid_monitor_metrics(m);

  /* Memory Pool */
  rapid_export_vars.mempool_capacity_bytes = m.mempool_capacity_bytes;
  rapid_export_vars.mempool_allocated_bytes = m.mempool_allocated_bytes;
  rapid_export_vars.mempool_used_bytes = m.mempool_used_bytes;
  rapid_export_vars.mempool_peak_usage_bytes = m.mempool_peak_usage_bytes;
  rapid_export_vars.mempool_usage_percentage = m.mempool_usage_percentage;
  rapid_export_vars.mempool_alloc_count = m.mempool_alloc_count;
  rapid_export_vars.mempool_dealloc_count = m.mempool_dealloc_count;
  rapid_export_vars.mempool_failed_allocs = m.mempool_failed_allocs;
  rapid_export_vars.mempool_expansion_count = m.mempool_expansion_count;
  rapid_export_vars.mempool_defrag_count = m.mempool_defrag_count;

  /* IMCS */
  rapid_export_vars.loaded_tables = m.loaded_tables;
  rapid_export_vars.loaded_part_tables = m.loaded_part_tables;
  rapid_export_vars.total_imcus = m.total_imcus;
  rapid_export_vars.total_cus = m.total_cus;
  rapid_export_vars.total_rows = m.total_rows;
  rapid_export_vars.total_physical_rows = m.total_physical_rows;
  rapid_export_vars.estimated_data_size_bytes = m.estimated_data_size_bytes;
  rapid_export_vars.estimated_compressed_size_bytes = m.estimated_compressed_size_bytes;

  /* Population */
  rapid_export_vars.pop_thread_running = m.rapid_pop_thread_running ? 1 : 0;
  rapid_export_vars.pop_loop_counter = m.rapid_pop_loop_counter;
  rapid_export_vars.pop_data_remaining_bytes = m.rapid_pop_data_sz;
  rapid_export_vars.pop_buffer_tables = m.total_buffer_tables;
  rapid_export_vars.pop_tables_in_progress = m.tables_in_progress;
  rapid_export_vars.pop_worker_threads = m.total_worker_threads;
  rapid_export_vars.pop_worker_pending_bytes = m.worker_pending_bytes;

  /* Background Worker Pool */
  rapid_export_vars.bg_queue_size = m.bg_pool_queue_size;
  rapid_export_vars.bg_active_workers = m.bg_active_workers;
  rapid_export_vars.bg_total_workers = m.bg_total_workers;
  rapid_export_vars.bg_concurrent_gc = m.bg_concurrent_gc;
  rapid_export_vars.bg_concurrent_compact = m.bg_concurrent_compact;
  rapid_export_vars.bg_concurrent_stats = m.bg_concurrent_stats;
  rapid_export_vars.bg_tasks_submitted = m.bg_tasks_submitted;
  rapid_export_vars.bg_tasks_completed = m.bg_tasks_completed;
  rapid_export_vars.bg_tasks_failed = m.bg_tasks_failed;
  rapid_export_vars.bg_tasks_cancelled = m.bg_tasks_cancelled;
  rapid_export_vars.bg_tasks_retried = m.bg_tasks_retried;

  /* GC */
  rapid_export_vars.gc_total_runs = m.gc_total_runs;
  rapid_export_vars.gc_total_purged_rows = m.gc_total_purged_rows;
  rapid_export_vars.gc_total_purged_versions = m.gc_total_purged_versions;
  rapid_export_vars.gc_last_run_scn = m.gc_last_run_scn;
  rapid_export_vars.gc_last_run_duration_us = m.gc_last_run_duration_us;

  /* Compaction */
  rapid_export_vars.compact_total_runs = m.compact_total_runs;
  rapid_export_vars.compact_total_merged_rows = m.compact_total_merged_rows;
  rapid_export_vars.compact_last_run_duration_us = m.compact_last_run_duration_us;

  /* Query Execution */
  rapid_export_vars.query_scans_total = m.query_scans_total;
  rapid_export_vars.query_index_lookups_total = m.query_index_lookups_total;
  rapid_export_vars.query_rows_read_total = m.query_rows_read_total;
  rapid_export_vars.query_offload_total = m.query_offload_total;
  rapid_export_vars.query_offload_fallback_total = m.query_offload_fallback_total;

  /* Transactions */
  rapid_export_vars.active_transactions = m.active_transactions;
  rapid_export_vars.transaction_commits_total = m.transaction_commits_total;
  rapid_export_vars.transaction_rollbacks_total = m.transaction_rollbacks_total;
}

/* SHOW_FUNC callbacks for individual metrics that need a function pointer.
   Each simply refers back to the appropriate rapid_export_vars field. */

#define RAPID_STATUS_FUNC(name, field)                         \
  static int show_rapid_##name(THD *, SHOW_VAR *var, char *) { \
    var->type = SHOW_LONGLONG;                                 \
    var->value = (char *)&rapid_export_vars.field;             \
    var->scope = SHOW_SCOPE_GLOBAL;                            \
    return 0;                                                  \
  }

RAPID_STATUS_FUNC(mempool_capacity_bytes, mempool_capacity_bytes)
RAPID_STATUS_FUNC(mempool_allocated_bytes, mempool_allocated_bytes)
RAPID_STATUS_FUNC(mempool_used_bytes, mempool_used_bytes)
RAPID_STATUS_FUNC(mempool_peak_usage_bytes, mempool_peak_usage_bytes)
RAPID_STATUS_FUNC(mempool_alloc_count, mempool_alloc_count)
RAPID_STATUS_FUNC(mempool_dealloc_count, mempool_dealloc_count)
RAPID_STATUS_FUNC(mempool_failed_allocs, mempool_failed_allocs)
RAPID_STATUS_FUNC(mempool_expansion_count, mempool_expansion_count)
RAPID_STATUS_FUNC(mempool_defrag_count, mempool_defrag_count)
RAPID_STATUS_FUNC(loaded_tables, loaded_tables)
RAPID_STATUS_FUNC(loaded_part_tables, loaded_part_tables)
RAPID_STATUS_FUNC(total_imcus, total_imcus)
RAPID_STATUS_FUNC(total_cus, total_cus)
RAPID_STATUS_FUNC(total_rows, total_rows)
RAPID_STATUS_FUNC(total_physical_rows, total_physical_rows)
RAPID_STATUS_FUNC(estimated_data_size_bytes, estimated_data_size_bytes)
RAPID_STATUS_FUNC(estimated_compressed_size_bytes, estimated_compressed_size_bytes)
RAPID_STATUS_FUNC(pop_thread_running, pop_thread_running)
RAPID_STATUS_FUNC(pop_loop_counter, pop_loop_counter)
RAPID_STATUS_FUNC(pop_data_remaining_bytes, pop_data_remaining_bytes)
RAPID_STATUS_FUNC(pop_buffer_tables, pop_buffer_tables)
RAPID_STATUS_FUNC(pop_tables_in_progress, pop_tables_in_progress)
RAPID_STATUS_FUNC(pop_worker_threads, pop_worker_threads)
RAPID_STATUS_FUNC(pop_worker_pending_bytes, pop_worker_pending_bytes)
RAPID_STATUS_FUNC(bg_queue_size, bg_queue_size)
RAPID_STATUS_FUNC(bg_active_workers, bg_active_workers)
RAPID_STATUS_FUNC(bg_total_workers, bg_total_workers)
RAPID_STATUS_FUNC(bg_concurrent_gc, bg_concurrent_gc)
RAPID_STATUS_FUNC(bg_concurrent_compact, bg_concurrent_compact)
RAPID_STATUS_FUNC(bg_concurrent_stats, bg_concurrent_stats)
RAPID_STATUS_FUNC(bg_tasks_submitted, bg_tasks_submitted)
RAPID_STATUS_FUNC(bg_tasks_completed, bg_tasks_completed)
RAPID_STATUS_FUNC(bg_tasks_failed, bg_tasks_failed)
RAPID_STATUS_FUNC(bg_tasks_cancelled, bg_tasks_cancelled)
RAPID_STATUS_FUNC(bg_tasks_retried, bg_tasks_retried)
RAPID_STATUS_FUNC(gc_total_runs, gc_total_runs)
RAPID_STATUS_FUNC(gc_total_purged_rows, gc_total_purged_rows)
RAPID_STATUS_FUNC(gc_total_purged_versions, gc_total_purged_versions)
RAPID_STATUS_FUNC(gc_last_run_scn, gc_last_run_scn)
RAPID_STATUS_FUNC(gc_last_run_duration_us, gc_last_run_duration_us)
RAPID_STATUS_FUNC(compact_total_runs, compact_total_runs)
RAPID_STATUS_FUNC(compact_total_merged_rows, compact_total_merged_rows)
RAPID_STATUS_FUNC(compact_last_run_duration_us, compact_last_run_duration_us)
RAPID_STATUS_FUNC(query_scans_total, query_scans_total)
RAPID_STATUS_FUNC(query_index_lookups_total, query_index_lookups_total)
RAPID_STATUS_FUNC(query_rows_read_total, query_rows_read_total)
RAPID_STATUS_FUNC(query_offload_total, query_offload_total)
RAPID_STATUS_FUNC(query_offload_fallback_total, query_offload_fallback_total)
RAPID_STATUS_FUNC(active_transactions, active_transactions)
RAPID_STATUS_FUNC(transaction_commits_total, transaction_commits_total)
RAPID_STATUS_FUNC(transaction_rollbacks_total, transaction_rollbacks_total)

static SHOW_VAR rapid_runtime_status_variables[] = {
    /* Memory Pool */
    {"rapid_mempool_capacity_bytes", (char *)&show_rapid_mempool_capacity_bytes, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_mempool_allocated_bytes", (char *)&show_rapid_mempool_allocated_bytes, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_mempool_used_bytes", (char *)&show_rapid_mempool_used_bytes, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_mempool_peak_usage_bytes", (char *)&show_rapid_mempool_peak_usage_bytes, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_mempool_alloc_count", (char *)&show_rapid_mempool_alloc_count, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_mempool_dealloc_count", (char *)&show_rapid_mempool_dealloc_count, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_mempool_failed_allocs", (char *)&show_rapid_mempool_failed_allocs, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_mempool_expansion_count", (char *)&show_rapid_mempool_expansion_count, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_mempool_defrag_count", (char *)&show_rapid_mempool_defrag_count, SHOW_FUNC, SHOW_SCOPE_GLOBAL},

    /* IMCS */
    {"rapid_loaded_tables", (char *)&show_rapid_loaded_tables, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_loaded_part_tables", (char *)&show_rapid_loaded_part_tables, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_total_imcus", (char *)&show_rapid_total_imcus, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_total_cus", (char *)&show_rapid_total_cus, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_total_rows", (char *)&show_rapid_total_rows, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_total_physical_rows", (char *)&show_rapid_total_physical_rows, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_estimated_data_size_bytes", (char *)&show_rapid_estimated_data_size_bytes, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_estimated_compressed_size_bytes", (char *)&show_rapid_estimated_compressed_size_bytes, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},

    /* Population / Propagation */
    {"rapid_pop_thread_running", (char *)&show_rapid_pop_thread_running, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_pop_loop_counter", (char *)&show_rapid_pop_loop_counter, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_pop_data_remaining_bytes", (char *)&show_rapid_pop_data_remaining_bytes, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_pop_buffer_tables", (char *)&show_rapid_pop_buffer_tables, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_pop_tables_in_progress", (char *)&show_rapid_pop_tables_in_progress, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_pop_worker_threads", (char *)&show_rapid_pop_worker_threads, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_pop_worker_pending_bytes", (char *)&show_rapid_pop_worker_pending_bytes, SHOW_FUNC, SHOW_SCOPE_GLOBAL},

    /* Background Worker Pool */
    {"rapid_bg_queue_size", (char *)&show_rapid_bg_queue_size, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_bg_active_workers", (char *)&show_rapid_bg_active_workers, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_bg_total_workers", (char *)&show_rapid_bg_total_workers, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_bg_concurrent_gc", (char *)&show_rapid_bg_concurrent_gc, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_bg_concurrent_compact", (char *)&show_rapid_bg_concurrent_compact, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_bg_concurrent_stats", (char *)&show_rapid_bg_concurrent_stats, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_bg_tasks_submitted", (char *)&show_rapid_bg_tasks_submitted, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_bg_tasks_completed", (char *)&show_rapid_bg_tasks_completed, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_bg_tasks_failed", (char *)&show_rapid_bg_tasks_failed, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_bg_tasks_cancelled", (char *)&show_rapid_bg_tasks_cancelled, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_bg_tasks_retried", (char *)&show_rapid_bg_tasks_retried, SHOW_FUNC, SHOW_SCOPE_GLOBAL},

    /* Garbage Collection */
    {"rapid_gc_total_runs", (char *)&show_rapid_gc_total_runs, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_gc_total_purged_rows", (char *)&show_rapid_gc_total_purged_rows, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_gc_total_purged_versions", (char *)&show_rapid_gc_total_purged_versions, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_gc_last_run_scn", (char *)&show_rapid_gc_last_run_scn, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_gc_last_run_duration_us", (char *)&show_rapid_gc_last_run_duration_us, SHOW_FUNC, SHOW_SCOPE_GLOBAL},

    /* Compaction */
    {"rapid_compact_total_runs", (char *)&show_rapid_compact_total_runs, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_compact_total_merged_rows", (char *)&show_rapid_compact_total_merged_rows, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_compact_last_run_duration_us", (char *)&show_rapid_compact_last_run_duration_us, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},

    /* Query Execution */
    {"rapid_query_scans_total", (char *)&show_rapid_query_scans_total, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_query_index_lookups_total", (char *)&show_rapid_query_index_lookups_total, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_query_rows_read_total", (char *)&show_rapid_query_rows_read_total, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_query_offload_total", (char *)&show_rapid_query_offload_total, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_query_offload_fallback_total", (char *)&show_rapid_query_offload_fallback_total, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},

    /* Transactions */
    {"rapid_active_transactions", (char *)&show_rapid_active_transactions, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_transaction_commits_total", (char *)&show_rapid_transaction_commits_total, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"rapid_transaction_rollbacks_total", (char *)&show_rapid_transaction_rollbacks_total, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},

    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

#undef RAPID_STATUS_FUNC

/** SHOW_FUNC callback: refresh all runtime vars and expose the array. */
static int show_rapid_runtime_status(THD *, SHOW_VAR *var, char *) {
  refresh_rapid_export_vars();
  var->type = SHOW_ARRAY;
  var->value = (char *)&rapid_runtime_status_variables;
  var->scope = SHOW_SCOPE_GLOBAL;
  return 0;
}

/** Validate passed-in "value" is a valid monitor counter name.
 This function is registered as a callback with MySQL.
 @return 0 for valid name */
static int rpd_mem_size_max_validate(THD *,                          /*!< in: thread handle */
                                     SYS_VAR *,                      /*!< in: pointer to system
                                                                                     variable */
                                     void *save,                     /*!< out: immediate result
                                                                     for update function */
                                     struct st_mysql_value *value) { /*!< in: incoming string */

  long long input_val;
  if (value->val_int(value, &input_val)) return HA_ERR_GENERIC;

  // Range check entirely in long long — no truncating casts
  constexpr long long min_val = 1;
  constexpr long long max_val = static_cast<long long>(ShannonBase::SHANNON_DEFAULT_MEMRORY_SIZE);
  if (input_val < min_val || input_val > max_val) return HA_ERR_GENERIC;

  *static_cast<unsigned long *>(save) = static_cast<unsigned long>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_memory_size_max.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_mem_size_max_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  const unsigned long new_size = *static_cast<const unsigned long *>(save);
  if (new_size == ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_mb) return;

  if (ShannonBase::Populate::Populator::active() || ShannonBase::shannon_loaded_tables->size()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Tables have been loaded, cannot change the rapid IMCS memory params,"
             "must unload all loaded tables");
    return;
  }

  const size_t pool_size = static_cast<size_t>(new_size);
  ShannonBase::Utils::MemoryPool::Config new_config(pool_size);
  ShannonBase::shannon_rpd_memory_pool->reinitialize(new_config);
  ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_mb = new_size;
  *static_cast<unsigned long *>(var_ptr) = new_size;
}

/** Validate passed-in "value" is a valid monitor counter name.
 This function is registered as a callback with MySQL.
 @return 0 for valid name */
static int rpd_pop_buff_size_max_validate(THD *,                          /*!< in: thread handle */
                                          SYS_VAR *,                      /*!< in: pointer to system
                                                                                          variable */
                                          void *save,                     /*!< out: immediate result
                                                                          for update function */
                                          struct st_mysql_value *value) { /*!< in: incoming string */
  long long input_val;

  if (ShannonBase::Populate::Populator::active() || ShannonBase::shannon_loaded_tables->size()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Tables have been loaded, cannot change the rapid params");
    return 1;
  }

  if (value->val_int(value, &input_val)) return 1;
  if (input_val < 1 || (uint)input_val > ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE) return 1;

  *static_cast<int *>(save) = static_cast<int>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_pop_buffer_size_max.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_pop_buff_size_max_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  if (*static_cast<int *>(var_ptr) == *static_cast<const int *>(save)) return;

  *static_cast<int *>(var_ptr) = *static_cast<const int *>(save);
  ShannonBase::shannon_rpd_engine_cfg.pop_buff_sz_max = *static_cast<const int *>(save);
}

/** Validate passed-in "value" is a valid monitor counter name.
 This function is registered as a callback with MySQL.
 @return 0 for valid name */
static int rpd_para_load_threshold_validate(THD *,                          /*!< in: thread handle */
                                            SYS_VAR *,                      /*!< in: pointer to system
                                                                                            variable */
                                            void *save,                     /*!< out: immediate result
                                                                            for update function */
                                            struct st_mysql_value *value) { /*!< in: incoming string */
  long long input_val;
  if (value->val_int(value, &input_val)) {
    return 1;
  }

  if (input_val < 1 || (uint)input_val > ShannonBase::SHANNON_PARALLEL_LOAD_THRESHOLD) {
    return 1;
  }

  *static_cast<int *>(save) = static_cast<int>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_parallel_load_threshold.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from chesck function */
static void rpd_para_load_threshold_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<int *>(var_ptr) == *static_cast<const int *>(save)) return;

  *static_cast<int *>(var_ptr) = *static_cast<const int *>(save);
  ShannonBase::shannon_rpd_engine_cfg.para_load_threshold = *static_cast<const int *>(save);
}

/** Validate passed-in "value" is a valid monitor counter name.
 This function is registered as a callback with MySQL.
 @return 0 for valid name */
static int rpd_para_parttb_load_threshold_validate(THD *,                          /*!< in: thread handle */
                                                   SYS_VAR *,                      /*!< in: pointer to system
                                                                                                   variable */
                                                   void *save,                     /*!< out: immediate result
                                                                                   for update function */
                                                   struct st_mysql_value *value) { /*!< in: incoming string */
  long long input_val;
  if (value->val_int(value, &input_val)) {
    return 1;
  }

  const auto max_allowed =
      std::max<uint64_t>(3ULL * std::thread::hardware_concurrency(), ShannonBase::SHANNON_PARALLEL_PARTTB_THRESHOLD);
  if (input_val < 1 || (uint64_t)input_val > max_allowed) {
    return 1;
  }

  *static_cast<int *>(save) = static_cast<int>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable shannon_rpd_para_parttb_load_threshold.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from chesck function */
static void rpd_para_parttb_load_threshold_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<int *>(var_ptr) == *static_cast<const int *>(save)) return;

  *static_cast<int *>(var_ptr) = *static_cast<const int *>(save);
  ShannonBase::shannon_rpd_engine_cfg.para_parttb_load_threshold = *static_cast<const int *>(save);
}

static const char *rapid_propagation_mode_names[] = {"DIRECT_NOTIFICATION", "REDO_LOG_PARSE", "HYBRID", nullptr};

// to update sync mode of propagation of changes.
static void rpd_sync_mode_update(MYSQL_THD thd [[maybe_unused]], SYS_VAR *var [[maybe_unused]], void *var_ptr,
                                 const void *save) {
  /* check if there is an actual change */
  if (*static_cast<ulong *>(var_ptr) == *static_cast<const ulong *>(save)) return;

  *static_cast<ulong *>(var_ptr) = *static_cast<const ulong *>(save);
}

/** Validate passed-in "value" is a valid propagation sync mode.
 This function is registered as a callback with MySQL.
 @return 0 for valid name */
static int rpd_sync_mode_validate(THD *,                          /*!< in: thread handle */
                                  SYS_VAR *,                      /*!< in: pointer to system
                                                                                  variable */
                                  void *save,                     /*!< out: immediate result
                                                                  for update function */
                                  struct st_mysql_value *value) { /*!< in: incoming string */

  if (ShannonBase::Populate::Populator::active() || ShannonBase::shannon_loaded_tables->size()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Tables have been loaded, cannot change the rapid sync mode to unload all loaded tables");
    return 1;
  }

  int type = value->value_type(value);
  if (type == MYSQL_VALUE_TYPE_INT) {
    long long input_val;
    if (value->val_int(value, &input_val)) return 1;

    if (input_val < 0 || input_val > 2) {
      sql_print_error("Sync mode value %lld is out of range [0, 2]", input_val);
      return 1;
    }
    *static_cast<ulong *>(save) = static_cast<ulong>(input_val);
  } else {
    const char *str_val;
    char buff[STRING_BUFFER_USUAL_SIZE];
    int length = sizeof(buff);

    if ((str_val = value->val_str(value, buff, &length)) == NULL) return 1;

    if (strcasecmp(str_val, "DIRECT_NOTIFICATION") == 0) {
      *static_cast<ulong *>(save) = 0;
    } else if (strcasecmp(str_val, "REDO_LOG_PARSE") == 0) {
      *static_cast<ulong *>(save) = 1;
    } else if (strcasecmp(str_val, "HYBRID") == 0) {
      *static_cast<ulong *>(save) = 2;
    } else {
      sql_print_error("Invalid sync mode name: %s", str_val);
      return 1;
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

static TYPELIB rapid_sync_mode_typelib = {array_elements(rapid_propagation_mode_names) - 1, "rapid_sync_mode_typelib",
                                          rapid_propagation_mode_names, nullptr};

/** Update the system variable rpd_async_threshold.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_async_threshold_update(MYSQL_THD thd [[maybe_unused]], SYS_VAR *var [[maybe_unused]], void *var_ptr,
                                       const void *save) {
  /* check if there is an actual change */
  if (*static_cast<int *>(var_ptr) == *static_cast<const int *>(save)) return;

  *static_cast<int *>(var_ptr) = *static_cast<const int *>(save);
  ShannonBase::shannon_rpd_engine_cfg.async_column_threshold = *static_cast<const int *>(save);
}

/** Validate passed-in "value" is a valid monitor counter name.
 This function is registered as a callback with MySQL.
 @return 0 for valid name */
static int rpd_async_threshold_validate(THD *,                          /*!< in: thread handle */
                                        SYS_VAR *,                      /*!< in: pointer to system
                                                                                        variable */
                                        void *save,                     /*!< out: immediate result
                                                                        for update function */
                                        struct st_mysql_value *value) { /*!< in: incoming string */
  long long input_val;

  if (ShannonBase::Populate::Populator::active() || ShannonBase::shannon_loaded_tables->size()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Tables have been loaded, cannot change the rapid params");
    return 1;
  }

  if (value->val_int(value, &input_val)) return 1;

  if (input_val < 1 || input_val > ShannonBase::MAX_N_FIELD_PARALLEL) return 1;

  *static_cast<int *>(save) = static_cast<int>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

static void update_use_dynmaic_offload_enabled(THD *, SYS_VAR *, void *var_ptr, const void *save) {
  if (*static_cast<bool *>(var_ptr) == *static_cast<const bool *>(save)) return;

  bool new_value = *static_cast<const bool *>(save);
  *static_cast<bool *>(var_ptr) = new_value;
  ShannonBase::shannon_rpd_engine_cfg.dynamic_offloads = *static_cast<const bool *>(save);
}

static void update_self_load_enabled(THD *, SYS_VAR *, void *var_ptr, const void *save) {
  if (*static_cast<bool *>(var_ptr) == *static_cast<const bool *>(save)) return;

  bool new_value = *static_cast<const bool *>(save);
  *static_cast<bool *>(var_ptr) = new_value;
  ShannonBase::shannon_rpd_engine_cfg.self_load_enabled = *static_cast<const bool *>(save);
  if (!ShannonBase::shannon_self_load_mgr_inst)
    ShannonBase::shannon_self_load_mgr_inst = ShannonBase::Autopilot::SelfLoadManager::instance();

  if (ShannonBase::shannon_rpd_engine_cfg.self_load_enabled) {  // to start AutoLoader thread.
    if (ShannonBase::shannon_self_load_mgr_inst && ShannonBase::shannon_self_load_mgr_inst->initialized())
      ShannonBase::shannon_self_load_mgr_inst->start();
  } else {
    if (ShannonBase::shannon_self_load_mgr_inst && ShannonBase::shannon_self_load_mgr_inst->initialized())
      ShannonBase::shannon_self_load_mgr_inst->shutdown();
  }
}

static int check_self_load_interval(THD *thd, SYS_VAR *var, void *save, st_mysql_value *value) {
  longlong new_value;
  value->val_int(value, &new_value);

  if (new_value < 60) {
    my_printf_error(ER_WRONG_VALUE_FOR_VAR, "rapid_self_load_interval_seconds must be at least 60 seconds", MYF(0));
    return 1;
  }

  if (new_value > 604800) {
    my_printf_error(ER_WRONG_VALUE_FOR_VAR, "rapid_self_load_interval_seconds cannot exceed 604800 seconds (1 week)",
                    MYF(0));
    return 1;
  }
  *static_cast<ulonglong *>(save) = new_value;
  return 0;
}

static void update_self_load_interval(THD *, SYS_VAR *, void *var_ptr, const void *save) {
  ulonglong new_value = *static_cast<const ulonglong *>(save);
  *static_cast<ulonglong *>(var_ptr) = new_value;
  ShannonBase::shannon_rpd_engine_cfg.self_load_interval_sec = new_value;
}

static void update_skip_quiet_check(THD *, SYS_VAR *, void *var_ptr, const void *save) {
  bool new_value = *static_cast<const bool *>(save);
  *static_cast<bool *>(var_ptr) = new_value;

  ShannonBase::shannon_rpd_engine_cfg.self_load_skip_quiet_check = new_value;
}

static void update_memory_fill_percentage(THD *, SYS_VAR *, void *var_ptr, const void *save) {
  int new_value = *static_cast<const int *>(save);
  *static_cast<int *>(var_ptr) = new_value;

  ShannonBase::shannon_rpd_engine_cfg.self_load_base_relation_fill_percentage = new_value;
}

/** Validate passed-in "value" is a valid monitor counter name.
 This function is registered as a callback with MySQL.
 @return 0 for valid name */
static int rpd_max_purger_timeout_validate(THD *,                          /*!< in: thread handle */
                                           SYS_VAR *,                      /*!< in: pointer to system
                                                                                           variable */
                                           void *save,                     /*!< out: immediate result
                                                                           for update function */
                                           struct st_mysql_value *value) { /*!< in: incoming string */
  longlong input_val;
  if (value->val_int(value, &input_val)) return 1;

  if (input_val < ShannonBase::SHANNON_MIN_PURGER_TIMEOUT) return 1;

  *static_cast<ulonglong *>(save) = static_cast<ulonglong>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_max_purger_timeout.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_max_purger_timeout_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<ulonglong *>(var_ptr) == *static_cast<const ulonglong *>(save)) return;

  *static_cast<ulonglong *>(var_ptr) = *static_cast<const ulonglong *>(save);
  ShannonBase::shannon_rpd_engine_cfg.gc_interval_seconds = *static_cast<const ulonglong *>(save);
}

/** Validate passed-in "value" is a valid monitor counter name.
 This function is registered as a callback with MySQL.
 @return 0 for valid name */
static int rpd_purge_batch_size_validate(THD *,                          /*!< in: thread handle */
                                         SYS_VAR *,                      /*!< in: pointer to system
                                                                                         variable */
                                         void *save,                     /*!< out: immediate result
                                                                         for update function */
                                         struct st_mysql_value *value) { /*!< in: incoming string */
  long long input_val;
  if (value->val_int(value, &input_val)) return 1;

  if (input_val < ShannonBase::SHANNON_MIN_PURGE_BATCH_SIZE || input_val > ShannonBase::SHANNON_MAX_PURGE_BATCH_SIZE)
    return 1;

  *static_cast<ulonglong *>(save) = static_cast<ulonglong>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_purge_batch_size.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_purge_batch_size_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<ulonglong *>(var_ptr) == *static_cast<const ulonglong *>(save)) return;

  *static_cast<ulonglong *>(var_ptr) = *static_cast<const ulonglong *>(save);
  ShannonBase::shannon_rpd_engine_cfg.gc_batch_size = *static_cast<const ulonglong *>(save);
}

/** Validate passed-in "value" is a valid monitor counter name.
 This function is registered as a callback with MySQL.
 @return 0 for valid name */
static int rpd_min_versions_for_purge_validate(THD *,                          /*!< in: thread handle */
                                               SYS_VAR *,                      /*!< in: pointer to system
                                                                                               variable */
                                               void *save,                     /*!< out: immediate result
                                                                               for update function */
                                               struct st_mysql_value *value) { /*!< in: incoming string */
  long long input_val;
  if (value->val_int(value, &input_val)) return 1;

  if (input_val < ShannonBase::SHANNON_MIN_PURGE_BATCH_SIZE || input_val > ShannonBase::SHANNON_MAX_PURGE_BATCH_SIZE)
    return 1;

  *static_cast<ulonglong *>(save) = static_cast<ulonglong>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_min_versions_for_purge.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_min_versions_for_purge_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<ulonglong *>(var_ptr) == *static_cast<const ulonglong *>(save)) return;

  *static_cast<ulonglong *>(var_ptr) = *static_cast<const ulonglong *>(save);
  ShannonBase::shannon_rpd_engine_cfg.gc_min_version = *static_cast<const ulonglong *>(save);
}

/** Update the system variable shannon_rpd_purge_efficiency_threshold.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_purge_efficiency_threshold_update(THD *thd,         /*!< in: thread handle */
                                                  SYS_VAR *,        /*!< in: pointer to
                                                                                    system variable */
                                                  void *var_ptr,    /*!< out: where the
                                                             formal string goes */
                                                  const void *save) /*!< in: immediate result
                                                                    from check function */
{
  /* check if there is an actual change */
  if (*static_cast<ulong *>(var_ptr) == *static_cast<const ulong *>(save)) return;

  double in_val = *static_cast<const double *>(save);

  if (in_val < 0.1) {
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                        "rapid_purge_efficiency_threshold cannot be"
                        " set lower than 0.1.");
    in_val = 0.1;
  }

  if (in_val > 1) {
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                        "rapid_purge_efficiency_threshold cannot be"
                        " set upper than 1");
    in_val = 1;
  }

  ShannonBase::shannon_rpd_engine_cfg.gc_version_ratio_threshold = in_val;
}

static int rpd_gc_interval_scn_validate(THD *,                          /*!< in: thread handle */
                                        SYS_VAR *,                      /*!< in: pointer to system
                                                                                        variable */
                                        void *save,                     /*!< out: immediate result
                                                                        for update function */
                                        struct st_mysql_value *value) { /*!< in: incoming string */
  long long input_val;
  if (value->val_int(value, &input_val)) return 1;
  if (input_val < 0) return 1;

  *static_cast<ulonglong *>(save) = static_cast<ulonglong>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

static void rpd_gc_interval_scn_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<ulonglong *>(var_ptr) == *static_cast<const ulonglong *>(save)) return;

  *static_cast<ulonglong *>(var_ptr) = *static_cast<const ulonglong *>(save);
}

// clang-format off
static MYSQL_SYSVAR_ULONG(memory_size_max,
                          ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_mb,
                          PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_PERSIST_AS_READ_ONLY,
                          "Number of memory size that used for rapid engine, and it must "
                          "not be oversize half of physical mem size(MB).",
                          rpd_mem_size_max_validate,
                          rpd_mem_size_max_update,
                          ShannonBase::SHANNON_MAX_MEMRORY_SIZE,
                          ShannonBase::SHANNON_MAX_MEMRORY_SIZE,
                          ShannonBase::SHANNON_MAX_MEMRORY_SIZE,
                          0);

static MYSQL_SYSVAR_ULONGLONG(pop_buffer_size_max,
                              ShannonBase::shannon_rpd_engine_cfg.pop_buff_sz_max,
                              PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                              "Number of memory used for populating the changes "
                              "in innodb to rapid engine..",
                              rpd_pop_buff_size_max_validate,
                              rpd_pop_buff_size_max_update,
                              ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE,
                              ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE,
                              ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE,
                              0);

static MYSQL_SYSVAR_ULONGLONG(parallel_load_max,
                              ShannonBase::shannon_rpd_engine_cfg.para_load_threshold,
                              PLUGIN_VAR_OPCMDARG,
                              "Max number of rows used to use parallel load for secondary_load "
                              "from innodb to rapid engine..",
                              rpd_para_load_threshold_validate,
                              rpd_para_load_threshold_update,
                              ShannonBase::SHANNON_PARALLEL_LOAD_THRESHOLD, //default val
                              0,  //min
                              ShannonBase::SHANNON_PARALLEL_LOAD_THRESHOLD, //max
                              0);

static MYSQL_SYSVAR_ULONGLONG(parallel_part_load_threshold,
                              ShannonBase::shannon_rpd_engine_cfg.para_parttb_load_threshold,
                              PLUGIN_VAR_OPCMDARG,
                              "Threshold number of part table used to use parallel load for secondary_load "
                              "from innodb to rapid engine..",
                              rpd_para_parttb_load_threshold_validate,
                              rpd_para_parttb_load_threshold_update,
                              ShannonBase::SHANNON_PARALLEL_PARTTB_THRESHOLD, //default val
                              ShannonBase::SHANNON_PARALLEL_PARTTB_THRESHOLD,  //min
                              1024, //max
                              0);

static MYSQL_SYSVAR_ENUM(propagation_mode,
                        ShannonBase::shannon_rpd_engine_cfg.propagate_mode,
                        PLUGIN_VAR_OPCMDARG,
                        "The synchronization mode of changes propagation: DIRECT_NOTIFICATION, REDO_LOG_PARSE, HYBRID",
                        rpd_sync_mode_validate,
                        rpd_sync_mode_update,
                        0, // default: DIRECT_NOTIFICATION
                        &rapid_sync_mode_typelib
);

static MYSQL_SYSVAR_INT(async_column_threshold,
                        ShannonBase::shannon_rpd_engine_cfg.async_column_threshold,
                        PLUGIN_VAR_OPCMDARG,
                        "Max number of columns will do async-corountine for reading data or parsing log ",
                        rpd_async_threshold_validate,
                        rpd_async_threshold_update,
                        ShannonBase::DEFAULT_N_FIELD_PARALLEL,
                        1,
                        ShannonBase::MAX_N_FIELD_PARALLEL,
                        0);

static MYSQL_SYSVAR_BOOL(use_dynamic_offload,
                         ShannonBase::shannon_rpd_engine_cfg.dynamic_offloads,
                         PLUGIN_VAR_OPCMDARG,
                        "When system variable rapid_use_dynamic_offload is 0/false , then we "
                        "fall back to normal cost threshold classifier, which also implies that "
                        "when use secondary engine is set to forced, eligible queries will go to "
                        "secondary engine, regardless of cost threshold or this classifier. "
                        "When rapid_use_dynamic_offload is 1/true, then we proceed with looking "
                        "for optimal execution engine for this queries, if secondary engine is "
                        "found more optimal, then query is offloaded, otherwise it is sent back "
                        "to mysql. default value: on",
                         nullptr,
                         update_use_dynmaic_offload_enabled,
                         true);

static MYSQL_SYSVAR_BOOL(self_load_enabled, 
                         ShannonBase::shannon_rpd_engine_cfg.self_load_enabled,
                         PLUGIN_VAR_OPCMDARG,
                        "self-loaded, tables will not interfere with user-issued secondary loads under any "
                        "resource constraint. For example, if there is not enough memory in the "
                        "system for an incoming user load, some self-loaded tables will have to "
                        "be unloaded to make room for the newly user-loaded table. default value: false.",
                         nullptr,
                         update_self_load_enabled,
                         false);

static MYSQL_SYSVAR_ULONGLONG(self_load_interval_seconds,
                         ShannonBase::shannon_rpd_engine_cfg.self_load_interval_sec,
                         PLUGIN_VAR_OPCMDARG,
                         "Wake-up interval of the Self-Load thread "
                         "Default value: 86400s (24h). Note that if the interval is changed while "
                         "it's TRUE, the new value might not be picked up "
                         "until the next wakeup of the Self-Load Worker. Therefore, the recommended order of "
                         "setting the variables is: 1.",
                         check_self_load_interval,
                         update_self_load_interval,
                         86400/**24hrs */,
                         60 /*a mins*/,
                         86400 * 7/*a week */,
                         0);

static MYSQL_SYSVAR_BOOL(self_load_skip_quiet_check,
                         ShannonBase::shannon_rpd_engine_cfg.self_load_skip_quiet_check,
                         PLUGIN_VAR_OPCMDARG,
                         "self-loaded, tables will not interfere with user-issued secondary loads under any "
                         "resource constraint. For example, if there is not enough memory in the "
                         "system for an incoming user load, some self-loaded tables will have to "
                         "be unloaded to make room for the newly user-loaded table. ",
                         nullptr,
                         update_skip_quiet_check,
                         false);

static MYSQL_SYSVAR_INT(self_load_base_relation_fill_percentage,
                         ShannonBase::shannon_rpd_engine_cfg.self_load_base_relation_fill_percentage,
                         PLUGIN_VAR_OPCMDARG,
                         "Percentage of base memory quota above which the self-load thread "
                         "rpdserver and rpdmaster. Default value: 70%.",
                         nullptr,
                         update_memory_fill_percentage,
                         70,
                         1,
                         100,
                         0);

static MYSQL_SYSVAR_ULONGLONG(max_purger_timeout,
                              ShannonBase::shannon_rpd_engine_cfg.gc_interval_seconds,
                              PLUGIN_VAR_OPCMDARG,
                              "Default value of spin delay (in spin rounds)"
                              "1000 spin round takes 4us, 25000 takes 1ms for busy waiting. therefore, 200ms means"
                              "5000000 spin rounds. for the more detail infor ref to : comment of"
                              "`innodb_log_writer_spin_delay`.",
                              rpd_max_purger_timeout_validate,
                              rpd_max_purger_timeout_update,
                              ShannonBase::SHANNON_DEFAULT_MAX_PURGER_TIMEOUT, // default val
                              ShannonBase::SHANNON_MIN_PURGER_TIMEOUT,  // min
                              ULLONG_MAX, // max
                              0);

static MYSQL_SYSVAR_ULONGLONG(purge_batch_size,
                              ShannonBase::shannon_rpd_engine_cfg.gc_batch_size,
                              PLUGIN_VAR_OPCMDARG,
                              "Process chunks in batches, number of chunks to process in a single purge batch",
                              rpd_purge_batch_size_validate,
                              rpd_purge_batch_size_update,
                              ShannonBase::SHANNON_DEFAULT_PURGE_BATCH_SIZE, // default val
                              ShannonBase::SHANNON_MIN_PURGE_BATCH_SIZE,  // min
                              ShannonBase::SHANNON_MAX_PURGE_BATCH_SIZE, // max
                              0);
                    
static MYSQL_SYSVAR_ULONGLONG(min_versions_for_purge,
                              ShannonBase::shannon_rpd_engine_cfg.gc_min_version,
                              PLUGIN_VAR_OPCMDARG,
                              "Minimum number of versions required for a chunk to be eligible for purging",
                              rpd_min_versions_for_purge_validate,
                              rpd_min_versions_for_purge_update,
                              ShannonBase::SHANNON_DEFAULT_MIN_VERSIONS_FOR_PURGE, // default val
                              ShannonBase::SHANNON_DEFAULT_MIN_VERSIONS_FOR_PURGE,  // min
                              ULLONG_MAX, // max
                              0);

static MYSQL_SYSVAR_DOUBLE(purge_efficiency_threshold,
                           ShannonBase::shannon_rpd_engine_cfg.gc_version_ratio_threshold,
                           PLUGIN_VAR_RQCMDARG,
                           "Purge efficiency threshold, only purge if >10% can be cleaned",
                           nullptr,
                           rpd_purge_efficiency_threshold_update, 
                           0.1,
                           0.1,
                           1,
                           0);

static MYSQL_SYSVAR_ULONGLONG(gc_interval_scn,
                              ShannonBase::shannon_rpd_engine_cfg.gc_interval_scn,
                              PLUGIN_VAR_OPCMDARG,
                              "Initiates a garbage collection cycle when the difference between the current System"
                              "Change Number (SCN) and the last recorded GC SCN exceeds this threshold value.",
                              rpd_gc_interval_scn_validate,
                              rpd_gc_interval_scn_update,
                              ShannonBase::SHANNON_DEFAULT_GC_INTERVAL_SCN, // default val
                              ShannonBase::SHANNON_DEFAULT_GC_INTERVAL_SCN,  // min
                              ULLONG_MAX, // max
                              0);

static MYSQL_SYSVAR_BOOL(reload_on_restart,
                            ShannonBase::shannon_rpd_engine_cfg.reload_on_restart,
                            PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_PERSIST_AS_READ_ONLY,
                            "Reload IMCS tables automatically after mysqld restart",
                            nullptr, nullptr, false  // default OFF
                        );

static MYSQL_SYSVAR_BOOL(schema_embedding,
                            ShannonBase::shannon_rpd_engine_cfg.enable_schema_embedding,
                            PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_PERSIST_AS_READ_ONLY,
                            "Enable schema embedding for natural language query schema information support",
                            nullptr, nullptr, true  // default ON                            
                        );
// clang-format on
static struct SYS_VAR *rapid_system_variables[] = {
    MYSQL_SYSVAR(memory_size_max),
    MYSQL_SYSVAR(pop_buffer_size_max),
    MYSQL_SYSVAR(parallel_load_max),
    MYSQL_SYSVAR(parallel_part_load_threshold),
    MYSQL_SYSVAR(propagation_mode),
    MYSQL_SYSVAR(async_column_threshold),
    MYSQL_SYSVAR(use_dynamic_offload),
    MYSQL_SYSVAR(self_load_enabled),
    MYSQL_SYSVAR(self_load_interval_seconds),
    MYSQL_SYSVAR(self_load_skip_quiet_check),
    MYSQL_SYSVAR(self_load_base_relation_fill_percentage),
    MYSQL_SYSVAR(max_purger_timeout),
    MYSQL_SYSVAR(purge_batch_size),
    MYSQL_SYSVAR(min_versions_for_purge),
    MYSQL_SYSVAR(purge_efficiency_threshold),
    MYSQL_SYSVAR(gc_interval_scn),
    MYSQL_SYSVAR(reload_on_restart),
    MYSQL_SYSVAR(schema_embedding),
    nullptr,
};

static SHOW_VAR rapid_status_variables_export[] = {
    {"ShannonBase Rapid", (char *)&show_rapid_vars, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"ShannonBase Rapid Runtime", (char *)&show_rapid_runtime_status, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

extern bool srv_is_upgrade_mode;
extern char mysql_home[FN_REFLEN];
extern char mysql_llm_home[FN_REFLEN];
extern bool opt_initialize;
extern long opt_upgrade_mode;
static int Shannonbase_Rapid_Init(MYSQL_PLUGIN p) {
  ShannonBase::shannon_loaded_tables = new ShannonBase::LoadedTables();

  ShannonBase::Utils::MemoryPool::Config config(ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_mb);
  ShannonBase::shannon_rpd_memory_pool = std::make_shared<ShannonBase::Utils::MemoryPool>(config);
  ShannonBase::shannon_rpd_cost_est_instances =
      ShannonBase::Optimizer::CostModelServer::Instance(ShannonBase::Optimizer::CostEstimator::Type::RPD_ENG);

  handlerton *shannon_rapid_hton = static_cast<handlerton *>(p);
  ShannonBase::shannon_rapid_hton_ptr = shannon_rapid_hton;
  shannon_rapid_hton->create = rapid_create_handler;
  shannon_rapid_hton->state = SHOW_OPTION_YES;
  shannon_rapid_hton->flags = HTON_IS_SECONDARY_ENGINE;
  shannon_rapid_hton->db_type = DB_TYPE_RAPID;
  shannon_rapid_hton->notify_create_table = NotifyCreateTable;
  shannon_rapid_hton->notify_drop_table = NotifyDropTable;
  shannon_rapid_hton->notify_alter_table = NotifyAlterTable;
  shannon_rapid_hton->notify_after_insert = NotifyAfterInsert;
  shannon_rapid_hton->notify_after_update = NotifyAfterUpdate;
  shannon_rapid_hton->notify_after_delete = NotifyAfterDelete;
  shannon_rapid_hton->notify_after_select = NotifyAfterSelect;

  shannon_rapid_hton->prepare_secondary_engine = PrepareSecondaryEngine;
  shannon_rapid_hton->secondary_engine_pre_prepare_hook = SecondaryEnginePrePrepareHook;
  shannon_rapid_hton->optimize_secondary_engine = OptimizeSecondaryEngine;
  shannon_rapid_hton->compare_secondary_engine_cost = CompareJoinCost;
  shannon_rapid_hton->secondary_engine_flags =
      MakeSecondaryEngineFlags(SecondaryEngineFlag::SUPPORTS_HASH_JOIN, SecondaryEngineFlag::SUPPORTS_NESTED_LOOP_JOIN);
  shannon_rapid_hton->secondary_engine_modify_access_path_cost = ModifyAccessPathCost;
  shannon_rapid_hton->get_secondary_engine_offload_or_exec_fail_reason = GetSecondaryEngineOffloadorExecFailedReason;
  shannon_rapid_hton->set_secondary_engine_offload_fail_reason = SetSecondaryEngineOffloadFailedReasonWrapper;
  shannon_rapid_hton->secondary_engine_check_optimizer_request = SecondaryEngineCheckOptimizerRequest;

  shannon_rapid_hton->commit = rapid_commit;
  shannon_rapid_hton->rollback = rapid_rollback;
  shannon_rapid_hton->se_after_commit = rapid_after_commit;
  shannon_rapid_hton->se_before_rollback = rapid_before_rollback;
  shannon_rapid_hton->start_consistent_snapshot = rapid_start_trx_and_assign_read_view;
  shannon_rapid_hton->savepoint_set = rapid_savepoint;
  shannon_rapid_hton->savepoint_rollback = rapid_rollback_to_savepoint;
  shannon_rapid_hton->savepoint_rollback_can_release_mdl = rapid_rollback_to_savepoint_can_release_mdl;
  shannon_rapid_hton->close_connection = rapid_close_connection;
  shannon_rapid_hton->kill_connection = rapid_kill_connection;
  shannon_rapid_hton->pre_dd_shutdown = rapid_pre_dd_shutdown;
  shannon_rapid_hton->panic = rapid_shutdown;
  shannon_rapid_hton->partition_flags = rapid_partition_flags;

  if (!opt_initialize && !srv_is_upgrade_mode && !opt_upgrade_mode) {
    std::string home_path(mysql_llm_home);
    if (home_path.empty()) home_path = mysql_home;
    if (!home_path.empty() && home_path.back() != '/') home_path += '/';
    const std::string model_path = home_path + "llm-models/shannon_rapid_classifier.onnx";
    if (!ShannonBase::ML::Query_arbitrator::initialize(model_path)) {
      sql_print_warning(
          "Shannon Rapid: classifier model not loaded (%s), "
          "decision_tree_classifier will fallback to primary engine",
          model_path.c_str());
    }
  }

  auto instance_ = ShannonBase::Imcs::Imcs::instance();
  if (!instance_) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "get IMCS instance");
    return HA_ERR_INITIALIZATION;
  };
  auto ret = instance_->initialize();

  if (!srv_is_upgrade_mode /**not in upgrade stage */) {
    // self-loader worker
    ShannonBase::shannon_self_load_mgr_inst = ShannonBase::Autopilot::SelfLoadManager::instance();

    // recovery worker
    ShannonBase::Recovery::rapid_recovery_startup();
  }
  return ret;
}

static int Shannonbase_Rapid_Deinit(MYSQL_PLUGIN) {
  // Release ONNX Runtime resources (thread pool, session, environment).
  ShannonBase::ML::Query_arbitrator::shutdown();

  // embedding worker thread shut down. Idempotent operation.
  ShannonBase::ML::EmbeddingManager::shutdown();

  // self-loader worker
  if (ShannonBase::shannon_self_load_mgr_inst && ShannonBase::shannon_self_load_mgr_inst->initialized())
    ShannonBase::shannon_self_load_mgr_inst->shutdown();

  // change populator
  ShannonBase::Populate::Populator::shutdown();

  // recovery worker (symmetric with rapid_recovery_startup in Init)
  ShannonBase::Recovery::rapid_recovery_shutdown();

  if (ShannonBase::shannon_loaded_tables) {
    delete ShannonBase::shannon_loaded_tables;
    ShannonBase::shannon_loaded_tables = nullptr;
  }

  auto instance_ = ShannonBase::Imcs::Imcs::instance();
  int ret = instance_->deinitialize();

  // Release the shared memory pool to join its background monitor threads.
  ShannonBase::shannon_rpd_memory_pool.reset();

  return ret;
}

static st_mysql_storage_engine rapid_storage_engine{MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(shannon_rapid){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &rapid_storage_engine,
    "Rapid",
    PLUGIN_AUTHOR_SHANNON,
    "Shannon Rapid storage engine",
    PLUGIN_LICENSE_GPL,
    Shannonbase_Rapid_Init,   /* Plugin Init */
    nullptr,                  /* Plugin Check uninstall */
    Shannonbase_Rapid_Deinit, /* Plugin Deinit */
    ShannonBase::SHANNON_RPD_VERSION,
    rapid_status_variables_export, /* status variables */
    rapid_system_variables,        /* system variables */
    nullptr,                       /* config options */
    0,
} mysql_declare_plugin_end;
