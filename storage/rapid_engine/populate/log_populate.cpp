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
#include "storage/rapid_engine/recovery/recovery.h"
#include "storage/rapid_engine/utils/try_operation.h"

#ifdef SHANNON_POSIX_PLATFORM
#include <pthread.h>  // For pthread_setname_np
#else
#include <Windows.h>  // For SetThreadDescription
#endif
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>

#include "current_thd.h"
#include "include/os0event.h"
#include "sql/sql_class.h"

#include "storage/innobase/handler/ha_innodb.h"  // thd_to_trx
#include "storage/innobase/include/data0type.h"
#include "storage/innobase/include/os0thread-create.h"

#include "storage/rapid_engine/autopilot/loader.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/monitor/rapid_monitor.h"
#include "storage/rapid_engine/populate/log_dml_notification.h"
#include "storage/rapid_engine/populate/log_redolog.h"
#include "storage/rapid_engine/recovery/table_persistence.h"
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

/**
 * Wake everything parked on this buffer's query barrier.
 *
 * wait_table_applied_for_impl() evaluates its predicate over atomics while
 * holding barrier_mutex. Signalling without ever taking that mutex loses the
 * wakeup whenever the waiter has evaluated the predicate but not yet parked:
 * the notify lands on an empty wait set and the query then sleeps out the full
 * QUERY_PROPAGATION_WAIT_SLICE_MS. Taking the mutex here orders the state
 * change the caller just made against the waiter's evaluation of it.
 *
 * The lock is dropped before notifying so a woken waiter does not immediately
 * block on a mutex the notifier still holds.
 */
static inline void SignalPropagationBarrier(const std::shared_ptr<table_pop_buffer_t> &tbuf) {
  if (!tbuf) return;
  { std::lock_guard<std::mutex> lk(tbuf->barrier_mutex); }
  tbuf->barrier_cv.notify_all();
}

static inline void MarkPropagationBufferBroken(const std::shared_ptr<table_pop_buffer_t> &tbuf) {
  if (!tbuf) return;
  tbuf->broken.store(true, std::memory_order_release);
  SignalPropagationBarrier(tbuf);
}

static inline void DetachPropagationBuffer(const std::shared_ptr<table_pop_buffer_t> &tbuf) {
  if (!tbuf) return;
  tbuf->detached.store(true, std::memory_order_release);

  const size_t queued = tbuf->data_size.exchange(0, std::memory_order_acq_rel);
  if (queued > 0) shannon_pop_data_sz.fetch_sub(queued, std::memory_order_relaxed);

  SignalPropagationBarrier(tbuf);
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
  bool pooled{false}, scheduled{false}, running{false};  // table_workers_mutex
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
   * Hand @a batch to the worker for @a table_key, starting one if none is
   * running. */
  static void dispatch_to_table_worker(const table_id_t &table_key, const std::shared_ptr<table_pop_buffer_t> &buffer,
                                       std::vector<change_candidate_t> &&batch, size_t bytes);
};

static std::shared_mutex table_workers_mutex;
static std::unordered_map<table_id_t, std::shared_ptr<table_worker_context>> table_workers;
// Logical table queues survive independently of a bounded physical pool.
static std::deque<std::shared_ptr<table_worker_context>> apply_ready;
static std::vector<IB_thread> apply_threads;
static std::condition_variable_any apply_cv;
static bool apply_stopping{false};  // table_workers_mutex
static std::atomic<size_t> apply_thread_count{0};
static void shared_apply_worker();
static void abandon_table_work(table_worker_context *ctx) {
  std::vector<change_candidate_t> abandoned;
  {
    std::lock_guard lock(ctx->mtx);
    abandoned.swap(ctx->pending_change_candidates);
    ctx->pending_size.store(0, std::memory_order_release);
  }
  size_t bytes = 0;
  for (const auto &candidate : abandoned) {
    bytes += candidate.record.m_size;
    TransactionManager::instance().on_change_applied(static_cast<Transaction::ID>(candidate.record.m_source_trx_id),
                                                     candidate.record.m_table_id);
  }
  if (bytes && ctx->buffer) ctx->buffer->inflight_size.fetch_sub(bytes, std::memory_order_acq_rel);
}

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
    if (old == 1) SignalPropagationBarrier(tbuf);
  }
}

void QuarantinePropagationTable(table_id_t table_id) noexcept {
  auto table = Imcs::Imcs::instance()->get_rpd_table_shared(table_id);
  // Refuse reads before attempting persistence or touching any growing container.
  if (table) table->quarantine_propagation();
  RpdMirror::Registry::mark_stale(static_cast<uint>(table_id), stale_reason_t::RELOAD_REQUIRED);
  auto *manager = table ? table->recovery_manager() : nullptr;
  if (manager) {
    manager->require_recovery();
    if (!manager->revoke_fast_recovery()) {
      sql_print_error("Rapid could not durably revoke fast recovery for table %llu; restart requires primary reload",
                      static_cast<unsigned long long>(table_id));
    }
  }
  auto &shard = get_pop_shard(table_id);
  std::shared_ptr<table_pop_buffer_t> tbuf;
  {
    std::shared_lock<std::shared_mutex> lk(shard.mutex);
    auto it = shard.buffers.find(table_id);
    if (it != shard.buffers.end()) tbuf = it->second;
  }
  if (tbuf) MarkPropagationBufferBroken(tbuf);
}

