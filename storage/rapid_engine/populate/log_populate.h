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

   The fundmental code for imcs. The chunk is used to store the data which
   transfer from row-based format to column-based format.
*/

#ifndef __SHANNONBASE_POPULATE_H__
#define __SHANNONBASE_POPULATE_H__
#include <memory>
#include <set>
#include <shared_mutex>
#include <unordered_map>

#include "storage/innobase/include/log0test.h"
#include "storage/innobase/include/mtr0types.h"
#include "storage/innobase/include/rem0types.h"
#include "storage/innobase/include/trx0types.h"

#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/populate/log_commons.h"

class IB_thread;
class dict_index_t;
namespace ShannonBase {
namespace LogSerivce {
class buf_block_t;

// used for aurora-style compuate-storage disaggregated architecture.
class RedoLogService {
 public:
  // send redo log to the remote storage node.
  void send_redo_to_storage_nodes(lsn_t lsn, const uchar *log_data, size_t size);

  // read the data page from storage node.
  buf_block_t *fetch_page_from_storage(space_id_t space, page_no_t page_no);
};
}  // namespace LogSerivce

namespace Populate {

#define log_rapid_pop_mutex_enter(log) mutex_enter(&((log).rapid_populator_mutex))

#define log_rapid_pop_mutex_enter_nowait(log) mutex_enter_nowait(&((log).rapid_populator_mutex))

#define log_rapid_pop_mutex_exit(log) mutex_exit(&((log).rapid_populator_mutex))

#define log_rapid_pop_mutex_own(log) (mutex_own(&((log).rapid_populator_mutex)) || !Populator::log_rapid_is_active())

/** a buffer to store redo log records, then parses these records. if we
use std::map, that will occure corruption at rb tree reblance. if we impl
copy cotr or assingment cotr, it's still corruption, due to access
empty address. Therefore, here, we use hash map to store it, and other reason:
the RB tree will do re-blancing when the size of items exceed a threshold,
that's performance issue. in future, we will use co-rountine to process every
item by a co-routine to promot the performance.
*/

constexpr uint64 POP_MAX_WAIT_TIMEOUT = 200;         // main worker, timeout time in ms.
constexpr uint64 TABLE_WORKER_IDLE_TIMEOUT = 30000;  // if 30s no incoming new data,the exit the table-level workers.
constexpr uint16_t BATCH_PROCESS_NUM = 256;
/**
 * key, (uint64_t)lsn_t, start lsn of this mtr record. a change_record_buff_t is consisted of
 * serveral mlog records. Taking ISNERT as an instance, an insert operation is
 * leading by a MLOG_REC_INSERT mlog record, then a serials mlog records. if it's a
 * multi-record. And ending with a end type MLOG. In fact, a mtr_log_rec is a transaction
 * opers.
 * for populate the changes to rapid. we copy all DML opers mlog record into change_record_buff_t
 * when transaction commits. `mtr_t::Command::execute`. After that cp all change_record_buff_t to
 * sys_pop_buff.
 */
// sys pop buffer, the changed records copied into this buffer. then propagation thread
// do the real work.
extern std::unordered_map<table_id_t, std::unique_ptr<table_pop_buffer_t>> sys_pop_buff;
extern std::shared_mutex sys_pop_buff_mutex;

// how many data was in sys_pop_buff?
extern std::atomic<uint64> sys_pop_data_sz;

extern std::shared_mutex g_propagating_table_mutex;
extern std::multiset<std::string> g_propagating_tables;

class PopulatorImpl : public Populator::Impl {
 public:
  /**
   * Whether the log pop main thread is active or not. true is alive, false dead.
   */
  bool active_impl() override;

  /**
   * To send notify to populator main thread to start do propagation.
   */
  void send_notify_impl() override;

  /**
   * To launch log pop main thread.
   */
  void start_impl() override;

  /**
   * To stop log pop main thread.
   */
  void unload_impl(const table_id_t &table_id) override;

  /**
   * To stop log pop main thread.
   */
  void end_impl() override;

  /**
   * @brief Write a parsed change record into the population buffer or a remote stream.
   *
   * This function serves as the main data ingestion entry point for the Rapid
   * log population subsystem. Depending on the input target, it either:
   *   - Appends the change record into the in-memory buffer (`sys_pop_buff`),
   *     to be consumed later by the background populator thread (`parse_log_func_main()`), or
   *   - Sends the change record to a remote sink (e.g., distributed node, log replicator)
   *     — currently marked as TODO.
   *
   * @param[in] file
   *   Target output stream. If `nullptr`, the record will be pushed into
   *   `sys_pop_buff`. Otherwise, it represents a writable file handle or
   *   network stream (future extension).
   *
   * @param[in] start_lsn
   *   The starting LSN (Log Sequence Number) corresponding to this change
   *   record. It serves as the unique key for indexing `sys_pop_buff`.
   *
   * @param[in,out] changed_rec
   *   The change record buffer, which contains serialized redo or copy-info
   *   data produced by the log parsing or change-capture layer.
   *   Ownership of the buffer is moved into the population subsystem.
   *
   * @return
   *   `ShannonBase::SHANNON_SUCCESS` (always success currently),
   *   since this function is synchronous and only performs local memory operations.
   *
   * @note
   * - When `file == nullptr`, this function transfers ownership of `changed_rec`
   *   into the global `sys_pop_buff` and increases the global memory usage counter
   *   `sys_pop_data_sz` atomically.
   * - The background thread (`rapid_log_coordinator`) periodically monitors
   *   the buffer size and launches worker threads to process and propagate
   *   these buffered changes.
   * - Thread safety is guaranteed by `rapid_populator_mutex` when the buffer
   *   is concurrently consumed and modified.
   * - When remote propagation is implemented, care must be taken to preserve
   *   LSN ordering guarantees and ensure durability.
   */
  uint write_impl(FILE *to, uint64_t start_lsn, change_record_buff *changed_rec) override;

  /**
   * Preload mysql.indexes into caches.
   */
  int load_indexes_caches_impl() override;

  /**
   * To print thread infos.
   */
  void print_info_impl(FILE *file) override;

  /**
   * To test the table is loaded or not.
   */
  bool is_loaded_table_impl(std::string sch_name, std::string table_name) override;

  /**
   * @brief Check whether a given table is currently in the population process, if
   * it was, then set to required by queries.
   *
   * This method is used to determine whether a specific table (identified by
   * its fully qualified name `"schema_name:table_name"`) is still being populated
   * by the background populator subsystem.
   * Typical use cases include:
   * - Query planning: deciding whether to route a query to ShannonBase or InnoDB.
   * - Status monitoring: reporting progress of background population tasks.
   *
   * @param sch_table_name A fully qualified table identifier
   *
   * @return `true` if the table is currently being populated, and then mark it required by qureies,
   *          `false` otherwise.
   *
   * @threadsafe Uses `std::shared_lock<std::shared_mutex>` to allow concurrent reads.
   * @note This function should not be called from within a write lock context
   *       on `g_processing_table_mutex`, as it would cause potential deadlocks.
   */
  bool mark_table_required_impl(const table_id_t &table_id) override;
};
}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_POPULATE_H__