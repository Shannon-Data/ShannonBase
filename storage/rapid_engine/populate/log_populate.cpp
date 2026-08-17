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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.
*/

#include "storage/rapid_engine/populate/log_populate.h"

#if !defined(_WIN32)
#include <pthread.h>  // For pthread_setname_np
#else
#include <Windows.h>  // For SetThreadDescription
#endif
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>

#include "current_thd.h"
#include "include/os0event.h"
#include "sql/sql_class.h"

#include "storage/innobase/handler/ha_innodb.h"  // thd_to_trx
#include "storage/innobase/include/btr0pcur.h"   //for btr_pcur_t
#include "storage/innobase/include/data0type.h"
#include "storage/innobase/include/dict0dd.h"
#include "storage/innobase/include/dict0dict.h"
#include "storage/innobase/include/dict0mem.h"  //for dict_index_t, etc.
#include "storage/innobase/include/os0thread-create.h"

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"
#include "storage/rapid_engine/populate/log_copyinfo.h"
#include "storage/rapid_engine/populate/log_redolog.h"
#include "storage/rapid_engine/trx/transaction.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_populate_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
namespace Populate {
constexpr uint32_t MAX_RETRY_COUNT = 3;

PopBufferShard shannon_pop_shards[POP_SHARD_COUNT];

// does the pop thread started or not.
std::atomic<bool> shannon_propagation_thread_started{false};

// how many data was in shannon_pop_buff in total?
std::atomic<uint64> shannon_pop_data_sz{0};
// to cache the which tables are processing. in populating queue. In query stage, we will check `shannon_pop_tables`
// to find out the rapid table is updated or not. If tables in query statement are still in do populating, then query
// should go to innnodb or go to rapid.
std::shared_mutex shannon_pop_table_mutex;
std::multiset<std::string> shannon_pop_tables;

// how many times applied round.
static uint64 shannon_rpd_loop_counter{0};

// A change id is an in-process identity only. LSN remains ordering/durability
// metadata and must not be used as a retry-map key because COPY_INFO records can
// legitimately observe the same current log LSN.
static std::atomic<uint64_t> shannon_change_id{1};

static inline void MarkPropagationBufferBroken(const std::shared_ptr<table_pop_buffer_t> &tbuf) {
  if (!tbuf) return;
  tbuf->broken.store(true, std::memory_order_release);
  tbuf->barrier_cv.notify_all();
}

static inline void DetachPropagationBuffer(const std::shared_ptr<table_pop_buffer_t> &tbuf) {
  if (!tbuf) return;
  tbuf->detached.store(true, std::memory_order_release);
  tbuf->barrier_cv.notify_all();
}

/**
 * 64MiB is a global change-propagation-buffer trigger, not a per-table
 * threshold.  When crossed, make every table that currently owns queued bytes
 * eligible for the next coordinator batch and wake the coordinator once.
 */
static void RequestAllBufferedTablesFlush() {
  bool any = false;
  for (auto &shard : shannon_pop_shards) {
    std::shared_lock<std::shared_mutex> lk(shard.mutex);
    for (const auto &[table_id, tbuf] : shard.buffers) {
      (void)table_id;
      if (!tbuf || tbuf->detached.load(std::memory_order_acquire) || tbuf->broken.load(std::memory_order_acquire))
        continue;
      if (tbuf->data_size.load(std::memory_order_acquire) == 0) continue;
      tbuf->pending_flush.store(true, std::memory_order_release);
      any = true;
    }
  }

  if (any && shannon_propagation_thread_started.load(std::memory_order_acquire)) os_event_set(log_sys->rapid_events[0]);
}

struct table_worker_context {
  table_id_t table_key;
  std::shared_ptr<table_pop_buffer_t> buffer;
  IB_thread thread_handle;
  std::atomic<bool> should_stop{false};
  std::mutex mtx;
  std::condition_variable cv;
  std::chrono::steady_clock::time_point last_activity;

  std::vector<change_candidate_t> pending_change_candidates;
  std::atomic<size_t> pending_size{0};

  // change_id <-> retry count.  Never key retries by LSN.
  std::unordered_map<uint64_t, uint32_t> retry_counts;

  table_worker_context(table_id_t key, std::shared_ptr<table_pop_buffer_t> tbuf)
      : table_key(key), buffer(std::move(tbuf)), last_activity(std::chrono::steady_clock::now()) {}

