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
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

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
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"

#include "log0log.h" /* log_get_lsn */

#include "storage/innobase/handler/ha_innodb.h"  //thd_to_trx
#include "storage/innobase/include/dict0dd.h"    //dd_is_partitioned

#include "storage/rapid_engine/autopilot/loader.h"
#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/handler/ha_shannon_rapidpart.h"
#include "storage/rapid_engine/imcs/imcs.h"        // IMCS
#include "storage/rapid_engine/imcs/table0view.h"  //RapidCursor
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_const.h"  //const
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"  //column stats
#include "storage/rapid_engine/optimizer/optimizer.h"
#include "storage/rapid_engine/optimizer/path/access_path.h"
#include "storage/rapid_engine/optimizer/writable_access_path.h"
#include "storage/rapid_engine/populate/log_populate.h"
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

MEM_ROOT rapid_mem_root(PSI_NOT_INSTRUMENTED, 1024);
std::shared_ptr<Utils::MemoryPool> g_rpd_memory_pool{nullptr};
handlerton *shannon_rapid_hton_ptr{nullptr};

LoadedTables *shannon_loaded_tables = nullptr;
uint64 rpd_mem_sz_max = ShannonBase::SHANNON_DEFAULT_MEMRORY_SIZE;
ulonglong rpd_pop_buff_sz_max = ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE;
ulonglong rpd_para_load_threshold = ShannonBase::SHANNON_PARALLEL_LOAD_THRESHOLD;
int rpd_async_column_threshold = ShannonBase::DEFAULT_N_FIELD_PARALLEL;
bool rpd_self_load_enabled = false;
ulonglong rpd_self_load_interval_seconds = SHANNON_DEFAULT_SELF_LOAD_INTERVAL;  // 24hurs
bool rpd_self_load_skip_quiet_check = false;
bool rprapid_max_purger_timeoutd_self_load_skip_quiet_check = false;
int rpd_self_load_base_relation_fill_percentage = SHANNON_DEFAULT_SELF_LOAD_FILL_PERCENTAGE;  // percentage.
ulonglong rpd_max_purger_timeout = SHANNON_DEFAULT_MAX_PURGER_TIMEOUT;
ulonglong rpd_purge_batch_size = SHANNON_DEFAULT_MAX_PURGER_TIMEOUT;
ulonglong rpd_min_versions_for_purge = SHANNON_DEFAULT_MAX_PURGER_TIMEOUT;
double rpd_purge_efficiency_threshold = SHANNON_DEFAULT_PURGE_EFFICIENCY_THRESHOLD;

std::atomic<size_t> rapid_allocated_mem_size = 0;
rpd_columns_container rpd_columns_info;

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

// Map from (db_name, table_name) to the RapidShare with table state.
LoadedTables::~LoadedTables() {
  std::lock_guard<std::mutex> guard(m_mutex);
  for (auto &entry : m_tables) {
    delete entry.second;
  }
}
void LoadedTables::add(const std::string &db, const std::string &table, RapidShare *rs) {
  std::lock_guard<std::mutex> guard(m_mutex);
  std::string keystr = db + ":" + table;
  auto it = m_tables.find(keystr);
  if (it != m_tables.end()) {  // replace with new one.
    delete it->second;
    it->second = rs;
  } else {
    m_tables.emplace(keystr, rs);
  }
}

RapidShare *LoadedTables::get(const std::string &db, const std::string &table) {
  std::lock_guard<std::mutex> guard(m_mutex);
  auto it = m_tables.find(db + ":" + table);
  return it != m_tables.end() ? it->second : nullptr;
}

void LoadedTables::erase(const std::string &db, const std::string &table) {
  std::lock_guard<std::mutex> guard(m_mutex);
  auto it = m_tables.find(db + ":" + table);
  if (it != m_tables.end()) {
    delete it->second;
    m_tables.erase(it);
  }
}

void LoadedTables::table_infos(uint index, ulonglong &tid, std::string &schema, std::string &table) {
  if (index > m_tables.size()) return;

  uint count = 0;
  for (auto &item : m_tables) {
    if (count == index) {
      std::string keystr = item.first;
      size_t colon_pos = keystr.find(':');
      if (colon_pos == std::string::npos) continue;

      schema = keystr.substr(0, colon_pos);
      table = keystr.substr(colon_pos + 1);
      tid = (item.second)->m_tableid;
    }
    count++;
  }
}

ha_rapid::ha_rapid(handlerton *hton, TABLE_SHARE *table_share_arg)
    : handler(hton, table_share_arg), m_share(nullptr), m_thd(ha_thd()) {}

