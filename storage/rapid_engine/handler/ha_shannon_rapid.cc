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

#include "storage/rapid_engine/utils/sql_exception.h"

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
#include "sql/partition_info.h"
#include "sql/replication.h"  // Trans_param, TRANS_IS_REAL_TRANS
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/sql_partition.h"  // get_part_for_delete, get_parts_for_update
#include "sql/table.h"

#include "log0log.h" /* log_get_lsn */

#include "ml/ml_retrieve_schema_metadata.h"  // shannon_ml_on_ddl_event

#include "storage/innobase/handler/ha_innodb.h"  //thd_to_trx
#include "storage/innobase/include/dict0dd.h"    //dd_table_is_partitioned
#include "storage/innobase/include/trx0trx.h"    // trx_t::id, trx_is_started

#include "sql/dd/types/foreign_key.h"  // dd::Foreign_key::enum_rule
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
#include "storage/rapid_engine/populate/log_dml_notification.h"  // DML notification capture side
#include "storage/rapid_engine/populate/log_populate.h"
#include "storage/rapid_engine/recovery/recovery.h"  // rapid_recovery_startup, rapid_recovery_shutdown
#include "storage/rapid_engine/trx/transaction.h"    //transaction
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

LoadedTables *shannon_loaded_tables{nullptr};

// Self-Load manager instance.
ShannonBase::Autopilot::SelfLoadManager *shannon_self_load_mgr_inst{nullptr};

bool Rapid_execution_context::BestPlanSoFar(const JOIN &join, double cost) {
  // join.best_read is DBL_MAX until the optimizer accepts a complete plan, so it
  // marks the start of a plan search. Testing it as well as the JOIN address
  // matters because a statement is optimized more than once (once for the
  // primary engine, then again for the secondary) while this context lives on
  // the LEX: the second pass reuses the same JOIN object, so the address alone
  // still looks familiar and m_best_cost is a stale cost from the earlier pass.
  // Comparing against it can report "not cheaper" for the first plan of the new
  // pass, which breaks the contract asserted in consider_plan()
  // (sql/sql_planner.cc): the first complete plan offered must always be chosen.
  if (&join != m_current_join || join.best_read == DBL_MAX) {
    // No plan has been seen for this plan search. The current one is best so far.
    m_current_join = &join;
    m_best_cost = cost;
    return true;
  }

  // Check if the current plan is the best seen so far.
  const bool cheaper = cost < m_best_cost;
  m_best_cost = std::min(m_best_cost, cost);
  return cheaper;
}

std::vector<LoadedTableInfo> LoadedTables::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(m_mutex);
  std::vector<LoadedTableInfo> result;
  result.reserve(m_tables.size());
  for (const auto &[key, share] : m_tables) result.push_back(LoadedTableInfo{share->m_tableid, key.schema, key.table});
  return result;
}

namespace {
/*
  Two server error codes reach the client from this engine, and they are not
  interchangeable:

    ER_SECONDARY_ENGINE_PLUGIN  "%s"
        The message IS the whole error. Used for everything this engine says
        in its own words -- load/unload refusals, plugin-boundary failures.

    ER_SECONDARY_ENGINE         "Secondary engine operation failed. %s."
        The message is a reason wrapped in the server's sentence. Used where
        the server asks the engine why an operation could not be offloaded.

  Both take exactly ONE argument. Passing two does not print the second, it
  leaves it to be read as whatever the format string did not consume.
*/
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

  if (table_def == nullptr) return secondary_error("Rapid: missing table definition", HA_ERR_GENERIC);

