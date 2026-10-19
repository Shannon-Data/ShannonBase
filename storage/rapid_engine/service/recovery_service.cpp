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

   Copyright (c) 2026, Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/service/recovery_service.h"
#include "storage/rapid_engine/populate/log_commons.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <thread>

#include "include/my_dbug.h"
#include "include/scope_guard.h"
#include "sql/dd/dd_kill_immunizer.h"  // dd::DD_kill_immunizer
#include "sql/debug_sync.h"
#include "sql/field.h"
#include "sql/handler.h"         // handler::ha_records
#include "sql/log.h"             // sql_print_error
#include "sql/mdl.h"             // MDL_SHARED_NO_WRITE
#include "sql/mysqld.h"          // connection_events_loop_aborted, mysql_real_data_home
#include "sql/partition_info.h"  // partition_info
#include "sql/sql_base.h"        // close_thread_tables
#include "sql/sql_class.h"       // THD
#include "sql/table.h"           // TABLE
#include "sql/transaction.h"
#include "storage/rapid_engine/populate/log_dml_notification.h"
#include "storage/rapid_engine/utils/binlog_position.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"  // shannon_loaded_tables, RapidShare
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/imcs/table0persistence.h"
#include "storage/rapid_engine/include/rapid_config.h"  // shannon_rpd_engine_cfg
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"  // recovery counters
#include "storage/rapid_engine/populate/log_binlog_parser.h"
#include "storage/rapid_engine/populate/log_populate.h"  // Populator::start
#include "storage/rapid_engine/recovery/recovery_load.h"
#include "storage/rapid_engine/trx/transaction.h"  // Transaction, TransactionCoordinator
#include "storage/rapid_engine/utils/utils.h"      // Util::open_table_by_name

#include "storage/rapid_engine/include/rapid_propagation_config.h"
#include "storage/rapid_engine/utils/background_session.h"

namespace ShannonBase {
namespace Service {
using Recovery::CheckpointScheduler;
using Recovery::LoadFlagManager;
using Recovery::RecoveryManager;
using Recovery::SecondaryLoadedTable;
using Recovery::SourceRecoveryValidation;
using Recovery::SourceValidationContext;
using Recovery::SourceValidationMode;
using Recovery::SourceValidationPolicy;
using Recovery::SourceValidationResult;
bool RecoveryJob::execute() {
  std::shared_lock mode_lock(propagation_mode_mutex);
  if (!propagation_backend_available(configured_change_propagation_mode.load())) return false;
  const auto &info = m_table_info;

  // Once the table is registered, or its previous image has been discarded, capture
  // (or the absence of an image) covers it and the pending-recovery fence can go.
  auto settle_pending = [&info]() {
    auto *sched = CheckpointScheduler::global();
    auto *recovery = sched ? sched->recovery_manager() : nullptr;
    if (recovery) recovery->clear_recovery_pending(info.schema_name, info.table_name);
  };

  DBUG_PRINT("recovery", ("RecoveryJob::execute - %s.%s (partitioned=%d)", info.schema_name.c_str(),
                          info.table_name.c_str(), info.is_partitioned ? 1 : 0));

  // Skip if already present in IMCS (idempotent).
  if (shannon_loaded_tables->get(info.schema_name, info.table_name)) {
    settle_pending();
    DBUG_PRINT("recovery",
               ("RecoveryJob: skip %s.%s - already in IMCS", info.schema_name.c_str(), info.table_name.c_str()));
    return true;
  }

  Utils::BackgroundSession session;
  if (!session.is_valid()) {
    std::string log_msg = "RecoveryJob: cannot create admin session for " + info.schema_name + "." + info.table_name;
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }
  THD *thd = session.thd();
  auto &framework = RecoveryFramework::instance();
  if (!framework.register_session(thd)) return false;
  auto unregister_session = create_scope_guard([&] { framework.unregister_session(thd); });

  // A revoked or uninspectable recovery image requires an explicit user load.
  // Decide before taking source MDL: AP failure must not trigger a primary scan
  // or hold a source write lock merely to rediscover the known failure.
  auto *scheduler = CheckpointScheduler::global();
  auto *recovery = scheduler ? scheduler->recovery_manager() : nullptr;
  auto manager = recovery ? recovery->table_manager(info.schema_name, info.table_name) : nullptr;
  std::unique_lock<std::mutex> recovery_lock;
  if (manager) recovery_lock = std::unique_lock<std::mutex>(manager->recovery_mutex());
  // MDL_SHARED_NO_WRITE is compatible with another recovery job. Recheck after
  // serializing the table so a second job cannot replay into the first image.
  if (shannon_loaded_tables->get(info.schema_name, info.table_name)) {
    settle_pending();
    return true;
  }
  auto reject_revoked_image = [&]() {
    if (!manager || (!manager->recovery_required() && !manager->recovery_tainted())) return false;
    const std::string message = "RecoveryJob: " + info.schema_name + "." + info.table_name +
                                " recovery eligibility is revoked or cannot be verified; automatic recovery stopped; "
                                "run ALTER TABLE SECONDARY_LOAD to reload from the primary";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, message.c_str());
    // Retain pending ownership and the old image's fence until SECONDARY_LOAD
    // resets the epoch. Returning failure does not change the primary commit.
    return true;
  };
  if (reject_revoked_image()) return false;