int ha_rapid::open(const char *name, int, uint open_flags, const dd::Table *table_def) {
  RapidShare *share = shannon_loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (share == nullptr) {
    // The table has not been loaded into the secondary storage engine yet.
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Table has not been loaded");
    return HA_ERR_GENERIC;
  }

  thr_lock_data_init(&share->lock, &m_lock, nullptr);

  std::string key(table_share->db.str);
  key.append(":").append(table_share->table_name.str);
  m_rpd_table = dd_table_is_partitioned(*table_def) ? Imcs::Imcs::instance()->get_rpd_parttable(key)
                                                    : Imcs::Imcs::instance()->get_rpd_table(key);
  m_cursor.reset(new Imcs::RapidCursor(table, m_rpd_table));

  m_cursor->open();
  if (end_range) m_cursor->set_end_range(end_range);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::close() {
  m_cursor->close();
  m_cursor.reset(nullptr);

  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::info(unsigned int flags) {
  ut_a(flags == (HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK));

  std::string sch_tb;
  sch_tb.append(table_share->db.str).append(":").append(table_share->table_name.str);

  if (table->part_info) {
    auto rpd_tb = Imcs::Imcs::instance()->get_rpd_parttable(sch_tb);
    stats.records = rpd_tb->meta().total_rows;
  } else {
    auto rpd_tb = Imcs::Imcs::instance()->get_rpd_table(sch_tb);
    stats.records = down_cast<ShannonBase::Imcs::Table *>(rpd_tb)->rows(nullptr);
  }
  return ShannonBase::SHANNON_SUCCESS;
}

/** Returns the operations supported for indexes.
 @return flags of supported operations */
handler::Table_flags ha_rapid::table_flags() const {
  /** Orignal:Secondary engines do not support index access. Indexes are only
   *  used for cost estimates. But, here, we support index too.*/

  // return HA_NO_INDEX_ACCESS | HA_STATS_RECORDS_IS_EXACT | HA_COUNT_ROWS_INSTANT;
  ulong flags = HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE | HA_KEYREAD_ONLY |
                HA_DO_INDEX_COND_PUSHDOWN | HA_STATS_RECORDS_IS_EXACT | HA_COUNT_ROWS_INSTANT;
  return ~HA_NO_INDEX_ACCESS || flags;
}

/** Returns the table type (storage engine name).
 @return table type */
const char *ha_rapid::table_type() const { return (rapid_hton_name); }

unsigned long ha_rapid::index_flags(unsigned int idx, unsigned int part, bool all_parts) const {
  const handler *primary = ha_get_primary_handler();
  const unsigned long primary_flags = primary == nullptr ? 0 : primary->index_flags(idx, part, all_parts);

  // Inherit the following index flags from the primary handler, if they are
  // set:
  //
  // HA_READ_RANGE - to signal that ranges can be read from the index, so that
  // the optimizer can use the index to estimate the number of rows in a range.
  //
  // HA_KEY_SCAN_NOT_ROR - to signal if the index returns records in rowid
  // order. Used to disable use of the index in the range optimizer if it is not
  // in rowid order.
  if (pushed_idx_cond) {
  }
  // Inherit the following index flags from the primary handler, if they are
  // set:
  //
  // HA_READ_RANGE - to signal that ranges can be read from the index, so that
  // the optimizer can use the index to estimate the number of rows in a range.
  //
  // HA_KEY_SCAN_NOT_ROR - to signal if the index returns records in rowid
  // order. Used to disable use of the index in the range optimizer if it is not
  // in rowid order.

  return ((HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN | HA_READ_RANGE |
           HA_KEY_SCAN_NOT_ROR) &
          primary_flags);
}

int ha_rapid::records(ha_rows *num_rows) {
  std::string sch_tb;
  sch_tb.append(table_share->db.str).append(":").append(table_share->table_name.str);
  auto rpd_tb = Imcs::Imcs::instance()->get_rpd_table(sch_tb);
  *num_rows = rpd_tb->meta().total_rows;

  return ShannonBase::SHANNON_SUCCESS;
}

ha_rows ha_rapid::records_in_range(unsigned int index, key_range *min_key, key_range *max_key) {
  // Get the number of records in the range from the primary storage engine.
  return ha_get_primary_handler()->records_in_range(index, min_key, max_key);
}

double ha_rapid::scan_time() {
  DBUG_TRACE;

  const double t = (stats.records + stats.deleted) * Optimizer::Rapid_SE_cost_constants::MEMORY_BLOCK_READ_COST;
  return t;
}

THR_LOCK_DATA **ha_rapid::store_lock(THD *, THR_LOCK_DATA **to, thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && m_lock.type == TL_UNLOCK) m_lock.type = lock_type;
  *to++ = &m_lock;
  return to;
}

int ha_rapid::load_table(const TABLE &table_arg, bool *skip_metadata_update [[maybe_unused]]) {
  ut_a(table_arg.file != nullptr);
  ut_ad(table_arg.s != nullptr);

  // between the tables loaded into rapid engine for log parser thread. perhaps, some new indexes are added into,
  // therefore, we reload the indexes caches at each table loaded into to refresh the global indexes cache.
  ShannonBase::Populate::Populator::load_indexes_caches();

  if (shannon_loaded_tables->get(table_arg.s->db.str, table_arg.s->table_name.str) != nullptr) {
    std::string err;
    err.append(table_arg.s->db.str).append(".").append(table_arg.s->table_name.str).append(" already loaded");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
    return HA_ERR_KEY_NOT_FOUND;
  }

  if (table_arg.s->is_missing_primary_key()) {
    std::string err;
    err.append(table_arg.s->db.str)
        .append(".")
        .append(table_arg.s->table_name.str)
        .append(" requires PK for loading into rapid");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
    return HA_ERR_KEY_NOT_FOUND;
  }

  for (auto idx = 0u; idx < table_arg.s->fields; idx++) {
    auto fld = *(table_arg.field + idx);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    if (!ShannonBase::Utils::Util::is_support_type(fld->type())) {
      std::string err;
      err.append(table_arg.s->table_name.str).append(fld->field_name).append(" type not allowed");
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return HA_ERR_GENERIC;
    }
  }

  m_thd->set_sent_row_count(0);
  // start to read data from innodb and load to rapid.
  ShannonBase::Rapid_load_context context;
  context.m_thd = m_thd;
  context.m_table = const_cast<TABLE *>(&table_arg);
  context.m_schema_name = table_arg.s->db.str;
  context.m_table_name = table_arg.s->table_name.str;
  context.m_sch_tb_name = context.m_schema_name + ":" + context.m_table_name;
  context.m_extra_info.m_keynr = active_index;
  context.m_extra_info.m_key_len = table_arg.file->ref_length;
  context.m_trx = Transaction::get_or_create_trx(m_thd);

  context.m_extra_info.m_trxid = context.m_trx->get_id();
  // at loading step, to set SCN to non-zero, it means it committed after inserted with explicit begin/commit.
  context.m_extra_info.m_scn = TransactionCoordinator::instance().allocate_scn();

  if (Imcs::Imcs::instance()->load_table(&context, const_cast<TABLE *>(&table_arg))) {
    std::string err;
    err.append(table_arg.s->db.str).append(".").append(table_arg.s->table_name.str).append(" load failed");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
    return HA_ERR_GENERIC;
  }

  m_share = new RapidShare(table_arg);
  m_share->file = this;
  m_share->m_tableid = table_arg.s->table_map_id.id();
  shannon_loaded_tables->add(table_arg.s->db.str, table_arg.s->table_name.str, m_share);
  if (shannon_loaded_tables->get(table_arg.s->db.str, table_arg.s->table_name.str) == nullptr) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), table_arg.s->db.str, table_arg.s->table_name.str);
    return HA_ERR_KEY_NOT_FOUND;
  }

  // add the mete info into 'rpd_column_id' and 'rpd_columns tables', etc.
  // to check whether it has been loaded or not. here, we dont use field_ptr != nullptr
  // because the ghost column.
  for (auto index = 0u; index < table_arg.s->fields; index++) {
    auto field_ptr = *(table_arg.field + index);
    // Skip columns marked as NOT SECONDARY.
    if ((field_ptr)->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    ShannonBase::rpd_column_info_t row_rpd_columns;
    strncpy(row_rpd_columns.schema_name, table_arg.s->db.str, table_arg.s->db.length);
    row_rpd_columns.table_id = static_cast<uint>(table_arg.s->table_map_id.id());
    row_rpd_columns.column_id = field_ptr->field_index();
    strncpy(row_rpd_columns.column_name, field_ptr->field_name, sizeof(row_rpd_columns.column_name) - 1);
    strncpy(row_rpd_columns.table_name, table_arg.s->table_name.str, sizeof(row_rpd_columns.table_name) - 1);
    auto key_name =
        ShannonBase::Utils::Util::get_key_name(table_arg.s->db.str, table_arg.s->table_name.str, field_ptr->field_name);
#if 0  // TODO: refact
    ShannonBase::Compress::Dictionary* dict =
      ShannonBase::Imcs::Imcs::instance()->get_cu(key_name)->get_header()->m_local_dict.get();
    if (dict)
      row_rpd_columns.data_dict_bytes = dict->content_size();
    row_rpd_columns.data_placement_index = 0;
#endif
    std::string comment(field_ptr->comment.str);
    memset(row_rpd_columns.encoding, 0x0, NAME_LEN);
    if (comment.find("SORTED") != std::string::npos)
      strncpy(row_rpd_columns.encoding, "SORTED", strlen("SORTED") + 1);
    else if (comment.find("VARLEN") != std::string::npos)
      strncpy(row_rpd_columns.encoding, "VARLEN", strlen("VARLEN") + 1);
    else
      strncpy(row_rpd_columns.encoding, "N/A", strlen("N/A") + 1);
    row_rpd_columns.ndv = 0;
    ShannonBase::rpd_columns_info.push_back(row_rpd_columns);
  }

  // start population thread if table loaded successfully.
  ShannonBase::Populate::Populator::start();
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::unload_table(const char *db_name, const char *table_name, bool error_if_not_loaded) {
  // stop the table worker thread.
  ShannonBase::Populate::Populator::unload(db_name, table_name);

  RapidShare *share = shannon_loaded_tables->get(db_name, table_name);
  if (error_if_not_loaded && !share) {
    std::string err(db_name);
    err.append(".").append(table_name).append(" table is not loaded into rapid yet");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
    return HA_ERR_GENERIC;
  }

  ShannonBase::Rapid_load_context context;
  context.m_table = share->m_source_table ? const_cast<TABLE *>(share->m_source_table) : nullptr;
  context.m_thd = m_thd;
  context.m_extra_info.m_keynr = active_index;
  context.m_schema_name = db_name;
  context.m_table_name = table_name;

  Imcs::Imcs::instance()->unload_table(&context, db_name, table_name, false);

  // ease the meta info.
  for (ShannonBase::rpd_columns_container::iterator it = ShannonBase::rpd_columns_info.begin();
       it != ShannonBase::rpd_columns_info.end();) {
    if (!strcmp(db_name, it->schema_name) && !strcmp(table_name, it->table_name))
      it = ShannonBase::rpd_columns_info.erase(it);
    else
      ++it;
  }

  shannon_loaded_tables->erase(db_name, table_name);

  // to try stop main thread, if there're no tables loaded.
  if (!shannon_loaded_tables->size()) ShannonBase::Populate::Populator::end();

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

  auto trx = ShannonBase::Transaction::get_or_create_trx(thd);
  rapid_register_tx(ShannonBase::shannon_rapid_hton_ptr, thd, trx);

  return ShannonBase::SHANNON_SUCCESS;
}

/** Initialize a table scan.
@param[in]      scan    whether this is a second call to rnd_init()
                        without rnd_end() in between
@return 0 or error number */
int ha_rapid::rnd_init(bool scan) {
  if (m_cursor->init()) {
    return HA_ERR_GENERIC;
  }

  inited = handler::RND;
  return ShannonBase::SHANNON_SUCCESS;
}

/** Ends a table scan.
 @return 0 or error number */

int ha_rapid::rnd_end(void) {
  if (m_cursor->end()) return HA_ERR_GENERIC;

  inited = handler::NONE;
  return ShannonBase::SHANNON_SUCCESS;
}

/** Reads the next row in a table scan (also used to read the FIRST row
 in a table scan).
 @return 0, HA_ERR_END_OF_FILE, or error number */
int ha_rapid::rnd_next(uchar *buf) {
  int error{HA_ERR_END_OF_FILE};

  if (inited == handler::RND) {
    if (table_share->fields <= static_cast<uint>(ShannonBase::rpd_async_column_threshold)) {
      error = m_cursor->next(buf);
    } else {
      auto reader_pool = ShannonBase::Imcs::Imcs::pool();
      std::future<int> fut = boost::asio::co_spawn(*reader_pool, m_cursor->next_async(buf), boost::asio::use_future);
      error = fut.get();  // co_await m_data_table->next_async(buf);  // index_first(buf);
      if (error == HA_ERR_KEY_NOT_FOUND) {
        error = HA_ERR_END_OF_FILE;
      }
    }
  }

  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return error;
}

int ha_rapid::index_init(uint keynr, bool sorted) {
  DBUG_TRACE;

  if (m_cursor->index_init(keynr, sorted)) {
    return HA_ERR_GENERIC;
  }

  active_index = keynr;
  inited = handler::INDEX;
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::index_end() {
  DBUG_TRACE;

  if (m_cursor->index_end()) return HA_ERR_GENERIC;

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

  int error;
  if (end_range) {
    m_cursor->set_end_range(end_range);
    error = m_cursor->index_read(buf, end_range->key, end_range->length, end_range->flag);
  } else
    error = m_cursor->index_next(buf);
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_first_count);
  return error;
}

int ha_rapid::index_prev(uchar *buf) {
  ut_a(false);  // not supported now.
  return ShannonBase::SHANNON_SUCCESS;
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
  return handler::read_range_first(start_key, end_key, eq_range_arg, sorted);
}

int ha_rapid::read_range_next() { return (handler::read_range_next()); }

}  // namespace ShannonBase