  /**
   * Returns an existing (still-running) worker for @a table_key, or
   * creates a new one.  The returned shared_ptr keeps the context alive
   * even after the worker thread exits and removes itself from the
   * global map — callers may safely use the pointer as long as they
   * hold the shared_ptr.
   */
  static std::shared_ptr<table_worker_context> get_or_create_table_worker(
      const table_id_t &table_key, const std::shared_ptr<table_pop_buffer_t> &buffer);
};

static std::shared_mutex table_workers_mutex;
static std::unordered_map<table_id_t, std::shared_ptr<table_worker_context>> table_workers;

uint64_t get_populator_loop_counter() noexcept { return shannon_rpd_loop_counter; }

void BeginCommittedTransactionPublish(const std::vector<table_id_t> &table_ids) {
  for (table_id_t table_id : table_ids) {
    auto &shard = get_pop_shard(table_id);
    std::shared_ptr<table_pop_buffer_t> tbuf;
    {
      std::unique_lock<std::shared_mutex> lk(shard.mutex);
      auto [it, inserted] = shard.buffers.emplace(table_id, std::shared_ptr<table_pop_buffer_t>{});
      if (inserted || !it->second) it->second = std::make_shared<table_pop_buffer_t>();
      tbuf = it->second;
    }
    tbuf->publish_fence.fetch_add(1, std::memory_order_acq_rel);
  }
}

void EndCommittedTransactionPublish(const std::vector<table_id_t> &table_ids) {
  for (table_id_t table_id : table_ids) {
    auto &shard = get_pop_shard(table_id);
    std::shared_ptr<table_pop_buffer_t> tbuf;
    {
      std::shared_lock<std::shared_mutex> lk(shard.mutex);
      auto it = shard.buffers.find(table_id);
      if (it != shard.buffers.end()) tbuf = it->second;
    }
    if (!tbuf) continue;
    uint32_t old = tbuf->publish_fence.load(std::memory_order_acquire);
    while (old > 0 && !tbuf->publish_fence.compare_exchange_weak(old, old - 1, std::memory_order_acq_rel,
                                                                 std::memory_order_acquire)) {
    }
    if (old == 1) tbuf->barrier_cv.notify_all();
  }
}

void QuarantinePropagationTables(const std::vector<table_id_t> &table_ids) {
  for (table_id_t table_id : table_ids) {
    auto &shard = get_pop_shard(table_id);
    std::shared_ptr<table_pop_buffer_t> tbuf;
    {
      std::unique_lock<std::shared_mutex> lk(shard.mutex);
      auto [it, inserted] = shard.buffers.emplace(table_id, std::shared_ptr<table_pop_buffer_t>{});
      if (inserted || !it->second) it->second = std::make_shared<table_pop_buffer_t>();
      tbuf = it->second;
    }
    MarkPropagationBufferBroken(tbuf);
  }
}

void TransactionManager::ensure_subscribed() {
  if (m_subscribed.load(std::memory_order_acquire)) return;

  std::lock_guard<std::mutex> lock(m_subscription_mutex);
  if (m_subscribed.load(std::memory_order_relaxed)) return;

  Transaction::subscribe(this);
  m_subscribed.store(true, std::memory_order_release);
}

TransactionManager::Registration TransactionManager::register_change(THD *thd, table_id_t table_id) {
  ensure_subscribed();
  if (thd == nullptr || table_id == 0) return {};

  trx_t *source_trx = thd_to_trx(thd);
  if (source_trx == nullptr || source_trx->id == 0) return {};

  const Transaction::ID current_id = static_cast<Transaction::ID>(source_trx->id);

  std::lock_guard<std::mutex> lock(m_mutex);
  auto &participant = m_participants[thd];
  if (participant.fail_closed) return {};

  if (participant.source_trx_id == 0) {
    participant.source_trx_id = current_id;
  } else if (participant.source_trx_id != current_id) {
    // A new InnoDB writer id while old COPY_INFO participation is still
    // retained means a terminal lifecycle callback was missed. Never mix two
    // source transactions in the same THD participant.
    if (!participant.touched_tables.empty()) {
      ib::error() << "Rapid: source trx id changed while COPY_INFO propagation state is still active "
                  << "(captured=" << participant.source_trx_id << ", current=" << current_id << ")";
      return {};
    }
    participant = Participant{};
    participant.source_trx_id = current_id;
  }

  participant.touched_tables.insert(table_id);
  participant.statement_has_changes = true;
  ++m_transactions[current_id].tables[table_id].registered;
  return Registration{current_id};
}

void TransactionManager::quarantine_participant(THD *thd, bool require_statement_change, const char *reason) {
  if (thd == nullptr) return;

  std::vector<table_id_t> tables;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_participants.find(thd);
    if (it == m_participants.end()) return;

    Participant &participant = it->second;
    if (require_statement_change && !participant.statement_has_changes) return;
    if (participant.touched_tables.empty()) {
      participant.statement_has_changes = false;
      return;
    }

    participant.fail_closed = true;
    participant.statement_has_changes = false;
    tables.assign(participant.touched_tables.begin(), participant.touched_tables.end());
  }

  std::sort(tables.begin(), tables.end());
  QuarantinePropagationTables(tables);
  sql_print_warning(
      "Rapid immediate COPY_INFO quarantined %zu table(s): %s. "
      "Transaction-level COMMIT/ROLLBACK is supported; partial statement/savepoint undo requires per-operation "
      "Rapid undo and the affected tables must be reloaded before offload",
      tables.size(), reason != nullptr ? reason : "partial rollback after DML was already propagated");
}

void TransactionManager::quarantine_partial_rollback(THD *thd, const char *reason) {
  quarantine_participant(thd, false, reason);
}

void TransactionManager::on_statement_commit(THD *thd) {
  if (thd == nullptr) return;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_participants.find(thd);
  if (it != m_participants.end()) it->second.statement_has_changes = false;
}

void TransactionManager::on_statement_rollback(THD *thd) {
  quarantine_participant(thd, true, "statement rollback after DML was already propagated");
}

void TransactionManager::on_transaction_commit(THD *thd) {
  if (thd == nullptr) return;

  Transaction::ID source_trx_id = 0;
  bool has_changes = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_participants.find(thd);
    if (it == m_participants.end()) return;

    source_trx_id = it->second.source_trx_id;
    has_changes = !it->second.touched_tables.empty();
    m_participants.erase(it);
  }

  if (source_trx_id == 0 || !has_changes) return;

  // Rapid SCN is physical publication/retention metadata. SQL creator
  // visibility remains exclusively the InnoDB ReadView.
  const uint64_t commit_scn = TransactionCoordinator::instance().allocate_scn();
  publish_commit(source_trx_id, commit_scn);
}

void TransactionManager::on_transaction_rollback(THD *thd) {
  if (thd == nullptr) return;

  Transaction::ID source_trx_id = 0;
  bool has_changes = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_participants.find(thd);
    if (it == m_participants.end()) return;

    source_trx_id = it->second.source_trx_id;
    has_changes = !it->second.touched_tables.empty();
    m_participants.erase(it);
  }

  if (source_trx_id != 0 && has_changes) publish_rollback(source_trx_id);
}

void TransactionManager::on_transaction_detach(THD *thd) {
  // Defensive teardown: unresolved COPY_INFO participation must not survive a
  // THD facade. Normal COMMIT/ROLLBACK has already erased the participant, so
  // this is idempotent.
  on_transaction_rollback(thd);
}

void TransactionManager::finalize_table(Transaction::ID txn_id, table_id_t table_id, Outcome outcome,
                                        uint64_t commit_scn) {
  auto *imcs = ShannonBase::Imcs::Imcs::instance();
  if (imcs == nullptr) return;

  auto rpd_table = imcs->get_rpd_table(table_id);
  if (!rpd_table) return;

  for (auto &imcu : rpd_table->get_imcus()) {
    if (!imcu) continue;
    if (outcome == Outcome::COMMITTED) {
      imcu->commit_transaction(txn_id, commit_scn);
    } else if (outcome == Outcome::ABORTED) {
      if (!imcu->rollback_transaction(txn_id)) {
        ib::error() << "Rapid: failed to rollback propagated source transaction " << txn_id << " on table " << table_id;
      }
    }
    TransactionCoordinator::instance().invalidate_visibility_cache(imcu.get());
  }
}

void TransactionManager::erase_if_complete_locked(Transaction::ID txn_id) {
  auto it = m_transactions.find(txn_id);
  if (it == m_transactions.end() || it->second.outcome == Outcome::ACTIVE) return;

  for (const auto &[table_id, progress] : it->second.tables) {
    (void)table_id;
    if (progress.applied < progress.registered) return;
  }
  m_transactions.erase(it);
}

TransactionManager::Outcome TransactionManager::get_outcome(Transaction::ID txn_id, uint64_t *commit_scn) {
  if (commit_scn != nullptr) *commit_scn = 0;
  if (txn_id == 0) return Outcome::ACTIVE;

  std::lock_guard<std::mutex> lock(m_mutex);
  auto it = m_transactions.find(txn_id);
  if (it == m_transactions.end()) return Outcome::ACTIVE;
  if (commit_scn != nullptr) *commit_scn = it->second.commit_scn;
  return it->second.outcome;
}