  DBUG_SIGNAL_WAIT_FOR(thd, "rapid_pause_restart_recovery", "rapid_restart_recovery_paused",
                       "rapid_restart_recovery_continue");
  if (thd->killed) return false;

  // Source state remains authoritative. Fast recovery requires a matching
  // column checkpoint and complete committed MySQL binlog history.
  // Block source DML and DDL until the primary scan, registration and
  // propagation startup are complete. Otherwise a source commit can fall
  // between the scan and registration, when capture still ignores this table.
  // Utils::BackgroundSession releases this transaction-duration lock on all exits.
  MDL_request source_lock;
  MDL_REQUEST_INIT(&source_lock, MDL_key::TABLE, info.schema_name.c_str(), info.table_name.c_str(), MDL_SHARED_NO_WRITE,
                   MDL_TRANSACTION);
  if (thd->mdl_context.acquire_lock(&source_lock, thd->variables.lock_wait_timeout)) {
    if (thd->killed) return false;
    std::string log_msg =
        "RecoveryJob: cannot lock " + info.schema_name + "." + info.table_name + " for a consistent primary reload";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }
  // A user SECONDARY_LOAD may have completed while we waited for the lock.
  if (shannon_loaded_tables->get(info.schema_name, info.table_name)) {
    settle_pending();
    return true;
  }

  // A source writer can revoke the image while we wait for MDL. Recheck at
  // the stable source cut instead of falling through to an automatic reload.
  if (reject_revoked_image()) return false;
  m_source_validation_failed = false;

  if (try_snapshot_recovery(thd)) {
    settle_pending();
    RapidMonitor::rapid_counters.recovery_storage_restores.fetch_add(1, std::memory_order_relaxed);
    start_change_propagation();
    schedule_checkpoint_async();
    std::string log_msg = "RecoveryJob: " + info.schema_name + "." + info.table_name +
                          " recovered from checkpoint and committed MySQL binlog";
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return true;
  }

  if (thd->killed || reject_revoked_image()) return false;

  // A requested FULL check cannot silently fall back to FAST or trigger an
  // automatic source reload. Keep the failed table unpublished for user load.
  if (m_source_validation_failed) {
    if (manager) manager->require_recovery();
    const std::string message = "RecoveryJob: " + info.schema_name + "." + info.table_name +
                                " recovery eligibility is revoked or cannot be verified; automatic recovery stopped; "
                                "full source validation failed or is unavailable; "
                                "run ALTER TABLE SECONDARY_LOAD to reload from the primary";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, message.c_str());
    return false;
  }

  // A reload renumbers every row from InnoDB, so nothing on disk for this
  // table describes the layout that is about to exist.  Start a fresh LSN epoch
  // before the reload so the first DML after publication belongs to the new
  // epoch, once the source write lock is released.
  //
  // This throws away whatever checkpoint state the table had. Say so, with the
  // reason, before it happens, including in release builds.
  {
    std::string log_msg = "RecoveryJob: " + info.schema_name + "." + info.table_name +
                          " has no complete, compatible binlog checkpoint anchor; rebuilding from InnoDB "
                          "and discarding its previous recovery epoch";
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
  }
  if (!discard_stale_recovery_state()) {
    std::string log_msg = "RecoveryJob: refusing to reload " + info.schema_name + "." + info.table_name +
                          " because its previous recovery epoch could not be safely discarded";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }
  settle_pending();  // nothing on disk describes the old image any more

  const auto t1 = std::chrono::steady_clock::now();
  bool ok = info.is_partitioned ? reload_partitioned_table(thd) : reload_normal_table(thd);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
  if (ok) {
    RapidMonitor::rapid_counters.recovery_primary_reloads.fetch_add(1, std::memory_order_relaxed);
    DBUG_PRINT("recovery", ("RecoveryJob: [RELOAD] successfully reloaded %s.%s in %ld ms — snapshot scheduled",
                            info.schema_name.c_str(), info.table_name.c_str(), ms));
  } else {
    std::string log_msg = "RecoveryJob: [RELOAD] FAILED to reload " + info.schema_name + "." + info.table_name +
                          " after " + std::to_string(ms) + " ms (recovery continues)";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
  }

  // Checkpoint the new epoch for subsequent binlog recovery.
  // Partitioned tables are excluded:
  // they have no per-partition checkpoint identity to snapshot against.
  if (ok && !info.is_partitioned) schedule_checkpoint_async();
  if (ok) start_change_propagation();

