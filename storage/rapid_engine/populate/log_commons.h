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

   The fundmental code for imcs. Interface of Log of Rapid.
*/

#ifndef __SHANNONBASE_LOG_COMMONS_H__
#define __SHANNONBASE_LOG_COMMONS_H__

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <shared_mutex>

#include "my_inttypes.h"

namespace ShannonBase {
namespace Populate {
/**
 * key, (uint64_t)lsn_t, start lsn of this mtr record. a mtr_log_rec is consisted of
 * serveral mlog records. Taking ISNERT as an instance, an insert operation is
 * leading by a MLOG_REC_INSERT mlog record, then a serials mlog records. if it's a
 * multi-record. And ending with a end type MLOG. In fact, a mtr_log_rec is a transaction
 * opers.
 * for populate the changes to rapid. we copy all DML opers mlog record into mtr_log_rec_t
 * when transaction commits. `mtr_t::Command::execute`. After that cp all mtr_log_rec_t to
 * sys_pop_buff.
 */
typedef struct mtr_log_rec_t {
  std::unique_ptr<uchar[]> data;
  size_t size;

  mtr_log_rec_t(size_t s) : data(std::make_unique<uchar[]>(s)), size(s) {}

  mtr_log_rec_t(mtr_log_rec_t &&other) noexcept : data(std::move(other.data)), size(other.size) { other.size = 0; }

  mtr_log_rec_t &operator=(mtr_log_rec_t &&other) noexcept {
    if (this != &other) {
      data = std::move(other.data);
      size = other.size;
      other.size = 0;
    }
    return *this;
  }

  // Deleted copy constructor and copy assignment operator to prevent copying
  mtr_log_rec_t(const mtr_log_rec_t &) = delete;
  mtr_log_rec_t &operator=(const mtr_log_rec_t &) = delete;

  ~mtr_log_rec_t() {}
} mtr_log_rec;

extern std::unordered_map<uint64_t, mtr_log_rec> sys_pop_buff;
extern std::shared_mutex g_processing_table_mutex;
extern std::set<std::string> g_processing_tables;
// pop change buffer size.
extern std::atomic<uint64> sys_pop_data_sz;
// flag of pop change thread. true is running, set to false to stop
extern std::atomic<bool> sys_pop_started;

class Populator {
 public:
  /**
   * Whether the log pop main thread is active or not. true is alive, false dead.
   */
  static bool active();

  /**
   * To launch log pop main thread.
   */
  static void start();

  /**
   * To stop log pop main thread.
   */
  static void end();

  /**
   * To print thread infos.
   */
  static void print_info(FILE *file);

  /**
   * To check whether the specific table are still do populating.
   * true is in pop queue, otherwise return false; tabel_name format: `schema_name/table_name`
   */
  static inline bool check_status(std::string &table_name) { return get_impl()->check_status_impl(table_name); }

  /**
   * To send notify to populator main thread to start do propagation.
   */
  static void send_notify();

  /**
   * Preload mysql.indexes into caches.
   */
  static int load_indexes_caches();

 public:
  // Internal implementation interface
  class Impl {
   public:
    virtual ~Impl() = default;

    /**
     * Whether the log pop main thread is active or not. true is alive, false dead.
     */
    virtual bool active_impl() = 0;

    /**
     * To launch log pop main thread.
     */
    virtual void start_impl() = 0;

    /**
     * To stop log pop main thread.
     */
    virtual void end_impl() = 0;

    /**
     * To print thread infos.
     */
    virtual void print_info_impl(FILE *file) = 0;

    /**
     * To send notify to populator main thread to start do propagation.
     */
    virtual void send_notify_impl() = 0;

    /**
     * Preload mysql.indexes into caches.
     */
    virtual int load_indexes_caches_impl() = 0;

    /**
     * To check whether the specific table are still do populating.
     * true is in pop queue, otherwise return false; tabel_name format: `schema_name/table_name`
     */
    virtual bool check_status_impl(std::string &table_name) = 0;
  };

  // Get implementation instance
  static std::unique_ptr<Impl> &get_impl();

  // Prevent instantiation
  Populator() = delete;
  ~Populator() = delete;

 private:
  static std::unique_ptr<Populator::Impl> impl_;
};
}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_LOG_COMMONS_H__