void QuarantinePropagationTables(const std::vector<table_id_t> &table_ids) {
  for (table_id_t table_id : table_ids) QuarantinePropagationTable(table_id);
}

bool IsPropagationBroken(table_id_t table_id) noexcept {
  auto table = Imcs::Imcs::instance()->get_rpd_table_shared(table_id);
  if (table && table->propagation_broken()) return true;
  auto &shard = get_pop_shard(table_id);
  std::shared_lock<std::shared_mutex> lk(shard.mutex);
  auto it = shard.buffers.find(table_id);
  return it != shard.buffers.end() && it->second && it->second->broken.load(std::memory_order_acquire);
}

size_t get_populator_worker_thread_count() noexcept {
  std::shared_lock<std::shared_mutex> lk(table_workers_mutex);
  size_t legacy = 0;
  for (const auto &[key, ctx] : table_workers) legacy += !ctx->pooled;
  return legacy + apply_thread_count.load(std::memory_order_acquire);
}

ResourceSnapshot resource_snapshot() {
  ResourceSnapshot out;
  for (auto &shard : shannon_pop_shards) {
    std::shared_lock lock(shard.mutex);
    for (const auto &[id, buffer] : shard.buffers) {
      if (!buffer || buffer->detached.load()) continue;
      ++out.buffers;
      out.ring_storage_bytes += sizeof(buffer->change_candiates);
      out.queued_bytes += buffer->data_size.load();
      out.inflight_bytes += buffer->inflight_size.load();
    }
  }
  {
    std::shared_lock lock(table_workers_mutex);
    out.logical_workers = table_workers.size();
    out.physical_workers = apply_thread_count.load();
    out.ready_tables = apply_ready.size();
    for (const auto &[id, ctx] : table_workers) {
      if (ctx->pooled)
        out.running_tables += ctx->running;
      else
        ++out.physical_workers;
    }
  }
  return out;
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
  ResMgmt::WorkerRegistration resource_registration;
  // Stay registered until the THD guard has released all handlerton data.
  // Shutdown joins registered workers before the plugin is destroyed.
  struct RegistrationGuard {
    table_worker_context *ctx;
    std::unique_lock<std::shared_mutex> exit_lock{table_workers_mutex, std::defer_lock};
    ~RegistrationGuard() {
      if (ctx->pooled) return;
      if (!exit_lock.owns_lock()) exit_lock.lock();
      auto it = table_workers.find(ctx->table_key);
      if (it != table_workers.end() && it->second.get() == ctx) table_workers.erase(it);
    }
  } registration_guard{ctx};
  const bool worker_completed = Utils::try_operation([&] {
#ifdef SHANNON_POSIX_PLATFORM
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
      if (ctx->pooled) {
        ctx->should_stop.store(true);
        return;
      }
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
      bool m_end_thread;
      explicit ThdGuard(THD *thd, bool end_thread) : m_thd(thd), m_end_thread(end_thread) {}
      ~ThdGuard() {
        if (!m_thd) return;
        Transaction::free_trx_from_thd(m_thd);
        close_thread_tables(m_thd);
        m_thd->mdl_context.release_transactional_locks();
        destroy_internal_thd(m_thd);
        m_thd = nullptr;
        if (m_end_thread) my_thread_end();
      }
    } thd_guard(thd, !ctx->pooled);

    SHANNON_THREAD_LOCAL RedoLog::LogParser redo_log;
    SHANNON_THREAD_LOCAL DML::CopyInfoParser copy_info_log;
    SHANNON_THREAD_LOCAL Rapid_load_context context;
    context.m_thd = thd;

    while (!ctx->should_stop.load(std::memory_order_acquire)) {
      bool should_exit = false;
      if (!ctx->pooled) {
        std::unique_lock<std::mutex> lk(ctx->mtx);
        ctx->cv.wait_for(lk, std::chrono::milliseconds(TABLE_WORKER_IDLE_TIMEOUT), [ctx] {
          return ctx->should_stop.load(std::memory_order_acquire) ||
                 ctx->pending_size.load(std::memory_order_acquire) > 0;
        });

        if (ctx->should_stop.load(std::memory_order_acquire)) {
          should_exit = true;
        } else {
          const auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                                  ctx->last_activity)
                                .count();
          if (static_cast<uint64_t>(idle) >= TABLE_WORKER_IDLE_TIMEOUT &&
              ctx->pending_size.load(std::memory_order_acquire) == 0) {
            should_exit = true;
          }
        }
      }

      if (should_exit) {
        // Widens the window this branch closes so a test can land a dispatch
        // inside it instead of racing the 5s idle timeout.
        DBUG_EXECUTE_IF("secondary_engine_rapid_worker_idle_exit_stall",
                        { std::this_thread::sleep_for(std::chrono::milliseconds(3000)); });

        std::unique_lock<std::shared_mutex> reg_lk(table_workers_mutex);
        if (!ctx->should_stop.load(std::memory_order_acquire) &&
            ctx->pending_size.load(std::memory_order_acquire) > 0) {
          continue;
        }
        // Serialize idle retirement with dispatch until THD cleanup finishes.
        // Removing the registry entry here lets shutdown destroy the handlerton
        // while this worker still owns engine data. Merely delaying removal
        // would instead let dispatch enqueue work into an exiting worker.
        registration_guard.exit_lock = std::move(reg_lk);
        break;
      }

      if (ctx->pending_size.load(std::memory_order_acquire) > 0) {
        DBUG_EXECUTE_IF("secondary_engine_rapid_propagation_stall",
                        { std::this_thread::sleep_for(std::chrono::seconds(6)); });
      }

      // Budget waits never hold the table queue/registry lock. One grant owns
      // one table-local batch; releasing it lets other tables advance fairly.
      ShannonBase::ResMgmt::WorkScope resource_scope;
      if (const char *error = resource_scope.enter(thd, ShannonBase::ResMgmt::WorkClass::SYNC, true, [ctx] {
            return ctx->should_stop.load(std::memory_order_acquire) || (ctx->buffer && ctx->buffer->broken.load());
          })) {
        const bool cancelled = ctx->should_stop.exchange(true);
        if (!cancelled && !ResMgmt::work_governor().stopping()) {
          MarkPropagationBufferBroken(ctx->buffer);
          sql_print_warning("Rapid apply resource admission failed: %s", error);
        }
        break;
      }
      std::vector<change_candidate_t> applying;
      {
        std::lock_guard<std::mutex> lk(ctx->mtx);
        if (ctx->pending_size.load(std::memory_order_acquire) > 0) {
          auto &pending = ctx->pending_change_candidates;
          size_t count = 0, bytes = 0;
          do {
            bytes += pending[count++].record.m_size;
          } while (count < pending.size() && count < 256 && bytes < 1024 * 1024);
          applying.insert(applying.end(), std::make_move_iterator(pending.begin()),
                          std::make_move_iterator(pending.begin() + count));
          pending.erase(pending.begin(), pending.begin() + count);
          ctx->pending_size.fetch_sub(bytes, std::memory_order_release);
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

        // A buffered change is applied by the module that owns its record format.
        // That module prepares Rapid_load_context, parses/applies the record and
        // classifies the outcome; this loop owns only the queue, the retry policy
        // and the quarantine decision.
        ChangeApplyResult result;
        switch (rec.m_source) {
          case Source::REDO_LOG:
            result = redo_log.apply_change(context, rec);
            break;
          case Source::COPY_INFO:
            result = copy_info_log.apply_change(context, rec, candidate.change_id);
            break;
          case Source::UN_KNOWN:
          default:
            // Never guess a record format. An unknown source quarantines this
            // table rather than risking corruption by invoking the wrong parser.
            result.status = ChangeApplyResult::Status::PERMANENT;
            result.stale_reason = stale_reason_t::UNIDENTIFIED_ERROR;
            break;
        }

        if (result.status == ChangeApplyResult::Status::APPLIED) {
          ctx->retry_counts.erase(candidate.change_id);

          if (ctx->buffer) {
            // Table-local enqueue is serialized, and the worker never lets a
            // later candidate overtake a failed predecessor. Therefore this is a
            // contiguous physical-apply watermark for this table even though
            // global change ids may contain gaps from other tables.
            ctx->buffer->inflight_size.fetch_sub(rec.m_size, std::memory_order_acq_rel);
            ctx->buffer->applied_change_id.store(candidate.change_id, std::memory_order_release);
            SignalPropagationBarrier(ctx->buffer);
          }
          continue;
        }

        failed_at = i;
        if (result.status == ChangeApplyResult::Status::RETRYABLE) {
          failed_retry = ++ctx->retry_counts[candidate.change_id];
          permanent_failure = failed_retry > MAX_RETRY_COUNT;
        } else {
          permanent_failure = true;
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
          // Give the quarantine a visible terminal state.  Without this the table keeps reading as AVAIL_RPDGSTABSTATE
          // in performance_schema.rpd_tables while its changes are no longer being applied.
          RpdMirror::Registry::mark_stale(static_cast<uint>(ctx->table_key), result.stale_reason);
          if (result.stale_reason == stale_reason_t::ERROR_CLUSTER_OOM)
            Autopilot::SelfLoadManager::notify_propagation_failure();
          push_warning_printf(thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE,
                              "Rapid propagation quarantined table %llu at change_id=%llu LSN=%llu; "
                              "the table is stale and must be reloaded before secondary-engine offload",
                              static_cast<unsigned long long>(ctx->table_key),
                              static_cast<unsigned long long>(candidate.change_id),
                              static_cast<unsigned long long>(lsn));
          sql_print_warning(
              "Rapid propagation quarantined table %llu at change_id=%llu LSN=%llu, source=%d, oper=%d, "
              "record_size=%zu, parsed_bytes=%zu: %s",
              static_cast<unsigned long long>(ctx->table_key), static_cast<unsigned long long>(candidate.change_id),
              static_cast<unsigned long long>(lsn), static_cast<int>(rec.m_source), static_cast<int>(rec.m_oper),
              rec.m_size, result.parsed_bytes, da_msg);
        } else {
          push_warning_printf(thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE,
                              "Propagation failed for table %llu at change_id=%llu LSN=%llu "
                              "(retry %u/%u); blocking later changes until this record succeeds",
                              static_cast<unsigned long long>(ctx->table_key),
                              static_cast<unsigned long long>(candidate.change_id),
                              static_cast<unsigned long long>(lsn), failed_retry, MAX_RETRY_COUNT);
          sql_print_warning(
              "Rapid propagation retry %u/%u for table %llu at change_id=%llu LSN=%llu, source=%d, oper=%d, "
              "record_size=%zu, parsed_bytes=%zu: %s",
              failed_retry, MAX_RETRY_COUNT, static_cast<unsigned long long>(ctx->table_key),
              static_cast<unsigned long long>(candidate.change_id), static_cast<unsigned long long>(lsn),
              static_cast<int>(rec.m_source), static_cast<int>(rec.m_oper), rec.m_size, result.parsed_bytes, da_msg);
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
      // One bounded batch per turn; a hot table goes to the ready queue tail.
      if (ctx->pooled) break;
    }
    if (ctx->pooled && !ctx->should_stop.load() && !ctx->buffer->broken.load() && !ctx->buffer->detached.load()) return;

    std::vector<change_candidate_t> abandoned;
    {
      std::lock_guard<std::mutex> lk(ctx->mtx);
      abandoned.swap(ctx->pending_change_candidates);
      ctx->pending_size.store(0, std::memory_order_release);
    }
    if (!abandoned.empty()) {
      size_t abandoned_bytes = 0;
      for (const auto &candidate : abandoned) {
        abandoned_bytes += candidate.record.m_size;
        TransactionManager::instance().on_change_applied(static_cast<Transaction::ID>(candidate.record.m_source_trx_id),
                                                         candidate.record.m_table_id);
      }
      // The coordinator moved these bytes into inflight_size when it handed them
      // over; nothing else takes them back out.
      if (ctx->buffer) ctx->buffer->inflight_size.fetch_sub(abandoned_bytes, std::memory_order_acq_rel);
    }
  });
  if (!worker_completed) {
    // A partially applied change must never be retried as though no mutation
    // happened. Preserve the broken barrier until an explicit reload/recovery.
    ctx->should_stop.store(true, std::memory_order_release);
    MarkPropagationBufferBroken(ctx->buffer);
    abandon_table_work(ctx);
    sql_print_error("Rapid propagation worker for table %llu stopped after an operation failed; reload required",
                    static_cast<unsigned long long>(ctx->table_key));
  }
}

static void shared_apply_worker() {
  ResMgmt::WorkerRegistration registration;
  for (;;) {
    std::shared_ptr<table_worker_context> ctx;
    {
      std::unique_lock lock(table_workers_mutex);
      apply_cv.wait(lock, [] { return apply_stopping || !apply_ready.empty(); });
      if (apply_stopping) break;
      ctx = std::move(apply_ready.front());
      apply_ready.pop_front();
      ctx->scheduled = false;
      auto found = table_workers.find(ctx->table_key);
      if (found == table_workers.end() || found->second != ctx || ctx->should_stop.load()) continue;
      ctx->running = true;
    }
    // All THD/TABLE cleanup completes before making the table eligible again.
    // A batch-local THD prevents table state from being transferred across workers.
    table_worker_func(ctx.get());
    bool scheduling_failed = false;
    {
      std::unique_lock lock(table_workers_mutex);
      ctx->running = false;
      if (!apply_stopping && !ctx->should_stop.load() && !ctx->buffer->broken.load() && !ctx->buffer->detached.load() &&
          ctx->pending_size.load() && !ctx->scheduled) {
        bool force_failure = false;
        DBUG_EXECUTE_IF("rapid_apply_pool_schedule_oom", { force_failure = true; });
        const bool queued = !force_failure && Utils::try_operation([&] { apply_ready.push_back(ctx); });
        if (queued) {
          ctx->scheduled = true;
        } else {
          ctx->should_stop.store(true);
          MarkPropagationBufferBroken(ctx->buffer);
          scheduling_failed = true;
        }
      }
      if (ctx->should_stop.load() || ctx->buffer->broken.load() || ctx->buffer->detached.load()) {
        abandon_table_work(ctx.get());
      }
    }
    if (scheduling_failed) {
      QuarantinePropagationTable(ctx->table_key);
      sql_print_error("Rapid could not reschedule table %llu; reload required",
                      static_cast<unsigned long long>(ctx->table_key));
    }
    apply_cv.notify_all();
  }
}

void table_worker_context::dispatch_to_table_worker(const table_id_t &table_key,
                                                    const std::shared_ptr<table_pop_buffer_t> &buffer,
                                                    std::vector<change_candidate_t> &&batch, size_t bytes) {
  if (batch.empty()) return;
  bool appended = false;
  const bool dispatched = Utils::try_operation([&] {
    std::unique_lock lock(table_workers_mutex);
    if (buffer->detached.load() || buffer->broken.load()) {
      buffer->inflight_size.fetch_sub(bytes);
      for (const auto &candidate : batch)
        TransactionManager::instance().on_change_applied(static_cast<Transaction::ID>(candidate.record.m_source_trx_id),
                                                         candidate.record.m_table_id);
      return;
    }
    const bool pooled = ResMgmt::limits_execution(ResMgmt::work_governor().mode());
    std::shared_ptr<table_worker_context> ctx;
    auto it = table_workers.find(table_key);
    if (it != table_workers.end() && (it->second->pooled || thread_is_active(it->second->thread_handle))) {
      ctx = it->second;
      if (ctx->buffer != buffer || ctx->should_stop.load()) {
        MarkPropagationBufferBroken(buffer);
        buffer->inflight_size.fetch_sub(bytes);
        for (const auto &candidate : batch)
          TransactionManager::instance().on_change_applied(
              static_cast<Transaction::ID>(candidate.record.m_source_trx_id), candidate.record.m_table_id);
        return;
      }
    } else {
      ctx = std::make_shared<table_worker_context>(table_key, buffer);
      ctx->pooled = pooled;
      table_workers[table_key] = ctx;
      if (!pooled) {
        auto *tramp = new table_worker_trampoline{ctx};
        ctx->thread_handle = os_thread_create(rapid_populate_thread_key, 0, table_worker_trampoline::launch, tramp);
        ctx->thread_handle.start();
      }
    }
    {
      std::lock_guard queue_lock(ctx->mtx);
      ctx->pending_change_candidates.insert(ctx->pending_change_candidates.end(),
                                            std::make_move_iterator(batch.begin()),
                                            std::make_move_iterator(batch.end()));
      ctx->pending_size.fetch_add(bytes, std::memory_order_release);
      appended = true;
    }
    if (pooled) {
      if (apply_threads.empty()) {
        apply_stopping = false;
        // Effective SYNC admission already includes AUTO's CPU planning and
        // expert upper bounds. Physical workers never scale with loaded tables.
        const size_t workers = ResMgmt::make_apply_pool_size(ResMgmt::work_governor().limit(ResMgmt::WorkClass::SYNC),
                                                             shannon_rpd_engine_cfg.memory_pool_size_bytes);
        apply_threads.reserve(workers);
        for (size_t i = 0; i < workers; ++i) {
          apply_threads.push_back(os_thread_create(rapid_populate_thread_key, 0, shared_apply_worker));
          apply_threads.back().start();
          apply_thread_count.fetch_add(1);
        }
      }
      if (!ctx->running && !ctx->scheduled) {
        apply_ready.push_back(ctx);
        ctx->scheduled = true;
      }
      apply_cv.notify_one();
    } else {
      ctx->cv.notify_one();
    }
  });
  if (!dispatched) {
    // Thread/queue allocation failures belong to Rapid, never the source DML
    // or a process-wide unhandled exception. Revoke reads/recovery first.
    MarkPropagationBufferBroken(buffer);
    QuarantinePropagationTable(table_key);
    {
      std::unique_lock lock(table_workers_mutex);
      auto found = table_workers.find(table_key);
      if (found != table_workers.end() && found->second->buffer == buffer) {
        auto ctx = found->second;
        ctx->should_stop.store(true);
        ctx->cv.notify_all();
        if (!ctx->running) abandon_table_work(ctx.get());
      }
    }
    if (!appended) {
      buffer->inflight_size.fetch_sub(bytes);
      for (const auto &candidate : batch)
        TransactionManager::instance().on_change_applied(static_cast<Transaction::ID>(candidate.record.m_source_trx_id),
                                                         candidate.record.m_table_id);
    }
    apply_cv.notify_all();
    sql_print_error("Rapid could not dispatch table %llu; reload required", static_cast<unsigned long long>(table_key));
  }
}

/**
 * main entry of pop thread. it monitors shannon_pop_buff, a new mtr_log_rect_t
 * is coming, then it starts a new worker to dealing with this mtr_log_rec_t.
 */
static void parse_log_func_main(log_t *log_ptr) {
  ResMgmt::WorkerRegistration resource_registration;
#ifdef SHANNON_POSIX_PLATFORM
  pthread_setname_np(pthread_self(), "rapid_log_coordinator");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_log_coordinator");
#endif

  Recovery::RecoveryAdminSession coordinator_session;

  // ref: https://dev.mysql.com/doc/heatwave/en/mys-hw-change-propagation.html
  uint64_t health_tick = 0;
  while (srv_shutdown_state.load(std::memory_order_acquire) == SRV_SHUTDOWN_NONE &&
         shannon_propagation_thread_started.load(std::memory_order_acquire)) {
    if (auto *mgr = ShannonBase::shannon_self_load_mgr_inst; mgr) mgr->dispatch_propagation_reload();
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

    ResMgmt::WorkScope resource_scope;
    if (const char *error = resource_scope.enter(coordinator_session.thd(), ResMgmt::WorkClass::SYNC, true, [] {
          return !shannon_propagation_thread_started.load() || srv_shutdown_state.load() != SRV_SHUTDOWN_NONE;
        })) {
      if (!ResMgmt::work_governor().stopping() && shannon_propagation_thread_started.load() &&
          srv_shutdown_state.load() == SRV_SHUTDOWN_NONE)
        sql_print_warning("Rapid coordinator resource admission failed: %s", error);
      break;
    }
    TransactionManager::instance().finalize_committed();

    if (++health_tick >= PROPAGATION_HEALTH_REFRESH_TICKS) {
      health_tick = 0;
      RpdMirror::refresh_propagation_health(nullptr);
    }

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

      table_worker_context::dispatch_to_table_worker(table_key, tbuf, std::move(batch_vec), moved_bytes);
    }

    shannon_rpd_loop_counter++;
  }

  shannon_propagation_thread_started.store(false, std::memory_order_seq_cst);
}

