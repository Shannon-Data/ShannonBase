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
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>

#include "current_thd.h"
#include "include/os0event.h"
#include "sql/sql_class.h"

#include "storage/innobase/include/btr0pcur.h"  //for btr_pcur_t
#include "storage/innobase/include/data0type.h"
#include "storage/innobase/include/dict0dd.h"
#include "storage/innobase/include/dict0dict.h"
#include "storage/innobase/include/dict0mem.h"  //for dict_index_t, etc.
#include "storage/innobase/include/os0thread-create.h"

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_table_info.h"
#include "storage/rapid_engine/populate/log_copyinfo.h"
#include "storage/rapid_engine/populate/log_redolog.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_populate_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
namespace Populate {
constexpr uint32_t MAX_RETRY_COUNT = 3;
// to identify synchonization mode.
std::atomic<SyncMode> g_sync_mode{SyncMode::DIRECT_NOTIFICATION};

// should be pay more attention to syc relation between this thread
// and log_buffer write. if we stop changes poping, should stop writing
// firstly, then stop this thread.
std::unordered_map<table_id_t, std::unique_ptr<table_pop_buffer_t>> sys_pop_buff;
std::shared_mutex sys_pop_buff_mutex;

// does the pop thread started or not.
std::atomic<bool> sys_pop_started{false};

// how many data was in sys_pop_buff in total?
std::atomic<uint64> sys_pop_data_sz{0};

// how many times applied round.
static uint64 sys_rapid_loop_count{0};

struct table_worker_context {
  table_id_t table_key;
  IB_thread thread_handle;
  std::atomic<bool> should_stop{false};
  std::mutex mtx;
  std::condition_variable cv;
  std::chrono::steady_clock::time_point last_activity;

  std::unordered_map<uint64_t, change_record_buff_t> pending_records;
  std::atomic<size_t> pending_size{0};