  return ok;
}

/**
  Bring change propagation up for a table this job just restored.

  ALTER TABLE ... SECONDARY_LOAD does this at the end of ha_rapid::load_table(),
  but recovery loads a table without going through the handler. Without this
  call the coordinator thread never starts, and the first DML after the restart
  takes the "captured a change while propagation is stopped" branch in
  PopulatorImpl::write_buffer_impl(): the table is quarantined for good, its
  contents freeze at the restored state, and Rapid reads it as stale until
  someone reloads it by hand.

  start() is idempotent -- it no-ops once the coordinator is running -- so
  calling it per recovered table costs nothing after the first.
*/
void RecoveryJob::start_change_propagation() const { ShannonBase::Populate::Populator::start(); }

bool RecoveryJob::try_snapshot_recovery(THD *thd) {
  // A partitioned table has no usable snapshot: its partitions never wrote one
  // (see RpdTable::recovery_supported()), and any generation left behind by an
  // older build describes only whichever partition triggered it.  Rebuild from
  // InnoDB.
  if (m_table_info.is_partitioned) return false;

  auto *sched = CheckpointScheduler::global();
  if (!sched) return false;

  auto *mgr = sched->recovery_manager();
  if (!mgr) return false;

  const auto &info = m_table_info;
  auto table_manager = mgr->table_manager(info.schema_name, info.table_name);
  auto *capture = table_manager ? table_manager->notifications() : nullptr;
  if (!capture) return false;  // no notification tracker
  // Bail before creating anything: this lane must not publish a table whose
  // previous image was revoked. load_from_snapshots() repeats the check, which
  // is what actually guards every caller.
  if (table_manager->recovery_tainted()) return false;
  std::lock_guard capture_gate(capture->mutex());

  // Is there a snapshot on disk?
  if (!mgr->has_durable_checkpoint(info.schema_name, info.table_name)) return false;

  // Create empty in-memory table structure (schema metadata).
  Utils::TableGuard source_guard(thd, info.schema_name, info.table_name, TL_READ_WITH_SHARED_LOCKS);
  TABLE *source = source_guard.get();
  if (!source) {
    DBUG_PRINT("recovery",
               ("RecoveryJob(snapshot): cannot open %s.%s", info.schema_name.c_str(), info.table_name.c_str()));
    return false;
  }

  Rapid_load_context ctx{info.schema_name, info.table_name};
  ctx.m_thd = thd;
  ctx.m_table = source;
  ctx.m_table_id = source->file->get_table_id();

  const auto table_id = ctx.m_table_id;
  const int rc = Imcs::Imcs::instance()->create_table_memo(&ctx, source);
  source_guard.close();
  source = nullptr;

  if (rc != SHANNON_SUCCESS) {
    DBUG_PRINT("recovery", ("RecoveryJob(snapshot): create_table_memo failed for %s.%s", info.schema_name.c_str(),
                            info.table_name.c_str()));
    return false;
  }

  // From here on the table has an entry in IMCS.  Every failure below has to
  // take it back out again: execute() falls through to the slow lane, whose
  // guard_load() sees a pre-existing entry and therefore does not clean up,
  // create_table_memo()'s emplace() is a no-op on an existing key, and
  // load_table_impl() then appends the whole InnoDB table on top of the rows
  // the fast lane had already restored -- duplicate rows, duplicate ART keys.
  bool published{false};
  auto drop_partial_table = create_scope_guard([&]() {
    if (!published) Imcs::Imcs::instance()->cleanup(table_id);
  });

  auto rpd_table = Imcs::Imcs::instance()->get_rpd_table_by_name(info.schema_name, info.table_name);
  if (!rpd_table) return false;

  // Restore committed column state before applying committed source changes.
  uint64_t generation = 0;
  if (!mgr->load_from_snapshots(info.schema_name, info.table_name, rpd_table.get(), &generation)) return false;
  auto manifest = table_manager->load_manifest(generation);
  if (!manifest.ok()) return false;

  TransactionCoordinator::instance().observe_commit_scn(manifest.value.persisted_scn);

  // Reconnect Field* (cannot be serialised; patch from live TABLE).
  TABLE *patched_src = nullptr;
  if (!reconstruct_field_pointers(thd, rpd_table.get(), patched_src)) return false;
  Utils::TableGuard patched_guard(thd, patched_src);

  // Rebuild the ART indexes from the restored column cells before binlog
  // mutations run through normal row/index maintenance. Without this the table
  // comes back with empty indexes, and an empty ART does not fall back to a
  // scan -- an indexed lookup just finds nothing. The full scan stays correct,
  // so the damage is silent: SELECT ... WHERE pk = ? answers zero rows on a
  // table that plainly holds the row.
  //
  // It runs after reconstruct_field_pointers() because the key codec encodes out of a
  // record image, which needs Fields bound to this TABLE.
  auto *table_impl = dynamic_cast<Imcs::Table *>(rpd_table.get());
  if (table_impl == nullptr) return false;
  if (table_impl->rebuild_indexes(&ctx, patched_src) != SHANNON_SUCCESS) {
    // Fall back to the slow lane rather than publish a table whose indexes
    // disagree with its rows. drop_partial_table takes the IMCS entry back out.
    sql_print_warning("RecoveryJob(snapshot): %s could not rebuild its indexes; recovery rejected",
                      ctx.m_sch_tb_name.c_str());
    return false;
  }

  rpd_table->foreach_imcu([](Imcs::Imcu *imcu) {
    if (imcu) TransactionCoordinator::instance().observe_commit_scn(imcu->get_max_scn());
  });
  ShannonBase::Populate::DML::CopyInfoParser parser;
  ctx.m_table = patched_src;
  bool replayed = Populate::BinlogReplay::replay(
      thd, patched_src, manifest.value.binlog, [&](Populate::change_record_buff_t &record) {
        record.m_table_id = table_id;
        record.m_source_trx_id = 0;  // recovered committed system version, not a live writer
        return parser.apply_change(ctx, record, 0).status == Populate::ChangeApplyResult::Status::APPLIED;
      });
  if (!replayed) return false;
  auto validation = m_source_validation;
  DBUG_EXECUTE_IF("rapid_recovery_full_validation_unavailable", {
    validation.mode = SourceValidationMode::FULL;
    validation.validator.reset();
  });
  const SourceValidationContext validation_context{
      thd, patched_src, rpd_table.get(), &manifest.value,
      std::chrono::steady_clock::now() + std::chrono::milliseconds(shannon_rpd_engine_cfg.query_sync_timeout_ms)};
  auto validation_result = validation.validate(validation_context);
  if (validation.mode == SourceValidationMode::FULL && (thd->killed || thd->is_error()))
    validation_result = SourceValidationResult::UNAVAILABLE;
  if (validation_result != SourceValidationResult::SKIPPED && validation_result != SourceValidationResult::VERIFIED) {
    m_source_validation_failed = true;
    return false;
  }
  capture->initialize(Utils::BinlogPositionReader::current_position());
  rpd_table->update_statistics(true);
  published = register_in_loaded_tables(thd, patched_src, rpd_table.get());
  if (published) {
    Utils::Util::update_rpd_meta_info(&ctx, patched_src, Utils::Util::STAGE::END);
    auto mirror = RpdMirror::Registry::find(info.schema_name + "." + info.table_name);
    if (mirror)
      mirror->with_meta([&](rpd_table_meta_info_t &meta) {
        meta.snapshot_scn = manifest.value.snapshot_scn;
        meta.persisted_scn = manifest.value.persisted_scn;
      });
  }
  return published;
}