void TransactionManager::on_change_applied(Transaction::ID txn_id, table_id_t table_id) {
  if (txn_id == 0 || table_id == 0) return;

  Outcome outcome = Outcome::ACTIVE;
  uint64_t commit_scn = 0;
  bool table_complete = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto &txn = m_transactions[txn_id];
    auto &table = txn.tables[table_id];
    ++table.applied;
    table_complete = table.applied >= table.registered;
    outcome = txn.outcome;
    commit_scn = txn.commit_scn;
  }

  // This runs before Populator decrements inflight_size. If the transaction
  // outcome raced ahead of async apply, finalize the last registered record
  // before the table becomes eligible for offload again.
  if (outcome != Outcome::ACTIVE && table_complete) {
    finalize_table(txn_id, table_id, outcome, commit_scn);
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  erase_if_complete_locked(txn_id);
}

void TransactionManager::publish_commit(Transaction::ID txn_id, uint64_t commit_scn) {
  if (txn_id == 0 || commit_scn == 0) return;

  std::vector<table_id_t> tables;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_transactions.find(txn_id);
    if (it == m_transactions.end()) return;

    it->second.outcome = Outcome::COMMITTED;
    it->second.commit_scn = commit_scn;
    tables.reserve(it->second.tables.size());
    for (const auto &[table_id, ignored] : it->second.tables) {
      (void)ignored;
      tables.push_back(table_id);
    }
  }

  TransactionCoordinator::instance().observe_commit_scn(commit_scn);
  for (table_id_t table_id : tables) finalize_table(txn_id, table_id, Outcome::COMMITTED, commit_scn);

  std::lock_guard<std::mutex> lock(m_mutex);
  erase_if_complete_locked(txn_id);
}

void TransactionManager::publish_rollback(Transaction::ID txn_id) {
  if (txn_id == 0) return;

  std::vector<table_id_t> tables;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_transactions.find(txn_id);
    if (it == m_transactions.end()) return;

    it->second.outcome = Outcome::ABORTED;
    it->second.commit_scn = 0;
    tables.reserve(it->second.tables.size());
    for (const auto &[table_id, ignored] : it->second.tables) {
      (void)ignored;
      tables.push_back(table_id);
    }
  }

  for (table_id_t table_id : tables) finalize_table(txn_id, table_id, Outcome::ABORTED, 0);

  std::lock_guard<std::mutex> lock(m_mutex);
  erase_if_complete_locked(txn_id);
}

void TransactionManager::forget_table(table_id_t table_id) {
  if (table_id == 0) return;

  std::lock_guard<std::mutex> lock(m_mutex);

  for (auto it = m_participants.begin(); it != m_participants.end();) {
    it->second.touched_tables.erase(table_id);
    if (it->second.touched_tables.empty())
      it = m_participants.erase(it);
    else
      ++it;
  }

  for (auto it = m_transactions.begin(); it != m_transactions.end();) {
    it->second.tables.erase(table_id);
    if (it->second.tables.empty())
      it = m_transactions.erase(it);
    else
      ++it;
  }
}

void TransactionManager::clear() {
  std::lock_guard<std::mutex> lock(m_mutex);
  m_participants.clear();
  m_transactions.clear();
}

void TransactionManager::start() { ensure_subscribed(); }

void TransactionManager::shutdown() {
  {
    std::lock_guard<std::mutex> lock(m_subscription_mutex);
    if (m_subscribed.exchange(false, std::memory_order_acq_rel)) {
      Transaction::unsubscribe(this);
    }
  }
  clear();
}

size_t get_populator_worker_thread_count() noexcept {
  std::shared_lock<std::shared_mutex> lk(table_workers_mutex);
  return table_workers.size();
}

uint64_t get_populator_worker_pending_bytes() noexcept {
  std::shared_lock<std::shared_mutex> lk(table_workers_mutex);
  uint64_t pending = 0;
  for (auto &entry : table_workers) {
    pending += entry.second->pending_size.load(std::memory_order_relaxed);
  }
  return pending;
}

// Forward declaration — trampoline calls table_worker_func below.
static void table_worker_func(table_worker_context *ctx);

struct table_worker_trampoline {
  std::shared_ptr<table_worker_context> ctx;
  static void *launch(void *arg) {
    auto *self = static_cast<table_worker_trampoline *>(arg);
    auto ctx = std::move(self->ctx);
    delete self;
    table_worker_func(ctx.get());
    return nullptr;
  }
};

