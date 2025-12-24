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

/*********************************************************************************************************************
 *
 * The current system employs two synchronization mechanisms to propagate data changes from InnoDB to the Rapid engine:
 *
 * (1) Real-time synchronization via Notification Hooks.
 * After a user executes a DML operation (such as INSERT, UPDATE, or DELETE), the system triggers registered
 * Notification Hooks to notify each storage engine to perform corresponding actions. For example:
 *
 *     shannon_rapid_hton->notify_create_table = NotifyCreateTable;
 *     shannon_rapid_hton->notify_after_insert = NotifyAfterInsert;
 *     shannon_rapid_hton->notify_after_update = NotifyAfterUpdate;
 *     shannon_rapid_hton->notify_after_delete = NotifyAfterDelete;
 *
 *
 * This approach does not require redo log parsing. Instead, it analyzes records directly through COPY_INFO, allowing it
 * to capture DML behavior instantly and synchronize changes to the Rapid engine efficiently. It provides excellent
 * real-time performance with relatively simple implementation.
 *
 * (2) Asynchronous synchronization via Redo Logs.(LogParser)
 * After a user transaction is committed, the system writes Mini Transaction Records (MTRs) into the buffer and then
 * parses the redo logs to apply corresponding row-store changes to the Rapid engine. Although more complex, this
 * approach offers higher extensibility and serves as the foundation for implementing a storage-compute separation
 * architecture, similar to the core design of AWS Aurora’s cloud-native engine.
 *
 * Considering both performance and future evolution, the system uses the Notification Hook mechanism as the default
 * synchronization path, while retaining the redo log–based mechanism to support future ShannonBase advancements toward
 * a cloud-native and distributed architecture.
 *
 *     ┌─────────────────────────────────────────────────────────┐
 *     │                    MySQL/InnoDB                         │
 *     │  ┌──────────┐      ┌──────────┐      ┌──────────┐       │
 *     │  │  INSERT  │      │  UPDATE  │      │  DELETE  │       │
 *     │  └────┬─────┘      └────┬─────┘      └────┬─────┘       │
 *     │       │                 │                  │            │
 *     │       ├─────────────────┼──────────────────┤            │
 *     │       │ (Hook Point)    │                  │            │
 *     │       ▼                 ▼                  ▼            │
 *     │ ....................................................... |
 *     | . ┌──────────────────────┐            ┌────────────┐  . │
 *     │ . │ xxx_hton->notify_xxx │            |  redo log  |  . │
 *     │ . └─────────────────┬────┘            └─────|──────┘  . │
 *     | ....................................................... |
 *     └──────────────┼───────────────────────|──────────────────┘
 *                    │                       |
 *          ┌─────────┴───────────────────────┴──────────┐
 *     way1: directly │                       │ way2: Redo Log
 *        (realtime)  │                       │  （decoupled ）
 *       low lentency ▼                       ▼    architect
 *          ┌─────────────┐               ┌─────────────┐
 *          │DirectCapture│               │   RedoLog   │
 *          │   Handler   │               │   Handler   │
 *          └─────┬───────┘               └──────┬──────┘
 *                │                              │
 *                └───────────────┬──────────────┘
 *                                ▼
 *                      ┌────────────────┐
 *                      │ UnifiedChange  │
 *                      │    Buffer      │
 *                      └────────┬───────┘
 *                               ▼
 *                      ┌────────────────┐
 *                      │   Processing   │
 *                      │     Thread     │
 *                      └────────┬───────┘
 *                               ▼
 *                      ┌────────────────┐
 *                      │  Rapid Engine  │
 *                      │     (IMCS)     │
 *                      └────────────────┘
 *
 *******************************************************************************************************************/
#ifndef __SHANNONBASE_LOG_COMMONS_H__
#define __SHANNONBASE_LOG_COMMONS_H__

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <shared_mutex>

#include "my_inttypes.h"
#include "storage/rapid_engine/include/rapid_const.h"  // SHANNON_ALIGNAS
#include "storage/rapid_engine/populate/log_buffer.h"