bool RecoveryJob::reconstruct_field_pointers(THD *thd, Imcs::RpdTable *rpd_table, TABLE *&out_source) {
  const auto &info = m_table_info;

  Utils::TableGuard source_guard(thd, info.schema_name, info.table_name, TL_READ_WITH_SHARED_LOCKS);
  out_source = nullptr;
  if (!source_guard.get()) {
    std::string log_msg =
        "RecoveryJob(snapshot): cannot open " + info.schema_name + "." + info.table_name + " to patch Field*";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }

  // Bind the Field clones the table itself owns (Table::Table clones them into its
  // mem_root, and the normal load path uses those). The Fields of out_source belong
  // to a TABLE that the caller closes right after publication; a CU holding one of
  // them would dangle from then on.
  const auto &fields = rpd_table->meta().fields;
  rpd_table->foreach_imcu([&](Imcs::Imcu *imcu) {
    if (!imcu) return;
    for (uint i = 0; i < fields.size(); ++i) {
      Field *f = fields[i].source_fld;
      if (f) imcu->reconstruct_cu_field(i, f, f->charset());
    }
  });
  out_source = source_guard.release();
  return true;
}

bool RecoveryJob::register_in_loaded_tables(THD *thd, TABLE *source, Imcs::RpdTable *rpd_table) {
  const auto &info = m_table_info;

  rpd_table->meta().total_rows.store(rpd_table->count_total_rows(), std::memory_order_relaxed);

  auto m_share = std::make_shared<RapidShare>(*source);
  m_share->is_partitioned = info.is_partitioned;
  m_share->m_tableid = source->file->get_table_id();

  bool registered = false;
  auto rollback_registration = create_scope_guard([&] {
    if (registered) return;
    auto *scheduler = CheckpointScheduler::global();
    auto *recovery = scheduler ? scheduler->recovery_manager() : nullptr;
    if (recovery) recovery->table_manager(info.schema_name, info.table_name)->require_recovery();
    Populate::CommittedBinlog::detach(m_share->m_tableid);
    shannon_loaded_tables->erase(info.schema_name.c_str(), info.table_name.c_str());
    Imcs::Imcs::instance()->cleanup(m_share->m_tableid);
  });
  if (thd->killed) return false;
  // Source MDL excludes writes until propagation is attached and publication
  // completes. Readers must never see a share whose attach can still fail.
  if (configured_change_propagation_mode.load() == CopyInfoMode::COMMITTED_BINLOG &&
      !Populate::CommittedBinlog::attach(thd, source, Utils::BinlogPositionReader::current_position())) {
    Populate::QuarantinePropagationTable(m_share->m_tableid);
    return false;
  }
  DBUG_EXECUTE_IF("rapid_recovery_publish_failure", { return false; });
  shannon_loaded_tables->add(source->s->db.str, source->s->table_name.str, m_share);
  if (!shannon_loaded_tables->get(source->s->db.str, source->s->table_name.str)) {
    // Runs on the recovery admin session: there is no client to raise this to.
    std::string log_msg = "RecoveryJob(snapshot): " + info.schema_name + "." + info.table_name +
                          " did not register in IMCS after restore";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }
  registered = true;
  return true;
}