static void table_worker_func(table_worker_context *ctx) {
#if !defined(_WIN32)
  const std::string tname = "rapid_change_worker_" + std::to_string(ctx->table_key);
  pthread_setname_np(pthread_self(), tname.c_str());
#else
  const std::wstring tname = L"rapid_change_worker_" + std::to_wstring(ctx->table_key);
  SetThreadDescription(GetCurrentThread(), tname.c_str());
#endif

  THD *thd = create_internal_thd();
  if (!thd) {
    // Losing a worker after the coordinator has moved records out of the ring
    // must not silently clear the lag fence. Quarantine the table so queries
    // stay on the primary engine until reload.
    MarkPropagationBufferBroken(ctx->buffer);
    std::unique_lock<std::shared_mutex> lk(table_workers_mutex);
    auto it = table_workers.find(ctx->table_key);
    if (it != table_workers.end() && it->second.get() == ctx) table_workers.erase(it);
    return;
  }
  thd->system_thread = SYSTEM_THREAD_BACKGROUND;
  thd->security_context()->skip_grants();
  thd->store_globals();
  struct ThdGuard {
    THD *m_thd;
    explicit ThdGuard(THD *thd) : m_thd(thd) {}
    ~ThdGuard() {
      if (!m_thd) return;
      Transaction::free_trx_from_thd(m_thd);
      close_thread_tables(m_thd);
      m_thd->mdl_context.release_transactional_locks();
      destroy_internal_thd(m_thd);
      m_thd = nullptr;
      my_thread_end();
    }
  } thd_guard(thd);

  SHANNON_THREAD_LOCAL LogParser redo_log;
  SHANNON_THREAD_LOCAL CopyInfoParser copy_info_log;
  SHANNON_THREAD_LOCAL Rapid_load_context context;
  context.m_thd = thd;

  while (!ctx->should_stop.load(std::memory_order_acquire)) {
    bool should_exit = false;
    {
      std::unique_lock<std::mutex> lk(ctx->mtx);
      ctx->cv.wait_for(lk, std::chrono::milliseconds(TABLE_WORKER_IDLE_TIMEOUT), [ctx] {
        return ctx->should_stop.load(std::memory_order_acquire) ||
               ctx->pending_size.load(std::memory_order_acquire) > 0;
      });

      if (ctx->should_stop.load(std::memory_order_acquire)) {
        should_exit = true;
      } else {
        const auto idle =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ctx->last_activity)
                .count();
        if (static_cast<uint64_t>(idle) >= TABLE_WORKER_IDLE_TIMEOUT &&
            ctx->pending_size.load(std::memory_order_acquire) == 0) {
          should_exit = true;
        }
      }
    }
    if (should_exit) break;

    std::vector<change_candidate_t> applying;
    {
      std::lock_guard<std::mutex> lk(ctx->mtx);
      if (ctx->pending_size.load(std::memory_order_acquire) > 0) {
        std::swap(applying, ctx->pending_change_candidates);
        ctx->pending_size.store(0, std::memory_order_release);
      }
    }
    if (applying.empty()) continue;

    if (ctx->buffer && ctx->buffer->broken.load(std::memory_order_acquire)) break;

    size_t failed_at = applying.size();
    uint32_t failed_retry = 0;
    bool permanent_failure = false;

    // Preserve table-local change order.  Once record N fails, N+1 and later
    // records are not allowed to overtake it.
    for (size_t i = 0; i < applying.size(); ++i) {
      auto &candidate = applying[i];
      const uint64_t lsn = candidate.lsn;
      change_record_buff_t &rec = candidate.record;
      size_t parsed_bytes = 0;

      switch (rec.m_source) {
        case Source::REDO_LOG: {
          // Source is the record-format contract. REDO records are routed only
          // to LogParser and never through COPY_INFO transaction semantics.
          context.m_trx = nullptr;
          context.m_extra_info.m_trxid = 0;
          context.m_extra_info.m_scn = 0;
          context.m_offpage_data0 = nullptr;
          context.m_offpage_data1 = nullptr;
#ifndef NDEBUG
          context.m_schema_name.clear();
          context.m_table_name.clear();
          context.m_sch_tb_name.clear();
#endif
          const byte *start = rec.m_buff0.get();
          const byte *end = start + rec.m_size;
          parsed_bytes = redo_log.parse_redo(&context, const_cast<byte *>(start), const_cast<byte *>(end));
          break;
        }

        case Source::COPY_INFO: {
          if (rec.m_source_trx_id == 0) {
            // Direct row-image propagation must preserve the real primary
            // InnoDB writer identity. commit_scn==0 is valid and means ACTIVE.
            permanent_failure = true;
            break;
          }

          const Transaction::ID source_txn_id = static_cast<Transaction::ID>(rec.m_source_trx_id);
          uint64_t terminal_scn = 0;
          const auto outcome = TransactionManager::instance().get_outcome(source_txn_id, &terminal_scn);

          if (outcome == TransactionManager::Outcome::ABORTED) {
            // The primary rollback won the race before this record reached Rapid.
            // Do not manufacture an ABORTED physical version only to undo it again.
            parsed_bytes = rec.m_size;
            break;
          }

          context.m_trx = nullptr;
          context.m_extra_info.m_trxid = source_txn_id;
          // A commit can also win the race before this record is applied. In
          // that case create the version as COMMITTED directly; otherwise 0
          // deliberately means ACTIVE until the primary outcome arrives.
          context.m_extra_info.m_scn =
              outcome == TransactionManager::Outcome::COMMITTED ? terminal_scn : rec.m_commit_scn;
#ifndef NDEBUG
          context.m_schema_name = rec.m_schema_name;
          context.m_table_name = rec.m_table_name;
          context.m_sch_tb_name = context.m_schema_name + "." + context.m_table_name;
#endif
          context.m_offpage_data0 = rec.m_offpage_data0.empty() ? nullptr : &rec.m_offpage_data0;
          context.m_offpage_data1 = rec.m_offpage_data1.empty() ? nullptr : &rec.m_offpage_data1;

          const byte *old_start = rec.m_buff0.get();
          const byte *old_end = old_start + rec.m_size;
          const byte *new_start = rec.m_buff1.get();
          const byte *new_end = new_start + rec.m_size;
          parsed_bytes = copy_info_log.parse_copy_info(&context, rec.m_table_id, rec.m_oper,
                                                       const_cast<byte *>(old_start), const_cast<byte *>(old_end),
                                                       const_cast<byte *>(new_start), const_cast<byte *>(new_end));
          break;
        }

        case Source::UN_KNOWN:
        default:
          // Never guess a record format. An unknown source quarantines this
          // table rather than risking corruption by invoking the wrong parser.
          permanent_failure = true;
          break;
      }

      if (parsed_bytes == rec.m_size) {
        ctx->retry_counts.erase(candidate.change_id);

        // The primary COMMIT/ROLLBACK callback may have raced ahead of this
        // asynchronous apply. Finalize this source transaction (if terminal)
        // before dropping inflight_size; once inflight reaches zero the table
        // is eligible for Rapid offload again.
        if (rec.m_source == Source::COPY_INFO) {
          TransactionManager::instance().on_change_applied(static_cast<Transaction::ID>(rec.m_source_trx_id),
                                                           rec.m_table_id);
        }

        if (ctx->buffer) {
          // Table-local enqueue is serialized, and the worker never lets a
          // later candidate overtake a failed predecessor. Therefore this is a
          // contiguous physical-apply watermark for this table even though
          // global change ids may contain gaps from other tables.
          ctx->buffer->inflight_size.fetch_sub(rec.m_size, std::memory_order_acq_rel);
          ctx->buffer->applied_change_id.store(candidate.change_id, std::memory_order_release);
          ctx->buffer->barrier_cv.notify_all();
        }
        continue;
      }

      failed_at = i;
      if (!permanent_failure) {
        failed_retry = ++ctx->retry_counts[candidate.change_id];
        permanent_failure = failed_retry > MAX_RETRY_COUNT;
      } else {
        failed_retry = MAX_RETRY_COUNT + 1;
      }

      // thd is this worker's own internal THD: nothing ever runs SHOW WARNINGS
      // on it, so push_warning_printf() alone is invisible to any operator.
      // Surface the same text (plus whatever specific error the failed parse
      // raised on this THD's diagnostics area, if any) to the server error
      // log too, so a quarantined table is actually diagnosable.
      const char *da_msg =
          thd->is_error() ? thd->get_stmt_da()->message_text() : "no error set on propagation worker THD";
      if (permanent_failure) {
        MarkPropagationBufferBroken(ctx->buffer);
        push_warning_printf(thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE,
                            "Rapid propagation quarantined table %llu at change_id=%llu LSN=%llu; "
                            "the table is stale and must be reloaded before secondary-engine offload",
                            static_cast<unsigned long long>(ctx->table_key),
                            static_cast<unsigned long long>(candidate.change_id), static_cast<unsigned long long>(lsn));
        sql_print_warning(
            "Rapid propagation quarantined table %llu at change_id=%llu LSN=%llu, source=%d, oper=%d, "
            "record_size=%zu, parsed_bytes=%zu: %s",
            static_cast<unsigned long long>(ctx->table_key), static_cast<unsigned long long>(candidate.change_id),
            static_cast<unsigned long long>(lsn), static_cast<int>(rec.m_source), static_cast<int>(rec.m_oper),
            rec.m_size, parsed_bytes, da_msg);
      } else {
        push_warning_printf(thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE,
                            "Propagation failed for table %llu at change_id=%llu LSN=%llu "
                            "(retry %u/%u); blocking later changes until this record succeeds",
                            static_cast<unsigned long long>(ctx->table_key),
                            static_cast<unsigned long long>(candidate.change_id), static_cast<unsigned long long>(lsn),
                            failed_retry, MAX_RETRY_COUNT);
        sql_print_warning(
            "Rapid propagation retry %u/%u for table %llu at change_id=%llu LSN=%llu, source=%d, oper=%d, "
            "record_size=%zu, parsed_bytes=%zu: %s",
            failed_retry, MAX_RETRY_COUNT, static_cast<unsigned long long>(ctx->table_key),
            static_cast<unsigned long long>(candidate.change_id), static_cast<unsigned long long>(lsn),
            static_cast<int>(rec.m_source), static_cast<int>(rec.m_oper), rec.m_size, parsed_bytes, da_msg);
      }
      thd->clear_error();
      break;
    }

    if (failed_at < applying.size()) {
      size_t retry_sz = 0;
      for (size_t i = failed_at; i < applying.size(); ++i) retry_sz += applying[i].record.m_size;

      {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        // Retry the failed record and untouched tail at the FRONT.  Newer records
        // may have been appended by the coordinator while this batch was running;
        // they must stay behind the failed prefix.
        ctx->pending_change_candidates.insert(
            ctx->pending_change_candidates.begin(),
            std::make_move_iterator(applying.begin() + static_cast<ptrdiff_t>(failed_at)),
            std::make_move_iterator(applying.end()));
        ctx->pending_size.fetch_add(retry_sz, std::memory_order_release);

        if (permanent_failure) {
          ctx->should_stop.store(true, std::memory_order_release);
        } else {
          ctx->cv.notify_one();
        }
      }

      if (!permanent_failure) {
        const uint32_t backoff_ms = 1U << std::min<uint32_t>(failed_retry, 6);
        std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
      }
    }

    {
      std::lock_guard<std::mutex> lk(ctx->mtx);
      ctx->last_activity = std::chrono::steady_clock::now();
    }
  }

  {
    std::unique_lock<std::shared_mutex> lk(table_workers_mutex);
    auto it = table_workers.find(ctx->table_key);
    if (it != table_workers.end() && it->second.get() == ctx) table_workers.erase(it);
  }
}