namespace ShannonBase {
namespace Populate {
inline const char *sync_mode_names[] = {"DIRECT_NOTIFICATION", "REDO_LOG_PARSE", "HYBRID", nullptr};
enum class SyncMode : uint {
  DIRECT_NOTIFICATION = 0,  // notification-based
  REDO_LOG_PARSE,           // redo-log based
  HYBRID                    // hybrid mode.
};

// to identify synchonization mode.
extern std::atomic<SyncMode> g_sync_mode;

// running flag of pop thread.
extern std::atomic<bool> sys_pop_started;

enum class Source : uint8 {
  UN_KNOWN = 0,
  REDO_LOG, /** come from redo log, mtr records */
  COPY_INFO /** come from direct capture in sql COPY_INFO */
};

// it's an iterterface struct to store all the info of changed data. such as where it comes from
// data and its length, etc.
typedef struct SHANNON_ALIGNAS change_record_buff_t {
  using off_page_data_t =
      std::map<size_t, std::pair<size_t, std::shared_ptr<uchar[]>>>;  //<field_id, <length, off_page_data>>
  Source m_source;                                                    // data source
  size_t m_size;
  enum class OperType : uint8 { UNSET = 0, INSERT, DELETE, UPDATE } m_oper{OperType::UNSET};  // oper type
  table_id_t m_table_id{0};
#ifndef NDEBUG
  std::string m_schema_name, m_table_name;
#endif
  std::shared_ptr<uchar[]> m_buff0{nullptr};  // rep: record[0]
  off_page_data_t m_offpage_data0;
  std::shared_ptr<uchar[]> m_buff1{nullptr};  // rep: record[1]
  off_page_data_t m_offpage_data1;            // using to store offpage data.

  change_record_buff_t(Source sc, size_t s)
      : m_source(sc),
        m_size(s),
        m_buff0(s > 0 ? std::shared_ptr<uchar[]>(new uchar[s]) : nullptr),
        m_buff1(s > 0 ? std::shared_ptr<uchar[]>(new uchar[s]) : nullptr) {}

  change_record_buff_t() : m_source(Source::UN_KNOWN), m_size(0), m_oper(OperType::UNSET) {}

  change_record_buff_t(const change_record_buff_t &other) = default;
  change_record_buff_t &operator=(const change_record_buff_t &other) = default;
  change_record_buff_t(change_record_buff_t &&other) noexcept = default;
  change_record_buff_t &operator=(change_record_buff_t &&other) noexcept = default;
  ~change_record_buff_t() = default;

  inline uchar *get_buff0() const { return m_buff0.get(); }
  inline uchar *get_buff1() const { return m_buff1.get(); }

  inline bool has_buff0() const { return m_buff0 != nullptr; }
  inline bool has_buff1() const { return m_buff1 != nullptr; }
  inline long use_count_buff0() const { return m_buff0.use_count(); }
  inline long use_count_buff1() const { return m_buff1.use_count(); }
} change_record_buff;

struct SHANNON_ALIGNAS change_candidate_t {
  uint64_t lsn{0};
  change_record_buff_t record;

  change_candidate_t() = default;
  change_candidate_t(uint64_t l, change_record_buff_t &&change_rec) : lsn(l), record(std::move(change_rec)) {}

  change_candidate_t(const change_candidate_t &) = default;
  change_candidate_t &operator=(const change_candidate_t &) = default;
  change_candidate_t(change_candidate_t &&) = default;
  change_candidate_t &operator=(change_candidate_t &&) = default;
};

typedef struct SHANNON_ALIGNAS table_pop_buffer_t {
  Ringbuffer<change_candidate_t> change_candiates;  // the change candidates.
  std::atomic<bool> queried{false};                 // this table is required by some queries.
  std::atomic<bool> pending_flush{false};           // mark it needs to be flushed
  std::atomic<size_t> data_size{0};
} table_pop_buffer;

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
   * To stop propagation oper for sche table
   */
  static void unload(const table_id_t &table_id);

  /**
   * Send the log buffer to system pop buffer via any type of connection.
   * Such as file handler or socket handler, ect.
   */
  static uint write(FILE *file, uint64_t start_lsn, change_record_buff *changed_rec);

  /**
   * To print thread infos.
   */
  static void print_info(FILE *file);

  /**
   * To test whether the table is loaded or not.
   */
  static inline bool is_loaded_table(std::string sch_name, std::string table_name) {
    return get_impl()->is_loaded_table_impl(sch_name, table_name);
  }

  /**
   * To mark the specific table are still do populating required by quires. which is mark table queried.
   * tabel_name format: `schema_name:table_name`
   */
  static inline bool mark_table_required(const table_id_t &table_id) {
    return get_impl()->mark_table_required_impl(table_id);
  }

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
     * To stop propagation oper for sche table
     */
    virtual void unload_impl(const table_id_t &table_id) = 0;

    /**
     * To stop log pop main thread.
     */
    virtual void end_impl() = 0;

    /**
     * Send the log buffer to system pop buffer via any type of connection.
     * Such as file handler or socket handler, ect.
     */
    virtual uint write_impl(FILE *file, uint64_t start_lsn, change_record_buff *changed_rec) = 0;

    /**
     * To print thread infos.
     */
    virtual void print_info_impl(FILE *file) = 0;

    /**
     * To test table is loaded or not.
     */
    virtual bool is_loaded_table_impl(std::string sch_name, std::string table_name) = 0;
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
    virtual bool mark_table_required_impl(const table_id_t &table_id) = 0;
  };

  // Get implementation instance
  static std::unique_ptr<Impl> &get_impl();

  // Prevent instantiation
  Populator() = delete;
  ~Populator() = delete;

 private:
  static std::unique_ptr<Populator::Impl> m_impl;
};

}  // namespace Populate
}  // namespace ShannonBase
#endif  //__SHANNONBASE_LOG_COMMONS_H__