bool RecoveryJob::discard_stale_recovery_state() {
  auto *sched = CheckpointScheduler::global();
  if (!sched) return true;
  auto *mgr = sched->recovery_manager();
  if (!mgr) return true;
  auto tbl_mgr = mgr->table_manager(m_table_info.schema_name, m_table_info.table_name);
  if (!tbl_mgr) return false;

  if (!tbl_mgr->reset_epoch()) {
    std::string log_msg = "RecoveryJob: could not reset the recovery epoch for " + m_table_info.schema_name + "." +
                          m_table_info.table_name + "; checkpointing is disabled for this table until restart";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }
  // The table is about to be rebuilt from InnoDB, so the earlier "must reload"
  // decision has been discharged.
  tbl_mgr->clear_recovery_taint();
  return true;
}

void RecoveryJob::schedule_checkpoint_async() {
  if (auto *sched = CheckpointScheduler::global()) sched->enqueue(m_table_info.schema_name, m_table_info.table_name);
}

bool RecoveryJob::reload_normal_table(THD *thd) {
  const auto &info = m_table_info;

  Utils::TableGuard source_guard(thd, info.schema_name, info.table_name, TL_READ_WITH_SHARED_LOCKS);
  TABLE *source = source_guard.get();
  if (!source) {
    std::string log_msg = "RecoveryJob: cannot open table " + info.schema_name + "." + info.table_name + " for reading";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }

  Rapid_load_context context{info.schema_name, info.table_name};
  context.m_thd = thd;
  context.m_table = source;
  context.m_table_id = source->file->get_table_id();
  context.m_extra_info.m_scn = TransactionCoordinator::instance().allocate_scn();
  // Bulk source loads create committed baseline rows without DML journals.
  context.m_extra_info.m_oper = ShannonBase::Rapid_context::extra_info_t::OperType::LOAD;

  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::BEGIN);
  int result = ShannonBase::Imcs::Imcs::instance()->load_table(&context, source);
  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::END);
  if (result == SHANNON_SUCCESS) {
    TransactionCoordinator::instance().observe_commit_scn(context.m_extra_info.m_scn);
    auto table =
        Imcs::Imcs::instance()->get_rpd_table<std::shared_ptr<ShannonBase::Imcs::RpdTable>>(context.m_table_id);
    if (!table || !register_in_loaded_tables(thd, source, table.get())) {
      result = HA_ERR_GENERIC;
    }
  }
  return (result == SHANNON_SUCCESS);
}