std::shared_ptr<table_worker_context> table_worker_context::get_or_create_table_worker(
    const table_id_t &table_key, const std::shared_ptr<table_pop_buffer_t> &buffer) {
  {
    std::shared_lock<std::shared_mutex> lock(table_workers_mutex);
    auto it = table_workers.find(table_key);
    if (it != table_workers.end() && thread_is_active(it->second->thread_handle)) {
      return it->second;
    }
  }

  std::unique_lock<std::shared_mutex> lock(table_workers_mutex);
  auto it = table_workers.find(table_key);
  if (it != table_workers.end() && thread_is_active(it->second->thread_handle)) {
    return it->second;  // double check
  }

  auto ctx = std::make_shared<table_worker_context>(table_key, buffer);
  auto *tramp = new table_worker_trampoline{ctx};
  IB_thread handle = os_thread_create(rapid_populate_thread_key, 0, table_worker_trampoline::launch, tramp);
  ctx->thread_handle = handle;
  table_workers[table_key] = ctx;
  table_workers[table_key]->thread_handle.start();
  return ctx;
}

/**
 * main entry of pop thread. it monitors shannon_pop_buff, a new mtr_log_rect_t
 * is coming, then it starts a new worker to dealing with this mtr_log_rec_t.
 */
static void parse_log_func_main(log_t *log_ptr) {
#if !defined(_WIN32)
  pthread_setname_np(pthread_self(), "rapid_log_coordinator");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_log_coordinator");
#endif

  // ref: https://dev.mysql.com/doc/heatwave/en/mys-hw-change-propagation.html
  while (srv_shutdown_state.load(std::memory_order_acquire) == SRV_SHUTDOWN_NONE &&
         shannon_propagation_thread_started.load(std::memory_order_acquire)) {
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{POP_MAX_WAIT_TIMEOUT};
    // stop_condition is invoked inside os_event_wait_for to decide whether
    // to stop waiting.  Each shard is locked independently (shared_lock per
    // shard), so concurrent unload_impl (which acquires the unique_lock on
    // the same shard) is properly serialised per shard.  The timeout branch
    // sets pending_flush on buffer objects that are held by shared_ptr,
    // therefore the buffers remain valid even if the corresponding map entry
    // is erased between the two iteration passes.
    auto stop_condition = [&](bool wait) {
      for (auto &shard : shannon_pop_shards) {
        std::shared_lock<std::shared_mutex> lk(shard.mutex);
        for (const auto &[key, tbuf] : shard.buffers) {
          if (tbuf->pending_flush.load(std::memory_order_acquire)) return true;
        }
      }

      if (wait && os_event_is_set(log_sys->rapid_events[0])) return true;

      if (std::chrono::steady_clock::now() >= wait_deadline) {
        for (auto &shard : shannon_pop_shards) {
          std::shared_lock<std::shared_mutex> lk(shard.mutex);
          for (const auto &[key, tbuf] : shard.buffers) {
            if (tbuf->data_size.load(std::memory_order_acquire) > 0)
              tbuf->pending_flush.store(true, std::memory_order_release);
          }
        }
        return true;
      }
      return false;
    };

    os_event_wait_for(log_ptr->rapid_events[0], 0, std::chrono::milliseconds{POP_MAX_WAIT_TIMEOUT}, stop_condition);
    os_event_reset(log_sys->rapid_events[0]);

    if (!shannon_propagation_thread_started.load()) break;

    using FlushEntry = std::pair<table_id_t, std::shared_ptr<table_pop_buffer_t>>;
    std::vector<FlushEntry> tables_to_flush;
    for (auto &shard : shannon_pop_shards) {
      std::shared_lock<std::shared_mutex> lk(shard.mutex);
      for (auto &[key, tbuf] : shard.buffers) {
        if (tbuf->pending_flush.exchange(false, std::memory_order_acq_rel)) {
          tables_to_flush.emplace_back(key, tbuf);
        }
      }
    }
    if (tables_to_flush.empty()) continue;

    for (auto &[table_key, tbuf] : tables_to_flush) {
      std::vector<change_candidate_t> batch_vec;
      batch_vec.reserve(BATCH_PROCESS_NUM);

      change_candidate_t batch[BATCH_PROCESS_NUM];
      size_t count = 0;
      if (tbuf->broken.load(std::memory_order_acquire)) continue;

      size_t moved_bytes = 0;
      {
        // Pair ring removal with its accounting transfer against producer's
        // ring insertion + watermark publication. The lock is table-local and
        // held only for the in-memory queue handoff; worker apply remains fully
        // asynchronous.
        std::unique_lock<std::mutex> queue_guard(tbuf->enqueue_mutex);
        while ((count = tbuf->change_candiates.try_pop_bulk(batch, BATCH_PROCESS_NUM)) > 0) {
          for (size_t i = 0; i < count; ++i) {
            const size_t rec_sz = batch[i].record.m_size;
            tbuf->data_size.fetch_sub(rec_sz, std::memory_order_relaxed);
            shannon_pop_data_sz.fetch_sub(rec_sz, std::memory_order_relaxed);
            moved_bytes += rec_sz;
            batch_vec.push_back(std::move(batch[i]));
          }
        }
      }

      if (batch_vec.empty()) continue;

      // Once a record leaves the ring it is still "pending" until the worker
      // successfully applies it.  This closes the optimizer visibility gap
      // between coordinator pop and worker apply.
      tbuf->inflight_size.fetch_add(moved_bytes, std::memory_order_acq_rel);

      auto worker = table_worker_context::get_or_create_table_worker(table_key, tbuf);
      {
        std::lock_guard lock(worker->mtx);
        worker->pending_change_candidates.insert(worker->pending_change_candidates.end(),
                                                 std::make_move_iterator(batch_vec.begin()),
                                                 std::make_move_iterator(batch_vec.end()));
        worker->pending_size.fetch_add(moved_bytes, std::memory_order_release);
        worker->cv.notify_one();
      }
    }

    shannon_rpd_loop_counter++;
  }

  shannon_propagation_thread_started.store(false, std::memory_order_seq_cst);
}