  // lsn<---> retry_counts
  std::unordered_map<uint64_t, uint32_t> retry_counts;
  table_worker_context(table_id_t key) : table_key(key), last_activity(std::chrono::steady_clock::now()) {}
  static table_worker_context *get_or_create_table_worker(const table_id_t &table_key);
};

static std::shared_mutex table_workers_mutex;
static std::unordered_map<table_id_t, std::unique_ptr<table_worker_context>> table_workers;

static void table_worker_func(table_worker_context *ctx) {
#if !defined(_WIN32)
  std::string thread_name = "rapid_change_table_worker_" + std::to_string(ctx->table_key);
  pthread_setname_np(pthread_self(), thread_name.c_str());
#else
  std::wstring thread_name = L"rapid_change_table_worker_" + std::to_string(ctx->table_key);
  SetThreadDescription(GetCurrentThread(), thread_name.c_str());
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

  SHANNON_THREAD_LOCAL LogParser parse_log;
  SHANNON_THREAD_LOCAL CopyInfoParser copy_info_log;
  SHANNON_THREAD_LOCAL Rapid_load_context context;
  context.m_thd = thd;

  while (!ctx->should_stop.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> lock(ctx->mtx);
    ctx->cv.wait_for(lock, std::chrono::milliseconds(TABLE_WORKER_IDLE_TIMEOUT),
                     [ctx] { return ctx->should_stop.load() || ctx->pending_size.load() > 0; });
    if (ctx->should_stop.load()) break;

    auto idle =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - ctx->last_activity)
            .count();
    if ((uint64_t)idle >= TABLE_WORKER_IDLE_TIMEOUT && ctx->pending_size.load() == 0) break;

    std::unordered_map<uint64_t, change_record_buff_t> applying;
    std::unordered_map<uint64_t, change_record_buff_t> failed;
    size_t applied_size = 0;
    if (ctx->pending_size.load() > 0) {
      std::swap(applying, ctx->pending_records);
      ctx->pending_records.clear();
      ctx->pending_size.store(0, std::memory_order_release);
      size_t total_batch_size = ctx->pending_size.load(std::memory_order_relaxed);
      sys_pop_data_sz.fetch_sub(total_batch_size, std::memory_order_release);
    }
    lock.unlock();

    if (applying.empty()) continue;

    for (auto &[lsn, change_rec] : applying) {
      size_t parsed_bytes = 0;
      if (change_rec.m_source == ShannonBase::Populate::Source::REDO_LOG) {
        const byte *start = change_rec.m_buff0.get();
        const byte *end = start + change_rec.m_size;
        parsed_bytes = parse_log.parse_redo(&context, const_cast<byte *>(start), const_cast<byte *>(end));
        assert(parsed_bytes == size_t(end - start));
      } else if (change_rec.m_source == ShannonBase::Populate::Source::COPY_INFO) {
        auto oper_type = change_rec.m_oper;
#ifndef NDEBUG
        context.m_schema_name = change_rec.m_schema_name;
        context.m_table_name = change_rec.m_table_name;
        context.m_sch_tb_name = context.m_schema_name + ":" + context.m_table_name;
#endif
        context.m_offpage_data0 = change_rec.m_offpage_data0.empty() ? nullptr : &change_rec.m_offpage_data0;
        context.m_offpage_data1 = change_rec.m_offpage_data1.empty() ? nullptr : &change_rec.m_offpage_data1;

        context.m_trx = Transaction::get_or_create_trx(thd);
        context.m_trx->begin();
        context.m_extra_info.m_trxid = context.m_trx->get_id();

        const byte *old_start = change_rec.m_buff0.get();
        const byte *old_end = old_start + change_rec.m_size;
        const byte *new_start = change_rec.m_buff1.get();
        const byte *new_end = new_start + change_rec.m_size;
        parsed_bytes = copy_info_log.parse_copy_info(&context, change_rec.m_table_id, oper_type,
                                                     const_cast<byte *>(old_start), const_cast<byte *>(old_end),
                                                     const_cast<byte *>(new_start), const_cast<byte *>(new_end));
        ShannonBase::Imcs::RpdTable *rpd_tb{nullptr};
        rpd_tb = Imcs::Imcs::instance()->get_rpd_parttable(change_rec.m_table_id);
        if (rpd_tb)
          rpd_tb->register_transaction(context.m_trx);
        else {
          rpd_tb = Imcs::Imcs::instance()->get_rpd_table(change_rec.m_table_id);
          if (rpd_tb) rpd_tb->register_transaction(context.m_trx);
        }
        context.m_trx->commit();
      }

      if (parsed_bytes == change_rec.m_size) {
        applied_size += change_rec.m_size;
        ctx->retry_counts.erase(lsn);
      } else {
        uint32_t current_retry = ++ctx->retry_counts[lsn];
        if (current_retry <= MAX_RETRY_COUNT) {
          push_warning_printf(thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE,
                              "Propagation failed for table %ld at LSN %lu (retry %u/%u), retrying...", ctx->table_key,
                              lsn, current_retry, MAX_RETRY_COUNT);
          failed.emplace(lsn, std::move(change_rec));
        } else {
          // has over max-try-count, the discard re-trying.
          push_warning_printf(thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE,
                              "Propagation failed for table %ld at LSN %lu after %u retries, dropping record",
                              ctx->table_key, lsn, MAX_RETRY_COUNT);
          ctx->retry_counts.erase(lsn);
          applied_size += change_rec.m_size;  // substract from pending_size.
        }
      }
    }

    if (!failed.empty()) {
      std::lock_guard<std::mutex> lock(ctx->mtx);
      for (auto &p : failed) {
        ctx->pending_records.emplace(p.first, std::move(p.second));
        ctx->pending_size.fetch_add(p.second.m_size);
      }
      ctx->cv.notify_one();
    }

    {
      std::lock_guard<std::mutex> lock(ctx->mtx);
      ctx->last_activity = std::chrono::steady_clock::now();
    }
  }  // end while.