bool RecoveryJob::reload_partitioned_table(THD *thd) {
  const auto &info = m_table_info;

  Utils::TableGuard source_guard(thd, info.schema_name, info.table_name, TL_READ_WITH_SHARED_LOCKS);
  TABLE *source = source_guard.get();
  if (!source) {
    std::string log_msg =
        "RecoveryJob: cannot open partitioned table " + info.schema_name + "." + info.table_name + " for reading";
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, log_msg.c_str());
    return false;
  }

  Rapid_load_context context{info.schema_name, info.table_name};
  context.m_thd = thd;
  context.m_table = source;
  context.m_table_id = source->file->get_table_id();
  context.m_extra_info.m_scn = TransactionCoordinator::instance().allocate_scn();
  // Bulk source loads create committed baseline rows without DML journals.
  context.m_extra_info.m_oper = ShannonBase::Rapid_context::extra_info_t::OperType::LOAD;

  // Enumerate every partition, the way ha_rapidpart::load_table() does for
  // ALTER TABLE ... SECONDARY_LOAD. Both PartTable::build_partitions() and
  // load_innodbpart() are driven entirely by this map: leaving it empty builds
  // no partitions, copies no rows, and still reports success -- the table comes
  // back from a restart empty while InnoDB still has every row.
  if (source->part_info == nullptr) {
    sql_print_error("RecoveryJob: %s has no partition info; cannot reload it", context.m_sch_tb_name.c_str());
    return false;
  }
  for (uint index = 0; index < source->part_info->get_tot_partitions(); ++index) {
    context.m_extra_info.m_partition_infos.emplace(source->part_info->partitions[index]->partition_name, index);
  }

  // The load path stamps every row with the loading transaction's id and SCN.
  context.m_trx = Transaction::get_or_create_trx(thd);
  if (context.m_trx == nullptr) {
    sql_print_error("RecoveryJob: cannot start a Rapid transaction for %s", context.m_sch_tb_name.c_str());
    return false;
  }
  context.m_trx->begin_stmt();
  context.m_extra_info.m_trxid = context.m_trx->get_id();
  context.m_extra_info.m_scn = TransactionCoordinator::instance().allocate_scn();

  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::BEGIN);
  int result = ShannonBase::Imcs::Imcs::instance()->load_parttable(&context, source);
  Utils::Util::update_rpd_meta_info(&context, source, Utils::Util::STAGE::END);

  bool success = (result == SHANNON_SUCCESS);
  if (success)
    context.m_trx->commit();
  else
    context.m_trx->rollback_stmt();
  DBUG_PRINT("recovery", ("reload_partitioned_table: load_parttable %s for %s.%s%s", success ? "succeeded" : "failed",
                          info.schema_name.c_str(), info.table_name.c_str(),
                          success ? "" : (std::string(" (err=") + std::to_string(result) + ")").c_str()));

  if (success) {
    auto table =
        Imcs::Imcs::instance()->get_rpd_table<std::shared_ptr<ShannonBase::Imcs::RpdTable>>(context.m_table_id);
    success = table && register_in_loaded_tables(thd, source, table.get());
  }

  return success;
}

bool DDWorker::start() {
  if (m_thread.joinable()) return true;

  m_stop.store(false, std::memory_order_release);
  m_done.store(false, std::memory_order_release);

  m_thread = std::thread(&DDWorker::run, this);
  DBUG_PRINT("recovery", ("DDWorker: background thread started"));
  return true;
}

void DDWorker::stop() {
  {
    std::unique_lock<std::mutex> lk(m_mutex);
    m_stop.store(true, std::memory_order_release);
  }
  m_cv.notify_all();
  if (m_thread.joinable()) {
    m_thread.join();
    DBUG_PRINT("recovery", ("DDWorker: background thread stopped"));
  }
}

void DDWorker::run() {
  if (!Utils::Util::wait_for_server_bootup(300, [this] { return m_stop.load(std::memory_order_acquire); })) {
    m_done.store(true, std::memory_order_release);
    return;
  }
  if (m_stop.load(std::memory_order_acquire)) {
    m_done.store(true, std::memory_order_release);
    return;
  }

  Utils::BackgroundSession session;
  if (!session.is_valid()) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "DDWorker: failed to create admin session");
    m_done.store(true, std::memory_order_release);
    return;
  }
  THD *thd = session.thd();

  {
    const dd::DD_kill_immunizer kill_immunizer(thd);
    std::vector<SecondaryLoadedTable> found;
    bool incomplete = false;
    int ret = LoadFlagManager::instance().query_loaded_tables(thd, found, &incomplete);
    if (ret == 0 && incomplete)
      LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG,
             "DDWorker: the data dictionary walk skipped at least one schema; its loaded tables are not recovered");
    if (ret != 0) {
      std::string warning_str = "DDWorker: query_loaded_tables failed with error code " + std::to_string(ret);
      LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, warning_str.c_str());
      m_done.store(true, std::memory_order_release);
      return;
    }
    DBUG_PRINT("recovery", ("DDWorker: found %zu table(s) with secondary_load=1 in Data "
                            "Dictionary",
                            found.size()));
    m_found_tables = std::move(found);
  }
  m_done.store(true, std::memory_order_release);
}

bool RecoveryFramework::is_global_state_empty() const {
  std::atomic<size_t> count{0};
  Imcs::Imcs::instance()->for_each_table([&count](Imcs::RpdTable *) { count++; });
  return count.load() == 0;
}

std::string RecoveryFramework::resolve_snapshot_dir() {
  if (!ShannonBase::shannon_rpd_engine_cfg.snapshot_dir.empty())
    return ShannonBase::shannon_rpd_engine_cfg.snapshot_dir;
  return std::string(mysql_real_data_home) + "/rapid_snapshots";
}

void RecoveryFramework::invalidate_external_global_state() {
  // No scheduler runs this session, so nothing captures source changes. Every
  // image and journal left on disk by an earlier session would keep a valid
  // certificate while going stale; revoke them so a later restart with the flag ON
  // reloads from the primary instead of fast-restoring them.
  RecoveryManager on_disk(resolve_snapshot_dir());
  const size_t revoked = on_disk.revoke_all_on_disk();
  if (revoked > 0) {
    std::string msg = "RecoveryFramework: rapid_reload_on_restart=OFF - revoked the on-disk recovery images of " +
                      std::to_string(revoked) + " table(s); they will be reloaded if the flag is turned back on";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, msg.c_str());
  }
}

