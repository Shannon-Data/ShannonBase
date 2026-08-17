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
 * (2) Asynchronous synchronization via Redo Logs.
 * REDO capture may be enabled independently by its producer. The unified
 * propagation buffer does not carry a second global "mode" state: each
 * change_record_buff_t is self-describing through Source::COPY_INFO or
 * Source::REDO_LOG and the worker dispatches directly from that source.
 *
 * COPY_INFO is the current real-time producer. The REDO parser/dispatch entry is
 * retained so redo-based or mixed-source propagation can be connected without
 * changing the buffer ABI. If both producers feed the buffer, the resulting
 * stream is naturally hybrid; no separate HYBRID mode state is required.
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
#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "my_inttypes.h"
#include "storage/rapid_engine/include/rapid_arch_inf.h"
#include "storage/rapid_engine/include/rapid_const.h"  // SHANNON_ALIGNAS
#include "storage/rapid_engine/populate/log_buffer.h"

namespace ShannonBase {
namespace Populate {
// running flag of pop thread.
extern std::atomic<bool> shannon_propagation_thread_started;

// The source tag is the single dispatch discriminator for buffered changes.
// There is intentionally no independent global propagation-mode state.
enum class Source : uint8 {
  UN_KNOWN = 0,
  REDO_LOG, /** serialized InnoDB redo/mtr records */
  COPY_INFO /** direct SQL/InnoDB row-image notification */
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

  // COPY_INFO transaction metadata. Row notifications are queued immediately
  // with the real primary InnoDB creator id and commit_scn == 0, which means
  // ACTIVE in Rapid MVCC. Primary COMMIT/ROLLBACK publishes only the outcome;
  // TransactionManager later finalizes the version to a Rapid commit
  // SCN or aborts it. SQL visibility always uses the primary InnoDB ReadView.
  uint64_t m_source_trx_id{0};
  uint64_t m_commit_scn{0};  // 0 => ACTIVE notification version
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
  uint64_t change_id{0};  // process-local unique id; never overload LSN as identity
  uint64_t lsn{0};
  change_record_buff_t record;

  change_candidate_t() = default;
  change_candidate_t(uint64_t id, uint64_t l, change_record_buff_t &&change_rec)
      : change_id(id), lsn(l), record(std::move(change_rec)) {}

  change_candidate_t(const change_candidate_t &) = default;
  change_candidate_t &operator=(const change_candidate_t &) = default;
  change_candidate_t(change_candidate_t &&) = default;
  change_candidate_t &operator=(change_candidate_t &&) = default;
};

enum class TablePropagationState : uint8_t { READY = 0, PENDING, BROKEN };

enum class TablePropagationWaitResult : uint8_t { APPLIED = 0, PENDING, BROKEN, GONE };

/**
 * Query-side physical propagation barrier.
 *
 * required_change_id is captured only after the primary InnoDB ReadView has
 * been acquired.  A Rapid scan may start once every table has physically
 * resolved through that watermark; later DML is intentionally not chased.
 */
struct PropagationBarrier {
  TablePropagationState state{TablePropagationState::READY};
  uint64_t required_change_id{0};
  uint64_t applied_change_id{0};

  bool needs_wait() const noexcept { return state == TablePropagationState::PENDING; }
};

typedef struct SHANNON_ALIGNAS table_pop_buffer_t {
  Ringbuffer<change_candidate_t> change_candiates;  // the change candidates.

  // Multiple client THDs may notify the same table concurrently. This short
  // queue-transfer lock serializes producer ring insertion/watermark publication
  // with coordinator ring removal/accounting, making table-local successful
  // insertion order identical to change_id order. It never covers Rapid apply.
  std::mutex enqueue_mutex;

  std::atomic<bool> queried{false};        // legacy query hint; retained for compatibility
  std::atomic<bool> pending_flush{false};  // coordinator should flush this table now
  std::atomic<bool> broken{false};         // propagation failed; reload before offload
  std::atomic<bool> detached{false};       // buffer was unloaded/replaced/shut down
  std::atomic<size_t> data_size{0};        // bytes still resident in the ring
  std::atomic<size_t> inflight_size{0};    // bytes handed to / retained by the worker
  std::atomic<uint32_t> publish_fence{0};  // explicit bulk/rebuild publication fence

  // The query barrier is based on change identity, not queue emptiness.  A
  // query waits only for the watermark it captured after acquiring ReadView,
  // so continuous OLTP traffic cannot starve a Rapid query.
  std::atomic<uint64_t> enqueued_change_id{0};
  std::atomic<uint64_t> applied_change_id{0};
  mutable std::mutex barrier_mutex;
  std::condition_variable barrier_cv;

  bool has_pending() const noexcept {
    return broken.load(std::memory_order_acquire) || publish_fence.load(std::memory_order_acquire) > 0 ||
           applied_change_id.load(std::memory_order_acquire) < enqueued_change_id.load(std::memory_order_acquire) ||
           data_size.load(std::memory_order_acquire) > 0 || inflight_size.load(std::memory_order_acquire) > 0;
  }
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
  static void shutdown();

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
   * Mark a table as query-required and capture the currently enqueued physical
   * change watermark. PENDING is normal asynchronous propagation, not an
   * offload failure. The caller may wait for exactly required_change_id.
   */
  static inline PropagationBarrier request_table_barrier(const table_id_t &table_id) {
    return get_impl()->request_table_barrier_impl(table_id);
  }

  /**
   * Event-driven timed wait used by the query execution boundary. A short
   * timeout lets the caller observe THD kill/shutdown without busy polling.
   */
  static inline TablePropagationWaitResult wait_table_applied_for(const table_id_t &table_id,
                                                                  uint64_t required_change_id, uint64_t wait_ms) {
    return get_impl()->wait_table_applied_for_impl(table_id, required_change_id, wait_ms);
  }

  /**
   * Compatibility wrapper for old callers. New query code must use the barrier
   * API so normal PENDING state is not confused with BROKEN.
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

    virtual PropagationBarrier request_table_barrier_impl(const table_id_t &table_id) = 0;
    virtual TablePropagationWaitResult wait_table_applied_for_impl(const table_id_t &table_id,
                                                                   uint64_t required_change_id, uint64_t wait_ms) = 0;

    /** Compatibility interface retained for callers not yet migrated. */
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