static bool rpd_thd_trx_is_auto_commit(THD *thd) { /*!< in: thread handle, can be NULL */
  return (thd != nullptr && !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN));
}

static void rapid_register_tx(handlerton *const hton, THD *const thd, ShannonBase::Transaction *const trx) {
  ut_a(trx != nullptr);

  trans_register_ha(thd, false, ShannonBase::shannon_rapid_hton_ptr, nullptr);

  if (!rpd_thd_trx_is_auto_commit(thd)) {
    trx->begin_stmt(ShannonBase::Transaction::get_rpd_isolation_level(thd));  // tx->stat_stmt()
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
  auto *trx = ShannonBase::Transaction::get_trx_from_thd(thd);

  if (trx != nullptr) {
    if (commit_trx || trx->is_auto_commit()) {
      /*
        We get here
         - For a COMMIT statement that finishes a multi-statement transaction
         - For a statement that has its own transaction
      */
      if (trx->commit()) {
        return HA_ERR_ERRORS;
      }
    } else {
      /*
        We get here when committing a statement within a transaction.
      */
    }

    if (trx->isolation_level() <= ShannonBase::Transaction::ISOLATION_LEVEL::READ_COMMITTED) {
      // For READ_COMMITTED, we release any existing snapshot so that we will
      // see any changes that occurred since the last statement.
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

  auto *trx = ShannonBase::Transaction::get_trx_from_thd(thd);
  if (trx != nullptr) {
    if (rollback_trx) {
      /*
        We get here, when
        - ROLLBACK statement is issued.

        Discard the changes made by the transaction
      */
      trx->rollback();
    } else {
      /*
        We get here when
        - a statement with AUTOCOMMIT=1 is being rolled back (because of some
          error)
        - a statement inside a transaction is rolled back
      */

      trx->rollback_stmt();
    }

    if (trx->isolation_level() <= ShannonBase::Transaction::ISOLATION_LEVEL::READ_COMMITTED) {
      // For READ_COMMITTED, we release any existing snapshot so that we will
      // see any changes that occurred since the last statement.
      trx->release_snapshot();
    }
  }

  return ShannonBase::SHANNON_SUCCESS;
}

/**
 * Prepares for two-phase commit
 * @param hton Handlerton pointer
 * @param thd THD pointer
 * @param all true = prepare entire transaction, false = prepare statement-level transaction
 */
static int rapid_prepare(handlerton *hton, THD *thd, bool all) {
  DBUG_TRACE;
  DBUG_PRINT("rapid_prepare", ("all: %d", all));

  ut_a(hton == ShannonBase::shannon_rapid_hton_ptr);

  // For the Rapid engine, we rely on InnoDB's two-phase commit
  // Can simply return success here
  // If independent two-phase commit implementation is needed in the future,
  // logic can be added here

  auto *trx = ShannonBase::Transaction::get_trx_from_thd(thd);
  if (trx == nullptr) return ShannonBase::SHANNON_SUCCESS;

  if (all) {
    DBUG_PRINT("rapid_prepare", ("preparing transaction %lu", trx->get_id()));
    // If needed, can set transaction status to PREPARED here
    // trx->set_status(Transaction::STATUS::PREPARED);
  }

  return ShannonBase::SHANNON_SUCCESS;
}

/** Creates an Rapid transaction struct for the thd if it does not yet have
 one. Starts a new Rapid transaction if a transaction is not yet started. And
 assigns a new snapshot for a consistent read if the transaction does not yet
 have one. Here, Rapid transaction is identical with innodb transaction.
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

  if (!trx->is_active()) {  // maybe the some error in primary engine. it should be started. here,in case.
    trx->begin(ShannonBase::Transaction::get_rpd_isolation_level(thd));
    if (trx->isolation_level() == ShannonBase::Transaction::ISOLATION_LEVEL::READ_REPEATABLE)
      trx->acquire_snapshot();
    else
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
static int rapid_savepoint(handlerton *const hton, THD *const thd, void *const savepoint) { return 0; }

static int rapid_rollback_to_savepoint(handlerton *const hton, THD *const thd, void *const savepoint) {
  auto *trx = ShannonBase::Transaction::get_trx_from_thd(thd);
  return trx->rollback_to_savepoint(savepoint);
}

static bool rapid_rollback_to_savepoint_can_release_mdl(handlerton *const hton, THD *const thd) { return true; }

/** Frees a possible trx object associated with the current THD.
 @return 0 or error number */
static int rapid_close_connection(handlerton *hton, /*!< in: handlerton */
                                  THD *thd) {       /*!< in: handle to the MySQL thread of the user
                                                   whose resources should be free'd */
  DBUG_TRACE;
  ut_a(hton == ShannonBase::shannon_rapid_hton_ptr);
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

static inline bool SetSecondaryEngineOffloadFailedReason(const THD *thd, std::string_view msg) {
  ut_a(thd);
  std::string msg_str(msg);
  thd->lex->m_secondary_engine_offload_or_exec_failed_reason = msg_str;

  my_error(ER_SECONDARY_ENGINE, MYF(0), msg_str.c_str());
  return ShannonBase::SHANNON_SUCCESS;
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
  if (!dd::get_dictionary()->is_dd_schema_name(db) && !dd::get_dictionary()->is_system_table_name(db, table_name) &&
      create_info->secondary_engine.str) {
    auto &self_load_inst = ShannonBase::Autopilot::SelfLoadManager::instance();
    self_load_inst->add_table(db, table_name, create_info->secondary_engine.str);
  }
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
  const uint field_count = table->s->fields;
  Field **fields = table->field;
  for (uint idx = 0; idx < field_count; idx++) {
    Field *fld = fields[idx];
    uchar *base_ptr = fld->field_ptr();
    if (likely(((fld->type() != MYSQL_TYPE_BLOB) && (fld->type() != MYSQL_TYPE_TINY_BLOB) &&
                (fld->type() != MYSQL_TYPE_MEDIUM_BLOB) && (fld->type() != MYSQL_TYPE_LONG_BLOB))))
      continue;

    auto bfld = down_cast<Field_blob *>(fld);
    uint pack_len = bfld->pack_length_no_ptr();
    size_t data_len = 0;
    switch (pack_len) {
      case 1:
        data_len = *base_ptr;
        break;
      case 2:
        data_len = uint2korr(base_ptr);
        break;
      case 3:
        data_len = uint3korr(base_ptr);
        break;
      case 4:
        data_len = uint4korr(base_ptr);
        break;
      default:
        continue;
    }
    uchar *blob_data_ptr = base_ptr + pack_len;
    uchar *actual_blob_data = nullptr;
    memcpy(&actual_blob_data, blob_data_ptr, sizeof(uchar *));
    if (actual_blob_data && data_len > 0) {
      std::shared_ptr<uchar[]> blob_copy(new uchar[data_len]);
      std::memcpy(blob_copy.get(), actual_blob_data, data_len);
      off_page_data.emplace(idx, std::make_pair(data_len, std::move(blob_copy)));
    }
  }
}

// To after insrt into primary engine, this function will be invoked. Then, you can get all chages from
// table->record[0], table->record[1] and COPY_INFO, etc. After that you can insert these changes to rapid. The other
// way is we use now, the redo log.
void NotifyAfterInsert(THD *thd, void *args) {
  if (!thd || !args || ShannonBase::Populate::g_sync_mode.load() == ShannonBase::Populate::SyncMode::REDO_LOG_PARSE)
    return;
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

  std::string sch_tb_name = table->s->db.str;
  sch_tb_name.append(":").append(table->s->table_name.str);
  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_rpd_table(sch_tb_name);
  if (ShannonBase::Populate::Populator::active() && rpd_table) {
    ShannonBase::Populate::change_record_buff_t copy_info_rec(ShannonBase::Populate::Source::COPY_INFO,
                                                              table->s->rec_buff_length);
    copy_info_rec.m_oper = ShannonBase::Populate::change_record_buff_t::OperType::INSERT;
    copy_info_rec.m_schema_name = table->s->db.str;
    copy_info_rec.m_table_name = table->s->table_name.str;
    lsn_t current_lsn = log_get_lsn(*log_sys);
    std::memcpy(copy_info_rec.m_buff0.get(), table->record[0], table->s->rec_buff_length);
    // read and store off-page data.
    read_off_page_data(table, copy_info_rec.m_offpage_data0);
    ShannonBase::Populate::Populator::write(nullptr, current_lsn, &copy_info_rec);
  }
}

// old_row = table->record[1], new_row = table->record[0]
void NotifyAfterUpdate(THD *thd, void *args) {
  if (!thd || !args || ShannonBase::Populate::g_sync_mode.load() == ShannonBase::Populate::SyncMode::REDO_LOG_PARSE)
    return;
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

  std::string sch_tb_name = table->s->db.str;
  sch_tb_name.append(":").append(table->s->table_name.str);
  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_rpd_table(sch_tb_name);
  if (ShannonBase::Populate::Populator::active() && rpd_table) {
    ShannonBase::Populate::change_record_buff_t copy_info_rec(ShannonBase::Populate::Source::COPY_INFO,
                                                              table->s->rec_buff_length);
    copy_info_rec.m_oper = ShannonBase::Populate::change_record_buff_t::OperType::UPDATE;
    copy_info_rec.m_schema_name = table->s->db.str;
    copy_info_rec.m_table_name = table->s->table_name.str;

    lsn_t current_lsn = log_get_lsn(*log_sys);
    std::memcpy(copy_info_rec.m_buff0.get(), old_row, table->s->rec_buff_length);
    if (new_row) {
      std::memcpy(copy_info_rec.m_buff1.get(), new_row, table->s->rec_buff_length);
      read_off_page_data(table, copy_info_rec.m_offpage_data0);
    }
    ShannonBase::Populate::Populator::write(nullptr, current_lsn, &copy_info_rec);
  }
}

void NotifyAfterDelete(THD *thd, void *args) {
  if (!thd || !args || ShannonBase::Populate::g_sync_mode.load() == ShannonBase::Populate::SyncMode::REDO_LOG_PARSE)
    return;
  struct comb_args {
    TABLE *arg1;
    const uchar *old_rec;
  };

  auto params = static_cast<comb_args *>(args);
  if (!params) return;

  auto table = params->arg1;
  auto old_row = params->old_rec;

  if (!table || !old_row) return;

  std::string sch_tb_name = table->s->db.str;
  sch_tb_name.append(":").append(table->s->table_name.str);
  auto rpd_table = ShannonBase::Imcs::Imcs::instance()->get_rpd_table(sch_tb_name);
  if (ShannonBase::Populate::Populator::active() && rpd_table) {
    ShannonBase::Populate::change_record_buff_t copy_info_rec(ShannonBase::Populate::Source::COPY_INFO,
                                                              table->s->rec_buff_length);
    copy_info_rec.m_oper = ShannonBase::Populate::change_record_buff_t::OperType::DELETE;
    copy_info_rec.m_schema_name = table->s->db.str;
    copy_info_rec.m_table_name = table->s->table_name.str;

    lsn_t current_lsn = log_get_lsn(*log_sys);
    std::memcpy(copy_info_rec.m_buff0.get(), old_row, table->s->rec_buff_length);

    read_off_page_data(table, copy_info_rec.m_offpage_data0);
    ShannonBase::Populate::Populator::write(nullptr, current_lsn, &copy_info_rec);
  }
}

void NotifyAfterSelect(THD *thd, SelectExecutedIn executed_in) {
  if (executed_in == SelectExecutedIn::kPrimaryEngine) return;

  if (!thd || !thd->lex) {
    return;
  }

  double query_cost = 0.0f;
  if (thd->lex->query_block && thd->lex->query_block->join && thd->lex->query_block->join->best_read) {
    query_cost = thd->lex->query_block->join->best_read *
                 (ShannonBase::SHANNON_HD_READ_FACTOR + ShannonBase::SHANNON_RAM_READ_FACTOR);
  }

  double cost_threshold = thd->variables.secondary_engine_cost_threshold;
  if (query_cost <= cost_threshold) {  // update only if query coast is higher than threshold.
    return;
  }

  auto &self_load_inst = ShannonBase::Autopilot::SelfLoadManager::instance();
  self_load_inst->update_table_stats(thd, thd->lex->query_tables, executed_in);
}

// In this function, Dynamic offload combines mysql plan features
// retrieved from rapid_statement_context
// and RAPID info such as rapid base table cardinality,
// dict encoding projection, varlen projection size, rapid queue
// size in to decide if query should be offloaded to RAPID.
// returns true, goes to innodb for execution.
// returns false, goes to next phase for secondary engine execution.
static bool RapidPrepareEstimateQueryCosts(THD *thd, LEX *lex) {
  if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_OFF) {
    SetSecondaryEngineOffloadFailedReason(thd, "use_secondary_engine set to off");
    return true;
  } else if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_FORCED)
    return false;

  auto shannon_statement_context = thd->secondary_engine_statement_context();
  auto primary_plan_info = shannon_statement_context->get_cached_primary_plan_info();
  ut_a(primary_plan_info);

  // 1: to check whether there're changes in sys_pop_buff, which will be used for query.
  // if there're still do populating, then goes to innodb. and gets cardinality of tables.
  ut_a(thd->variables.use_secondary_engine != SECONDARY_ENGINE_FORCED);
  for (auto &table_ref : shannon_statement_context->get_query_tables()) {
    std::string table_name(table_ref->db);
    table_name.append(":").append(table_ref->table_name);
    if (ShannonBase::Populate::Populator::mark_table_required(table_name)) return true;
  }

  // 2: to check whether the sys_pop_data_sz has too many data to populate.
  uint64 too_much_pop_threshold =
      static_cast<uint64_t>(ShannonBase::SHANNON_TO_MUCH_POP_THRESHOLD_RATIO * ShannonBase::rpd_pop_buff_sz_max);
  if (ShannonBase::Populate::sys_pop_data_sz > too_much_pop_threshold) {
    SetSecondaryEngineOffloadFailedReason(thd, "too much changes need to populate");
    return true;
  }

  // 3: checks dict encoding projection, and varlen project size, etc.
  if (ShannonBase::Utils::Util::check_dict_encoding_projection(thd)) {
    SetSecondaryEngineOffloadFailedReason(thd, "dict encoding is not supported");
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

  /* Disable use of constant tables and evaluation of subqueries during
   * optimization. But, it is not to producce an optima count query plan.
   * if we enable the following statement, in JOIN::optimnze, it can not
   * optimize the aggregate statement. such as:  count(*), min() and max() to const fields if
   * there is implicit grouping (aggregate functions but no group_list).
   * In this case, the result set shall only contain one row. ref to:
   *  if (tables_list && implicit_grouping &&
   *    !(query_block->active_options() & OPTION_NO_CONST_TABLES)) {
   *    aggregate_evaluated outcome;
   *    if (optimize_aggregated_query(thd, query_block, *fields, where_cond,
   *                                &outcome)) {
   *  If enable it, will use aggregate access path to the result of `count(*), min() and max()`.
   */

  lex->add_statement_options(OPTION_NO_CONST_TABLES | OPTION_NO_SUBQUERY_DURING_OPTIMIZATION);

  return RapidPrepareEstimateQueryCosts(thd, lex);
}

// caches primary info. Here, we have done optimization with primary engine, and it
// will retry with secondary engine, and therefore, be here. JOIN are available here,
// and, we can get all optimzation information, then caches all these info.
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

// If dynamic offload is enabled and query is not "very fast":
// This caches features from mysql plan in rapid_statement_context
// to be used for dynamic offload.
// If dynamic offload is disabled This function invokes standary mysql
// cost threshold classifier, which decides if query needs further
// RAPID optimisation. the query is "very fast" does not care about dynamic
// offload flag.
// returns true, goes to secondary engine, otherwise, goes to innodb.
bool SecondaryEnginePrePrepareHook(THD *thd) {
  RapidCachePrimaryInfoAtPrimaryTentativelyStep(thd);

  if (unlikely(!thd->variables.rapid_use_dynamic_offload || is_very_fast_query(thd))) {
    // invokes standary mysql cost threshold classifier, which decides if query needs further RAPID optimisation.
    return ShannonBase::Utils::Util::standard_cost_threshold_classifier(thd);
  } else if (likely(thd->variables.rapid_use_dynamic_offload && !is_very_fast_query(thd))) {
    // 1: static sceanrio.
    if (likely(!ShannonBase::Populate::Populator::active() ||
               (ShannonBase::Populate::Populator::active() && ShannonBase::Populate::sys_pop_buff.empty()))) {
      return ShannonBase::Utils::Util::decision_tree_classifier(thd);
    } else {
      // 2: dynamic scenario.
      return ShannonBase::Utils::Util::dynamic_feature_normalization(thd);
    }
  } else
    ut_a(false);

  // go to innodb for execution.
  return false;
}

static void AssertSupportedPath(const AccessPath *path) {
  switch (path->type) {
    // The only supported join type is hash join. Other join types are disabled
    // in handlerton::secondary_engine_flags.
    case AccessPath::NESTED_LOOP_JOIN: /* purecov: deadcode */
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
    case AccessPath::BKA_JOIN:
    // Index access is disabled in ha_rapid::table_flags(), so we should see
    // none of these access types.
    case AccessPath::INDEX_SCAN:
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::EQ_REF:
    case AccessPath::PUSHED_JOIN_REF:
    case AccessPath::INDEX_RANGE_SCAN:
    case AccessPath::INDEX_SKIP_SCAN:
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
    case AccessPath::ROWID_INTERSECTION:
    case AccessPath::ROWID_UNION:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      ut_a(false); /* purecov: deadcode */
      break;
    default:
      break;
  }

  // This secondary storage engine does not yet store anything in the auxiliary
  // data member of AccessPath.
  ut_a(path->secondary_engine_data == nullptr);
}

//  In this function, Dynamic offload retrieves info from rapid_statement_context and
// additionally looks at Change  propagation lag to decide if query should be offloaded
// to rapid returns true, goes to innodb engine. otherwise, false, goes to secondary engine.
static bool RapidOptimize(ShannonBase::Optimizer::OptimizeContext *context, THD *thd, LEX *lex) {
  if (likely(thd->variables.use_secondary_engine == SECONDARY_ENGINE_OFF)) {
    SetSecondaryEngineOffloadFailedReason(thd, "RapidOptimize, set use_secondary_engine to false");
    return true;
  }

  // auto statement_context = thd->secondary_engine_statement_context();
  // to much changes to populate, then goes to primary engine.
  ulonglong too_much_pop_threshold =
      static_cast<ulonglong>(ShannonBase::SHANNON_TO_MUCH_POP_THRESHOLD_RATIO * ShannonBase::rpd_pop_buff_sz_max);
  if (unlikely(ShannonBase::Populate::sys_pop_buff.size() > ShannonBase::SHANNON_POP_BUFF_THRESHOLD_COUNT ||
               ShannonBase::Populate::sys_pop_data_sz > too_much_pop_threshold)) {
    SetSecondaryEngineOffloadFailedReason(thd, "RapidOptimize, the change propation lag is too much");
    return true;
  }

  if (lex->unit && !lex->unit->is_optimized()) {
    if (lex->unit->optimize(thd, nullptr, true, true)) return true;
  }

  JOIN *join = lex->unit->first_query_block()->join;
  lex->unit->root_access_path() = ShannonBase::Optimizer::WalkAndRewriteAccessPaths(
      lex->unit->root_access_path(), join, WalkAccessPathPolicy::ENTIRE_TREE,
      [&](AccessPath *path, const JOIN *join) -> AccessPath * {
        return ShannonBase::Optimizer::Optimizer::OptimizeAndRewriteAccessPath(context, path, join);
      });
  // Here, because we cannot get the parent node of corresponding iterator, we reset the type of access
  // path, then re-generates all the iterators. But, it makes the preformance regression for a `short`
  // AP workload. But, we will replace the itertor when we traverse iterator tree from root to leaves.
  lex->unit->release_root_iterator().reset();

  auto new_root_iter = ShannonBase::Optimizer::PathGenerator::PathGenerator::CreateIteratorFromAccessPath(
      thd, context, lex->unit->root_access_path(), join, /*eligible_for_batch_mode=*/true);

  lex->unit->set_root_iterator(new_root_iter);
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

  if (lex->using_hypergraph_optimizer()) {
    WalkAccessPaths(lex->unit->root_access_path(), nullptr, WalkAccessPathPolicy::ENTIRE_TREE,
                    [](AccessPath *path, const JOIN *) {
                      AssertSupportedPath(path);
                      return false;
                    });
  }

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

  // Check if the calculated cost is cheaper than the best cost seen so far.
  *cheaper = down_cast<ShannonBase::Rapid_execution_context *>(thd->lex->secondary_engine_execution_context())
                 ->BestPlanSoFar(join, *secondary_engine_cost);

  return false;
}

static bool ModifyAccessPathCost(THD *thd [[maybe_unused]], const JoinHypergraph &hypergraph [[maybe_unused]],
                                 AccessPath *path) {
  ut_a(!thd->is_error());
  ut_a(hypergraph.query_block()->join == hypergraph.join());
  AssertSupportedPath(path);
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

/**
 * Rapid engine system variables to control the behavior of Rapid Engine, such as the max memory used, etc.
 */
static SHOW_VAR rapid_status_variables[] = {
    /*the max memory used for rapid.*/
    {"rapid_memory_size_max", (char *)&ShannonBase::rpd_mem_sz_max, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    /*the max size of pop buffer size.*/
    {"rapid_pop_buffer_size_max", (char *)&ShannonBase::rpd_pop_buff_sz_max, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    /*the max row number of used to enable parallel load for secondary_load*/
    {"rapid_parallel_load_max", (char *)&ShannonBase::rpd_para_load_threshold, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    /*the max column number of used to aysnc reading or parsing log*/
    {"rapid_async_column_threshold", (char *)&ShannonBase::rpd_async_column_threshold, SHOW_INT, SHOW_SCOPE_GLOBAL},
    /*to enable self load or disable*/
    {"rapid_self_load_enabled", (char *)&ShannonBase::rpd_self_load_enabled, SHOW_BOOL, SHOW_SCOPE_GLOBAL},
    /*the interval value of self load in second*/
    {"rapid_self_load_interval_seconds", (char *)&ShannonBase::rpd_self_load_interval_seconds, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    /*to skip the quiet check or not*/
    {"rapid_self_load_skip_quiet_check", (char *)&ShannonBase::rpd_self_load_skip_quiet_check, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    /*the value of fill percentage of main memory*/
    {"rapid_self_load_base_relation_fill_percentage", (char *)&ShannonBase::rpd_self_load_base_relation_fill_percentage,
     SHOW_INT, SHOW_SCOPE_GLOBAL},
    /*to enable self load or disable*/
    {"rapid_max_purger_timeout", (char *)&ShannonBase::rpd_max_purger_timeout, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"rapid_purge_batch_size", (char *)&ShannonBase::rpd_purge_batch_size, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"rapid_min_versions_for_purge", (char *)&ShannonBase::rpd_min_versions_for_purge, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"rapid_purge_efficiency_threshold", (char *)&ShannonBase::rpd_purge_efficiency_threshold, SHOW_DOUBLE,
     SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_INT, SHOW_SCOPE_GLOBAL}};

/** Callback function for accessing the Rapid variables from MySQL:  SHOW
 * VARIABLES. */
static int show_rapid_vars(THD *, SHOW_VAR *var, char *) {
  // gets the latest variables of shannonbase rapid.
  var->type = SHOW_ARRAY;
  var->value = (char *)&rapid_status_variables;
  var->scope = SHOW_SCOPE_GLOBAL;

  return (ShannonBase::SHANNON_SUCCESS);
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

  if (value->val_int(value, &input_val)) {
    return HA_ERR_GENERIC;
  }

  if (input_val < 1 || (uint)input_val > ShannonBase::SHANNON_DEFAULT_MEMRORY_SIZE) {
    return HA_ERR_GENERIC;
  }

  *static_cast<int *>(save) = static_cast<int>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_memory_size_max.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_mem_size_max_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  if (*static_cast<int *>(var_ptr) == *static_cast<const int *>(save)) return;

  if (ShannonBase::Populate::Populator::active() || ShannonBase::shannon_loaded_tables->size()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Tables have been loaded, cannot change the rapid params,"
             "must unload all loaded tables");
    return;
  }

  *static_cast<int *>(var_ptr) = *static_cast<const int *>(save);
  ShannonBase::rpd_mem_sz_max = *static_cast<const int *>(save);

  ShannonBase::g_rpd_memory_pool = nullptr;  // reset first, then reallocation the new size.
  ShannonBase::Utils::MemoryPool::Config config(ShannonBase::rpd_mem_sz_max);
  ShannonBase::g_rpd_memory_pool = std::make_shared<ShannonBase::Utils::MemoryPool>(config);
  if (!ShannonBase::g_rpd_memory_pool) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "resize the rapid memory size failed");
    return;
  }
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

  if (value->val_int(value, &input_val)) {
    return 1;
  }

  if (input_val < 1 || (uint)input_val > ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE) {
    return 1;
  }

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
  ShannonBase::rpd_pop_buff_sz_max = *static_cast<const int *>(save);
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
  ShannonBase::rpd_para_load_threshold = *static_cast<const int *>(save);
}

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
  ShannonBase::rpd_async_column_threshold = *static_cast<const int *>(save);
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

  if (value->val_int(value, &input_val)) {
    return 1;
  }

  if (input_val < 1 || input_val > ShannonBase::MAX_N_FIELD_PARALLEL) {
    return 1;
  }

  *static_cast<int *>(save) = static_cast<int>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

static void update_self_load_enabled(THD *, SYS_VAR *, void *var_ptr, const void *save) {
  bool new_value = *static_cast<const bool *>(save);
  *static_cast<bool *>(var_ptr) = new_value;
  ShannonBase::rpd_self_load_enabled = *static_cast<const bool *>(save);
  auto instance = ShannonBase::Autopilot::SelfLoadManager::instance();
  if (ShannonBase::rpd_self_load_enabled) {  // to start AutoLoader thread.
    if (!instance->initialized()) {
      instance->initialize();
      instance->start_self_load_worker();
    }
  } else {
    instance->stop_self_load_worker();
    instance->deinitialize();
  }
}

static void update_self_load_interval(THD *, SYS_VAR *, void *var_ptr, const void *save) {
  auto new_value = *static_cast<const int *>(save);
  *static_cast<int *>(var_ptr) = new_value;

  ShannonBase::rpd_self_load_interval_seconds = new_value;
}

static void update_skip_quiet_check(THD *, SYS_VAR *, void *var_ptr, const void *save) {
  bool new_value = *static_cast<const bool *>(save);
  *static_cast<bool *>(var_ptr) = new_value;

  ShannonBase::rpd_self_load_skip_quiet_check = new_value;
}

static void update_memory_fill_percentage(THD *, SYS_VAR *, void *var_ptr, const void *save) {
  int new_value = *static_cast<const int *>(save);
  *static_cast<int *>(var_ptr) = new_value;

  ShannonBase::rpd_self_load_base_relation_fill_percentage = new_value;
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
  if (value->val_int(value, &input_val)) {
    return 1;
  }

  if (input_val < ShannonBase::SHANNON_MIN_PURGER_TIMEOUT) {
    return 1;
  }

  *static_cast<ulong *>(save) = static_cast<ulong>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_max_purger_timeout.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_max_purger_timeout_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<ulong *>(var_ptr) == *static_cast<const ulong *>(save)) return;

  *static_cast<ulong *>(var_ptr) = *static_cast<const ulong *>(save);
  ShannonBase::rpd_max_purger_timeout = *static_cast<const ulong *>(save);
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
  if (value->val_int(value, &input_val)) {
    return 1;
  }

  if (input_val < ShannonBase::SHANNON_MIN_PURGE_BATCH_SIZE || input_val > ShannonBase::SHANNON_MAX_PURGE_BATCH_SIZE) {
    return 1;
  }

  *static_cast<ulong *>(save) = static_cast<ulong>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_purge_batch_size.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_purge_batch_size_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<ulong *>(var_ptr) == *static_cast<const ulong *>(save)) return;

  *static_cast<ulong *>(var_ptr) = *static_cast<const ulong *>(save);
  ShannonBase::rpd_purge_batch_size = *static_cast<const ulong *>(save);
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
  if (value->val_int(value, &input_val)) {
    return 1;
  }

  if (input_val < ShannonBase::SHANNON_MIN_PURGE_BATCH_SIZE || input_val > ShannonBase::SHANNON_MAX_PURGE_BATCH_SIZE) {
    return 1;
  }

  *static_cast<ulong *>(save) = static_cast<ulong>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_min_versions_for_purge.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_min_versions_for_purge_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<ulong *>(var_ptr) == *static_cast<const ulong *>(save)) return;

  *static_cast<ulong *>(var_ptr) = *static_cast<const ulong *>(save);
  ShannonBase::rpd_min_versions_for_purge = *static_cast<const ulong *>(save);
}

/** Update the system variable rpd_purge_efficiency_threshold.
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

  ShannonBase::rpd_purge_efficiency_threshold = in_val;
}

// clang-format off
static MYSQL_SYSVAR_ULONG(memory_size_max,
                          ShannonBase::rpd_mem_sz_max,
                          PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_PERSIST_AS_READ_ONLY,
                          "Number of memory size that used for rapid engine, and it must "
                          "not be oversize half of physical mem size.",
                          rpd_mem_size_max_validate,
                          rpd_mem_size_max_update,
                          ShannonBase::SHANNON_MAX_MEMRORY_SIZE,
                          ShannonBase::SHANNON_MAX_MEMRORY_SIZE,
                          ShannonBase::SHANNON_MAX_MEMRORY_SIZE,
                          0);

static MYSQL_SYSVAR_ULONGLONG(pop_buffer_size_max,
                              ShannonBase::rpd_pop_buff_sz_max,
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
                              ShannonBase::rpd_para_load_threshold,
                              PLUGIN_VAR_OPCMDARG,
                              "Max number of rows used to use parallel load for secondary_load "
                              "from innodb to rapid engine..",
                              rpd_para_load_threshold_validate,
                              rpd_para_load_threshold_update,
                              ShannonBase::SHANNON_PARALLEL_LOAD_THRESHOLD, //default val
                              0,  //min
                              ShannonBase::SHANNON_PARALLEL_LOAD_THRESHOLD, //max
                              0);

static MYSQL_SYSVAR_INT(async_column_threshold,
                        ShannonBase::rpd_async_column_threshold,
                        PLUGIN_VAR_OPCMDARG,
                        "Max number of columns will do async-corountine for reading data or parsing log ",
                        rpd_async_threshold_validate,
                        rpd_async_threshold_update,
                        ShannonBase::DEFAULT_N_FIELD_PARALLEL,
                        1,
                        ShannonBase::MAX_N_FIELD_PARALLEL,
                        0);

static MYSQL_SYSVAR_BOOL(self_load_enabled, 
                         ShannonBase::rpd_self_load_enabled,
                         PLUGIN_VAR_OPCMDARG,
                        "self-loaded, tables will not interfere with user-issued secondary loads under any "
                        "resource constraint. For example, if there is not enough memory in the "
                        "system for an incoming user load, some self-loaded tables will have to "
                        "be unloaded to make room for the newly user-loaded table. default value: false.",
                         nullptr,
                         update_self_load_enabled,
                         false);

static MYSQL_SYSVAR_ULONGLONG(self_load_interval_seconds,
                         ShannonBase::rpd_self_load_interval_seconds,
                         PLUGIN_VAR_OPCMDARG,
                         "Wake-up interval of the Self-Load thread "
                         "Default value: 86400s (24h). Note that if the interval is changed while "
                         "rapid_self_load_enabled=TRUE, the new value might not be picked up "
                         "until the next wakeup of the Self-Load Worker. Therefore, the recommended order of "
                         "setting the variables is: 1. rapid_self_load_interval_seconds=<new value>; "
                         "2. rapid_self_load_enabled=TRUE;",
                         nullptr,
                         update_self_load_interval,
                         86400/**24hrs */,
                         60 /*a hr*/,
                         86400 * 7/*a week */,
                         0);

static MYSQL_SYSVAR_BOOL(self_load_skip_quiet_check,
                         ShannonBase::rpd_self_load_skip_quiet_check,
                         PLUGIN_VAR_OPCMDARG,
                         "self-loaded, tables will not interfere with user-issued secondary loads under any "
                         "resource constraint. For example, if there is not enough memory in the "
                         "system for an incoming user load, some self-loaded tables will have to "
                         "be unloaded to make room for the newly user-loaded table. ",
                         nullptr,
                         update_skip_quiet_check,
                         false);

static MYSQL_SYSVAR_INT(self_load_base_relation_fill_percentage,
                         ShannonBase::rpd_self_load_base_relation_fill_percentage,
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
                              ShannonBase::rpd_max_purger_timeout,
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
                              ShannonBase::rpd_purge_batch_size,
                              PLUGIN_VAR_OPCMDARG,
                              "Process chunks in batches, number of chunks to process in a single purge batch",
                              rpd_purge_batch_size_validate,
                              rpd_purge_batch_size_update,
                              ShannonBase::SHANNON_DEFAULT_PURGE_BATCH_SIZE, // default val
                              ShannonBase::SHANNON_MIN_PURGE_BATCH_SIZE,  // min
                              ShannonBase::SHANNON_MAX_PURGE_BATCH_SIZE, // max
                              0);
                    
static MYSQL_SYSVAR_ULONGLONG(min_versions_for_purge,
                              ShannonBase::rpd_min_versions_for_purge,
                              PLUGIN_VAR_OPCMDARG,
                              "Minimum number of versions required for a chunk to be eligible for purging",
                              rpd_min_versions_for_purge_validate,
                              rpd_min_versions_for_purge_update,
                              ShannonBase::SHANNON_DEFAULT_MIN_VERSIONS_FOR_PURGE, // default val
                              ShannonBase::SHANNON_DEFAULT_MIN_VERSIONS_FOR_PURGE,  // min
                              ULLONG_MAX, // max
                              0);

static MYSQL_SYSVAR_DOUBLE(purge_efficiency_threshold,
                           ShannonBase::rpd_purge_efficiency_threshold,
                           PLUGIN_VAR_RQCMDARG,
                           "Purge efficiency threshold, only purge if >10% can be cleaned",
                           nullptr,
                           rpd_purge_efficiency_threshold_update, 
                           0.1,
                           0.1,
                           1,
                           0);

// clang-format on
static struct SYS_VAR *rapid_system_variables[] = {
    MYSQL_SYSVAR(memory_size_max),
    MYSQL_SYSVAR(pop_buffer_size_max),
    MYSQL_SYSVAR(parallel_load_max),
    MYSQL_SYSVAR(async_column_threshold),
    MYSQL_SYSVAR(self_load_enabled),
    MYSQL_SYSVAR(self_load_interval_seconds),
    MYSQL_SYSVAR(self_load_skip_quiet_check),
    MYSQL_SYSVAR(self_load_base_relation_fill_percentage),
    MYSQL_SYSVAR(max_purger_timeout),
    MYSQL_SYSVAR(purge_batch_size),
    MYSQL_SYSVAR(min_versions_for_purge),
    MYSQL_SYSVAR(purge_efficiency_threshold),
    nullptr,
};

static SHOW_VAR rapid_status_variables_export[] = {
    {"ShannonBase Rapid", (char *)&show_rapid_vars, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

static int Shannonbase_Rapid_Init(MYSQL_PLUGIN p) {
  ShannonBase::shannon_loaded_tables = new ShannonBase::LoadedTables();

  ShannonBase::Utils::MemoryPool::Config config(ShannonBase::rpd_mem_sz_max);
  ShannonBase::g_rpd_memory_pool = std::make_shared<ShannonBase::Utils::MemoryPool>(config);

  handlerton *shannon_rapid_hton = static_cast<handlerton *>(p);
  ShannonBase::shannon_rapid_hton_ptr = shannon_rapid_hton;
  shannon_rapid_hton->create = rapid_create_handler;
  shannon_rapid_hton->state = SHOW_OPTION_YES;
  shannon_rapid_hton->flags = HTON_IS_SECONDARY_ENGINE;
  shannon_rapid_hton->db_type = DB_TYPE_RAPID;
  shannon_rapid_hton->notify_create_table = NotifyCreateTable;
  shannon_rapid_hton->notify_after_insert = NotifyAfterInsert;
  shannon_rapid_hton->notify_after_update = NotifyAfterUpdate;
  shannon_rapid_hton->notify_after_delete = NotifyAfterDelete;

  shannon_rapid_hton->prepare_secondary_engine = PrepareSecondaryEngine;
  shannon_rapid_hton->secondary_engine_pre_prepare_hook = SecondaryEnginePrePrepareHook;
  shannon_rapid_hton->optimize_secondary_engine = OptimizeSecondaryEngine;
  shannon_rapid_hton->compare_secondary_engine_cost = CompareJoinCost;
  shannon_rapid_hton->secondary_engine_flags = MakeSecondaryEngineFlags(SecondaryEngineFlag::SUPPORTS_HASH_JOIN);
  shannon_rapid_hton->secondary_engine_modify_access_path_cost = ModifyAccessPathCost;
  shannon_rapid_hton->get_secondary_engine_offload_or_exec_fail_reason = GetSecondaryEngineOffloadorExecFailedReason;
  shannon_rapid_hton->set_secondary_engine_offload_fail_reason = SetSecondaryEngineOffloadFailedReason;
  shannon_rapid_hton->secondary_engine_check_optimizer_request = SecondaryEngineCheckOptimizerRequest;
  shannon_rapid_hton->notify_after_select = NotifyAfterSelect;

  shannon_rapid_hton->commit = rapid_commit;
  shannon_rapid_hton->rollback = rapid_rollback;
  shannon_rapid_hton->prepare = rapid_prepare;
  shannon_rapid_hton->start_consistent_snapshot = rapid_start_trx_and_assign_read_view;
  shannon_rapid_hton->savepoint_set = rapid_savepoint;
  shannon_rapid_hton->savepoint_rollback = rapid_rollback_to_savepoint;
  shannon_rapid_hton->savepoint_rollback_can_release_mdl = rapid_rollback_to_savepoint_can_release_mdl;
  shannon_rapid_hton->close_connection = rapid_close_connection;
  shannon_rapid_hton->kill_connection = rapid_kill_connection;
  shannon_rapid_hton->partition_flags = rapid_partition_flags;

  auto instance_ = ShannonBase::Imcs::Imcs::instance();
  if (!instance_) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "get IMCS instance");
    return 1;
  };

  return instance_->initialize();
}

static int Shannonbase_Rapid_Deinit(MYSQL_PLUGIN) {
  while (ShannonBase::Populate::Populator::active()) {
    ShannonBase::Populate::sys_pop_started.store(false);
    os_event_set(log_sys->rapid_events[0]);
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }

  if (ShannonBase::shannon_loaded_tables) {
    delete ShannonBase::shannon_loaded_tables;
    ShannonBase::shannon_loaded_tables = nullptr;
  }

  auto instance_ = ShannonBase::Imcs::Imcs::instance();
  return instance_->deinitialize();
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