std::unique_ptr<Populator::Impl> Populator::m_impl = nullptr;

std::unique_ptr<Populator::Impl> &Populator::get_impl() {
  if (!m_impl) {
    // Lazy initialization
    m_impl = std::make_unique<PopulatorImpl>();
  }
  return m_impl;
}

/**
 * Whether the log pop main thread is active or not. true is alive, false dead.
 */
bool Populator::active() { return get_impl()->active_impl(); }

/**
 * To launch log pop main thread.
 */
void Populator::start() { get_impl()->start_impl(); }

void Populator::unload(const table_id_t &table_id) { get_impl()->unload_impl(table_id); }

/**
 * To stop log pop main thread.
 */
void Populator::shutdown() { get_impl()->end_impl(); }

/**
 * write log buffer to remote.
 */
uint Populator::write(FILE *file, uint64_t start_lsn, change_record_buff *changed_rec) {
  return get_impl()->write_impl(file, start_lsn, changed_rec);
}

/**
 * To print thread infos.
 */
void Populator::print_info(FILE *file) { get_impl()->print_info_impl(file); }

/**
 * To send notify to populator main thread to start do propagation.
 */
void Populator::send_notify() { get_impl()->send_notify_impl(); }

/**
 * Preload mysql.indexes into caches.
 */
int Populator::load_indexes_caches() { return get_impl()->load_indexes_caches_impl(); }

bool PopulatorImpl::active_impl() { return thread_is_active(srv_threads.m_change_pop_cordinator); }

void PopulatorImpl::send_notify_impl() { os_event_set(log_sys->rapid_events[0]); }

void PopulatorImpl::start_impl() {
  if (!active_impl() && shannon_loaded_tables->size()) {
    TransactionManager::instance().start();
    srv_threads.m_change_pop_cordinator = os_thread_create(rapid_populate_thread_key, 0, parse_log_func_main, log_sys);
    ShannonBase::Populate::shannon_propagation_thread_started.store(true, std::memory_order_seq_cst);
    srv_threads.m_change_pop_cordinator.start();
    ut_a(active_impl());
  }
}

void PopulatorImpl::unload_impl(const table_id_t &table_id) {
  TransactionManager::instance().forget_table(table_id);
  if (unlikely(!active_impl() || !shannon_loaded_tables->size())) return;
  auto &shard = get_pop_shard(table_id);
  std::shared_ptr<table_pop_buffer_t> detached_buffer;
  {
    std::unique_lock<std::shared_mutex> lk(shard.mutex);
    auto it = shard.buffers.find(table_id);
    if (it != shard.buffers.end()) {
      detached_buffer = it->second;
      DetachPropagationBuffer(detached_buffer);
      shard.buffers.erase(it);
    }
  }

  std::shared_ptr<table_worker_context> ctx_to_destroy;
  {
    std::shared_lock<std::shared_mutex> read_lock(table_workers_mutex);
    auto it = table_workers.find(table_id);
    if (it == table_workers.end()) return;
  }

  {
    std::unique_lock<std::shared_mutex> write_lock(table_workers_mutex);

    auto it = table_workers.find(table_id);
    if (it == table_workers.end()) return;

    if (!thread_is_active(it->second->thread_handle)) {
      ctx_to_destroy = std::move(it->second);
      table_workers.erase(it);
    } else {
      it->second->should_stop.store(true, std::memory_order_release);
      it->second->cv.notify_one();

      ctx_to_destroy = std::move(it->second);
      table_workers.erase(it);
    }
  }

  if (ctx_to_destroy) {
    if (thread_is_active(ctx_to_destroy->thread_handle)) ctx_to_destroy->thread_handle.join();
  }
}

void PopulatorImpl::end_impl() {
  // Only proceed if the coordinator was ever started. If it was not started,
  // there is nothing to tear down and accessing log_sys would be unsafe.
  if (!active_impl()) {
    TransactionManager::instance().shutdown();
    return;
  }

  // Step 1: stop and join the coordinator main thread FIRST, before touching table_workers.
  if (active_impl()) {
    shannon_propagation_thread_started.store(false, std::memory_order_seq_cst);
    os_event_set(log_sys->rapid_events[0]);
    if (thread_is_active(srv_threads.m_change_pop_cordinator)) srv_threads.m_change_pop_cordinator.join();
  }

  // Step 2: signal every table worker still registered (the coordinator is now stopped, so this
  // set can no longer grow).
  {
    std::shared_lock<std::shared_mutex> read_lock(table_workers_mutex);
    for (auto &[tid, ctx] : table_workers) {
      ctx->should_stop.store(true, std::memory_order_release);
      ctx->cv.notify_one();
    }
  }

  // Step 3: move out all ctx
  decltype(table_workers) workers_to_join;
  {
    std::unique_lock<std::shared_mutex> write_lock(table_workers_mutex);
    workers_to_join = std::move(table_workers);
    table_workers.clear();
  }

  // Step 4: waiting for all table workers to finish.
  for (auto &[tid, ctx] : workers_to_join) {
    if (thread_is_active(ctx->thread_handle)) ctx->thread_handle.join();
  }

  // Step 5: clear all status
  shannon_rpd_loop_counter = 0;
  {
    std::unique_lock<std::shared_mutex> ex_lock(shannon_indexes_cache_mutex);
    shannon_indexes_cache.clear();
    shannon_indexes_name.clear();
  }
  shannon_pop_tables.clear();

  for (auto &shard : shannon_pop_shards) {
    std::unique_lock<std::shared_mutex> lk(shard.mutex);
    for (auto &[table_id, tbuf] : shard.buffers) {
      (void)table_id;
      DetachPropagationBuffer(tbuf);
    }
    shard.buffers.clear();
  }
  TransactionManager::instance().shutdown();
  ut_a(!active_impl());
}