  // Keep the share alive for as long as m_lock points into share->lock.
  m_share = share;
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
  m_share.reset();

  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::info(unsigned int flags) {
  if (!(flags & HA_STATUS_VARIABLE)) return ShannonBase::SHANNON_SUCCESS;

  auto share = shannon_loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (share == nullptr) return secondary_error("Table has not been loaded", HA_ERR_GENERIC);

  auto rpd_tb = Imcs::Imcs::instance()->get_rpd_table_shared(share->m_tableid);
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

namespace {
using ShannonBase::Populate::Populator;
using ShannonBase::Populate::TablePropagationState;
using ShannonBase::Populate::TablePropagationWaitResult;
using Barrier = decltype(Populator::request_table_barrier(table_id_t{}));
using RpdTablePtr = decltype(Imcs::Imcs::instance()->get_rpd_table_shared(table_id_t{}));

constexpr char kMsgBrokenInitial[] =
    "Rapid table has failed DML propagation and must be reloaded before secondary-engine reads";
constexpr char kMsgBrokenWaiting[] =
    "Rapid table DML propagation failed while waiting for the query watermark; reload required";
constexpr char kMsgBrokenRecapture[] =
    "Rapid table DML propagation failed while re-capturing the query watermark; reload required";
constexpr char kMsgUnloading[] = "Rapid table is being unloaded; the count cannot be served from the secondary engine";
constexpr char kMsgUnloadedWaiting[] =
    "Rapid table was unloaded while waiting for the query watermark; reload required";
constexpr char kMsgPopulatorStopped[] =
    "Rapid change propagation is not running; reload the table before secondary-engine reads";

// nullptr when the barrier state permits proceeding. GONE means the buffer owning this
// table's changes was detached, so there is no live watermark a read could wait for.
const char *barrier_error(const Barrier &b, const char *broken_msg) {
  if (b.state == TablePropagationState::BROKEN) return broken_msg;
  if (b.state == TablePropagationState::GONE) return kMsgUnloading;
  return nullptr;
}

// Binds the statement's transaction and snapshot to `ctx`. False on failure.
bool prepare_count_scan(THD *thd, ShannonBase::Rapid_scan_context &ctx) {
  auto *trx = ShannonBase::Transaction::get_or_create_trx(thd);
  if (trx == nullptr || trx->begin() != ShannonBase::SHANNON_SUCCESS) return false;

  ctx.m_thd = thd;
  ctx.m_trx = trx;
  ctx.m_extra_info.m_trxid = trx->get_id();
  ctx.m_extra_info.m_scn = ShannonBase::TransactionCoordinator::instance().get_current_scn();

  ::ReadView *read_view = trx->acquire_snapshot();
  return !(trx->isolation_level() > ShannonBase::Transaction::ISOLATION_LEVEL::READ_UNCOMMITTED &&
           read_view == nullptr);
}

// `msg == nullptr` with ok == false means the statement was killed (generic error, no message).
struct WaitOutcome {
  bool ok;
  const char *msg;
};

// Waits until the table's propagation watermark covers this query, so committed but
// not-yet-propagated DML cannot make the count short. `rpd_tb` is refreshed if the
// table is reloaded while waiting.
WaitOutcome wait_for_query_watermark(THD *thd, const TABLE_SHARE *ts, RpdTablePtr &rpd_tb, Barrier barrier) {
  while (barrier.needs_wait()) {
    if (thd != nullptr && thd->killed) return {false, nullptr};

    // A stopped populator can never advance the apply watermark; waiting would spin forever.
    bool populator_running = Populator::active();
    DBUG_EXECUTE_IF("secondary_engine_rapid_barrier_populator_stopped", populator_running = false;);
    if (!populator_running) return {false, kMsgPopulatorStopped};

    switch (Populator::wait_table_applied_for(rpd_tb->meta().table_id, barrier.required_change_id,
                                              ShannonBase::Populate::QUERY_PROPAGATION_WAIT_SLICE_MS,
                                              barrier.buffer_generation)) {
      case TablePropagationWaitResult::APPLIED:
        return {true, nullptr};
      case TablePropagationWaitResult::BROKEN:
        return {false, kMsgBrokenWaiting};
      case TablePropagationWaitResult::GONE: {
        auto share = shannon_loaded_tables->get(ts->db.str, ts->table_name.str);
        if (share == nullptr) return {false, kMsgUnloadedWaiting};
        rpd_tb = Imcs::Imcs::instance()->get_rpd_table_shared(share->m_tableid);
        if (rpd_tb == nullptr) return {false, kMsgUnloadedWaiting};

        barrier = Populator::request_table_barrier(rpd_tb->meta().table_id);
        if (const char *e = barrier_error(barrier, kMsgBrokenRecapture)) return {false, e};
        break;  // READY leaves the loop via needs_wait(); PENDING waits on the fresh watermark.
      }
      default:
        break;  // PENDING: the same watermark is still outstanding.
    }
  }
  return {true, nullptr};
}

std::optional<std::string> check_loadable(const TABLE &table) {
  const char *db = table.s->db.str;
  const char *tbl = table.s->table_name.str;

  if (shannon_loaded_tables->get(db, tbl) != nullptr) return std::string(db) + "." + tbl + " already loaded";

  if (table.s->is_missing_primary_key()) return std::string(db) + "." + tbl + " requires PK for loading into rapid";

  for (uint i = 0; i < table.s->fields; ++i) {
    const Field *fld = table.field[i];
    if (fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;
    if (!ShannonBase::Utils::Util::is_support_type(fld->type()))
      return std::string(tbl) + "." + fld->field_name + " type not allowed";
  }
  return std::nullopt;
}
}  // namespace

// COUNT(*) is answered here (UNQUALIFIED_COUNT -> ha_records()), so the value IS
// the result: count at this statement's snapshot, and wait for the query
// watermark so committed-but-unpropagated DML cannot make the count short.
int ha_rapid::records(ha_rows *num_rows) {
  *num_rows = HA_POS_ERROR;  // overwritten only on success

  auto share = shannon_loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (share == nullptr) return secondary_error("Table has not been loaded", HA_ERR_GENERIC);

  auto rpd_tb = Imcs::Imcs::instance()->get_rpd_table_shared(share->m_tableid);
  ShannonBase::Rapid_scan_context scan_context;
  if (rpd_tb == nullptr || !prepare_count_scan(current_thd, scan_context)) return HA_ERR_GENERIC;

  auto barrier = Populator::request_table_barrier(rpd_tb->meta().table_id);
  if (const char *e = barrier_error(barrier, kMsgBrokenInitial)) return secondary_error(e, HA_ERR_GENERIC);

  if (const auto w = wait_for_query_watermark(current_thd, table_share, rpd_tb, barrier); !w.ok)
    return w.msg ? secondary_error(w.msg, HA_ERR_GENERIC) : HA_ERR_GENERIC;

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

#ifndef NDEBUG
  assert(m_thd != nullptr);
  assert(m_thd->mdl_context.owns_equal_or_stronger_lock(MDL_key::TABLE, table_arg.s->db.str,
                                                        table_arg.s->table_name.str, MDL_SHARED_READ));
#endif

  const char *db = table_arg.s->db.str;
  const char *tbl = table_arg.s->table_name.str;
  auto *table = const_cast<TABLE *>(&table_arg);
  if (auto err = check_loadable(table_arg)) return secondary_error(*err, HA_ERR_KEY_NOT_FOUND);

  m_thd->set_sent_row_count(0);

  // Read data from InnoDB and load it into Rapid.
  ShannonBase::Rapid_load_context context;
  context.m_thd = m_thd;
  context.m_table = table;
  context.m_table_id = table_arg.file->get_table_id();
  context.m_schema_name = db;
  context.m_table_name = tbl;
  context.m_sch_tb_name = context.m_schema_name + "." + context.m_table_name;
  context.m_extra_info.m_oper = ShannonBase::Rapid_context::extra_info_t::OperType::LOAD;
  context.m_extra_info.m_keynr = active_index;
  context.m_extra_info.m_key_len = table_arg.file->ref_length;

  context.m_trx = Transaction::get_or_create_trx(m_thd);
  if (context.m_trx == nullptr)
    return secondary_error("Rapid: cannot get the primary InnoDB transaction information", HA_ERR_GENERIC);
  ShannonBase::TransactionGuard guard(context.m_trx);
  context.m_extra_info.m_trxid = context.m_trx->get_id();

  // A non-zero SCN at load time means "committed after insert with explicit begin/commit".
  context.m_extra_info.m_scn = TransactionCoordinator::instance().allocate_scn();

  Utils::Util::update_rpd_meta_info(&context, &table_arg, Utils::Util::STAGE::BEGIN);
  if (Imcs::Imcs::instance()->load_table(&context, table))
    return secondary_error(std::string(db) + "." + tbl + " load failed", HA_ERR_GENERIC);
  Utils::Util::update_rpd_meta_info(&context, &table_arg, Utils::Util::STAGE::END);

  guard.commit();

  m_share = std::make_shared<RapidShare>(table_arg);
  m_share->is_partitioned = false;
  m_share->file = this;
  m_share->m_tableid = context.m_table_id;

  shannon_loaded_tables->add(db, tbl, m_share);
  if (shannon_loaded_tables->get(db, tbl) == nullptr)
    return secondary_error("Failed to load table", HA_ERR_KEY_NOT_FOUND);

  // Start the population thread now that a table is loaded.
  ShannonBase::Populate::Populator::start();
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::unload_table(const char *db_name, const char *table_name, bool error_if_not_loaded) {
  // stop the table worker thread.
  const auto share = shannon_loaded_tables->get(db_name, table_name);
  if (!share && error_if_not_loaded)
    return secondary_error(std::string(db_name) + "." + table_name + " table is not loaded into rapid yet",
                           HA_ERR_GENERIC);

  const auto table_id = share ? share->m_tableid : 0;

  // Stop change propagation for this table before tearing down its data.
  ShannonBase::Populate::Populator::unload(table_id);

  // The unload path works from the table id and names only and never reads context.m_table,
  // which is why the share no longer keeps a TABLE* to hand over here.
  ShannonBase::Rapid_load_context context;
  context.m_table = nullptr;
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
  ShannonBase::RpdMirror::Registry::mark_unloaded(db_name, table_name);

  // Last loaded table gone: stop the population thread too.
  if (shannon_loaded_tables->size() == 0) ShannonBase::Populate::Populator::shutdown();

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
  if (inited != handler::RND) return HA_ERR_END_OF_FILE;

  auto *reader_pool = ShannonBase::Imcs::Imcs::pool();
  const bool use_async =
      reader_pool != nullptr &&
      table_share->fields > static_cast<uint>(ShannonBase::shannon_rpd_engine_cfg.async_column_threshold);

  int error = use_async ? boost::asio::co_spawn(*reader_pool, m_cursor->next_async(buf), boost::asio::use_future).get()
                        : m_cursor->next(buf);

  // Both paths report "no more rows" as either code; the handler API wants END_OF_FILE.
  if (error == HA_ERR_KEY_NOT_FOUND) return HA_ERR_END_OF_FILE;
  if (error != ShannonBase::SHANNON_SUCCESS) return error;

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::rnd_next_batch(size_t batch_size, std::vector<ShannonBase::Executor::ColumnChunk> &data,
                             size_t &read_cnt) {
  if (inited != handler::RND) return HA_ERR_END_OF_FILE;

  const int error = m_cursor->next(batch_size, data, read_cnt);
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return error;
}

int ha_rapid::index_next_batch(size_t batch_size, std::vector<ShannonBase::Executor::ColumnChunk> &data,
                               size_t &read_cnt, bool reverse) {
  if (inited != handler::INDEX) return HA_ERR_END_OF_FILE;

  const int error = m_cursor->index_next_batch(batch_size, data, read_cnt, reverse);
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_next_count);
  return error;
}

const std::vector<row_id_t> &ha_rapid::last_batch_row_ids() const { return m_cursor->last_batch_row_ids(); }

void ha_rapid::set_last_returned_rowid(row_id_t rid) { m_cursor->set_last_returned_rowid(rid); }

int ha_rapid::index_init(uint keynr, bool sorted) {
  DBUG_TRACE;

  if (auto ret = m_cursor->index_init(keynr, sorted); ret) return ret;

  active_index = keynr;
  inited = handler::INDEX;
#ifndef NDEBUG
  m_active_index_flags = index_flags(keynr, 0, true);
#endif
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::index_end() {
  DBUG_TRACE;

  if (auto ret = m_cursor->index_end(); ret) return ret;

  active_index = MAX_KEY;
  inited = handler::NONE;
#ifndef NDEBUG
  m_active_index_flags = 0;
#endif
  return ShannonBase::SHANNON_SUCCESS;
}

#ifndef NDEBUG
void ha_rapid::assert_index_capability(unsigned long flag) const {
  assert(active_index != MAX_KEY);
  assert((m_active_index_flags & flag) != 0);
}
#endif

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
  assert_index_capability(HA_READ_NEXT);

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
  assert_index_capability(HA_READ_PREV);

  auto error = m_cursor->index_prev(buf);
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_prev_count);
  return error;
}

int ha_rapid::index_last(uchar *buf) {
  DBUG_TRACE;
  ut_ad(inited == handler::INDEX);
  assert_index_capability(HA_READ_ORDER);  // reading the last key is a backwards seek on the index order

  m_cursor->set_end_range(end_range);
  const int error = m_cursor->index_read(buf, nullptr, 0, HA_READ_BEFORE_KEY);

  // The handler API does not allow index_last() to return HA_ERR_KEY_NOT_FOUND.
  if (error == HA_ERR_KEY_NOT_FOUND) return HA_ERR_END_OF_FILE;
  if (error != ShannonBase::SHANNON_SUCCESS) return error;

  ha_statistic_increment(&System_status_var::ha_read_last_count);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapid::read_range_first(const key_range *start_key, const key_range *end_key, bool eq_range_arg, bool sorted) {
  // The range optimizer keeps an equality range even on an index that does not
  // advertise HA_READ_RANGE (range_analysis.cc spares EQ_FUNC), and serves it
  // with index_read(HA_READ_KEY_EXACT) + index_next_same. An exact-only ART
  // answers that by prefix traversal, which needs no key ordering. Only an
  // inequality range does.
  if (!eq_range_arg) assert_index_capability(HA_READ_RANGE);
  m_cursor->set_start_range(start_key);

  const int error = handler::read_range_first(start_key, end_key, eq_range_arg, sorted);

  m_cursor->set_start_range(nullptr);
  return error;
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
      // Request the existing post-engine-commit observer dispatch even when
      // binlog is disabled. This flag does not publish a source outcome here.
      thd->get_transaction()->m_flags.run_hooks = true;
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
  /*
    Take a copy before touching the member, for two reasons. msg may be a view
    into that very member -- sql_select.cc's set_fail_reason_and_raise_error()
    passes back whatever find_secondary_engine_offload_fail_reason() returned,
    which is a view onto the recorded reason -- so assigning to the member can
    reallocate the buffer the view points at and leave msg dangling. And a
    string_view is not guaranteed to be NUL-terminated, which my_error() needs.
    Reporting msg.data() after the assignment printed whatever was left in
    freed memory.
  */
  const std::string reason(msg);
  thd->lex->m_secondary_engine_offload_or_exec_failed_reason = reason;

  if (raise_error) my_error(ER_SECONDARY_ENGINE, MYF(0), reason.c_str());
  return ShannonBase::SHANNON_SUCCESS;
}

static bool SetSecondaryEngineOffloadFailedReasonWrapper(const THD *thd, std::string_view msg) {
  return SetSecondaryEngineOffloadFailedReason(thd, msg, /*raise_error=*/true);
}

std::string_view GetSecondaryEngineOffloadorExecFailedReason(const THD *thd) {
  ut_a(thd);
  return thd->lex->m_secondary_engine_offload_or_exec_failed_reason.c_str();
}

/**
 * Core calls this under use_secondary_engine=FORCED to turn a refusal into a
 * user-visible error, when no reason was recorded while rejecting a plan. The
 * one refusal core reaches on its own is a query reading a NOT SECONDARY
 * column: such a column is deliberately excluded from the Rapid load, so the
 * statement is routed to InnoDB. reads_not_secondary_columns()
 * (sql/sql_select.cc) tells core that some column is at fault but not which,
 * so name it here -- otherwise the user is left with the generic "All plans
 * were rejected by the secondary storage engine", which is not actionable.
 */
static std::string_view FindSecondaryEngineOffloadFailedReason(THD *thd) {
  ut_a(thd);
  const LEX *lex = thd->lex;
  const Table_ref *tl = (lex != nullptr) ? lex->query_tables : nullptr;
  // For INSERT INTO ... SELECT the insert target comes first and does not need
  // a secondary engine, matching the skip in reads_not_secondary_columns().
  if (lex != nullptr && lex->sql_command == SQLCOM_INSERT_SELECT && tl != nullptr) tl = tl->next_global;
  for (; tl != nullptr; tl = tl->next_global) {
    if (tl->is_placeholder() || tl->table == nullptr) continue;
    for (uint i = bitmap_get_first_set(tl->table->read_set); i != MY_BIT_NONE;
         i = bitmap_get_next_set(tl->table->read_set, i)) {
      const Field *field = tl->table->field[i];
      if (field == nullptr || !field->is_flag_set(NOT_SECONDARY_FLAG)) continue;
      // Record it before returning: the caller feeds this view straight back
      // into set_secondary_engine_offload_fail_reason(), which copies it.
      thd->lex->m_secondary_engine_offload_or_exec_failed_reason =
          // No trailing period: ER_SECONDARY_ENGINE is "Secondary engine operation
          // failed. %s." and supplies its own.
          std::string("Column ") + field->field_name + " is marked as NOT SECONDARY, so it is not loaded into Rapid";
      return thd->lex->m_secondary_engine_offload_or_exec_failed_reason.c_str();
    }
  }
  // Callers assert the reason is never empty, so keep core's wording as the
  // fallback for every other refusal rather than handing back an empty view.
  const std::string &recorded = thd->lex->m_secondary_engine_offload_or_exec_failed_reason;
  if (!recorded.empty()) return recorded.c_str();
  return "All plans were rejected by the secondary storage engine";
}

SecondaryEngineGraphSimplificationRequestParameters SecondaryEngineCheckOptimizerRequest(
    THD *thd [[maybe_unused]], const JoinHypergraph &hypergraph [[maybe_unused]],
    const AccessPath *access_path [[maybe_unused]], int current_subgraph_pairs [[maybe_unused]],
    int current_subgraph_pairs_limit [[maybe_unused]], bool is_root_access_path [[maybe_unused]],
    std::string *trace [[maybe_unused]]) {
  SecondaryEngineGraphSimplificationRequestParameters params;
  params.secondary_engine_optimizer_request = SecondaryEngineGraphSimplificationRequest::kContinue;
  params.subgraph_pair_limit = 0;
  return params;
}

/**
  Keep a DDL notification hook from leaving an error on the running statement.

  These hooks are advisory: they return void (or always false), so the DDL
  reports success no matter what they do. An error raised underneath one -- the
  ML embedder failing to find its model, say -- therefore survives into
  my_ok(), where Diagnostics_area::set_ok_status() asserts on it. Conditions
  raised inside the guarded scope land in a scratch area and are dropped.
*/
namespace {
class Notify_hook_da_guard {
 public:
  Notify_hook_da_guard() : m_thd(current_thd), m_da(false) {
    if (m_thd) m_thd->push_diagnostics_area(&m_da, /*copy_conditions=*/false);
  }
  ~Notify_hook_da_guard() {
    if (!m_thd) return;
    if (m_da.is_error())
      LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG,
             (std::string("Rapid DDL notification hook raised an error, ignored: ") + m_da.message_text()).c_str());
    m_thd->pop_diagnostics_area();
  }

  Notify_hook_da_guard(const Notify_hook_da_guard &) = delete;
  Notify_hook_da_guard &operator=(const Notify_hook_da_guard &) = delete;

 private:
  THD *m_thd;
  Diagnostics_area m_da;
};
}  // namespace

void NotifyCreateTable(struct HA_CREATE_INFO *create_info, const char *db, const char *table_name) {
  if (dd::get_dictionary()->is_dd_schema_name(db) || dd::get_dictionary()->is_system_table_name(db, table_name)) return;

  Notify_hook_da_guard da_guard;

  auto is_partitioned{false};
  dd::cache::Dictionary_client *dc = current_thd->dd_client();
  const dd::cache::Dictionary_client::Auto_releaser releaser(dc);
  const dd::Table *table_obj = nullptr;
  if (dc && !dc->acquire(db, table_name, &table_obj) && table_obj)
    is_partitioned = (table_obj->partition_type() != dd::Table::PT_NONE);

  std::string eng_str;
  if (create_info->secondary_engine.str) eng_str = create_info->secondary_engine.str;

  // The mirror tracks every table regardless of self-load being enabled, so this
  // must not be gated on the self-loader instance.
  const auto tid = table_obj ? table_obj->se_private_id() : 0;
  ShannonBase::RpdMirror::Registry::upsert(tid, db, table_name, eng_str, is_partitioned);

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

  Notify_hook_da_guard da_guard;

  ShannonBase::RpdMirror::Registry::erase(tab->get_db_name(), tab->get_table_name());

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

  Notify_hook_da_guard da_guard;

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
static inline bool table_has_off_page_blob_data(const TABLE *table) {
  return table != nullptr && table->s != nullptr && table->s->blob_fields > 0;
}

/**
 * Point every Field of @a table at @a record for the lifetime of the guard.
 *
 * Field accessors read through Field::ptr, which is bound to record[0]. A
 * COPY_INFO UPDATE has to capture the *pre*-image too, and that lives in
 * record[1], so the fields have to be walked over there and back. This is the
 * same move_field_offset() idiom the partitioning and trigger code uses.
 */
namespace {
class RecordImageGuard {
 public:
  RecordImageGuard(TABLE *table, const uchar *record) : m_table(table), m_diff(record - table->record[0]) {
    move(m_diff);
  }

  ~RecordImageGuard() { move(-m_diff); }

  RecordImageGuard(const RecordImageGuard &) = delete;
  RecordImageGuard &operator=(const RecordImageGuard &) = delete;

 private:
  void move(ptrdiff_t diff) {
    if (diff == 0) return;
    // Bound by s->fields rather than a nullptr sentinel: the loop in
    // read_off_page_data() below indexes the same way.
    for (uint idx = 0; idx < m_table->s->fields; ++idx) m_table->field[idx]->move_field_offset(diff);
  }

  TABLE *m_table;
  ptrdiff_t m_diff;
};
}  // namespace

/**
 * Copy the out-of-line bytes of every off-page column of @a record.
 *
 * The row image only stores a length plus a pointer into the primary engine's
 * blob heap, and that heap is released when the originating statement ends --
 * long before the asynchronous propagation worker parses the record. So the
 * payload has to be copied here, at capture time, for *both* the pre- and the
 * post-image: RowBuffer::extract_field_data() otherwise falls through to the
 * raw-pointer path and dereferences freed memory.
 *
 * Columns are admitted regardless of the statement's read_set. An UPDATE need
 * not read a blob column it does not touch, but the apply side still has to
 * encode that column into the row's index keys, and a map that is non-empty
 * yet missing an entry is exactly the shape extract_field_data() cannot
 * resolve.
 */
static void read_off_page_data(TABLE *table, const uchar *record,
                               ShannonBase::Populate::change_record_buff_t::off_page_data_t &off_page_data) {
  if (!table_has_off_page_blob_data(table) || record == nullptr) return;

  RecordImageGuard image(table, record);
  ShannonBase::Utils::ColumnMapGuard columns(table, ShannonBase::Utils::ColumnMapGuard::TYPE::READ);

  for (uint idx = 0; idx < table->s->fields; idx++) {
    Field *fld = *(table->field + idx);
    if (!bitmap_is_set(table->read_set, idx) || fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    // Must admit exactly the types RowBuffer::extract_field_data() reads back
    // out of this map. JSON, GEOMETRY and VECTOR are Field_blob subclasses
    // whose data is off-page just like a BLOB's, but they report their own
    // field type, so a BLOB-only filter left them uncaptured. The reader then
    // found a non-empty map without an entry for that column and asserted.
    if (!ShannonBase::Utils::IsOffPageField(fld)) continue;
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

/**
 * @brief Build the Rapid routing key of one physical partition.
 *
 * Rapid stores a partitioned table as one sub-table per physical partition,
 * registered under "<part_name>#<part_id>" (see PartTable::build_partitions,
 * ha_rapidpart::rnd_init_in_part). Change records therefore have to name the
 * partition explicitly: their table id only identifies the parent PartTable.
 *
 * @return the partition key, empty if part_id is not one of this table's
 *         partitions.
 */
static std::string rapid_partition_key(partition_info *part_info, uint32 part_id) {
  if (part_info == nullptr || part_id == NOT_A_PARTITION_ID || part_id >= part_info->get_tot_partitions())
    return std::string();

  const partition_element *elem = part_info->partitions[part_id];
  if (elem == nullptr || elem->partition_name == nullptr) return std::string();

  std::string key(elem->partition_name);
  key.append("#").append(std::to_string(part_id));
  return key;
}

/**
 * @brief Resolve which partition(s) a captured row change belongs to.
 *
 * The propagation worker owns no TABLE and cannot evaluate a partition
 * function, so the routing decision is made here while partition_info and the
 * row images are still in hand. An UPDATE is allowed to move a row between
 * partitions, hence the separate pre-image / post-image keys.
 *
 * Both keys stay empty for a non-partitioned table.
 *
 * @return false when the partition cannot be resolved. The caller must then
 *         quarantine the table instead of enqueueing a record that the apply
 *         worker would not be able to route.
 */
static bool resolve_change_partitions(TABLE *table, ShannonBase::Populate::change_record_buff_t::OperType oper,
                                      const uchar *old_row, const uchar *new_row, std::string &part_key,
                                      std::string &old_part_key) {
  using OperType = ShannonBase::Populate::change_record_buff_t::OperType;

  partition_info *part_info = table->part_info;
  assert(part_info != nullptr);

  uint32 part_id{NOT_A_PARTITION_ID}, old_part_id{NOT_A_PARTITION_ID};
  longlong func_value{0};
  int error{0};

  // The partitioning fields are not necessarily in the statement's read_set;
  // mark them readable exactly like Partition_helper::ph_write_row() does.
  ShannonBase::Utils::ColumnMapGuard guard(table, ShannonBase::Utils::ColumnMapGuard::TYPE::READ);
  switch (oper) {
    case OperType::INSERT:
      error = part_info->get_partition_id(part_info, &part_id, &func_value);
      old_part_id = part_id;
      break;
    case OperType::DELETE:
      error = get_part_for_delete(old_row, table->record[0], part_info, &old_part_id);
      part_id = old_part_id;
      break;
    case OperType::UPDATE:
      error = get_parts_for_update(old_row, new_row, table->record[0], part_info, &old_part_id, &part_id, &func_value);
      break;
    default:
      error = HA_ERR_GENERIC;
      break;
  }

  if (error != 0) return false;

  part_key = rapid_partition_key(part_info, part_id);
  old_part_key = rapid_partition_key(part_info, old_part_id);
  return !part_key.empty() && !old_part_key.empty();
}

namespace {
/**
 * True when @a field holds a different value in the two raw row images.
 */
bool FieldDiffersBetweenRows(const Field *field, const uchar *old_row, const uchar *new_row) {
  if (field->is_nullable()) {
    const bool was_null = (old_row[field->null_offset()] & field->null_bit) != 0;
    const bool is_null = (new_row[field->null_offset()] & field->null_bit) != 0;
    if (was_null != is_null) return true;
    if (was_null) return false;
  }
  const ptrdiff_t off = field->offset(field->table->record[0]);
  return std::memcmp(old_row + off, new_row + off, field->pack_length()) != 0;
}

/**
 * A foreign key can only reference a unique key, so ON UPDATE CASCADE can only
 * fire when the UPDATE actually changed one of the parent's unique-key columns.
 * Without this an ordinary UPDATE of an unrelated column would stale every
 * loaded child table.
 */
bool ParentUniqueKeyChanged(const TABLE *table, const uchar *old_row, const uchar *new_row) {
  if (table->key_info == nullptr) return true;  // cannot tell: stay conservative
  for (uint k = 0; k < table->s->keys; ++k) {
    const KEY &key = table->key_info[k];
    if ((key.flags & HA_NOSAME) == 0) continue;  // not unique: never an FK target
    for (uint part = 0; part < key.user_defined_key_parts; ++part) {
      const Field *field = key.key_part[part].field;
      if (field != nullptr && FieldDiffersBetweenRows(field, old_row, new_row)) return true;
    }
  }
  return false;
}

void QuarantineCascadeChildren(const TABLE *table, bool for_delete) {
  if (table == nullptr || table->s == nullptr) return;
  // Single integer test on the hot path: almost no table is an FK parent.
  if (table->s->foreign_key_parents == 0 || table->s->foreign_key_parent == nullptr) return;

  std::vector<ShannonBase::table_id_t> to_quarantine;
  for (uint i = 0; i < table->s->foreign_key_parents; ++i) {
    const auto &fk = table->s->foreign_key_parent[i];
    const auto rule = for_delete ? fk.delete_rule : fk.update_rule;
    // NO ACTION / RESTRICT reject the parent DML instead of touching the child.
    if (rule != dd::Foreign_key::RULE_CASCADE && rule != dd::Foreign_key::RULE_SET_NULL &&
        rule != dd::Foreign_key::RULE_SET_DEFAULT)
      continue;

    auto child = ShannonBase::shannon_loaded_tables->get(fk.referencing_table_db.str, fk.referencing_table_name.str);
    if (!child) continue;

    to_quarantine.push_back(child->m_tableid);
    ShannonBase::RpdMirror::Registry::mark_stale(static_cast<uint>(child->m_tableid),
                                                 ShannonBase::stale_reason_t::RELOAD_REQUIRED);
    sql_print_warning(
        "Rapid: %s on %s.%s cascades into loaded table %s.%s, which change propagation cannot observe; "
        "the table is now stale. Reload it to resume change propagation.",
        for_delete ? "DELETE" : "UPDATE", table->s->db.str, table->s->table_name.str, fk.referencing_table_db.str,
        fk.referencing_table_name.str);
  }
  if (!to_quarantine.empty()) ShannonBase::Populate::QuarantinePropagationTables(to_quarantine);
}

/**
 * Capture one row change as a COPY_INFO record and hand it to change propagation.
 *
 * @param pre   image copied into buff0: the old row, or record[0] for INSERT
 * @param post  image copied into buff1 (UPDATE only), otherwise nullptr
 * @param partition_role  wording for the "cannot resolve partition" warning
 */
void EnqueueRowChange(THD *thd, TABLE *table, ShannonBase::Populate::change_record_buff_t::OperType oper,
                      const uchar *pre, const uchar *post, const char *partition_role) {
  auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
  if (!share) {
    // Not registered, so nothing is captured. If the table still has an on-disk image
    // waiting for restart recovery, this change makes that image stale: revoke it.
    ShannonBase::Recovery::note_unregistered_source_change(table->s->db.str, table->s->table_name.str);
    return;
  }

  ShannonBase::Populate::change_record_buff_t rec(ShannonBase::Populate::Source::COPY_INFO, table->s->rec_buff_length);
  rec.m_oper = oper;
  rec.m_table_id = share->m_tableid;
#ifndef NDEBUG
  rec.m_schema_name = table->s->db.str;
  rec.m_table_name = table->s->table_name.str;
#endif

  if (table->part_info &&
      !resolve_change_partitions(table, oper, pre, post ? post : pre, rec.m_part_key, rec.m_old_part_key)) {
    ShannonBase::Populate::QuarantinePropagationTables({share->m_tableid});
    sql_print_warning("Rapid COPY_INFO could not resolve the %s on table %llu", partition_role,
                      static_cast<unsigned long long>(share->m_tableid));
    return;
  }

  // read_off_page_data() is a no-op for tables without blob-like columns.
  std::memcpy(rec.m_buff0.get(), pre, table->s->rec_buff_length);
  read_off_page_data(table, pre, rec.m_offpage_data0);
  if (post) {
    std::memcpy(rec.m_buff1.get(), post, table->s->rec_buff_length);
    read_off_page_data(table, post, rec.m_offpage_data1);
  }

  ShannonBase::Populate::RegisterCopyInfoParticipant(thd);
  if (!ShannonBase::Populate::EnqueueCopyInfo(thd, std::move(rec))) {
    ShannonBase::Populate::QuarantinePropagationTables({share->m_tableid});
    sql_print_warning("Rapid COPY_INFO could not register COPY_INFO transaction participation for table %llu",
                      static_cast<unsigned long long>(share->m_tableid));
  }
}
}  // namespace

using RowChangeOper = ShannonBase::Populate::change_record_buff_t::OperType;
namespace {
// A failed row notification has no SQL error channel: the callback runs after
// write_record() and must not touch the statement diagnostics. Quarantine the
// affected tables instead, so Rapid stops serving rows it did not capture.
void QuarantineFailedNotification(void *args) {
  TABLE *table = nullptr;
  if (args) std::memcpy(&table, args, sizeof(table));
  if (!table || !table->s) return;
  QuarantineCascadeChildren(table, true);
  QuarantineCascadeChildren(table, false);
  auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
  if (share) ShannonBase::Populate::QuarantinePropagationTables({share->m_tableid});
  sql_print_error("Rapid row notification failed; affected loaded tables require reload");
}
}  // namespace

void NotifyAfterInsert(THD *thd, void *args) {
  if (!thd || !args) return;
  DBUG_EXECUTE_IF("rapid_notification_bad_alloc", {
    QuarantineFailedNotification(args);
    return;
  });
  struct comb_args {
    TABLE *arg1;
    COPY_INFO *arg2;
    COPY_INFO *arg3;
  };

  auto *params = static_cast<comb_args *>(args);
  if (!params->arg1 || !params->arg2 || !params->arg3) return;

  TABLE *table = params->arg1;
  if (thd->lex->is_ignore() || params->arg2->get_duplicate_handling() != DUP_ERROR) {
    // This server hook reports successful write_record(), not a successful
    // physical INSERT. IGNORE can report a rejected row; REPLACE can remove
    // several unique-key conflicts; ON DUPLICATE KEY can perform an UPDATE.
    // Its cumulative counters and final record buffer do not describe every
    // affected pre-image. Never certify the attempted INSERT in the WAL.
    // Keep this guard engine-local until a complete row outcome is available.
    if (params->arg2->get_duplicate_handling() == DUP_REPLACE) QuarantineCascadeChildren(table, true);
    if (params->arg2->get_duplicate_handling() == DUP_UPDATE) QuarantineCascadeChildren(table, false);
    auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
    if (share) ShannonBase::Populate::QuarantinePropagationTables({share->m_tableid});
    return;
  }
  EnqueueRowChange(thd, table, RowChangeOper::INSERT, table->record[0], nullptr, "target partition of an INSERT");
}

// old_row = table->record[1], new_row = table->record[0]
void NotifyAfterUpdate(THD *thd, void *args) {
  if (!thd || !args) return;
  DBUG_EXECUTE_IF("rapid_notification_bad_alloc", {
    QuarantineFailedNotification(args);
    return;
  });
  struct comb_args {
    TABLE *arg1;
    const uchar *arg2;
    const uchar *arg3;
  };

  auto *params = static_cast<comb_args *>(args);
  TABLE *table = params->arg1;
  const uchar *old_row = params->arg2;
  const uchar *new_row = params->arg3;
  if (!table || !old_row || !new_row) return;

  // Runs whether or not this table is itself loaded: the parent may live only
  // in InnoDB while the child it cascades into is loaded in Rapid.  Only an
  // UPDATE that moves a referenced unique key can cascade.
  if (table->s->foreign_key_parents != 0 && ParentUniqueKeyChanged(table, old_row, new_row))
    QuarantineCascadeChildren(table, /*for_delete=*/false);

  EnqueueRowChange(thd, table, RowChangeOper::UPDATE, old_row, new_row, "target partition of an UPDATE");
}

void NotifyAfterDelete(THD *thd, void *args) {
  if (!thd || !args) return;
  DBUG_EXECUTE_IF("rapid_notification_bad_alloc", {
    QuarantineFailedNotification(args);
    return;
  });
  struct comb_args {
    TABLE *arg1;
    const uchar *old_rec;
  };

  auto *params = static_cast<comb_args *>(args);
  TABLE *table = params->arg1;
  const uchar *old_row = params->old_rec;
  if (!table || !old_row) return;

  // Runs whether or not this table is itself loaded: the parent may live only
  // in InnoDB while the child it cascades into is loaded in Rapid.
  QuarantineCascadeChildren(table, /*for_delete=*/true);

  EnqueueRowChange(thd, table, RowChangeOper::DELETE, old_row, nullptr, "source partition of a DELETE");
}

void NotifyAfterSelect(THD *thd, SelectExecutedIn executed_in) {
  if (executed_in == SelectExecutedIn::kPrimaryEngine) return;

  if (!thd || !thd->lex) return;

  double query_cost = 0.0;
  auto *stmt_context = thd->secondary_engine_statement_context();
  if (stmt_context != nullptr && stmt_context->get_cached_primary_plan_info() != nullptr) {
    query_cost =
        stmt_context->get_primary_cost() * (ShannonBase::SHANNON_HD_READ_FACTOR + ShannonBase::SHANNON_RAM_READ_FACTOR);
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
const char *table_offload_blocker(const LEX *lex) {
  if (lex == nullptr) return nullptr;

  for (const Table_ref *t = lex->query_tables; t != nullptr; t = t->next_global) {
    if (t->is_placeholder()) continue;

    const auto share = ShannonBase::shannon_loaded_tables->get(t->db, t->table_name);
    if (!share) return "table is not loaded in Rapid";

    const auto barrier = ShannonBase::Populate::Populator::request_table_barrier(share->m_tableid);
    if (barrier.state == ShannonBase::Populate::TablePropagationState::BROKEN)
      return "table has failed DML propagation and must be reloaded";
  }
  return nullptr;
}

static bool RapidPrepareEstimateQueryCosts(THD *thd, LEX *lex) {
  // Records the reason and rejects the offload.
  const auto reject = [thd](const char *reason) {
    SetSecondaryEngineOffloadFailedReason(thd, reason);
    return true;
  };

  // Hard preconditions: apply in every mode, including FORCED.
  if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_OFF) return reject("use_secondary_engine set to off");

  const auto isolation = thd_tx_isolation(thd);
  if (isolation == ISO_READ_UNCOMMITTED || isolation == ISO_SERIALIZABLE)
    return reject("Rapid MVCC offload supports READ COMMITTED and REPEATABLE READ");

  if (const char *blocker = table_offload_blocker(lex)) return reject(blocker);

  if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_FORCED) return false;

  // Cost arbitration (non-FORCED only) needs the primary-plan cache filled in the PRIMARY_TENTATIVELY phase.
  auto *stmt_ctx = thd->secondary_engine_statement_context();
  if (stmt_ctx == nullptr) return reject("missing Rapid statement context");
  ut_a(stmt_ctx->get_cached_primary_plan_info() != nullptr);

  // Too many pending changes to populate.
  const auto too_much_pop = static_cast<uint64_t>(ShannonBase::SHANNON_TO_MUCH_POP_THRESHOLD_RATIO *
                                                  ShannonBase::shannon_rpd_engine_cfg.pop_buff_sz_max);
  if (ShannonBase::Populate::shannon_pop_data_sz > too_much_pop) return reject("too much changes need to populate");

  // Dict-encoding projection, varlen projection size, etc.
  if (ShannonBase::ML::Query_arbitrator::check_dict_encoding_projection(thd))
    return reject("dict encoding, varlen pj size, etc. not supported");

  return false;
}

static bool PrepareSecondaryEngine(THD *thd, LEX *lex) {
  DBUG_EXECUTE_IF("secondary_engine_rapid_prepare_error", {
    // An empty "%s" raised an error row with no text, which tells neither the
    // user nor a .result file which injection fired.
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid: injected prepare failure");
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
  const auto reject = [thd](const char *reason) {
    SetSecondaryEngineOffloadFailedReason(thd, reason);
    return true;
  };

  if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_OFF)
    return reject("RapidOptimize, set use_secondary_engine to false");

  const auto pop_threshold = static_cast<ulonglong>(ShannonBase::SHANNON_TO_MUCH_POP_THRESHOLD_RATIO *
                                                    ShannonBase::shannon_rpd_engine_cfg.pop_buff_sz_max);
  auto too_much_lagging =
      ShannonBase::Populate::pop_buff_table_count() > ShannonBase::SHANNON_POP_BUFF_THRESHOLD_COUNT ||
      ShannonBase::Populate::shannon_pop_data_sz > pop_threshold;
  if (unlikely(too_much_lagging)) return reject("RapidOptimize, the change propagation lag is too much");

  Query_expression *unit = lex->unit;
  if (unit == nullptr) return false;
  if (!unit->is_optimized() && unit->optimize(thd, nullptr, true, true)) return true;

  // Resets the EXPLAIN description on every Rapid handler in the statement, so each optimization pass starts clean.
  for (Query_block *qb = unit->first_query_block(); qb != nullptr; qb = qb->next_query_block()) {
    for (Table_ref *tr = qb->leaf_tables; tr != nullptr; tr = tr->next_leaf) {
      if (tr->table == nullptr) continue;  // dynamic_cast of a null file yields nullptr
      if (auto *rpd_hdl = dynamic_cast<ShannonBase::ha_rapid *>(tr->table->file)) rpd_hdl->set_extra_description("");
    }
  }

  Query_block *first_block = unit->first_query_block();
  JOIN *join = first_block != nullptr ? first_block->join : nullptr;
  if (join == nullptr) return false;

  // Let the Rapid optimizer propose a plan; on any "cannot do it" outcome keep MySQL's plan.
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
      thd, context, candidate_root_path, join, /*eligible_for_batch_mode=*/true);
  if (!candidate_root_iter) {
    if (thd->is_error()) return true;
    DBUG_PRINT("rapid_optimizer", ("Rapid iterator construction failed; keeping original plan"));
    return false;
  }

  // Swap in the Rapid plan; the old iterator is destroyed when it goes out of scope.
  auto old_root_iter = unit->release_root_iterator();
  unit->root_access_path() = candidate_root_path;
  unit->set_root_iterator(candidate_root_iter);
  return false;
}

static bool OptimizeSecondaryEngine(THD *thd [[maybe_unused]], LEX *lex) {
  // The context should have been set by PrepareSecondaryEngine.
  ut_a(lex->secondary_engine_execution_context() != nullptr);

  DBUG_EXECUTE_IF("secondary_engine_rapid_optimize_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid: injected optimize failure");
    return true;
  });

  DEBUG_SYNC(thd, "before_rapid_optimize");

  auto optimizer_context = std::make_unique<ShannonBase::Optimizer::OptimizeContext>();
  return RapidOptimize(optimizer_context.get(), thd, lex);
}

static bool CompareJoinCost(THD *thd, const JOIN &join, double optimizer_cost, bool *use_best_so_far, bool *cheaper,
                            double *secondary_engine_cost) {
  *use_best_so_far = false;

  DBUG_EXECUTE_IF("secondary_engine_rapid_compare_cost_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Rapid: injected cost-comparison failure");
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
  } else {  // legacy optimizer.
    *secondary_engine_cost = optimizer_cost;
    bool estimation_error =
        ShannonBase::Optimizer::Optimizer::RapidEstimateJoinCostHGO(thd, join, secondary_engine_cost);
    if (estimation_error) {
      SetSecondaryEngineOffloadFailedReason(thd, "Calc Rapid Estimated Join Cost failed");
      return true;
    }
  }

  *cheaper = rapid_ctx->BestPlanSoFar(join, *secondary_engine_cost);

  // Keep false: this flag stops greedy join-order search; it does not indicate
  // whether the current plan is cheaper. Using cost < join.best_read here would
  // stop at the first plan because best_read starts at DBL_MAX, potentially
  // locking Rapid into a poor join order (including Cartesian joins).
  //
  // BestPlanSoFar() already determines whether the current plan is cheaper.
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
  if (thd->is_error()) return true;  // reject: something upstream already failed
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
  // rescan_cost() is cost() - init_once_cost(), and CompareAccessPaths() asserts
  // every cost dimension is non-negative. A callback that lowers cost() without
  // lowering the one-time cost the core optimizer already charged would leave
  // that difference negative, so clamp it here for every callback rather than
  // relying on each one to remember.
  // Keep the one-time init cost within [0, total cost].
  path->set_init_once_cost(std::max(0.0, std::min(path->init_once_cost(), path->cost())));

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

// Enum labels of the propagation_mode system variable (see rapid_sync_mode_typelib).
static const char *rapid_propagation_mode_names[] = {"DIRECT_NOTIFICATION", "REDO_LOG_PARSE", "HYBRID", nullptr};

// These globals are refreshed by refresh_rapid_export_vars() and exposed
// as SHOW STATUS variables so that Prometheus / mysqld_exporter can scrape
// them.  All names are prefixed with "rapid_" for easy identification.
//
// Single source of truth: X(export_name, RapidMonitor::Metrics field).
// The struct field, the refresh assignment and the SHOW_VAR entry are all
// generated from this list, so adding a metric is a one-line change.
#define RAPID_STATUS_VARS(X)                                                                                      \
  X(mempool_capacity_bytes, mempool_capacity_bytes)                                                               \
  X(mempool_allocated_bytes, mempool_allocated_bytes)                                                             \
  X(mempool_used_bytes, mempool_used_bytes)                                                                       \
  X(mempool_peak_usage_bytes, mempool_peak_usage_bytes)                                                           \
  X(mempool_alloc_count, mempool_alloc_count)                                                                     \
  X(mempool_dealloc_count, mempool_dealloc_count)                                                                 \
  X(mempool_failed_allocs, mempool_failed_allocs)                                                                 \
  X(mempool_expansion_count, mempool_expansion_count)                                                             \
  X(mempool_defrag_count, mempool_defrag_count)                                                                   \
  X(loaded_tables, loaded_tables)                                                                                 \
  X(loaded_part_tables, loaded_part_tables)                                                                       \
  X(total_imcus, total_imcus)                                                                                     \
  X(total_cus, total_cus)                                                                                         \
  X(total_rows, total_rows)                                                                                       \
  X(total_physical_rows, total_physical_rows)                                                                     \
  X(estimated_data_size_bytes, estimated_data_size_bytes)                                                         \
  X(estimated_compressed_size_bytes, estimated_compressed_size_bytes)                                             \
  X(pop_thread_running, rapid_pop_thread_running)                                                                 \
  X(pop_loop_counter, rapid_pop_loop_counter)                                                                     \
  X(pop_data_remaining_bytes, rapid_pop_data_sz)                                                                  \
  X(pop_buffer_tables, total_buffer_tables)                                                                       \
  X(pop_tables_in_progress, tables_in_progress)                                                                   \
  X(pop_worker_threads, total_worker_threads)                                                                     \
  X(pop_worker_pending_bytes, worker_pending_bytes)                                                               \
  X(bg_queue_size, bg_pool_queue_size)                                                                            \
  X(bg_active_workers, bg_active_workers)                                                                         \
  X(bg_total_workers, bg_total_workers)                                                                           \
  X(bg_concurrent_gc, bg_concurrent_gc)                                                                           \
  X(bg_concurrent_compact, bg_concurrent_compact)                                                                 \
  X(bg_concurrent_stats, bg_concurrent_stats)                                                                     \
  X(bg_tasks_submitted, bg_tasks_submitted)                                                                       \
  X(bg_tasks_completed, bg_tasks_completed)                                                                       \
  X(bg_tasks_failed, bg_tasks_failed)                                                                             \
  X(bg_tasks_cancelled, bg_tasks_cancelled)                                                                       \
  X(bg_tasks_retried, bg_tasks_retried)                                                                           \
  X(gc_total_runs, gc_total_runs)                                                                                 \
  X(gc_total_purged_rows, gc_total_purged_rows)                                                                   \
  X(gc_total_purged_versions, gc_total_purged_versions)                                                           \
  X(gc_last_run_scn, gc_last_run_scn)                                                                             \
  X(gc_last_run_duration_us, gc_last_run_duration_us)                                                             \
  X(recovery_storage_restores, recovery_storage_restores)                                                         \
  X(recovery_primary_reloads, recovery_primary_reloads)                                                           \
  X(recovery_wal_truncation_failures, recovery_wal_truncation_failures)                                           \
  X(compact_total_runs, compact_total_runs)                                                                       \
  X(compact_total_merged_rows, compact_total_merged_rows)                                                         \
  X(compact_last_run_duration_us, compact_last_run_duration_us)                                                   \
  X(query_scans_total, query_scans_total)                                                                         \
  X(query_index_lookups_total, query_index_lookups_total)                                                         \
  X(query_rows_read_total, query_rows_read_total)                                                                 \
  X(query_offload_total, query_offload_total)                                                                     \
  X(query_vectorized_window_rows_total, query_vectorized_window_rows_total)                                       \
  X(query_vectorized_window_simd_rows_total, query_vectorized_window_simd_rows_total)                             \
  X(query_vectorized_window_scalar_rows_total, query_vectorized_window_scalar_rows_total)                         \
  X(query_vectorized_window_spill_rows_total, query_vectorized_window_spill_rows_total)                           \
  X(query_vectorized_window_spill_bytes_total, query_vectorized_window_spill_bytes_total)                         \
  X(query_vectorized_hash_join_spill_rows_total, query_vectorized_hash_join_spill_rows_total)                     \
  X(query_vectorized_aggregate_spill_rows_total, query_vectorized_aggregate_spill_rows_total)                     \
  X(query_vectorized_aggregate_batch_rows_total, query_vectorized_aggregate_batch_rows_total)                     \
  X(query_vectorized_aggregate_row_materializations_total, query_vectorized_aggregate_row_materializations_total) \
  X(query_vectorized_aggregate_dict_cache_hits_total, query_vectorized_aggregate_dict_cache_hits_total)           \
  X(query_vectorized_aggregate_dict_cache_misses_total, query_vectorized_aggregate_dict_cache_misses_total)       \
  X(query_vectorized_aggregate_hash_mem_peak_bytes_sum_total,                                                     \
    query_vectorized_aggregate_hash_mem_peak_bytes_sum_total)                                                     \
  X(query_vectorized_aggregate_hash_memory_peak_bytes_max, query_vectorized_aggregate_hash_memory_peak_bytes_max) \
  X(query_vectorized_sort_rows_total, query_vectorized_sort_rows_total)                                           \
  X(query_vectorized_sort_spill_rows_total, query_vectorized_sort_spill_rows_total)                               \
  X(query_offload_fallback_total, query_offload_fallback_total)                                                   \
  X(active_transactions, active_transactions)                                                                     \
  X(transaction_commits_total, transaction_commits_total)                                                         \
  X(recovery_unresolved_txn_revokes, recovery_unresolved_txn_revokes)                                             \
  X(transaction_rollbacks_total, transaction_rollbacks_total)

struct RapidExportVars {
#define X(f, src) ulonglong f{0};
  RAPID_STATUS_VARS(X)
#undef X
  /* The only non-integer metric: percentage of the memory pool in use. */
  double mempool_usage_percentage{0.0};
};

static RapidExportVars rapid_export_vars;

/** Refresh all rapid_export_vars from the live RapidMonitor::Metrics. */
static void refresh_rapid_export_vars() {
  ShannonBase::RapidMonitor::Metrics m;
  ShannonBase::RapidMonitor::collect_rapid_monitor_metrics(m);

#define X(f, src) rapid_export_vars.f = static_cast<ulonglong>(m.src);
  RAPID_STATUS_VARS(X)
#undef X
  rapid_export_vars.mempool_usage_percentage = m.mempool_usage_percentage;
}

static int show_rapid_change_propagation_status(THD *, SHOW_VAR *var, char *) {
  static const char *const kOn = "ON";
  static const char *const kOff = "OFF";
  bool running = ShannonBase::Populate::shannon_propagation_thread_started.load(std::memory_order_acquire);
  var->type = SHOW_CHAR;
  var->value = const_cast<char *>(running ? kOn : kOff);
  var->scope = SHOW_SCOPE_GLOBAL;
  return 0;
}

// Entries point straight at the refreshed struct fields; show_rapid_runtime_status()
// refreshes them before exposing this array, so no per-metric SHOW_FUNC is needed.
static SHOW_VAR rapid_runtime_status_variables[] = {
#define X(f, src) {"rapid_" #f, (char *)&rapid_export_vars.f, SHOW_LONGLONG, SHOW_SCOPE_GLOBAL},
    RAPID_STATUS_VARS(X)
#undef X
        {"rapid_mempool_usage_percentage", (char *)&rapid_export_vars.mempool_usage_percentage, SHOW_DOUBLE,
         SHOW_SCOPE_GLOBAL},
    {"rapid_change_propagation_status", (char *)&show_rapid_change_propagation_status, SHOW_FUNC, SHOW_SCOPE_GLOBAL},

    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

#undef RAPID_STATUS_VARS

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

  // Range check entirely in long long — no truncating casts. The bound is the
  // ceiling, not the default: capping at the default made the variable
  // impossible to raise, which is the only direction anyone needs it.
  constexpr long long min_val = static_cast<long long>(ShannonBase::SHANNON_MIN_MEMORY_SIZE);
  constexpr long long max_val = static_cast<long long>(ShannonBase::SHANNON_MAX_MEMORY_SIZE);
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
  if (new_size == ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_bytes) return;

  if (ShannonBase::Populate::Populator::active() || ShannonBase::shannon_loaded_tables->size()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Tables have been loaded, cannot change the rapid IMCS memory params,"
             "must unload all loaded tables");
    return;
  }

  ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_bytes = new_size;
  ShannonBase::Utils::MemoryPool::Config new_config(static_cast<size_t>(new_size));
  ShannonBase::shannon_rpd_memory_pool->reinitialize(new_config);
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

  *static_cast<ulonglong *>(save) = static_cast<ulonglong>(input_val);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_pop_buffer_size_max.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from check function */
static void rpd_pop_buff_size_max_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  if (*static_cast<ulonglong *>(var_ptr) == *static_cast<const ulonglong *>(save)) return;

  *static_cast<ulonglong *>(var_ptr) = *static_cast<const ulonglong *>(save);
  ShannonBase::shannon_rpd_engine_cfg.pop_buff_sz_max = *static_cast<const ulonglong *>(save);
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
  long long input;
  if (value->val_int(value, &input)) return 1;  // NULL or non-integer

  // Range-check in 64 bits; the old `(uint)input_val` cast wrapped values >= 2^32 back into range.
  if (input < 1 || static_cast<ulonglong>(input) > ShannonBase::SHANNON_PARALLEL_LOAD_THRESHOLD) return 1;

  *static_cast<ulonglong *>(save) = static_cast<ulonglong>(input);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable rapid_parallel_load_threshold.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from chesck function */
static void rpd_para_load_threshold_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<ulonglong *>(var_ptr) == *static_cast<const ulonglong *>(save)) return;

  *static_cast<ulonglong *>(var_ptr) = *static_cast<const ulonglong *>(save);
  ShannonBase::shannon_rpd_engine_cfg.para_load_threshold = *static_cast<const ulonglong *>(save);
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
  long long input;
  if (value->val_int(value, &input)) return 1;  // NULL or non-integer

  // Allow at least the compile-time default, or 3x the core count on big machines.
  // hardware_concurrency() may return 0 ("unknown"); std::max covers that.
  const uint64_t max_allowed =
      std::max<uint64_t>(3ULL * std::thread::hardware_concurrency(), ShannonBase::SHANNON_PARALLEL_PARTTB_THRESHOLD);

  if (input < 1 || static_cast<uint64_t>(input) > max_allowed) return 1;

  *static_cast<ulonglong *>(save) = static_cast<ulonglong>(input);
  return ShannonBase::SHANNON_SUCCESS;
}

/** Update the system variable shannon_rpd_para_parttb_load_threshold.
This function is registered as a callback with MySQL.
@param[in]  thd       thread handle
@param[out] var_ptr   where the formal string goes
@param[in]  save      immediate result from chesck function */
static void rpd_para_parttb_load_threshold_update(THD *thd, SYS_VAR *, void *var_ptr, const void *save) {
  /* check if there is an actual change */
  if (*static_cast<ulonglong *>(var_ptr) == *static_cast<const ulonglong *>(save)) return;

  *static_cast<ulonglong *>(var_ptr) = *static_cast<const ulonglong *>(save);
  ShannonBase::shannon_rpd_engine_cfg.para_parttb_load_threshold = *static_cast<const ulonglong *>(save);
}

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

  using ShannonBase::Populate::Populator;
  if (Populator::active() || ShannonBase::shannon_loaded_tables->size()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Tables have been loaded, cannot change the rapid sync mode; unload all loaded tables first");
    return 1;
  }

  // rapid_propagation_mode_names is nullptr-terminated; its index IS the mode number.
  const long long mode_count = static_cast<long long>(std::size(rapid_propagation_mode_names)) - 1;
  long long mode{-1};
  if (value->value_type(value) == MYSQL_VALUE_TYPE_INT) {
    if (value->val_int(value, &mode)) return 1;
    if (mode < 0 || mode >= mode_count) {
      sql_print_error("Sync mode value %lld is out of range [0, %lld]", mode, mode_count - 1);
      return 1;
    }
  } else {
    char buff[STRING_BUFFER_USUAL_SIZE];
    int length = sizeof(buff);
    const char *name = value->val_str(value, buff, &length);
    if (name == nullptr) return 1;

    for (long long i = 0; i < mode_count; ++i) {
      if (strcasecmp(name, rapid_propagation_mode_names[i]) == 0) {
        mode = i;
        break;
      }
    }
    if (mode < 0) {
      sql_print_error("Invalid sync mode name: %s", name);
      return 1;
    }
  }

  *static_cast<ulong *>(save) = static_cast<ulong>(mode);
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
  const bool enabled = *static_cast<const bool *>(save);
  bool &current = *static_cast<bool *>(var_ptr);
  if (current == enabled) return;

  current = enabled;
  ShannonBase::shannon_rpd_engine_cfg.self_load_enabled = enabled;

  auto *&mgr = ShannonBase::shannon_self_load_mgr_inst;
  if (!mgr) mgr = ShannonBase::Autopilot::SelfLoadManager::instance();
  if (!mgr || !mgr->initialized()) return;

  if (enabled)
    mgr->start();  // start the AutoLoader thread
  else
    mgr->shutdown();
}

static int check_self_load_interval(THD *thd, SYS_VAR *var, void *save, st_mysql_value *value) {
  constexpr longlong kMinSeconds = 60;      // one minute
  constexpr longlong kMaxSeconds = 604800;  // one week

  longlong seconds;
  if (value->val_int(value, &seconds)) return 1;  // NULL or non-integer input

  if (seconds < kMinSeconds || seconds > kMaxSeconds) {
    my_printf_error(ER_WRONG_VALUE_FOR_VAR, "rapid_self_load_interval_seconds must be between %lld and %lld seconds",
                    MYF(0), kMinSeconds, kMaxSeconds);
    return 1;
  }

  *static_cast<ulonglong *>(save) = static_cast<ulonglong>(seconds);
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
  constexpr double kMin = 0.1;
  constexpr double kMax = 1.0;

  const double requested = *static_cast<const double *>(save);
  const double value = std::clamp(requested, kMin, kMax);

  if (value != requested)
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                        "rapid_purge_efficiency_threshold must be between %.1f and %.1f; adjusted to %.1f", kMin, kMax,
                        value);

  *static_cast<double *>(var_ptr) = value;
  ShannonBase::shannon_rpd_engine_cfg.gc_version_ratio_threshold = value;
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
                          ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_bytes,
                          PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_PERSIST_AS_READ_ONLY,
                          "Size in bytes of the memory pool the rapid engine loads column "
                          "data into. Should not exceed half of physical memory.",
                          rpd_mem_size_max_validate,
                          rpd_mem_size_max_update,
                          ShannonBase::SHANNON_DEFAULT_MEMORY_SIZE,
                          ShannonBase::SHANNON_MIN_MEMORY_SIZE,
                          ShannonBase::SHANNON_MAX_MEMORY_SIZE,
                          0);

static MYSQL_SYSVAR_ULONGLONG(query_memory_size_total,
                              ShannonBase::shannon_rpd_engine_cfg.query_memory_size_total,
                              PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                              "Total memory reservations for Rapid query operators in bytes.",
                              nullptr, nullptr, 256ULL * 1024 * 1024, 64 * 1024, ULLONG_MAX, 0);
static MYSQL_SYSVAR_ULONGLONG(query_memory_size_max,
                              ShannonBase::shannon_rpd_engine_cfg.query_memory_size_max,
                              PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                              "Maximum memory reserved by operators of one Rapid query in bytes.",
                              nullptr, nullptr, 64ULL * 1024 * 1024, 64 * 1024, ULLONG_MAX, 0);
static MYSQL_SYSVAR_ULONGLONG(operator_memory_size_max,
                              ShannonBase::shannon_rpd_engine_cfg.operator_memory_size_max,
                              PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                              "Maximum memory reservation for one Rapid query operator in bytes.",
                              nullptr, nullptr, 16ULL * 1024 * 1024, 64 * 1024, ULLONG_MAX, 0);

static MYSQL_SYSVAR_ULONGLONG(sort_spill_size_max,
                              ShannonBase::shannon_rpd_engine_cfg.sort_spill_size_max,
                              PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                              "Maximum live temporary file bytes per Rapid sort, including merge outputs.",
                              nullptr, nullptr, 8ULL * 1024 * 1024 * 1024,
                              1024 * 1024, ULLONG_MAX, 0);

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
static MYSQL_SYSVAR_ULONGLONG(unresolved_txn_revoke_secs,
                              ShannonBase::shannon_rpd_engine_cfg.unresolved_txn_revoke_secs, PLUGIN_VAR_OPCMDARG,
                              "Seconds an unresolved source transaction may block a capture checkpoint before "
                              "fast recovery is revoked and the table is reloaded from the primary (0 = disabled).",
                              nullptr, nullptr,
                              0,           // default: disabled
                              0,           // min
                              ULLONG_MAX,  // max
                              0);

static MYSQL_SYSVAR_BOOL(lazy_commit_marker, ShannonBase::shannon_rpd_engine_cfg.lazy_commit_marker,
                         PLUGIN_VAR_OPCMDARG,
                         "Certify source COMMIT outcomes in the capture journal lazily, after InnoDB has flushed "
                         "its own redo, instead of forcing a redo flush on every commit.",
                         nullptr, nullptr, false  // default OFF
);
static struct SYS_VAR *rapid_system_variables[] = {
    MYSQL_SYSVAR(memory_size_max),
    MYSQL_SYSVAR(sort_spill_size_max),
    MYSQL_SYSVAR(query_memory_size_total),
    MYSQL_SYSVAR(query_memory_size_max),
    MYSQL_SYSVAR(operator_memory_size_max),
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
    MYSQL_SYSVAR(unresolved_txn_revoke_secs),
    MYSQL_SYSVAR(lazy_commit_marker),
    nullptr,
};

// Runtime metrics only: the configuration knobs are system variables, not status.
static SHOW_VAR rapid_status_variables_export[] = {
    {"", (char *)&show_rapid_runtime_status, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

extern bool srv_is_upgrade_mode;
extern char mysql_home[FN_REFLEN];
extern char mysql_llm_home[FN_REFLEN];
extern bool opt_initialize;
extern long opt_upgrade_mode;
static int Shannonbase_Rapid_Init(MYSQL_PLUGIN p) {
  ShannonBase::shannon_loaded_tables = new ShannonBase::LoadedTables();

  ShannonBase::Utils::MemoryPool::Config config(
      static_cast<size_t>(ShannonBase::shannon_rpd_engine_cfg.memory_pool_size_bytes));
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
  shannon_rapid_hton->find_secondary_engine_offload_fail_reason = FindSecondaryEngineOffloadFailedReason;
  shannon_rapid_hton->set_secondary_engine_offload_fail_reason = SetSecondaryEngineOffloadFailedReasonWrapper;
  shannon_rapid_hton->secondary_engine_check_optimizer_request = SecondaryEngineCheckOptimizerRequest;

  shannon_rapid_hton->commit = rapid_commit;
  shannon_rapid_hton->rollback = rapid_rollback;
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
  }
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
