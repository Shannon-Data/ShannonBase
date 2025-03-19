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
#include "sql/join_optimizer/make_join_hypergraph.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/opt_trace.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"
#include "storage/innobase/handler/ha_innodb.h"  //thd_to_trx
#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/imcs/data_table.h"      //DataTable
#include "storage/rapid_engine/imcs/imcs.h"            // IMCS
#include "storage/rapid_engine/include/rapid_const.h"  //const
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"  //column stats
#include "storage/rapid_engine/optimizer/optimizer.h"
#include "storage/rapid_engine/populate/populate.h"
#include "storage/rapid_engine/trx/transaction.h"  //transaction
#include "storage/rapid_engine/utils/utils.h"

#include "template_utils.h"
#include "thr_lock.h"

namespace dd {
class Table;
}

static void rapid_register_tx(handlerton *const hton, THD *const thd, ShannonBase::Transaction *const trx);

namespace ShannonBase {

MEM_ROOT rapid_mem_root(PSI_NOT_INSTRUMENTED, 1024);
handlerton *shannon_rapid_hton_ptr{nullptr};

LoadedTables *shannon_loaded_tables = nullptr;
uint64 rpd_mem_sz_max = ShannonBase::SHANNON_DEFAULT_MEMRORY_SIZE;
ulonglong rpd_pop_buff_sz_max = ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE;
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
void LoadedTables::add(const std::string &db, const std::string &table, RapidShare *rs) {
  std::lock_guard<std::mutex> guard(m_mutex);
  std::string keystr = db + ":" + table;
  m_tables.emplace(keystr, rs);
}

RapidShare *LoadedTables::get(const std::string &db, const std::string &table) {
  std::lock_guard<std::mutex> guard(m_mutex);
  std::string keystr = db + ":" + table;
  auto it = m_tables.find(keystr);
  return it == m_tables.end() ? nullptr : it->second;
}

void LoadedTables::erase(const std::string &db, const std::string &table) {
  std::lock_guard<std::mutex> guard(m_mutex);
  std::string keystr = db + ":" + table;
  if (m_tables.find(keystr) == m_tables.end()) return;

  auto sh = m_tables[keystr];
  if (sh) {
    delete sh;
    sh = nullptr;
  }
  m_tables.erase(keystr);
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

  ut_a(!m_data_table.get());
  m_data_table.reset(new ShannonBase::Imcs::DataTable(table));
  m_data_table->open();
  return 0;
}

int ha_rapid::close() {
  m_data_table->close();
  return 0;
}

int ha_rapid::info(unsigned int flags) {
  ut_a(flags == (HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK));

  std::ostringstream oss;
  oss << table_share->db.str << ":" << table_share->table_name.str << ":";

  Rapid_load_context context;
  context.m_trx = Transaction::get_or_create_trx(m_thd);
  for (auto it = Imcs::Imcs::instance()->get_cus().begin(); it != Imcs::Imcs::instance()->get_cus().end(); it++) {
    if (it->first.find(oss.str()) == std::string::npos || !it->second)
      continue;
    else {
      stats.records = it->second->rows(&context);
      break;
    }
  }

  return 0;
}

/** Returns the operations supported for indexes.
 @return flags of supported operations */
handler::Table_flags ha_rapid::table_flags() const {
  /** Orignal:Secondary engines do not support index access. Indexes are only
   *  used for cost estimates. But, here, we support index too.*/

  // return HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
  //               HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN;

  // TODO:[remove when index supported] Secondary engines do not support index
  // access. Indexes are only used for cost estimates.
  return HA_NO_INDEX_ACCESS | HA_STATS_RECORDS_IS_EXACT | HA_COUNT_ROWS_INSTANT;
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
  Rapid_load_context context;
  context.m_trx = Transaction::get_or_create_trx(m_thd);
  std::string sch = table_share->db.str;
  std::string tb = table_share->table_name.str;
  *num_rows = Imcs::Imcs::instance()->at(sch, tb, 0)->rows(&context);
  return 0;
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
  if (shannon_loaded_tables->get(table_arg.s->db.str, table_arg.s->table_name.str) != nullptr) {
    std::ostringstream err;
    err << table_arg.s->db.str << "." << table_arg.s->table_name.str << " already loaded";
    my_error(ER_SECONDARY_ENGINE_DDL, MYF(0), err.str().c_str());
    return HA_ERR_KEY_NOT_FOUND;
  }

  for (auto idx = 0u; idx < table_arg.s->fields; idx++) {
    auto fld = *(table_arg.field + idx);
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    if (!ShannonBase::Utils::Util::is_support_type(fld->type())) {
      std::ostringstream err;
      err << fld->field_name << " type not allowed";
      my_error(ER_SECONDARY_ENGINE_DDL, MYF(0), err.str().c_str());
      return HA_ERR_GENERIC;
    }
  }

  m_thd->set_sent_row_count(0);
  // start to read data from innodb and load to rapid.
  ShannonBase::Rapid_load_context context;
  context.m_table = const_cast<TABLE *>(&table_arg);
  context.m_thd = m_thd;

  if (Imcs::Imcs::instance()->load_table(&context, const_cast<TABLE *>(&table_arg))) {
    my_error(ER_SECONDARY_ENGINE_DDL, MYF(0), table_arg.s->db.str, table_arg.s->table_name.str);
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
  return 0;
}

int ha_rapid::unload_table(const char *db_name, const char *table_name, bool error_if_not_loaded) {
  if (error_if_not_loaded && shannon_loaded_tables->get(db_name, table_name) == nullptr) {
    std::ostringstream err;
    err << db_name << "." << table_name << " table is not loaded into";
    my_error(ER_SECONDARY_ENGINE_DDL, MYF(0), err.str().c_str());

    return HA_ERR_GENERIC;
  } else {
    Imcs::Imcs::instance()->unload_table(nullptr, db_name, table_name, false);
    shannon_loaded_tables->erase(db_name, table_name);
    return 0;
  }
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

  return 0;
}

/** Initialize a table scan.
@param[in]      scan    whether this is a second call to rnd_init()
                        without rnd_end() in between
@return 0 or error number */
int ha_rapid::rnd_init(bool scan) {
  if (m_data_table->init()) {
    m_start_of_scan = false;
    return HA_ERR_GENERIC;
  }

  m_start_of_scan = true;
  return 0;
}

/** Ends a table scan.
 @return 0 or error number */

int ha_rapid::rnd_end(void) {
  m_start_of_scan = false;
  return m_data_table->end();
}

/** Reads the next row in a table scan (also used to read the FIRST row
 in a table scan).
 @return 0, HA_ERR_END_OF_FILE, or error number */
int ha_rapid::rnd_next(uchar *buf) {
  int error;

  if (m_start_of_scan) {
    error = m_data_table->next(buf);  // index_first(buf);

    if (error == HA_ERR_KEY_NOT_FOUND) {
      error = HA_ERR_END_OF_FILE;
    }

    m_start_of_scan = false;
  } else {  // general fetch.
    error = m_data_table->next(buf);
  }

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return error;
}

}  // namespace ShannonBase

static bool rpd_thd_trx_is_auto_commit(THD *thd) { /*!< in: thread handle, can be NULL */
  return thd != nullptr && !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN) && thd_is_query_block(thd);
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