  {
    std::unique_lock<std::shared_mutex> lock(table_workers_mutex);
    auto it = table_workers.find(ctx->table_key);
    if (it != table_workers.end() && it->second.get() == ctx) {
      table_workers.erase(it);
    }
  }
  close_thread_tables(thd);
}

table_worker_context *table_worker_context::get_or_create_table_worker(const table_id_t &table_key) {
  {
    std::shared_lock<std::shared_mutex> lock(table_workers_mutex);
    auto it = table_workers.find(table_key);
    if (it != table_workers.end() && thread_is_active(it->second->thread_handle)) {
      return it->second.get();
    }
  }

  std::unique_lock<std::shared_mutex> lock(table_workers_mutex);
  auto it = table_workers.find(table_key);
  if (it != table_workers.end() && thread_is_active(it->second->thread_handle)) {
    return it->second.get();  // double check
  }

  auto ctx = std::make_unique<table_worker_context>(table_key);
  auto *ctx_ptr = ctx.get();
  IB_thread handle = os_thread_create(rapid_populate_thread_key, 0, table_worker_func, ctx_ptr);
  ctx_ptr->thread_handle = handle;
  table_workers[table_key] = std::move(ctx);
  table_workers[table_key]->thread_handle.start();
  return ctx_ptr;
}