uint PopulatorImpl::write_impl(FILE *file, uint64_t start_lsn, change_record_buff *changed_rec) {
  (void)file;
  if (!changed_rec || !shannon_loaded_tables->size()) return SHANNON_SUCCESS;

  const table_id_t table_key = changed_rec->m_table_id;
  const size_t rec_sz = changed_rec->m_size;
  auto &shard = get_pop_shard(table_key);

  std::shared_ptr<table_pop_buffer_t> tbuf;
  {
    std::shared_lock<std::shared_mutex> slk(shard.mutex);
    auto it = shard.buffers.find(table_key);
    if (it != shard.buffers.end()) tbuf = it->second;
  }

  if (!tbuf) {
    std::unique_lock<std::shared_mutex> ulk(shard.mutex);
    auto [it, inserted] = shard.buffers.emplace(table_key, std::shared_ptr<table_pop_buffer_t>{});
    if (inserted || !it->second) it->second = std::make_shared<table_pop_buffer_t>();
    tbuf = it->second;
  }

  // The record source is the only propagation-format discriminator. Validate
  // only source-specific invariants here; do not maintain a second global mode
  // that can disagree with the record already stored in the table buffer.
  bool invalid_record = changed_rec->m_size == 0 || changed_rec->m_buff0 == nullptr;
  switch (changed_rec->m_source) {
    case Source::COPY_INFO:
      // Row-image propagation must keep the real primary creator transaction.
      // commit_scn==0 is valid: it represents an ACTIVE Rapid MVCC version.
      invalid_record = invalid_record || changed_rec->m_source_trx_id == 0;
      break;

    case Source::REDO_LOG:
      // REDO records need an ordering position and serialized redo bytes.
      invalid_record = invalid_record || start_lsn == 0;
      break;

    case Source::UN_KNOWN:
    default:
      invalid_record = true;
      break;
  }

  if (invalid_record) {
    MarkPropagationBufferBroken(tbuf);
    sql_print_warning("Rapid rejected invalid propagation source %u for table %llu",
                      static_cast<unsigned>(changed_rec->m_source), static_cast<unsigned long long>(table_key));
    return SHANNON_SUCCESS;
  }

  // A captured source change cannot simply disappear because propagation was
  // stopped after the primary DML succeeded. Keep the table quarantined until
  // a reload reconstructs it from the primary source.
  if (!active_impl()) {
    MarkPropagationBufferBroken(tbuf);
    return SHANNON_SUCCESS;
  }

  // Once a table is quarantined we deliberately stop accumulating an unbounded
  // backlog.  The table remains permanently non-offloadable until unload/reload.
  if (tbuf->broken.load(std::memory_order_acquire) || tbuf->detached.load(std::memory_order_acquire))
    return SHANNON_SUCCESS;

  // Serialize only each actual ring insertion attempt for this table.  The
  // lock is never held while waiting for a full ring to drain.  A change id is
  // assigned inside this critical section, so successful ring order and
  // table-local change-id order are identical and applied_change_id is a sound
  // watermark. Normal DML never waits for Rapid apply.
  change_candidate_t item(0, start_lsn, std::move(*changed_rec));
  bool warned_full = false;

  for (;;) {
    if (tbuf->broken.load(std::memory_order_acquire) || tbuf->detached.load(std::memory_order_acquire))
      return SHANNON_SUCCESS;

    bool enqueued = false;
    bool crossed_global_buffer = false;
    {
      std::unique_lock<std::mutex> enqueue_guard(tbuf->enqueue_mutex);
      if (tbuf->broken.load(std::memory_order_acquire) || tbuf->detached.load(std::memory_order_acquire))
        return SHANNON_SUCCESS;

      // IDs consumed by failed full-ring attempts are harmless gaps.  What
      // matters is that every successful insertion gets its id while holding
      // the same table-local enqueue lock.
      const uint64_t assigned_change_id = shannon_change_id.fetch_add(1, std::memory_order_relaxed);
      item.change_id = assigned_change_id;
      if (tbuf->change_candiates.try_put(std::move(item))) {
        tbuf->data_size.fetch_add(rec_sz, std::memory_order_relaxed);
        const uint64_t old_global = shannon_pop_data_sz.fetch_add(rec_sz, std::memory_order_acq_rel);

        // Publish the query-visible watermark only after the record is
        // physically owned by the ring. release/acquire pairs this with query
        // capture.
        tbuf->enqueued_change_id.store(assigned_change_id, std::memory_order_release);

        crossed_global_buffer = old_global < CHANGE_PROPAGATION_BUFFER_TRIGGER_BYTES &&
                                old_global + rec_sz >= CHANGE_PROPAGATION_BUFFER_TRIGGER_BYTES;
        enqueued = true;
      }
    }

    if (enqueued) {
      if (crossed_global_buffer) RequestAllBufferedTablesFlush();

      // Do not wake merely because this is the first record for a table. Normal
      // DML batches until the 200ms timer, the global 64MiB trigger, or a Rapid
      // query explicitly requests this table.
      return SHANNON_SUCCESS;
    }

    // Full is backpressure, not permission to drop the current committed
    // change.  try_put() has not moved from `item` on failure, so request a
    // flush and retry the SAME record after the consumer makes room.
    tbuf->pending_flush.store(true, std::memory_order_release);
    os_event_set(log_sys->rapid_events[0]);

    if (!warned_full) {
      if (current_thd) {
        push_warning_printf(current_thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE,
                            "Rapid ringbuffer full for table %llu; applying producer backpressure",
                            static_cast<unsigned long long>(table_key));
      } else {
        sql_print_warning("Rapid ringbuffer full for table %llu; applying producer backpressure",
                          static_cast<unsigned long long>(table_key));
      }
      warned_full = true;
    }

    if (!active_impl()) {
      MarkPropagationBufferBroken(tbuf);
      return SHANNON_SUCCESS;
    }

    // If unload removed/replaced this buffer, the table is no longer an active
    // replica target; let a future load rebuild from the primary source.
    {
      std::shared_lock<std::shared_mutex> slk(shard.mutex);
      auto it = shard.buffers.find(table_key);
      if (it == shard.buffers.end() || it->second.get() != tbuf.get()) return SHANNON_SUCCESS;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

int PopulatorImpl::load_indexes_caches_impl() {
  btr_pcur_t pcur;
  const rec_t *rec;
  mem_heap_t *heap;
  mtr_t mtr;
  MDL_ticket *mdl = nullptr;
  dict_table_t *dd_indexes;
  THD *thd = current_thd;
  const dict_index_t *index_rec{nullptr};

  DBUG_TRACE;

  heap = mem_heap_create(100, UT_LOCATION_HERE);
  dict_sys_mutex_enter();
  mtr_start(&mtr);

  /* Start scan the mysql.indexes */
  rec = dd_startscan_system(thd, &mdl, &pcur, &mtr, dd_indexes_name.c_str(), &dd_indexes);
  /* Process each record in the table */
  while (rec) {
    MDL_ticket *mdl_on_tab = nullptr;
    dict_table_t *parent = nullptr;
    MDL_ticket *mdl_on_parent = nullptr;

    /* Populate a dict_index_t structure with information from
    a INNODB_INDEXES row */
    auto ret = dd_process_dd_indexes_rec(heap, rec, &index_rec, &mdl_on_tab, &parent, &mdl_on_parent, dd_indexes, &mtr);

    /** we dont care about the dd or system objs. and attention to
    `RECOVERY_INDEX_TABLE_NAME` table. TRX_SYS_SPACE*/
    if (ret && ((index_rec->space_id() != SYSTEM_TABLE_SPACE) && !index_rec->table->is_system_schema() &&
                !index_rec->table->is_dd_table)) {
      std::shared_lock slock(shannon_indexes_cache_mutex);
      if (shannon_indexes_cache.find(index_rec->id) == shannon_indexes_cache.end()) {  // add new one.
        slock.unlock();
        std::unique_lock<std::shared_mutex> ex_lock(shannon_indexes_cache_mutex);
        if (shannon_indexes_cache.find(index_rec->id) == shannon_indexes_cache.end()) {  // double check
          shannon_indexes_cache[index_rec->id] = index_rec;
          std::string db_name, table_name;
          index_rec->table->get_table_name(db_name, table_name);
          shannon_indexes_name[index_rec->id] = std::make_pair(db_name, table_name);
        }
      }
    }

    dict_sys_mutex_exit();

    mem_heap_empty(heap);

    /* Get the next record */
    dict_sys_mutex_enter();

    if (index_rec != nullptr) {
      dd_table_close(index_rec->table, thd, &mdl_on_tab, true);

      /* Close parent table if it's a fts aux table. */
      if (index_rec->table->is_fts_aux() && parent) {
        dd_table_close(parent, thd, &mdl_on_parent, true);
      }
    }

    mtr_start(&mtr);
    rec = dd_getnext_system_rec(&pcur, &mtr);
  }  // while(rec)

  mtr_commit(&mtr);
  dd_table_close(dd_indexes, thd, &mdl, true);
  dict_sys_mutex_exit();
  mem_heap_free(heap);
  return ShannonBase::SHANNON_SUCCESS;
}

void PopulatorImpl::print_info_impl(FILE *file) { /* in: output stream */
  ShannonBase::RapidMonitor::print_rapid_monitor_info(file);
}

bool PopulatorImpl::is_loaded_table_impl(std::string sch_name, std::string table_name) {
  auto share = ShannonBase::shannon_loaded_tables->get(sch_name.c_str(), table_name.c_str());
  return (share) ? true : false;
}

PropagationBarrier PopulatorImpl::request_table_barrier_impl(const table_id_t &table_id) {
  PropagationBarrier barrier;
  auto &shard = get_pop_shard(table_id);
  std::shared_ptr<table_pop_buffer_t> tbuf;
  {
    std::shared_lock<std::shared_mutex> lk(shard.mutex);
    auto it = shard.buffers.find(table_id);
    if (it == shard.buffers.end() || !it->second) return barrier;
    tbuf = it->second;
  }

  if (tbuf->detached.load(std::memory_order_acquire)) return barrier;

  barrier.required_change_id = tbuf->enqueued_change_id.load(std::memory_order_acquire);
  barrier.applied_change_id = tbuf->applied_change_id.load(std::memory_order_acquire);

  if (tbuf->broken.load(std::memory_order_acquire)) {
    barrier.state = TablePropagationState::BROKEN;
    return barrier;
  }

  const bool pending = tbuf->publish_fence.load(std::memory_order_acquire) > 0 ||
                       barrier.applied_change_id < barrier.required_change_id ||
                       tbuf->data_size.load(std::memory_order_acquire) > 0 ||
                       tbuf->inflight_size.load(std::memory_order_acquire) > 0;
  if (!pending) return barrier;

  barrier.state = TablePropagationState::PENDING;

  // Query-demand trigger: do not wait for the normal 200ms batch timer. If the
  // target is already inflight, the worker itself will signal barrier_cv.
  if (tbuf->data_size.load(std::memory_order_acquire) > 0 && active_impl()) {
    tbuf->pending_flush.store(true, std::memory_order_release);
    os_event_set(log_sys->rapid_events[0]);
  }
  return barrier;
}

TablePropagationWaitResult PopulatorImpl::wait_table_applied_for_impl(const table_id_t &table_id,
                                                                      uint64_t required_change_id, uint64_t wait_ms) {
  auto &shard = get_pop_shard(table_id);
  std::shared_ptr<table_pop_buffer_t> tbuf;
  {
    std::shared_lock<std::shared_mutex> lk(shard.mutex);
    auto it = shard.buffers.find(table_id);
    if (it == shard.buffers.end() || !it->second)
      return required_change_id == 0 ? TablePropagationWaitResult::APPLIED : TablePropagationWaitResult::GONE;
    tbuf = it->second;
  }

  auto completed = [&]() {
    return tbuf->detached.load(std::memory_order_acquire) || tbuf->broken.load(std::memory_order_acquire) ||
           (tbuf->applied_change_id.load(std::memory_order_acquire) >= required_change_id &&
            tbuf->publish_fence.load(std::memory_order_acquire) == 0);
  };

  {
    std::unique_lock<std::mutex> lk(tbuf->barrier_mutex);
    if (!completed()) tbuf->barrier_cv.wait_for(lk, std::chrono::milliseconds(wait_ms), completed);
  }

  if (tbuf->detached.load(std::memory_order_acquire)) return TablePropagationWaitResult::GONE;
  if (tbuf->broken.load(std::memory_order_acquire)) return TablePropagationWaitResult::BROKEN;
  if (tbuf->applied_change_id.load(std::memory_order_acquire) >= required_change_id &&
      tbuf->publish_fence.load(std::memory_order_acquire) == 0)
    return TablePropagationWaitResult::APPLIED;
  return TablePropagationWaitResult::PENDING;
}

bool PopulatorImpl::mark_table_required_impl(const table_id_t &table_id) {
  return request_table_barrier_impl(table_id).state != TablePropagationState::READY;
}
}  // namespace Populate
}  // namespace ShannonBase