  return 0;
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

  return 0;
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

  trx->set_read_only(true);
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

  return 0;
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
  return 0;
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

static inline bool SetSecondaryEngineOffloadFailedReason(const THD *thd, std::string_view msg) {
  ut_a(thd);
  std::string msg_str(msg);
  thd->lex->m_secondary_engine_offload_or_exec_failed_reason = msg_str;

  my_error(ER_SECONDARY_ENGINE, MYF(0), msg_str.c_str());
  return 0;
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

// In this function, Dynamic offload combines mysql plan features
// retrieved from rapid_statement_context
// and RAPID info such as rapid base table cardinality,
// dict encoding projection, varlen projection size, rapid queue
// size in to decide if query should be offloaded to RAPID.
// returns true, goes to innodb for execution.
// returns false, goes to next phase for secondary engine execution.
static bool RapidPrepareEstimateQueryCosts(THD *thd, LEX *lex) {
  if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_OFF) {
    SetSecondaryEngineOffloadFailedReason(thd, "use_secondary_engine set to off.");
    return true;
  } else if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_FORCED)
    return false;

  auto shannon_statement_context = thd->secondary_engine_statement_context();
  auto primary_plan_info = shannon_statement_context->get_cached_primary_plan_info();

  // 1: to check whether the sys_pop_data_sz has too many data to populate.
  uint64 too_much_pop_threshold =
      static_cast<uint64_t>(ShannonBase::SHANNON_TO_MUCH_POP_THRESHOLD_RATIO * ShannonBase::rpd_pop_buff_sz_max);
  if (ShannonBase::Populate::sys_pop_data_sz > too_much_pop_threshold) {
    SetSecondaryEngineOffloadFailedReason(thd, "too much changes need to populate.");
    return true;
  }