/**
 * main entry of pop thread. it monitors sys_pop_buff, a new mtr_log_rect_t
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
         sys_pop_started.load(std::memory_order_acquire)) {
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::microseconds{POP_MAX_WAIT_TIMEOUT};

    // Step 1: Wait for trigger condition (64MB or 200ms or query-driven)
    auto stop_condition = [&](bool wait) {
      {
        // cond1: table has be marked flushed.
        std::shared_lock<std::shared_mutex> lk(sys_pop_buff_mutex);
        for (const auto &[key, tbuf] : sys_pop_buff) {
          if (tbuf->pending_flush.load(std::memory_order_acquire)) {
            return true;
          }
        }
      }

      // cond2: the event is set.
      if (wait) {
        if (os_event_is_set(log_sys->rapid_events[0])) return true;
      }

      // cond3: has waited for `MAX_WAIT_TIMEOUT`, timeout.
      if (std::chrono::steady_clock::now() >= wait_deadline) {
        {
          std::shared_lock<std::shared_mutex> lk(sys_pop_buff_mutex);
          for (const auto &[key, tbuf] : sys_pop_buff) {
            if (tbuf->data_size.load(std::memory_order_acquire) > 0) {
              tbuf->pending_flush.store(true, std::memory_order_release);
            }
          }
        }
        return true;
      }

      // NOT MATCH
      return false;
    };

    os_event_wait_for(log_ptr->rapid_events[0], 0, std::chrono::microseconds{POP_MAX_WAIT_TIMEOUT}, stop_condition);
    os_event_reset(log_sys->rapid_events[0]);

    if (!sys_pop_started.load()) break;

    // Step 2: collect all need flush tables (query-driven + over threshold）
    std::vector<table_id_t> tables_to_flush;
    {
      std::shared_lock<std::shared_mutex> lk(sys_pop_buff_mutex);
      for (auto &[key, tbuf] : sys_pop_buff) {
        if (tbuf->pending_flush.exchange(false, std::memory_order_acq_rel)) {
          tables_to_flush.push_back(key);
        }
      }
    }
    if (tables_to_flush.empty()) continue;

    // Step 3: apply
    for (const auto &table_key : tables_to_flush) {
      std::unique_ptr<table_pop_buffer_t> tbuf;
      {
        std::unique_lock lk(sys_pop_buff_mutex);
        auto it = sys_pop_buff.find(table_key);
        if (it == sys_pop_buff.end()) continue;
        tbuf = std::move(it->second);
        sys_pop_buff.erase(it);
      }

      // batch processing
      change_candidate_t candidate_batch[BATCH_PROCESS_NUM];
      size_t count = 0;
      std::unordered_map<uint64_t, change_record_buff_t> applying;  // lsn<-->change_record_buff_t
      while ((count = tbuf->change_candiates.try_pop_bulk(candidate_batch, BATCH_PROCESS_NUM)) > 0) {
        for (size_t i = 0; i < count; ++i)
          applying.emplace(candidate_batch[i].lsn, std::move(candidate_batch[i].record));
      }
      if (applying.empty()) continue;

      auto *worker = table_worker_context::get_or_create_table_worker(table_key);
      {
        std::lock_guard lock(worker->mtx);
        for (auto &p : applying) {
          worker->pending_records.emplace(p.first, std::move(p.second));
          worker->pending_size.fetch_add(p.second.m_size);
        }
        worker->last_activity = std::chrono::steady_clock::now();
        worker->cv.notify_one();
      }
    }

    sys_rapid_loop_count++;
  }

  sys_pop_started.store(false, std::memory_order_seq_cst);
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
void Populator::end() { get_impl()->end_impl(); }

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
    srv_threads.m_change_pop_cordinator = os_thread_create(rapid_populate_thread_key, 0, parse_log_func_main, log_sys);
    ShannonBase::Populate::sys_pop_started.store(true, std::memory_order_seq_cst);
    srv_threads.m_change_pop_cordinator.start();
    ut_a(active_impl());
  }
}

void PopulatorImpl::unload_impl(const table_id_t &table_id) {
  if (unlikely(!active_impl() || !shannon_loaded_tables->size())) return;
  {
    std::unique_lock lk(sys_pop_buff_mutex);
    sys_pop_buff.erase(table_id);
  }

  std::unique_ptr<table_worker_context> ctx_to_destroy;
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
  if (!active_impl() && !ShannonBase::shannon_loaded_tables->size()) return;

  {
    std::shared_lock<std::shared_mutex> read_lock(table_workers_mutex);
    for (auto &[tid, ctx] : table_workers) {
      ctx->should_stop.store(true, std::memory_order_release);
      ctx->cv.notify_one();
    }
  }

  // Step 2: move out all ctx
  decltype(table_workers) workers_to_join;
  {
    std::unique_lock<std::shared_mutex> write_lock(table_workers_mutex);
    workers_to_join = std::move(table_workers);
    table_workers.clear();
  }

  // Step 3: waiting for all table workers to finish.
  for (auto &[tid, ctx] : workers_to_join) {
    if (thread_is_active(ctx->thread_handle)) ctx->thread_handle.join();
  }

  // Step 4: stop coordinator main thread
  sys_pop_started.store(false, std::memory_order_seq_cst);
  os_event_set(log_sys->rapid_events[0]);
  if (thread_is_active(srv_threads.m_change_pop_cordinator)) {
    srv_threads.m_change_pop_cordinator.join();
  }

  // Step 5: clear all status
  sys_rapid_loop_count = 0;
  g_index_cache.clear();
  g_index_names.clear();
  g_propagating_tables.clear();
  {
    std::unique_lock<std::shared_mutex> lock(sys_pop_buff_mutex);
    sys_pop_buff.clear();
  }

  ut_a(!active_impl());
}

uint PopulatorImpl::write_impl(FILE *file, uint64_t start_lsn, change_record_buff *changed_rec) {
  if (!active_impl() || !shannon_loaded_tables->size()) return SHANNON_SUCCESS;

  const table_id_t table_key = changed_rec->m_table_id;
  const size_t rec_sz = changed_rec->m_size;
  bool need_wakeup = false;

  {
    std::shared_lock<std::shared_mutex> slk(sys_pop_buff_mutex);
    auto it = sys_pop_buff.find(table_key);
    if (it != sys_pop_buff.end()) {
      auto &tbuf = *it->second;

      change_candidate_t candidate_item(start_lsn, std::move(*changed_rec));
      if (tbuf.change_candiates.try_put(std::move(candidate_item))) {
        size_t old_sz = tbuf.data_size.fetch_add(rec_sz, std::memory_order_relaxed);
        sys_pop_data_sz.fetch_add(rec_sz, std::memory_order_relaxed);

        bool crossed_threshold =
            (old_sz <= SHANNON_POPULATION_HRESHOLD_SIZE) && (old_sz + rec_sz > SHANNON_POPULATION_HRESHOLD_SIZE);
        bool was_queried = tbuf.queried.exchange(false, std::memory_order_acq_rel);
        if (crossed_threshold || was_queried) {
          tbuf.pending_flush.store(true, std::memory_order_release);
          need_wakeup = true;
        }
        if (need_wakeup) os_event_set(log_sys->rapid_events[0]);
        return SHANNON_SUCCESS;
      }
      // TODO: if ring buffer is full. to degrage
    }
  }

  {
    std::unique_lock<std::shared_mutex> ulk(sys_pop_buff_mutex);
    auto [it, inserted] = sys_pop_buff.emplace(table_key, std::make_unique<table_pop_buffer_t>());
    auto &tbuf = *it->second;
    change_candidate_t item(start_lsn, std::move(*changed_rec));  // new item to insert.
    if (tbuf.change_candiates.try_put(std::move(item))) {
      size_t old_sz = tbuf.data_size.fetch_add(rec_sz, std::memory_order_relaxed);
      sys_pop_data_sz.fetch_add(rec_sz, std::memory_order_relaxed);

      if (old_sz == 0 || old_sz + rec_sz > SHANNON_POPULATION_HRESHOLD_SIZE ||
          tbuf.queried.exchange(false, std::memory_order_acq_rel)) {
        tbuf.pending_flush.store(true, std::memory_order_release);
        need_wakeup = true;
      }
    } else {  // is full.
      tbuf.pending_flush.store(true, std::memory_order_release);
      need_wakeup = true;
      push_warning_printf(current_thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE,
                          "Rapid ringbuffer full for table %llu, forcing flush", (unsigned long long)table_key);
    }
  }

  if (need_wakeup) os_event_set(log_sys->rapid_events[0]);
  return SHANNON_SUCCESS;
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
      std::shared_lock slock(g_index_cache_mutex);
      if (g_index_cache.find(index_rec->id) == g_index_cache.end()) {  // add new one.
        slock.unlock();
        std::unique_lock<std::shared_mutex> ex_lock(g_index_cache_mutex);
        if (g_index_cache.find(index_rec->id) == g_index_cache.end()) {  // double check
          g_index_cache[index_rec->id] = index_rec;
          std::string db_name, table_name;
          index_rec->table->get_table_name(db_name, table_name);
          g_index_names[index_rec->id] = std::make_pair(db_name, table_name);
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
  fprintf(file,
          "rapid log pop thread : %s \n"
          "rapid log pop thread loops: " ULINTPF
          "\n"
          "rapid log data remaining size: " ULINTPF
          " KB\n"
          "rapid log data remaining line: " ULINTPF "\n",
          ShannonBase::Populate::sys_pop_started ? "running" : "stopped", ShannonBase::Populate::sys_rapid_loop_count,
          ShannonBase::Populate::sys_pop_data_sz / 1024, ShannonBase::Populate::sys_pop_buff.size());
}

bool PopulatorImpl::is_loaded_table_impl(std::string sch_name, std::string table_name) {
  auto share = ShannonBase::shannon_loaded_tables->get(sch_name.c_str(), table_name.c_str());
  return (share) ? true : false;
}

bool PopulatorImpl::mark_table_required_impl(const table_id_t &table_id) {
  std::shared_lock<std::shared_mutex> lk(sys_pop_buff_mutex);
  auto it = sys_pop_buff.find(table_id);
  if (it != sys_pop_buff.end()) {
    it->second->queried.store(true, std::memory_order_release);
    it->second->pending_flush.store(true, std::memory_order_release);
    os_event_set(log_sys->rapid_events[0]);  // <--- invoke the coordinator immdiately！！
    return true;
  }

  return false;
}
}  // namespace Populate
}  // namespace ShannonBase