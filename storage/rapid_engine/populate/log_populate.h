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

/** Default value of spin delay (in spin rounds)
 * 1000 spin round takes 4us,  25000 takes 1ms for busy waiting. therefore, 200ms means
 * 5000000 spin rounds. for the more detail infor ref to : comment of
 * `innodb_log_writer_spin_delay`.
 */
constexpr uint64 MAX_LOG_POP_SPINS = 5000000;
constexpr uint64 MAX_WAIT_TIMEOUT = 200;

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

extern std::shared_mutex g_processing_table_mutex;
extern std::multiset<std::string> g_processing_tables;

// sys pop buffer, the changed records copied into this buffer. then propagation thread
// do the real work.
extern std::unordered_map<uint64_t, change_record_buff_t> sys_pop_buff;

// how many data was in sys_pop_buff?
extern std::atomic<uint64> sys_pop_data_sz;

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
  void end_impl() override;

  /**
   * Send the log buffer to system pop buffer via any type of connection.
   * Such as file handler or socket handler, ect.
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
   * To check whether the specific table are still do populating.
   * true is in pop queue, otherwise return false; tabel_name format: `schema_name/table_name`
   */
  bool check_status_impl(std::string &table_name) override;
};
}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_POPULATE_H__