void RecoveryFramework::process_external_global_state() {
  if (!ShannonBase::shannon_rpd_engine_cfg.reload_on_restart) {
    if (m_global_state_empty.load()) invalidate_external_global_state();
    return;
  }

  const std::string snap_dir = resolve_snapshot_dir();

  std::error_code ec;
  std::filesystem::create_directories(snap_dir, ec);
  if (ec) {
    std::string warning_str = "RecoveryFramework: cannot create snapshot dir '" + snap_dir + "': " + ec.message() +
                              " - snapshots will be unavailable this session";
    LogErr(WARNING_LEVEL, ER_LOG_PRINTF_MSG, warning_str.c_str());
  }

  uint32_t interval_secs = (ShannonBase::shannon_rpd_engine_cfg.snapshot_interval_secs > 0)
                               ? ShannonBase::shannon_rpd_engine_cfg.snapshot_interval_secs
                               : 300u;
  DBUG_EXECUTE_IF("rapid_table_state_test_tick", { interval_secs = 1; });

  CheckpointScheduler::Config chk_cfg;
  chk_cfg.snapshot_base_dir = snap_dir;
  chk_cfg.interval = std::chrono::seconds(interval_secs);

  m_checkpoint_scheduler = std::make_unique<CheckpointScheduler>(chk_cfg);
  if (m_checkpoint_scheduler->start()) {
    CheckpointScheduler::set_global(m_checkpoint_scheduler.get());
    // Runs in plugin init, before connections: from here until each table's
    // RecoveryJob settles it, a source change to a table with an on-disk image
    // revokes that image (see RecoveryManager::note_unregistered_change).
    const size_t pending = m_checkpoint_scheduler->recovery_manager()->scan_pending_recovery();
    DBUG_PRINT("recovery", ("RecoveryFramework: CheckpointScheduler started, %zu table(s) awaiting recovery", pending));
  } else {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
           "RecoveryFramework: CheckpointScheduler failed to start — "
           "checkpoint maintenance unavailable this session");
    m_checkpoint_scheduler.reset();
  }

  m_dd_worker = std::make_unique<DDWorker>();
  if (!m_dd_worker->start()) {
    LogErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
           "RecoveryFramework: failed to start DDWorker - "
           "restart reload will not be performed");
    m_dd_worker.reset();
  }
}