std::unique_ptr<Populator::Impl> Populator::m_impl = nullptr;

std::unique_ptr<Populator::Impl> &Populator::get_impl() {
  static std::once_flag init_flag;
  std::call_once(init_flag, [] { m_impl = std::make_unique<PopulatorImpl>(); });
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
int Populator::write(FILE *file, uint64_t start_lsn, change_record_buff *changed_rec) {
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

bool PopulatorImpl::active_impl() { return thread_is_active(srv_threads.m_change_pop_cordinator); }

void PopulatorImpl::send_notify_impl() { os_event_set(log_sys->rapid_events[0]); }

void PopulatorImpl::start_impl() {
  // Recovery dispatches one thread per table and each calls here once its
  // table is restored, so this runs concurrently. Unlocked, the test and the
  // create below interleave: both threads see an inactive coordinator, both
  // assign srv_threads.m_change_pop_cordinator, and the second overwrites the
  // IB_thread the first is about to start -- which trips
  // ut_a(state_after_start == STARTED || STOPPED) in IB_thread::start().
  // Re-check under the lock so the later caller still no-ops.
  static std::mutex start_mutex;
  std::lock_guard<std::mutex> guard(start_mutex);
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

  {
    std::unique_lock lock(table_workers_mutex);
    auto it = table_workers.find(table_id);
    if (it != table_workers.end() && it->second->pooled) {
      auto ctx = it->second;
      ctx->should_stop.store(true, std::memory_order_release);
      apply_ready.erase(std::remove(apply_ready.begin(), apply_ready.end(), ctx), apply_ready.end());
      ctx->scheduled = false;
      apply_cv.notify_all();
      apply_cv.wait(lock, [&] { return !ctx->running; });
      abandon_table_work(ctx.get());
      auto current = table_workers.find(table_id);
      if (current != table_workers.end() && current->second == ctx) table_workers.erase(current);
      return;
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
  // An exited coordinator does not imply that its table workers have exited.
  // Always drain the worker registry; touch log_sys only for a live coordinator.
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

  std::vector<IB_thread> pool_to_join;
  {
    std::unique_lock lock(table_workers_mutex);
    apply_stopping = true;
    apply_ready.clear();
    pool_to_join.swap(apply_threads);
  }
  apply_cv.notify_all();
  for (auto &thread : pool_to_join) thread.join();
  apply_thread_count.store(0);

  // Step 3: move out all ctx
  decltype(table_workers) workers_to_join;
  {
    std::unique_lock<std::shared_mutex> write_lock(table_workers_mutex);
    workers_to_join = std::move(table_workers);
    table_workers.clear();
  }

  // Step 4: waiting for all table workers to finish.
  for (auto &[tid, ctx] : workers_to_join) {
    if (ctx->pooled)
      abandon_table_work(ctx.get());
    else if (thread_is_active(ctx->thread_handle))
      ctx->thread_handle.join();
  }

  // Step 5: clear all status
  shannon_rpd_loop_counter = 0;
  RedoLog::LogParser::clear_index_cache();
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

int PopulatorImpl::write_impl(FILE *file [[maybe_unused]], uint64_t start_lsn, change_record_buff *changed_rec) {
  if (!changed_rec || !shannon_loaded_tables->size()) return SHANNON_SUCCESS;

  const table_id_t table_key = changed_rec->m_table_id;
  const size_t rec_sz = changed_rec->m_size;
  const Source record_source = changed_rec->m_source;  // changed_rec is moved from below
  const bool committed_binlog = changed_rec->is_committed_binlog();
  auto &shard = get_pop_shard(table_key);

  std::shared_ptr<table_pop_buffer_t> tbuf;
  {
    std::shared_lock<std::shared_mutex> slk(shard.mutex);
    auto it = shard.buffers.find(table_key);
    if (it != shard.buffers.end()) tbuf = it->second;
  }

  if (!tbuf) {
    const bool created = Utils::try_operation([&] {
      std::unique_lock<std::shared_mutex> ulk(shard.mutex);
      auto [it, inserted] = shard.buffers.emplace(table_key, std::shared_ptr<table_pop_buffer_t>{});
      if (inserted || !it->second) it->second = std::make_shared<table_pop_buffer_t>();
      tbuf = it->second;
    });
    if (!created) {
      // Buffer creation owns these allocations; never throw into primary DML.
      // Redo producers hold log latches, so keep their failure path free of I/O.
      if (record_source == Source::COPY_INFO) {
        QuarantinePropagationTable(table_key);
      } else {
        auto table = Imcs::Imcs::instance()->get_rpd_table_shared(table_key);
        if (table) table->quarantine_propagation();
      }
      RpdMirror::Registry::mark_stale(static_cast<uint>(table_key), stale_reason_t::ERROR_CLUSTER_OOM);
      Autopilot::SelfLoadManager::notify_propagation_failure();
      return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);
    }
  }

  // The record source is the only propagation-format discriminator. Validate
  // only source-specific invariants here; do not maintain a second global mode
  // that can disagree with the record already stored in the table buffer. Each
  // source owns its own invariant check so this producer stays format-agnostic.
  // Dropping a COPY_INFO change leaves the live image behind the source,
  // so revoke fast recovery as well
  // as marking the buffer broken. (REDO_LOG records are produced under log latches
  // where durable file I/O must not run, so they only mark the buffer.)
  const auto quarantine_dropped = [&]() {
    if (record_source == Source::COPY_INFO)
      QuarantinePropagationTable(table_key);
    else
      MarkPropagationBufferBroken(tbuf);
    RpdMirror::Registry::mark_stale(static_cast<uint>(table_key), stale_reason_t::RELOAD_REQUIRED);
  };

  bool invalid_record = changed_rec->m_size == 0 || changed_rec->m_buff0 == nullptr;
  switch (changed_rec->m_source) {
    case Source::COPY_INFO:
      // Row-image propagation must keep the real primary creator transaction.
      // commit_scn==0 is valid: it represents an ACTIVE Rapid MVCC version.
      invalid_record = invalid_record || !DML::CopyInfoParser::validate_record(*changed_rec);
      break;
    case Source::REDO_LOG:
      // REDO records need an ordering position and serialized redo bytes.
      invalid_record = invalid_record || !RedoLog::LogParser::validate_record(*changed_rec, start_lsn);
      break;
    case Source::UN_KNOWN:
    default:
      invalid_record = true;
      break;
  }

  if (invalid_record) {
    quarantine_dropped();
    sql_print_warning("Rapid rejected invalid propagation source %u for table %llu",
                      static_cast<unsigned>(changed_rec->m_source), static_cast<unsigned long long>(table_key));
    return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);
  }

  // A captured source change cannot simply disappear because propagation was
  // stopped after the primary DML succeeded. Keep the table quarantined until
  // a reload reconstructs it from the primary source.
  if (!active_impl()) {
    quarantine_dropped();
    return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);
  }

  // Once a table is quarantined we deliberately stop accumulating an unbounded
  // backlog.  The table remains permanently non-offloadable until unload/reload.
  if (tbuf->broken.load(std::memory_order_acquire) || tbuf->detached.load(std::memory_order_acquire))
    return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);

  // Serialize only each actual ring insertion attempt for this table.  The
  // lock is never held while waiting for a full ring to drain.  A change id is
  // assigned inside this critical section, so successful ring order and
  // table-local change-id order are identical and applied_change_id is a sound
  // watermark. Normal DML never waits for Rapid apply.
  change_candidate_t item(0, start_lsn, std::move(*changed_rec));

  if (tbuf->broken.load(std::memory_order_acquire) || tbuf->detached.load(std::memory_order_acquire))
    return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);

  bool enqueued = false;
  bool crossed_global_buffer = false;
  {
    std::unique_lock<std::mutex> enqueue_guard(tbuf->enqueue_mutex);
    if (tbuf->broken.load(std::memory_order_acquire) || tbuf->detached.load(std::memory_order_acquire))
      return static_cast<int>(PROPAGATION_FAILED::WRITE_REJECTED);

    // IDs consumed by a failed full-ring attempt are harmless gaps. What matters is that every successful insertion
    // gets its id while holding the same table-local enqueue lock.
    const uint64_t assigned_change_id = shannon_change_id.fetch_add(1, std::memory_order_relaxed);
    item.change_id = assigned_change_id;
    bool force_full = false;
    DBUG_EXECUTE_IF("rapid_propagation_buffer_full", { force_full = true; });
    if (!force_full && tbuf->change_candiates.try_put(std::move(item))) {
      tbuf->data_size.fetch_add(rec_sz, std::memory_order_relaxed);
      const uint64_t old_global = shannon_pop_data_sz.fetch_add(rec_sz, std::memory_order_acq_rel);

      // Publish the query-visible watermark only after the record is physically owned by the ring. release/acquire
      // pairs this with query capture.
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

  // The ring is full and the commit path is not allowed to wait for it.  DML on
  // the primary must never be delayed by change propagation, so the invariant is
  // held by degrading the replica rather than by pushing back on the writer:
  // ask for a flush, quarantine the table, and let the session commit.  The
  // table goes stale, queries stop being offloaded and run on InnoDB instead,
  // and a reload restores propagation.  Anything that waits here -- even
  // briefly -- puts backpressure back on the commit path, where it only shows
  // up under load.
  tbuf->pending_flush.store(true, std::memory_order_release);
  os_event_set(log_sys->rapid_events[0]);

  if (committed_binlog) {
    // The producer is the binlog reader, never the primary writer. Retain the
    // row and cursor on temporary saturation; backpressure belongs here.
    *changed_rec = std::move(item.record);
    return static_cast<int>(PROPAGATION_FAILED::BUFFER_FULL);
  }

  quarantine_dropped();
  RpdMirror::Registry::mark_stale(static_cast<uint>(table_key), stale_reason_t::ERROR_CLUSTER_OOM);
  Autopilot::SelfLoadManager::notify_propagation_failure();

  sql_print_warning(
      "Rapid propagation buffer full for table %llu; quarantining the table rather than delaying DML. "
      "Queries fall back to InnoDB; reload the table to resume change propagation.",
      static_cast<unsigned long long>(table_key));
  return static_cast<int>(PROPAGATION_FAILED::BUFFER_FULL);
}

void PopulatorImpl::print_info_impl(FILE *file) { /* in: output stream */
}

bool PopulatorImpl::is_loaded_table_impl(std::string sch_name, std::string table_name) {
  auto share = ShannonBase::shannon_loaded_tables->get(sch_name.c_str(), table_name.c_str());
  return (share) ? true : false;
}

PropagationBarrier PopulatorImpl::request_table_barrier_impl(const table_id_t &table_id) {
  PropagationBarrier barrier;

  DBUG_EXECUTE_IF("secondary_engine_rapid_barrier_force_pending", {
    barrier.state = TablePropagationState::PENDING;
    barrier.required_change_id = 0;
    return barrier;
  });

  // Report READY no matter what is still queued, so a scan starts while a
  // change is mid-apply. That is not an artificial state: the barrier only
  // covers what was enqueued when the scan began, and a scan already walking
  // its IMCUs re-reads each one's row count as it goes, so DML that lands
  // afterwards is applied underneath it. Forcing READY is simply how a test
  // reaches that window deterministically instead of by timing.
  DBUG_EXECUTE_IF("secondary_engine_rapid_barrier_force_ready", {
    barrier.state = TablePropagationState::READY;
    return barrier;
  });

  DBUG_EXECUTE_IF("secondary_engine_rapid_barrier_stale_generation_once", {
    // Self-disable rather than latch a process-wide static: a static fires only
    // on the first call in the server's lifetime, so the case silently stops
    // being exercised on any re-run or second use within one test while still
    // passing.
    DBUG_SET("-d,secondary_engine_rapid_barrier_stale_generation_once");
    barrier.state = TablePropagationState::PENDING;
    // Unreachable on purpose. A small id is already applied, so without the
    // generation check the wait would simply report APPLIED and the test would
    // pass either way -- it has to be a watermark that only the generation
    // mismatch can get the caller out of.
    barrier.required_change_id = std::numeric_limits<uint64_t>::max();
    barrier.buffer_generation = std::numeric_limits<uint64_t>::max();
    return barrier;
  });

  if (IsPropagationBroken(table_id)) {
    barrier.state = TablePropagationState::BROKEN;
    return barrier;
  }

  auto &shard = get_pop_shard(table_id);
  std::shared_ptr<table_pop_buffer_t> tbuf;
  {
    std::shared_lock<std::shared_mutex> lk(shard.mutex);
    auto it = shard.buffers.find(table_id);
    if (it == shard.buffers.end() || !it->second) return barrier;
    tbuf = it->second;
  }

  // A detached buffer means the table is being unloaded (or its buffer was replaced by a reload). Change ids are
  // process-global but a replacement buffer restarts them at 0, so a reader must not wait on a watermark the current
  // buffer can never reach.
  if (tbuf->detached.load(std::memory_order_acquire)) {
    barrier.state = TablePropagationState::GONE;
    return barrier;
  }

  barrier.required_change_id = tbuf->enqueued_change_id.load(std::memory_order_acquire);
  barrier.applied_change_id = tbuf->applied_change_id.load(std::memory_order_acquire);
  barrier.buffer_generation = tbuf->generation;

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
                                                                      uint64_t required_change_id, uint64_t wait_ms,
                                                                      uint64_t buffer_generation) {
  DBUG_EXECUTE_IF("secondary_engine_rapid_barrier_gone_once", {
    // Was a function-local static, which fires only once per process: under
    // --repeat, or any run that reuses the server, the GONE branch stopped
    // being tested while the test kept passing. Self-disabling makes it fire
    // once per enable instead.
    DBUG_SET("-d,secondary_engine_rapid_barrier_gone_once");
    return TablePropagationWaitResult::GONE;
  });

  auto &shard = get_pop_shard(table_id);
  std::shared_ptr<table_pop_buffer_t> tbuf;
  {
    std::shared_lock<std::shared_mutex> lk(shard.mutex);
    auto it = shard.buffers.find(table_id);
    if (it == shard.buffers.end() || !it->second)
      return required_change_id == 0 ? TablePropagationWaitResult::APPLIED : TablePropagationWaitResult::GONE;
    tbuf = it->second;
  }

  // The watermark belongs to a buffer that has since been replaced (unload + reload under the same table_id).
  if (buffer_generation != 0 && tbuf->generation != buffer_generation) return TablePropagationWaitResult::GONE;

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