  // 2: to check whether there're changes in sys_pop_buff, which will be used for query.
  // if there're still do populating, then goes to innodb. and gets cardinality of tables.
  for (uint i = primary_plan_info->tables; i < primary_plan_info->tables; i++) {
    // primary_plan_info->query_expression()->first_query_block()->leaf_tables;
    // for (Table_ref *tr = leaf_tables; tr != nullptr; tr = tr->next_leaf)
    std::string db_tb;
    if (ShannonBase::Populate::Populator::check_status(db_tb)) {
      SetSecondaryEngineOffloadFailedReason(thd, "table queried is populating.");
      return true;
    }
  }

  // 3: checks dict encoding projection, and varlen project size, etc.
  if (ShannonBase::Utils::Util::check_dict_encoding_projection(thd)) {
    SetSecondaryEngineOffloadFailedReason(thd, "dict encoding is not supported.");
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

  // Disable use of constant tables and evaluation of subqueries during
  // optimization. But, it is not to producce an optima count query plan.
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
// If dynamic offload is disabled or the query is "very fast":
// This function invokes standary mysql cost threshold classifier,
// which decides if query needs further RAPID optimisation.
//
// returns true, goes to secondary engine, otherwise, goes to innodb.
bool SecondaryEnginePrePrepareHook(THD *thd) {
  RapidCachePrimaryInfoAtPrimaryTentativelyStep(thd);

  if (unlikely(!thd->variables.rapid_use_dynamic_offload)) {
    // invokes standary mysql cost threshold classifier, which decides if query needs further RAPID optimisation.
    return ShannonBase::Utils::Util::standard_cost_threshold_classifier(thd);
  } else if (likely(thd->variables.rapid_use_dynamic_offload)) {
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

//  In this function, Dynamic offload retrieves info from
//  rapid_statement_context and additionally looks at Change
//  propagation lag to decide if query should be offloaded to rapid
//  returns true, goes to innodb engine. otherwise, false, goes to secondary engine.
static bool RapidOptimize(THD *thd, LEX *lex) {
  if (likely(thd->variables.use_secondary_engine == SECONDARY_ENGINE_OFF)) {
    SetSecondaryEngineOffloadFailedReason(thd, "in RapidOptimize, set use_secondary_engine to false.");
    return true;
  }

  // auto statement_context = thd->secondary_engine_statement_context();
  // to much changes to populate, then goes to primary engine.
  ulonglong too_much_pop_threshold =
      static_cast<ulonglong>(ShannonBase::SHANNON_TO_MUCH_POP_THRESHOLD_RATIO * ShannonBase::rpd_pop_buff_sz_max);
  if (unlikely(ShannonBase::Populate::sys_pop_buff.size() > ShannonBase::SHANNON_POP_BUFF_THRESHOLD_COUNT ||
               ShannonBase::Populate::sys_pop_data_sz > too_much_pop_threshold)) {
    SetSecondaryEngineOffloadFailedReason(thd, "in RapidOptimize, the CP lag is too much.");
    return true;
  }

  JOIN *join = lex->unit->first_query_block()->join;
  WalkAccessPaths(lex->unit->root_access_path(), join, WalkAccessPathPolicy::ENTIRE_TREE,
                  [&](AccessPath *path, const JOIN *join) {
                    ShannonBase::Optimizer::OptimzieAccessPath(path, const_cast<JOIN *>(join));
                    return false;
                  });
  // Here, because we cannot get the parent node of corresponding iterator, we reset the type of access
  // path, then re-generates all the iterators. But, it makes the preformance regression for a `short`
  // AP workload. But, we will replace the itertor when we traverse iterator tree from root to leaves.
  lex->unit->release_root_iterator().reset();
  auto new_root_iter =
      CreateIteratorFromAccessPath(thd, lex->unit->root_access_path(), join, /*eligible_for_batch_mode=*/true);

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

  return RapidOptimize(thd, lex);
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

static handler *Create(handlerton *hton, TABLE_SHARE *table_share, bool, MEM_ROOT *mem_root) {
  return new (mem_root) ShannonBase::ha_rapid(hton, table_share);
}

static SHOW_VAR rapid_status_variables[] = {
    {"rapid_memory_size_max", (char *)&ShannonBase::rpd_mem_sz_max, SHOW_LONG, SHOW_SCOPE_GLOBAL},

    {"rapid_pop_buffer_size_max", (char *)&ShannonBase::rpd_pop_buff_sz_max, SHOW_LONG, SHOW_SCOPE_GLOBAL},

    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

/** Callback function for accessing the Rapid variables from MySQL:  SHOW
 * VARIABLES. */
static int show_rapid_vars(THD *, SHOW_VAR *var, char *) {
  // gets the latest variables of shannonbase rapid.
  var->type = SHOW_ARRAY;
  var->value = (char *)&rapid_status_variables;
  var->scope = SHOW_SCOPE_GLOBAL;

  return (0);
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

  return 0;
}

/** Update the system variable rapid_memory_size_max.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_mem_size_max_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {}

/** Validate passed-in "value" is a valid monitor counter name.
 This function is registered as a callback with MySQL.
 @return 0 for valid name */
static int rpd_pop_buff_size_max_validate(THD *,                          /*!< in: thread handle */
                                          SYS_VAR *,                      /*!< in: pointer to system
                                                                                          variable */
                                          void *save,                     /*!< out: immediate result
                                                                          for update function */
                                          struct st_mysql_value *value) { /*!< in: incoming string */
  return 0;
}

/** Update the system variable rapid_pop_buffer_size_max.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_pop_buff_size_max_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {}

static MYSQL_SYSVAR_ULONG(rapid_memory_size_max, ShannonBase::rpd_mem_sz_max, PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                          "Number of memory size that used for rapid engine, and it must "
                          "not be oversize half of physical mem size.",
                          rpd_mem_size_max_validate, rpd_mem_size_max_update, ShannonBase::SHANNON_MAX_MEMRORY_SIZE, 0,
                          ShannonBase::SHANNON_MAX_MEMRORY_SIZE, 0);

static MYSQL_SYSVAR_ULONGLONG(rapid_pop_buffer_size_max, ShannonBase::rpd_pop_buff_sz_max,
                              PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                              "Number of memory used for populating the changes "
                              "in innodb to rapid engine..",
                              rpd_pop_buff_size_max_validate, rpd_pop_buff_size_max_update,
                              ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE, 0,
                              ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE, 0);

static struct SYS_VAR *rapid_system_variables[] = {
    MYSQL_SYSVAR(rapid_memory_size_max),
    MYSQL_SYSVAR(rapid_pop_buffer_size_max),
    nullptr,
};

static SHOW_VAR rapid_status_variables_export[] = {
    {"ShannonBase Rapid", (char *)&show_rapid_vars, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

static int Shannonbase_Rapid_Init(MYSQL_PLUGIN p) {
  ShannonBase::shannon_loaded_tables = new ShannonBase::LoadedTables();

  handlerton *shannon_rapid_hton = static_cast<handlerton *>(p);
  ShannonBase::shannon_rapid_hton_ptr = shannon_rapid_hton;
  shannon_rapid_hton->create = Create;
  shannon_rapid_hton->state = SHOW_OPTION_YES;
  shannon_rapid_hton->flags = HTON_IS_SECONDARY_ENGINE;
  shannon_rapid_hton->db_type = DB_TYPE_RAPID;
  shannon_rapid_hton->prepare_secondary_engine = PrepareSecondaryEngine;
  shannon_rapid_hton->secondary_engine_pre_prepare_hook = SecondaryEnginePrePrepareHook;
  shannon_rapid_hton->optimize_secondary_engine = OptimizeSecondaryEngine;
  shannon_rapid_hton->compare_secondary_engine_cost = CompareJoinCost;
  shannon_rapid_hton->secondary_engine_flags = MakeSecondaryEngineFlags(SecondaryEngineFlag::SUPPORTS_HASH_JOIN);
  shannon_rapid_hton->secondary_engine_modify_access_path_cost = ModifyAccessPathCost;
  shannon_rapid_hton->get_secondary_engine_offload_or_exec_fail_reason = GetSecondaryEngineOffloadorExecFailedReason;
  shannon_rapid_hton->set_secondary_engine_offload_fail_reason = SetSecondaryEngineOffloadFailedReason;
  shannon_rapid_hton->secondary_engine_check_optimizer_request = SecondaryEngineCheckOptimizerRequest;

  shannon_rapid_hton->commit = rapid_commit;
  shannon_rapid_hton->rollback = rapid_rollback;
  shannon_rapid_hton->start_consistent_snapshot = rapid_start_trx_and_assign_read_view;
  shannon_rapid_hton->savepoint_set = rapid_savepoint;
  shannon_rapid_hton->savepoint_rollback = rapid_rollback_to_savepoint;
  shannon_rapid_hton->savepoint_rollback_can_release_mdl = rapid_rollback_to_savepoint_can_release_mdl;
  shannon_rapid_hton->close_connection = rapid_close_connection;
  shannon_rapid_hton->kill_connection = rapid_kill_connection;

  auto instance_ = ShannonBase::Imcs::Imcs::instance();
  if (!instance_) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "get IMCS instance.");
    return 1;
  };

  return instance_->initialize();
}

static int Shannonbase_Rapid_Deinit(MYSQL_PLUGIN) {
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