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
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/populate/log_copyinfo.h"
#include "storage/rapid_engine/populate/log_redolog.h"

#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t rapid_populate_thread_key;
#endif /* UNIV_PFS_THREAD */

namespace ShannonBase {
namespace Populate {
// to identify synchonization mode.
std::atomic<SyncMode> g_sync_mode{SyncMode::DIRECT_NOTIFICATION};

// should be pay more attention to syc relation between this thread
// and log_buffer write. if we stop changes poping, should stop writing
// firstly, then stop this thread.
std::unordered_map<std::string, std::unique_ptr<table_pop_buffer_t>> sys_pop_buff;
std::shared_mutex sys_pop_buff_mutex;

// does the pop thread started or not.
std::atomic<bool> sys_pop_started{false};

// how many data was in sys_pop_buff in total?
std::atomic<uint64> sys_pop_data_sz{0};

// how many times applied round.
static uint64 sys_rapid_loop_count{0};

/**
 * return lsn_t, key of processing mtr_log_rec_t. Parse the log comes from redo log records.
 */
static uint64_t parse_mtr_log_worker(uint64_t start_lsn, const change_record_buff_t *change_rec) {
  change_record_buff_t local_rec = *change_rec;  // in case the changed buffer be modified in concurrent env.
  ut_a(local_rec.m_source == Source::REDO_LOG);

  THD *log_pop_thread_thd{nullptr};
  if (current_thd == nullptr) {
    my_thread_init();
    log_pop_thread_thd = create_internal_thd();
    if (!log_pop_thread_thd) {
      my_thread_end();
      return start_lsn;
    }

    log_pop_thread_thd->system_thread = SYSTEM_THREAD_BACKGROUND;
    log_pop_thread_thd->security_context()->skip_grants();

    log_pop_thread_thd->store_globals();
  } else {
    log_pop_thread_thd = current_thd;
  }

#if !defined(_WIN32)  // here we
  pthread_setname_np(pthread_self(), "rapid_log_wkr");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_log_wkr");
#endif
  const byte *start = local_rec.m_buff0.get();
  const byte *end = local_rec.m_buff0.get() + local_rec.m_size;
  size_t sz = local_rec.m_size;

  SHANNON_THREAD_LOCAL LogParser parse_log;
  SHANNON_THREAD_LOCAL Rapid_load_context context;
  context.m_thd = log_pop_thread_thd;

  auto parsed_bytes = parse_log.parse_redo(&context, const_cast<byte *>(start), const_cast<byte *>(end));
  ut_a(parsed_bytes == sz);

  if (log_pop_thread_thd) {
    close_thread_tables(log_pop_thread_thd);

    my_thread_end();
    destroy_internal_thd(log_pop_thread_thd);
    log_pop_thread_thd = nullptr;
  }

  return (parsed_bytes == sz) ? start_lsn : 0;
}

/**
 * return lsn_t, key of processing mtr_log_rec_t. Parse the log comes from COPY_INFO records.
 */
static uint64_t parse_copy_info_record_worker(uint64_t start_lsn, const change_record_buff_t *change_rec) {
  change_record_buff_t local_rec = *change_rec;  // in case the changed buffer be modified in concurrent env.

  THD *log_pop_thread_thd{nullptr};
  if (current_thd == nullptr) {
    my_thread_init();
    log_pop_thread_thd = create_internal_thd();
    if (!log_pop_thread_thd) {
      my_thread_end();
      return start_lsn;
    }

    log_pop_thread_thd->system_thread = SYSTEM_THREAD_BACKGROUND;
    log_pop_thread_thd->security_context()->skip_grants();

    log_pop_thread_thd->store_globals();
  } else {
    log_pop_thread_thd = current_thd;
  }

#if !defined(_WIN32)  // here we
  pthread_setname_np(pthread_self(), "rapid_copy_info_wkr");
#else
  SetThreadDescription(GetCurrentThread(), L"rapid_copy_info_wkr");
#endif
  SHANNON_THREAD_LOCAL CopyInfoParser copy_info_log;
  SHANNON_THREAD_LOCAL Rapid_load_context context;
  auto oper_type = local_rec.m_oper;

  context.m_schema_name = local_rec.m_schema_name;
  context.m_table_name = local_rec.m_table_name;
  context.m_sch_tb_name = context.m_schema_name + ":" + context.m_table_name;

  context.m_offpage_data0 = local_rec.m_offpage_data0.size() ? &local_rec.m_offpage_data0 : nullptr;
  context.m_offpage_data1 = local_rec.m_offpage_data0.size() ? &local_rec.m_offpage_data1 : nullptr;

  context.m_trx = Transaction::get_or_create_trx(current_thd);
  context.m_trx->begin();
  context.m_extra_info.m_trxid = context.m_trx->get_id();

  const byte *start = local_rec.m_buff0.get();
  const byte *end = local_rec.m_buff0.get() + local_rec.m_size;
  const byte *new_start = local_rec.m_buff1.get();
  const byte *new_end_ptr = local_rec.m_buff1.get() + local_rec.m_size;
  size_t sz = local_rec.m_size;

  auto parsed_bytes =
      copy_info_log.parse_copy_info(&context, oper_type, const_cast<byte *>(start), const_cast<byte *>(end),
                                    const_cast<byte *>(new_start), const_cast<byte *>(new_end_ptr));

  std::string key_part;
  key_part.append(context.m_schema_name.c_str()).append(":").append(context.m_table_name.c_str());
  ShannonBase::Imcs::RpdTable *rpd_tb{nullptr};
  rpd_tb = Imcs::Imcs::instance()->get_rpd_parttable(key_part);
  if (rpd_tb)
    rpd_tb->register_transaction(context.m_trx);
  else {
    rpd_tb = Imcs::Imcs::instance()->get_rpd_table(key_part);
    if (rpd_tb) rpd_tb->register_transaction(context.m_trx);
  }
  context.m_trx->commit();

  ut_a(parsed_bytes == sz);

  if (log_pop_thread_thd) {
    close_thread_tables(log_pop_thread_thd);

    my_thread_end();
    destroy_internal_thd(log_pop_thread_thd);
    log_pop_thread_thd = nullptr;
  }

  return (parsed_bytes == sz) ? start_lsn : 0;
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
    std::vector<std::string> tables_to_flush;
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
      std::unique_ptr<ShannonBase::Populate::table_pop_buffer_t> tbuf;
      {
        std::unique_lock<std::shared_mutex> ulk(sys_pop_buff_mutex);
        auto it = sys_pop_buff.find(table_key);
        if (it == sys_pop_buff.end() || it->second->swapping.load()) continue;
        it->second->swapping = true;
        tbuf = std::move(it->second);
        sys_pop_buff.erase(it);
      }

      std::unordered_map<uint64_t, change_record_buff_t> applying;
      {
        std::lock_guard<std::mutex> lock(tbuf->mutex);
        applying = std::move(tbuf->writing);
        tbuf->writing.clear();
        tbuf->data_size.store(0, std::memory_order_release);
      }

      std::vector<std::future<uint64_t>> results;
      size_t thread_num = std::min(std::thread::hardware_concurrency() / 2, (uint)applying.size());
      auto curr_iter = applying.begin();
      for (size_t i = 0; i < thread_num && curr_iter != applying.end(); ++i, ++curr_iter) {
        if (curr_iter->second.m_source == Source::REDO_LOG)
          results.emplace_back(
              std::async(std::launch::async, parse_mtr_log_worker, curr_iter->first, &curr_iter->second));
        else
          results.emplace_back(
              std::async(std::launch::async, parse_copy_info_record_worker, curr_iter->first, &curr_iter->second));
      }

      for (auto &res : results) {
        auto ret_lsn = res.get();
        if (!ret_lsn) {
          push_warning_printf(current_thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE,
                              "Propagation failed for table %s, data is stale. Please SECONDARY_UNLOAD/LOAD",
                              table_key.c_str());
        }

        auto iter = applying.find(ret_lsn);
        if (iter != applying.end()) {
          sys_pop_data_sz.fetch_sub(iter->second.m_size);
          applying.erase(iter);
        } else
          assert(false);
      }

      if (!applying.empty()) {  // remaings re-add into pop buffer.
        std::lock_guard<std::mutex> lock(tbuf->mutex);
        for (auto &p : applying) {
          tbuf->writing.emplace(p.first, std::move(p.second));
          tbuf->data_size.fetch_add(p.second.m_size);
          sys_pop_data_sz.fetch_add(p.second.m_size);
        }
        {
          std::unique_lock<std::shared_mutex> ulk(sys_pop_buff_mutex);
          sys_pop_buff[table_key] = std::move(tbuf);
        }
      }
    }  // end for each table

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

void PopulatorImpl::end_impl() {
  if (active_impl() && shannon_loaded_tables->size()) {
    sys_pop_started.store(false, std::memory_order_seq_cst);
    os_event_set(log_sys->rapid_events[0]);
    srv_threads.m_change_pop_cordinator.join();
    sys_rapid_loop_count = 0;

    g_index_cache.clear();
    g_index_names.clear();
    g_processing_tables.clear();

    sys_pop_buff.clear();
    ut_a(active_impl() == false);
  }
}

uint PopulatorImpl::write_impl(FILE *file, uint64_t start_lsn, change_record_buff *changed_rec) {
  if (!active_impl() || !shannon_loaded_tables->size()) return SHANNON_SUCCESS;

  bool need_wakeup{false};
  std::string table_key = changed_rec->m_schema_name + ":" + changed_rec->m_table_name;
  size_t rec_sz = changed_rec->m_size;

  {
    std::shared_lock<std::shared_mutex> slk(sys_pop_buff_mutex);
    auto it = sys_pop_buff.find(table_key);
    if (it != sys_pop_buff.end()) {
      auto &tbuf = *it->second;
      std::lock_guard<std::mutex> lock(tbuf.mutex);
      tbuf.writing.emplace(start_lsn, std::move(*changed_rec));
      tbuf.data_size.fetch_add(rec_sz);
      sys_pop_data_sz.fetch_add(rec_sz);

      if (tbuf.data_size > SHANNON_POPULATION_HRESHOLD_SIZE) {
        tbuf.pending_flush.store(true, std::memory_order_release);
        need_wakeup = true;
      }

      if (tbuf.queried.exchange(false)) {
        tbuf.pending_flush.store(true, std::memory_order_release);
        need_wakeup = true;
      }

      if (need_wakeup) os_event_set(log_sys->rapid_events[0]);
      return SHANNON_SUCCESS;
    }
  }

  {
    std::unique_lock<std::shared_mutex> ulk(sys_pop_buff_mutex);
    auto [it, inserted] = sys_pop_buff.emplace(table_key, std::make_unique<table_pop_buffer_t>());
    auto &tbuf = *it->second;

    std::lock_guard<std::mutex> lock(tbuf.mutex);
    tbuf.writing.emplace(start_lsn, std::move(*changed_rec));
    tbuf.data_size.fetch_add(rec_sz);
    sys_pop_data_sz.fetch_add(rec_sz);

    if (tbuf.data_size > SHANNON_POPULATION_HRESHOLD_SIZE) need_wakeup = true;
    if (tbuf.queried.exchange(false)) need_wakeup = true;
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

bool PopulatorImpl::mark_table_required_impl(std::string &sch_table_name) {
  std::shared_lock<std::shared_mutex> lk(sys_pop_buff_mutex);
  auto it = sys_pop_buff.find(sch_table_name);
  if (it != sys_pop_buff.end()) {
    it->second->queried.store(true, std::memory_order_release);
    os_event_set(log_sys->rapid_events[0]);  // <--- invoke the coordinator immdiately！！
    return true;
  }

  return false;
}
}  // namespace Populate
}  // namespace ShannonBase