void RecoveryFramework::dispatch_jobs(const std::vector<SecondaryLoadedTable> &tables) {
  if (tables.empty()) {
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, "RecoveryFramework: no tables to reload");
    return;
  }

  DBUG_PRINT("recovery", ("RecoveryFramework: dispatching reload jobs for %zu table(s)", tables.size()));

  // Each job holds a THD, an MDL lock and (on the slow lane) a full primary scan;
  // one thread per table would start thousands at once on a large instance.
  constexpr size_t kMaxParallelRecoveryJobs = 8;

  auto jobs = std::make_shared<const std::vector<SecondaryLoadedTable>>(tables);
  auto next = std::make_shared<std::atomic<size_t>>(0);
  for (size_t worker = 0; worker < std::min(kMaxParallelRecoveryJobs, tables.size()); ++worker) {
    if (m_stopped.load(std::memory_order_acquire)) break;
    m_active_jobs.fetch_add(1, std::memory_order_relaxed);
    std::thread job_thread([this, jobs, next]() {
      auto finish_worker = create_scope_guard([this] {
        {
          std::lock_guard<std::mutex> lk(m_jobs_mutex);
          m_active_jobs.fetch_sub(1, std::memory_order_acq_rel);
        }
        m_jobs_cv.notify_all();
      });
      while (!m_stopped.load(std::memory_order_acquire)) {
        const size_t index = next->fetch_add(1, std::memory_order_relaxed);
        if (index >= jobs->size()) break;
        const auto &tbl = (*jobs)[index];
#ifndef NDEBUG
        bool repeat_job = false;
        {
          // execute() destroys its session and thread-local DBUG state on return.
          // Capture the test request while a valid admin THD owns that state.
          Utils::BackgroundSession debug_session;
          if (debug_session.is_valid()) DBUG_EXECUTE_IF("rapid_recovery_repeat_job", { repeat_job = true; });
        }
#endif
        RecoveryJob job(tbl);
        bool ok = job.execute();
#ifndef NDEBUG
        if (repeat_job) {
          const bool repeated = job.execute();
          LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                 repeated == ok ? "RecoveryFramework: repeated recovery returned the same result"
                                : "RecoveryFramework: repeated recovery changed its result");
        }
#endif
        if (ok) m_reloaded_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    {
      std::lock_guard<std::mutex> lock(m_job_threads_mutex);
      m_job_threads.push_back(std::move(job_thread));
    }
  }
}

bool RecoveryFramework::register_session(THD *thd) {
  std::lock_guard lock(m_sessions_mutex);
  if (m_stopped.load(std::memory_order_acquire)) return false;
  m_sessions.insert(thd);
  return true;
}

void RecoveryFramework::unregister_session(THD *thd) {
  std::lock_guard lock(m_sessions_mutex);
  m_sessions.erase(thd);
}

void RecoveryFramework::startup() {
  if (m_started.exchange(true, std::memory_order_acq_rel)) return;
  m_stopped.store(false, std::memory_order_release);
  m_reloaded_count.store(0, std::memory_order_relaxed);
  m_restart_in_progress.store(true, std::memory_order_release);
  auto finish_startup = create_scope_guard([this]() { m_restart_in_progress.store(false, std::memory_order_release); });

  DBUG_PRINT("recovery", ("RecoveryFramework: startup initiated"));

  bool empty = is_global_state_empty();
  m_global_state_empty.store(empty, std::memory_order_release);

  if (!empty) {
    // Not an error -- a non-empty global state means something already
    // populated IMCS -- but it silently skips ALL restart recovery, so a
    // release build gave no clue why no table came back.
    LogErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
           "RecoveryFramework: IMCS global state is not empty; skipping restart recovery");
    return;
  }

  process_external_global_state();

  if (!m_dd_worker) return;

  m_monitoring_thread = std::thread([this]() {
    auto finish_recovery =
        create_scope_guard([this]() { m_restart_in_progress.store(false, std::memory_order_release); });
    while (m_dd_worker && !m_dd_worker->is_done()) {
      if (m_stopped.load(std::memory_order_acquire)) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!m_stopped.load(std::memory_order_acquire) && m_dd_worker) dispatch_jobs(m_dd_worker->found_tables());
    // Dispatch finishing is insufficient: the last jobs still own table state.
    std::unique_lock<std::mutex> lock(m_jobs_mutex);
    m_jobs_cv.wait(lock, [this]() {
      return m_stopped.load(std::memory_order_acquire) || m_active_jobs.load(std::memory_order_acquire) == 0;
    });
  });
  finish_startup.release();
}

void RecoveryFramework::shutdown() {
  // 1. Signal shutdown flags
  m_stopped.store(true, std::memory_order_release);
  {
    std::lock_guard lock(m_sessions_mutex);
    for (THD *thd : m_sessions) {
      mysql_mutex_lock(&thd->LOCK_thd_data);
      thd->awake(THD::KILL_CONNECTION);
      mysql_mutex_unlock(&thd->LOCK_thd_data);
    }
  }
  {
    std::lock_guard<std::mutex> lk(m_jobs_mutex);  // pair with the waiters' predicate
  }
  m_jobs_cv.notify_all();  // wake a dispatcher waiting for a free job slot

  // 2. Stop the DD worker FIRST so that m_dd_worker->is_done() becomes true
  if (m_dd_worker) m_dd_worker->stop();

  // 3. Now it is completely safe to wait for the monitoring thread to complete
  if (m_monitoring_thread.joinable()) m_monitoring_thread.join();

  {
    std::unique_lock<std::mutex> lk(m_jobs_mutex);
    bool drained = m_jobs_cv.wait_for(lk, std::chrono::seconds(5), [this] { return m_active_jobs.load() == 0; });
    if (!drained) {
      // The join below waits for these jobs; say so instead of claiming otherwise.
      sql_print_warning(
          "RecoveryFramework: %zu job(s) still in flight after 5 s; waiting for them to finish before "
          "shutdown continues",
          m_active_jobs.load());
    }
  }
  {
    std::lock_guard<std::mutex> lock(m_job_threads_mutex);
    for (auto &thread : m_job_threads) {
      if (thread.joinable()) {
        if (thread.get_id() != std::this_thread::get_id())
          thread.join();
        else
          thread.detach();
      }
    }
    m_job_threads.clear();
  }

  // A job already publishing when shutdown began may have restarted a
  // propagation worker after the caller's first stop. Drain again only after
  // every recovery session has exited, while persistence managers are alive.
  Populate::CommittedBinlog::shutdown();
  Populate::Populator::shutdown();

  // Stop scheduler AFTER jobs drain. Unpublish it first: tables and hooks reach it
  // through global(), and it must not be handed out once it is stopping.
  if (m_checkpoint_scheduler) {
    CheckpointScheduler::set_global(nullptr);
    m_checkpoint_scheduler->stop();
    m_checkpoint_scheduler.reset();
  }
  m_dd_worker.reset();
  m_started.store(false, std::memory_order_release);
}
}  // namespace Service
}  // namespace ShannonBase
