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

namespace ShannonBase {
namespace Populate {
enum class SyncMode : uint8 {
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
  // the changed records come from where.
  Source m_source;

  // the changed record buffer. IF SOURCE FROM REDO, IT ONLY USE `m_buff0`, OTHERWISE, FROM
  // COPY_INFO, THE `m_buff1`, `m_schema_name` AND `m_table_name` MAYBE USED, AND `m_oper` IS SET.
  std::unique_ptr<uchar[]> m_buff0;

  // size of the records;
  size_t m_size;

  // oper type is used for which log comes from COPY_INFO, in REDO_LOG we dont set the oper type.
  // is set in redo log parsing stage.
  enum class OperType : uint8 { UNSET = 0, INSERT, DELETE, UPDATE } m_oper{OperType::UNSET};
  std::string m_schema_name, m_table_name;
  std::unique_ptr<uchar[]> m_buff1;

  change_record_buff_t(Source sc, size_t s)
      : m_source(sc), m_buff0(std::make_unique<uchar[]>(s)), m_size(s), m_buff1(std::make_unique<uchar[]>(s)) {}

  change_record_buff_t(change_record_buff_t &&other) noexcept
      : m_source(other.m_source),
        m_buff0(std::move(other.m_buff0)),
        m_size(other.m_size),
        m_schema_name(std::move(other.m_schema_name)),
        m_table_name(std::move(other.m_table_name)),
        m_buff1(std::move(other.m_buff1)) {
    other.m_size = 0;
  }

  change_record_buff_t &operator=(change_record_buff_t &&other) noexcept {
    if (this != &other) {
      m_source = other.m_source;
      m_buff0 = std::move(other.m_buff0);
      m_buff1 = std::move(other.m_buff1);
      m_size = other.m_size;
      m_schema_name = std::move(other.m_schema_name);
      m_table_name = std::move(other.m_table_name);

      other.m_source = Source::UN_KNOWN;
      other.m_size = 0;
    }
    return *this;
  }

  // Deleted copy constructor and copy assignment operator to prevent copying
  change_record_buff_t(const change_record_buff_t &) = delete;
  change_record_buff_t &operator=(const change_record_buff_t &) = delete;

  ~change_record_buff_t() {}
} change_record_buff;

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
    virtual bool check_status_impl(std::string &table_name) = 0;
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