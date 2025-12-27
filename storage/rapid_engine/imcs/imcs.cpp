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

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/imcs/imcs.h"

#include <threads.h>
#include <condition_variable>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "include/decimal.h"
#include "include/my_dbug.h"            //DBUG_EXECUTE_IF
#include "include/row0pread-adapter.h"  //Parallel Reader
#include "sql/dd_table_share.h"
#include "sql/histograms/table_histograms.h"     // decrement_reference_counter
#include "sql/partitioning/partition_handler.h"  //partition handler
#include "sql/sql_base.h"
#include "sql/transaction.h"  // trans_rollback_stmt, trans_commit_stmt

#include "storage/innobase/handler/ha_innopart.h"
#include "storage/innobase/include/data0type.h"
#include "storage/innobase/include/mach0data.h"
#include "storage/innobase/include/univ.i"    //UNIV_SQL_NULL
#include "storage/innobase/include/ut0dbg.h"  //ut_ad
#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/index/encoder.h"
#include "storage/rapid_engine/imcs/worker.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_loaded_table.h"
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/populate/log_commons.h"
#include "storage/rapid_engine/utils/utils.h"  //Utils

namespace ShannonBase {
extern ulonglong rpd_para_load_threshold;
extern ulonglong rpd_para_parttb_load_threshold;
SHANNON_THREAD_LOCAL std::string Rapid_load_context::extra_info_t::m_active_part_key;
namespace Imcs {
Imcs *Imcs::m_instance{nullptr};
std::unique_ptr<boost::asio::thread_pool> Imcs::m_imcs_pool{nullptr};
std::once_flag Imcs::one;

SHANNON_THREAD_LOCAL Imcs *current_imcs_instance = Imcs::instance();

bool PartitionLoadThreadContext::initialize(const Rapid_load_context *context) {
  // Create THD
  m_thd = new THD;
  if (!m_thd) return true;

  m_thd->set_new_thread_id();
  m_thd->thread_stack = (char *)this;
  m_thd->set_command(COM_DAEMON);
  m_thd->security_context()->skip_grants();
  m_thd->system_thread = NON_SYSTEM_THREAD;
  m_thd->store_globals();
  m_thd->lex->sql_command = SQLCOM_SELECT;

  // Open table from source table share.
  TABLE_SHARE *share = context->m_table->s;
  m_table = (TABLE *)m_thd->mem_root->Alloc(sizeof(TABLE));
  if (!m_table) return true;
  // get a copy of source TABLE object with its table share. TABLE will be used for feteching data from part tables.
  // we will clone a new handler for using multi-cursor. The invoker[mysql_secodary_load_unload] hold the refcnt
  // of shhare, here, we dont need to warry about its be released.
  if (open_table_from_share(m_thd, share, share->path.str, 0, SKIP_NEW_HANDLER, 0, m_table, false, nullptr)) {
    return true;
  }

  m_table->in_use = nullptr;
  m_table->file = nullptr;
  m_table->alias_name_used = context->m_table->alias_name_used;

  bitmap_copy(&m_table->def_read_set, context->m_table->read_set);
  bitmap_copy(&m_table->def_write_set, context->m_table->write_set);

  return false;
}

int PartitionLoadThreadContext::end_transactions() {
  auto ret{ShannonBase::SHANNON_SUCCESS};
  if (m_transactions_ended || !m_thd) return ret;

  ret = (m_error.load()) ? (trans_rollback_stmt(m_thd) || trans_rollback(m_thd))
                         : (trans_commit_stmt(m_thd) || trans_commit(m_thd));
  m_transactions_ended = true;
  return ret;
}

void PartitionLoadThreadContext::cleanup() {
  // Ensure transactions are ended first (idempotent)
  end_transactions();

  if (m_handler) {
    m_handler->ha_close();
    m_handler = nullptr;
  }

  if (m_table) {
    closefrm(m_table, false);  // should be freed by mysql_secondary_load_or_unload. in `closefrm`, it dont decrease
                               // refcnt of m_histograms.
    /**
     * in `open_table_from_share` `m_histograms` ref_cnt is increased, therefore, here, we should decrease its refcnt by
     * onw.
     */
    if (m_table->histograms) {
      mysql_mutex_lock(&LOCK_open);
      m_table->s->m_histograms->release(m_table->histograms);
      mysql_mutex_unlock(&LOCK_open);
    }

    m_table = nullptr;
  }

  if (m_thd) {
    m_thd->restore_globals();
    delete m_thd;
    m_thd = nullptr;
  }
}

bool PartitionLoadThreadContext::clone_handler(ha_innopart *file, const Rapid_load_context *context,
                                               std::mutex &clone_mutex) {
  const char *normalized_path = context->m_table->s->normalized_path.str;
  ha_innopart *cloned_handler = nullptr;
  {
    std::lock_guard<std::mutex> lock(clone_mutex);
    THD *saved = context->m_table->in_use;
    context->m_table->in_use = m_thd;
    cloned_handler = static_cast<ha_innopart *>(file->clone(normalized_path, m_thd->mem_root));
    context->m_table->in_use = saved;

    if (!cloned_handler) return true;

    m_handler = cloned_handler;
    m_handler->change_table_ptr(m_table, m_table->s);
    m_table->file = m_handler;
  }
  // Note: ha_open() is not needed because:
  // 1. ha_innopart::clone() inherits the open state from the source handler
  // 2. change_table_ptr() updates internal pointers while preserving the open state
  // 3. Partition-level operations (rnd_init_in_part/rnd_next_in_part) work directly

  return false;
}

int Imcs::initialize() {
  if (!m_inited.load()) {
    m_inited.store(1);
    Imcs::m_imcs_pool = std::make_unique<boost::asio::thread_pool>(std::thread::hardware_concurrency());
  }

  BkgWorkerPool::instance();
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::deinitialize() {
  if (m_inited.load(std::memory_order_acquire)) {
    BkgWorkerPool::shutdown_all(true);

    if (Imcs::m_imcs_pool) Imcs::m_imcs_pool.reset();

    m_rpd_tables.clear();
    m_rpd_parttables.clear();
    m_inited.store(0, std::memory_order_release);
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::create_table_memo(const Rapid_load_context *context, const TABLE *source) {
  ut_a(source);
  auto ret{ShannonBase::SHANNON_SUCCESS};
  TableConfig table_cfg;
  table_cfg.max_table_mem_size = SHANNON_TABLE_MEMRORY_SIZE;  // size of per table.
  table_cfg.rows_per_imcu = SHANNON_ROWS_IN_CHUNK;            // size of per imcu
  std::unique_ptr<RpdTable> rpd_table = std::make_unique<Table>(source, table_cfg);

  // step 1: create index memo of this table, which is built from MySQL Table key info.
  if ((ret = rpd_table.get()->create_index_memo(context))) return ret;

  // step 2: set the load type.
  rpd_table.get()->set_load_type(LoadType::USER_LOADED);

  // step3 : add to rpd_table cahce.
  m_rpd_tables.emplace(rpd_table->meta().table_id, std::move(rpd_table));
  return ShannonBase::SHANNON_SUCCESS;
  /* in secondary load phase, the table not loaded into imcs. therefore, it can be seen
     by any transactions. If this table has been loaded into imcs. A new data such as
     insert/update/delete will associated with a SMU items to trace its visibility. Therefore
     the following DB_TRX_ID cu no need to build.
  key.clear();
  std::unique_ptr<Mock_field_trxid> trx_fld = std::make_unique<Mock_field_trxid>();
  trx_fld.get()->table = const_cast<TABLE *>(source);
  key.append(keypart).append(SHANNON_DB_TRX_ID);
  m_cus.emplace(key, std::make_unique<Cu>(trx_fld.get()));
  */
}

int Imcs::create_parttable_memo(const Rapid_load_context *context, const TABLE *source) {
  ut_a(source);
  TableConfig table_cfg;
  table_cfg.max_table_mem_size = 0.1 * SHANNON_SMALL_TABLE_MEMRORY_SIZE;  // Parent Table[placeholder]
  auto rpd_part_table = std::make_unique<PartTable>(source, table_cfg);
  if (rpd_part_table->build_partitions(context)) {
    std::string errmsg;
    errmsg.append("try to build ")
        .append(context->m_schema_name.c_str())
        .append(".")
        .append(context->m_table_name.c_str())
        .append(" partitions failed");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return HA_ERR_GENERIC;
  }

  m_rpd_parttables.emplace(rpd_part_table->meta().table_id, std::move(rpd_part_table));
  return ShannonBase::SHANNON_SUCCESS;
}

void Imcs::cleanup(const table_id_t &table_id) {
  if (!m_rpd_tables.size() || m_rpd_tables.find(table_id) == m_rpd_tables.end()) return;
  m_rpd_tables.erase(table_id);
}

int Imcs::load_innodb(const Rapid_load_context *context, ha_innobase *file) {
  auto m_thd = context->m_thd;
  handler *shannon_file = file;
  // should be RC isolation level. set_tx_isolation(m_thd, ISO_READ_COMMITTED, true);
  if (shannon_file->inited == handler::NONE && shannon_file->ha_rnd_init(true)) {
    shannon_file->ha_rnd_end();
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
    return HA_ERR_GENERIC;
  }

  int tmp{HA_ERR_GENERIC};
  m_thd->set_sent_row_count(0);

  auto table_id = context->m_table_id;
  ut_a(m_rpd_tables.find(table_id) != m_rpd_tables.end());

  auto &meta_ref = shannon_loading_tables_meta[context->m_table_id];
  while ((tmp = shannon_file->ha_rnd_next(context->m_table->record[0])) != HA_ERR_END_OF_FILE) {
    /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
     without locks. Now, do full scan, but multi-thread scan will impl in future. */
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    DBUG_EXECUTE_IF("secondary_engine_rapid_load_table_error", {
      my_error(ER_SECONDARY_ENGINE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
      shannon_file->ha_rnd_end();
      return HA_ERR_GENERIC;
    });

    meta_ref.load_status = load_status_t::LOADING_RPDGSTABSTATE;
    meta_ref.loading_progress =
        0.1 + ((m_thd->get_sent_row_count() * 1.0) / (meta_ref.nrows ? meta_ref.nrows : 1)) * 0.7;  // up to 80%

    // ref to `row_sel_store_row_id_to_prebuilt` in row0sel.cc
    if ((m_rpd_tables[table_id].get()->insert_row(context, context->m_table->record[0])) == INVALID_ROW_ID) {
      std::string errmsg;
      errmsg.append("load data from ")
          .append(context->m_schema_name.c_str())
          .append(".")
          .append(context->m_table_name.c_str())
          .append(" to rapid failed");
      my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
      break;
    }
    m_thd->inc_sent_row_count(1);

    if (tmp == HA_ERR_RECORD_DELETED && !m_thd->killed) continue;
  }
  // end of load the data from innodb to imcs.
  shannon_file->ha_rnd_end();
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::load_innodb_parallel(const Rapid_load_context *context, ha_innobase *file) {
  auto m_thd = context->m_thd;
  handler *shannon_file = file;

  // should be RC isolation level. set_tx_isolation(m_thd, ISO_READ_COMMITTED, true);
  size_t num_threads;
  auto max_threads = thd_parallel_read_threads(m_thd);
  int tmp{HA_ERR_GENERIC};

  m_thd->set_sent_row_count(0);
  RpdTable *source_table{nullptr};
  {
    std::shared_lock lk(this->m_table_mutex);
    auto table_id = context->m_table_id;
    if (m_rpd_tables.find(table_id) == m_rpd_tables.end()) {
      my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
      return HA_ERR_GENERIC;
    }
    source_table = m_rpd_tables[table_id].get();
  }

  struct ParallelScanCtxGuard {
    handler *file;
    void *ctx{nullptr};
    std::vector<parall_scan_cookie_t *> thread_ctxs;
    std::vector<std::unique_ptr<parall_scan_cookie_t>> cookie_storage;
    ParallelScanCtxGuard(handler *f) : file(f) {}
    ~ParallelScanCtxGuard() {
      if (ctx) file->parallel_scan_end(ctx);
      cookie_storage.clear();
      thread_ctxs.clear();
    }

    void set_thread_ctxs(size_t num_threads) {
      thread_ctxs.resize(num_threads);
      cookie_storage.resize(num_threads);
      for (auto i = 0u; i < num_threads; ++i) {
        cookie_storage[i] = std::make_unique<parall_scan_cookie_t>();
        thread_ctxs[i] = cookie_storage[i].get();
      }
    }
  } scan_ctx_guard(shannon_file);

  std::unique_ptr<Utils::latch> completion_latch{nullptr};
  std::atomic<bool> error_flag{false};
  std::atomic<size_t> total_rows{0};

  if (shannon_file->inited == handler::NONE &&
      shannon_file->parallel_scan_init(scan_ctx_guard.ctx, &num_threads, true, max_threads)) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
    return HA_ERR_GENERIC;
  }

  auto &meta_ref = shannon_loading_tables_meta[context->m_table_id];

  // to set the thread contexts. now set to nullptr,  you can use your own ctx. or resize(num_threads,
  // (void*)&scan_cookie);
  scan_ctx_guard.set_thread_ctxs(num_threads);
  completion_latch = std::make_unique<Utils::latch>(num_threads);

  Parallel_reader_adapter::Init_fn init_fn = [](void *cookie, ulong ncols, ulong row_len, const ulong *col_offsets,
                                                const ulong *null_byte_offsets, const ulong *null_bitmasks) -> bool {
    auto ck = static_cast<parall_scan_cookie_t *>(cookie);
    ck->scan_done = false;
    ck->n_cols = ncols;
    ck->row_len = row_len;
    ck->col_offsets.assign(col_offsets, col_offsets + ncols);
    ck->null_byte_offsets.assign(null_byte_offsets, null_byte_offsets + ncols);
    ck->null_bitmasks.assign(null_bitmasks, null_bitmasks + ncols);
    ck->tid = std::this_thread::get_id();
    return false;
  };

  Parallel_reader_adapter::Load_fn load_fn = [&context, &shannon_file, &source_table, &error_flag, &total_rows,
                                              &meta_ref](void *cookie, uint nrows, void *rowdata,
                                                         uint64_t partition_id) -> bool {
    // ref to `row_sel_store_row_id_to_prebuilt` in row0sel.cc
    auto scan_cookie = static_cast<parall_scan_cookie_t *>(cookie);  //, if you enable thread contexs.
    ut_a(scan_cookie);
    ut_a(scan_cookie->tid == std::this_thread::get_id());

    auto data_ptr = static_cast<uchar *>(rowdata);
    auto end_data_ptr = static_cast<uchar *>(rowdata) + ptrdiff_t(nrows * scan_cookie->row_len);
    for (auto index = 0u; index < nrows; data_ptr += ptrdiff_t(scan_cookie->row_len), index++) {
      meta_ref.load_status = load_status_t::LOADING_RPDGSTABSTATE;

      if ((source_table->insert_row(context, (uchar *)data_ptr)) == INVALID_ROW_ID) {
        error_flag.store(true);
        std::string errmsg;
        errmsg.append("load data from ")
            .append(context->m_schema_name.c_str())
            .append(".")
            .append(context->m_table_name.c_str())
            .append(" to rapid failed.");
        my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
        return true;
      }
    }

    ut_a(data_ptr == end_data_ptr);
    scan_cookie->n_rows.store(nrows);
    total_rows.fetch_add(nrows);

    meta_ref.loading_progress = 0.1 + ((total_rows * 1.0) / (meta_ref.nrows ? meta_ref.nrows : 1)) * 0.7;  // up to 80%

    return false;
  };

  Parallel_reader_adapter::End_fn end_fn = [&completion_latch](void *cookie) {
    auto scan_cookie = static_cast<parall_scan_cookie_t *>(cookie);  //, if you enable thread contexs.
    ut_a(scan_cookie);
    ut_a(scan_cookie->tid == std::this_thread::get_id());
    scan_cookie->scan_done.store(true);

    completion_latch->count_down();
  };

  tmp = shannon_file->parallel_scan(scan_ctx_guard.ctx, reinterpret_cast<void **>(scan_ctx_guard.thread_ctxs.data()),
                                    init_fn, load_fn, end_fn);
  // Wait for scan to complete or error
  if (!completion_latch->wait_for(std::chrono::seconds(900))) {
    std::string errmsg;
    errmsg.append("Parallel load timeout for ")
        .append(context->m_schema_name.c_str())
        .append(".")
        .append(context->m_table_name.c_str());
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return HA_ERR_GENERIC;
  }

  /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
    without locks. Now, do full scan, but multi-thread scan will impl in future. */
  // if (tmp == HA_ERR_KEY_NOT_FOUND) return HA_ERR_KEY_NOT_FOUND;
  if (tmp || error_flag.load()) {
    DBUG_EXECUTE_IF("secondary_engine_rapid_load_table_error", {
      my_error(ER_SECONDARY_ENGINE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
      return tmp ? tmp : HA_ERR_GENERIC;
    });

    std::string errmsg;
    errmsg.append("Parallel load failed for ")
        .append(context->m_schema_name.c_str())
        .append(".")
        .append(context->m_table_name.c_str())
        .append(" to rapid failed.");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return tmp ? tmp : HA_ERR_GENERIC;
  }

  context->m_thd->inc_sent_row_count(total_rows.load());
  // end of load the data from innodb to imcs.
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::load_innodbpart(const Rapid_load_context *context, ha_innopart *file) {
  auto table_id = context->m_table_id;
  if (m_rpd_parttables.find(table_id) == m_rpd_parttables.end()) return ShannonBase::SHANNON_SUCCESS;
  auto part_tb_ptr = down_cast<PartTable *>(m_rpd_parttables[table_id].get());
  assert(part_tb_ptr);

  auto &meta_ref = shannon_loading_tables_meta[context->m_table_id];
  context->m_thd->set_sent_row_count(0);
  for (auto &[part_name, part_id] : context->m_extra_info.m_partition_infos) {
    auto partkey{part_name};
    partkey.append("#").append(std::to_string(part_id));
    auto partition_ptr = part_tb_ptr->get_partition(partkey);

    Rapid_load_context::extra_info_t::m_active_part_key = partkey;
    // should be RC isolation level. set_tx_isolation(m_thd, ISO_READ_COMMITTED, true);
    if (file->inited == handler::NONE && file->rnd_init_in_part(part_id, true)) {
      my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name, context->m_table_name);
      return HA_ERR_GENERIC;
    }

    int tmp{HA_ERR_GENERIC};
    while ((tmp = file->rnd_next_in_part(part_id, context->m_table->record[0])) != HA_ERR_END_OF_FILE) {
      /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
       without locks. Now, do full scan, but multi-thread scan will impl in future. */
      if (tmp == HA_ERR_KEY_NOT_FOUND) break;

      DBUG_EXECUTE_IF("secondary_engine_rapid_load_table_error", {
        my_error(ER_SECONDARY_ENGINE, MYF(0), context->m_schema_name, context->m_table_name);
        file->rnd_end_in_part(part_id, true);
        return HA_ERR_GENERIC;
      });

      // ref to `row_sel_store_row_id_to_prebuilt` in row0sel.cc
      if ((partition_ptr->insert_row(context, context->m_table->record[0])) == INVALID_ROW_ID) {
        file->rnd_end_in_part(part_id, true);
        std::string errmsg;
        errmsg.append("load data from ")
            .append(context->m_schema_name)
            .append(".")
            .append(context->m_table_name)
            .append(".")
            .append(partkey)
            .append(" to rapid failed");
        my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
      }

      context->m_thd->inc_sent_row_count(1);
      meta_ref.load_status = load_status_t::LOADING_RPDGSTABSTATE;
      meta_ref.loading_progress =
          0.1 +
          ((context->m_thd->get_sent_row_count() * 1.0) / (meta_ref.nrows ? meta_ref.nrows : 1)) * 0.7;  // up to 80%

      if (tmp == HA_ERR_RECORD_DELETED && !context->m_thd->killed) continue;
    }

    // end of load the data from innodb to imcs.
    file->rnd_end_in_part(part_id, true);
  }

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::load_innodbpart_parallel(const Rapid_load_context *context, ha_innopart *file) {
  auto table_id = context->m_table_id;
  if (m_rpd_parttables.find(table_id) == m_rpd_parttables.end()) return ShannonBase::SHANNON_SUCCESS;
  auto part_tb_ptr = down_cast<PartTable *>(m_rpd_parttables[table_id].get());
  assert(part_tb_ptr);

  context->m_thd->set_sent_row_count(0);

  std::vector<partition_load_task_t> tasks;
  tasks.reserve(context->m_extra_info.m_partition_infos.size());

  for (auto &[part_name, part_id] : context->m_extra_info.m_partition_infos) {
    partition_load_task_t task;
    task.part_id = part_id;
    task.part_key = part_name + "#" + std::to_string(part_id);
    task.result = ShannonBase::SHANNON_SUCCESS;
    task.rows_loaded = 0;
    tasks.push_back(std::move(task));
  }
  unsigned int num_threads = std::max(1u, static_cast<unsigned int>(std::thread::hardware_concurrency() * 0.8));
  num_threads = std::min(num_threads, static_cast<unsigned int>(tasks.size()));
  if (num_threads < 2 && tasks.size() > 1) num_threads = 2;

  std::mutex error_mutex, clone_mutex;
  std::atomic<uint64_t> total_rows{0};
  std::atomic<bool> has_error{false};

  std::vector<ulong> col_offsets(context->m_table->s->fields);
  std::vector<ulong> null_byte_offsets(context->m_table->s->fields);
  std::vector<ulong> null_bitmasks(context->m_table->s->fields);

  for (uint idx = 0; idx < context->m_table->s->fields; idx++) {
    auto fld = *(context->m_table->field + idx);
    col_offsets[idx] = fld->offset(context->m_table->record[0]);
    if (fld->is_nullable()) {
      null_byte_offsets[idx] = fld->null_offset();
      null_bitmasks[idx] = fld->null_bit;
    }
  }

  auto load_one_partition = [&](PartitionLoadThreadContext *ctx, partition_load_task_t &task,
                                ha_innopart *task_handler) -> int {  // Lambda: load a partition.
    int result{ShannonBase::SHANNON_SUCCESS};
    task.rows_loaded = 0;

#if !defined(_WIN32)  // here we
    pthread_setname_np(pthread_self(), "load_partition_wkr");
#else
    SetThreadDescription(GetCurrentThread(), L"load_partition_wkr");
#endif

    if (task_handler == nullptr) {
      task.error_msg = "Handler clone is null for partition " + std::to_string(task.part_id);
      task.result = HA_ERR_GENERIC;
      std::lock_guard<std::mutex> lock(error_mutex);
      has_error.store(true);
      return HA_ERR_GENERIC;
    }

    Rapid_load_context::extra_info_t::m_active_part_key = task.part_key;

    bool part_initialized = false;
    struct PartitionGuard {  // Scope guard using local struct - will be called at scope exit
      bool &initialized;
      ha_innopart *handler;
      uint part_id;

      ~PartitionGuard() {
        if (initialized && handler) {
          handler->rnd_end_in_part(part_id, true);
        }
      }
    } part_guard{part_initialized, task_handler, task.part_id};

    if (task_handler->inited == handler::NONE && task_handler->rnd_init_in_part(task.part_id, true)) {
      std::lock_guard<std::mutex> lock(error_mutex);
      task.error_msg = "Failed to initialize partition " + std::to_string(task.part_id);
      task.result = HA_ERR_GENERIC;
      return HA_ERR_GENERIC;
    }
    part_initialized = true;

    int tmp{HA_ERR_GENERIC};
    auto rec_buff = ctx->allocated_buffer();
    memset(rec_buff, 0, ctx->buffer_size());

    while ((tmp = task_handler->rnd_next_in_part(task.part_id, rec_buff)) != HA_ERR_END_OF_FILE) {
      if (tmp == HA_ERR_KEY_NOT_FOUND) break;

      DBUG_EXECUTE_IF("secondary_engine_rapid_part_table_load_error", {
        std::lock_guard<std::mutex> lock(error_mutex);
        task.error_msg = "Secondary engine part table loaded error";
        task.result = HA_ERR_GENERIC;
        return HA_ERR_GENERIC;
      });

      auto partition_ptr = part_tb_ptr->get_partition(task.part_key);
      if (!partition_ptr) {
        std::lock_guard<std::mutex> lock(error_mutex);
        task.error_msg = "partition not found: " + task.part_key;
        task.result = HA_ERR_GENERIC;
        return HA_ERR_GENERIC;
      }
      // parttable is shared_ptr/unique_ptr to PartTable
      if ((partition_ptr->insert_row(context, rec_buff)) == INVALID_ROW_ID) {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (!has_error.load()) {
          task.error_msg = "load data from " + context->m_schema_name + "." + context->m_table_name + "." +
                           task.part_key + " to rapid failed";
          task.result = HA_ERR_GENERIC;
          has_error.store(true);
        }
        return HA_ERR_GENERIC;
      }

      memset(rec_buff, 0, ctx->buffer_size());
      task.rows_loaded++;
      if (tmp == HA_ERR_RECORD_DELETED && !context->m_thd->killed) continue;
    }

    task.result = ShannonBase::SHANNON_SUCCESS;
    return result;
  };

  if (context->m_thd->in_multi_stmt_transaction_mode()) trans_commit_stmt(context->m_thd);

  DBUG_EXECUTE_IF("check_trx_state", {
    if (UT_LIST_GET_LEN(trx_sys->mysql_trx_list) > 0) {
      sql_print_warning("trx_sys->mysql_trx_list has %lu transactions before load",
                        UT_LIST_GET_LEN(trx_sys->mysql_trx_list));
    }
  });

  std::atomic_size_t task_idx{0};
  std::vector<std::thread> workers_pool;  // thread pool.
  auto worker_func = [&]() {
    PartitionLoadThreadContext ctx;
    ctx.allocate_buffer(context->m_table->s->rec_buff_length);

    std::unique_ptr<PartitionLoadHandlerLock> handler_lock{nullptr};
    if (ctx.initialize(context) || ctx.clone_handler(file, context, clone_mutex)) {
      has_error.store(true);
      ctx.set_error();
      return;
    }
    handler_lock = std::make_unique<PartitionLoadHandlerLock>(ctx.handler(), ctx.thd(), F_RDLCK);

    while (true) {
      size_t current_task = task_idx.fetch_add(1);
      if (current_task >= tasks.size() || has_error.load()) break;

      auto result = load_one_partition(&ctx, tasks[current_task], ctx.handler());
      if (result != ShannonBase::SHANNON_SUCCESS) {
        has_error.store(true);
        ctx.set_error();
        break;
      }
      total_rows.fetch_add(tasks[current_task].rows_loaded);
    }

    handler_lock.reset();    // Release handler lock first
    ctx.end_transactions();  // Then end transactions
  };

  for (unsigned int i = 0; i < num_threads; ++i) workers_pool.emplace_back(worker_func);

  for (auto &worker : workers_pool) {
    if (worker.joinable()) worker.join();
  }

  if (has_error.load()) {
    for (const auto &task : tasks) {
      if (task.result == ShannonBase::SHANNON_SUCCESS) continue;
      task.error_msg.size()
          ? my_error(ER_SECONDARY_ENGINE, MYF(0), task.error_msg.c_str())
          : my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name.c_str(), context->m_table_name.c_str());
      return HA_ERR_GENERIC;
    }
  }

  context->m_thd->set_sent_row_count(total_rows.load());
  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::load_table(const Rapid_load_context *context, const TABLE *source) {
  if (create_table_memo(context, source)) {
    std::string errmsg;
    cleanup(context->m_table_id);

    errmsg.append("create table memo for ")
        .append(source->s->db.str)
        .append(".")
        .append(source->s->table_name.str)
        .append(" failed.");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return HA_ERR_GENERIC;
  }

  // if the rec count is more than threshold and has primary key, it can be use parallel load, otherwise not.
  auto parall_scan = (dynamic_cast<ha_innobase *>(source->file)->stats.records > ShannonBase::rpd_para_load_threshold &&
                      !context->m_table->s->is_missing_primary_key())
                         ? true
                         : false;
  return !parall_scan ? load_innodb(context, dynamic_cast<ha_innobase *>(source->file))
                      : load_innodb_parallel(context, dynamic_cast<ha_innobase *>(source->file));
}

int Imcs::load_parttable(const Rapid_load_context *context, const TABLE *source) {
  auto table_id = context->m_table_id;
  if (create_parttable_memo(context, source)) {
    std::string errmsg;
    cleanup(table_id);
    errmsg.append("create table memo for ")
        .append(context->m_schema_name)
        .append(".")
        .append(context->m_table_name)
        .append(" failed.");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return HA_ERR_GENERIC;
  }

  ut_a(m_rpd_parttables.find(table_id) != m_rpd_parttables.end());

  auto ret{ShannonBase::SHANNON_SUCCESS};
  auto parall_scan =
      (context->m_extra_info.m_partition_infos.size() > ShannonBase::rpd_para_parttb_load_threshold) ? true : false;
  ret = parall_scan ? load_innodbpart_parallel(context, dynamic_cast<ha_innopart *>(source->file))
                    : load_innodbpart(context, dynamic_cast<ha_innopart *>(source->file));
  if (ret) {
    std::string errmsg;
    cleanup(table_id);
    errmsg.append("load data from")
        .append(context->m_schema_name)
        .append(".")
        .append(context->m_table_name)
        .append(" failed.");
    my_error(ER_SECONDARY_ENGINE, MYF(0), errmsg.c_str());
    return HA_ERR_GENERIC;
  }
  return ret;
}

int Imcs::unload_innodb(const Rapid_load_context *context, const table_id_t &table_id, bool error_if_not_loaded) {
  if (m_rpd_tables.find(table_id) == m_rpd_tables.end() && error_if_not_loaded) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name, context->m_table_name);
    return HA_ERR_GENERIC;
  }

  std::unique_lock lock(m_table_mutex);
  m_rpd_tables.erase(table_id);

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::unload_innodbpart(const Rapid_load_context *context, const table_id_t &table_id, bool error_if_not_loaded) {
  if (m_rpd_parttables.find(table_id) == m_rpd_parttables.end() && error_if_not_loaded) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), context->m_schema_name, context->m_table_name);
    return HA_ERR_GENERIC;
  }

  std::unique_lock lock(m_table_mutex);
  m_rpd_parttables.erase(table_id);

  return ShannonBase::SHANNON_SUCCESS;
}

int Imcs::unload_table(const Rapid_load_context *context, const char *db_name, const char *table_name,
                       bool error_if_not_loaded, bool is_partition) {
  /** the key format: "db_name:table_name:field_name", all the ghost columns also should be
   *  removed*/
  RapidShare *share = shannon_loaded_tables->get(db_name, table_name);
  if (error_if_not_loaded && !share) {
    std::string err(db_name);
    err.append(".").append(table_name).append(" table is not loaded into rapid yet");
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
    return HA_ERR_GENERIC;
  }

  auto table_id = context->m_table_id;
  int ret{ShannonBase::SHANNON_SUCCESS};
  if (is_partition)
    unload_innodbpart(context, table_id, error_if_not_loaded);
  else
    unload_innodb(context, table_id, error_if_not_loaded);
  return ret;
}

int Imcs::unload_table(const Rapid_load_context *context, const table_id_t &table_id, bool error_if_not_loaded,
                       bool is_partition) {
  /** the key format: "db_name:table_name:field_name", all the ghost columns also should be
   *  removed*/
  int ret{ShannonBase::SHANNON_SUCCESS};
  if (is_partition)
    unload_innodbpart(context, table_id, error_if_not_loaded);
  else
    unload_innodb(context, table_id, error_if_not_loaded);
  return ret;
}
}  // namespace Imcs
}  // namespace ShannonBase