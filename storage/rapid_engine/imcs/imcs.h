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

   The fundmental code for imcs. IMCS - in memory column store.

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.
*/

#ifndef __SHANNONBASE_IMCS_H__
#define __SHANNONBASE_IMCS_H__

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>

#include "my_inttypes.h"
#include "sql/field.h"
#include "sql/handler.h"

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"
#include "storage/rapid_engine/utils/concurrent.h"

class ha_innobase;
class ha_innopart;
namespace ShannonBase {
class Rapid_load_context;
namespace Imcs {
class DataTable;
/** An IMCS is consist of CUs. Some chunks are made of a CU. Header and body is
 * two parts of a CU. Header has meta information about this CU, and the body
 * has the read data. All chunks stored consecutively in a CU.  key format of a
 * CU is listed as following: `db_name_str` + ":" + `table_name_str` + ":" +
 * `field_index`. */
class Imcs : public MemoryObject {
 public:
  // make ctor and dctor private.
  Imcs() {}
  virtual ~Imcs() {}

  int initialize();
  int deinitialize();
  // gets initialized flag.
  inline bool initialized() { return m_inited; }

  inline static boost::asio::thread_pool *pool() { return m_imcs_pool.get(); }
  inline static Imcs *instance() {
    std::call_once(one, [&] { m_instance = new Imcs(); });
    return m_instance;
  }

  // get cu at Nth index key
  // Cu *at(std::string_view schema, std::string_view table, size_t indexx);

  /**create all cus needed by source table, and ready to write the data into.*/
  int create_table_memo(const Rapid_load_context *context, const TABLE *source);

  /**create all cus needed by source table, and ready to write the data into.*/
  int create_parttable_memo(const Rapid_load_context *context, const TABLE *source);

  /** load the current table rows data into imcs. the caller's responsible
   for moving to next row */
  int load_table(const Rapid_load_context *context, const TABLE *source);

  /** load the current partition table rows data into imcs. the caller's responsible
   for moving to next row */
  int load_parttable(const Rapid_load_context *context, const TABLE *source);

  // unload the table rows data from imcs.
  int unload_table(const Rapid_load_context *context, const char *db_name, const char *table_name,
                   bool error_if_not_loaded);

  // insert a row into IMCS, where located at 'rowid'.
  int insert_row(const Rapid_load_context *context, row_id_t rowid, uchar *buf);

  // insert a row into IMCS to specific address from log_parser thread.
  int write_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                         std::unordered_map<std::string, mysql_field_t> &fields);

  // delete a row in IMCS by using its rowid.
  int delete_row(const Rapid_load_context *context, row_id_t rowid);

  // delete a row in IMCS by using its rowid. if vector is empty that means delete all rows.
  int delete_rows(const Rapid_load_context *context, const std::vector<row_id_t> &rowids);

  // update a cu in IMCS by using its rowid.
  int update_row(const Rapid_load_context *context, row_id_t rowid, std::string &field_key, const uchar *new_field_data,
                 size_t nlen);

  /** row_id[in], which row will be updated.
   *  upd_recs[in], new values of updating row at row_id.
   */
  int update_row_from_log(const Rapid_load_context *context, row_id_t rowid,
                          std::unordered_map<std::string, mysql_field_t> &upd_recs);

  int build_indexes_from_keys(const Rapid_load_context *context, std::map<std::string, key_info_t> &keys,
                              row_id_t rowid);

  int build_indexes_from_log(const Rapid_load_context *context, std::map<std::string, mysql_field_t> &field_values,
                             row_id_t rowid);

  int rollback_changes_by_trxid(Transaction::ID trxid);

  void cleanup(std::string &sch_name, std::string &table_name);

  inline row_id_t reserve_row_id(std::string &sch_table) {
    if (m_tables.size() == 0 || m_tables.find(sch_table) == m_tables.end()) return INVALID_ROW_ID;
    return m_tables[sch_table]->reserve_id(nullptr);
  }

  inline RapidTable *get_table(std::string &sch_table) {
    std::shared_lock lk(m_table_mutex);
    if (m_tables.find(sch_table) == m_tables.end())
      return nullptr;
    else
      return m_tables[sch_table].get();
  }

  inline std::unordered_map<std::string, std::unique_ptr<RapidTable>> &get_tables() {
    std::shared_lock lk(m_table_mutex);
    return m_tables;
  }

  inline RapidTable *get_parttable(std::string &sch_table) {
    std::shared_lock lk(m_table_mutex);
    if (m_parttables.find(sch_table) == m_parttables.end())
      return nullptr;
    else
      return m_parttables[sch_table].get();
  }

 private:
  Imcs(Imcs &&) = delete;
  Imcs(Imcs &) = delete;
  Imcs &operator=(const Imcs &) = delete;
  Imcs &operator=(const Imcs &&) = delete;

  int load_innodb(const Rapid_load_context *context, ha_innobase *file);
  int load_innodb_parallel(const Rapid_load_context *context, ha_innobase *file);
  int load_innodbpart(const Rapid_load_context *context, ha_innopart *file);
  int load_innodbpart_parallel(const Rapid_load_context *context, ha_innopart *file);

  int unload_innodb(const Rapid_load_context *context, const char *db_name, const char *table_name,
                    bool error_if_not_loaded);

  int unload_innodbpart(const Rapid_load_context *context, const char *db_name, const char *table_name,
                        bool error_if_not_loaded);

 private:
  // Thread context for parallel scanning operations
  typedef struct {
    // if you dont use this, remove the boost_thread and boost_system libs in cmake file.
    // thread id
    std::thread::id tid;

    // this thread work whether done or not.
    std::atomic<bool> scan_done{false};

    // # of rows scan in this thread.
    std::atomic<size_t> n_rows{0};

    //# of column and row len of a row in this thread work.
    std::atomic<ulong> n_cols{0}, row_len{0};
    std::vector<ulong> col_offsets;
    std::vector<ulong> null_byte_offsets;
    std::vector<ulong> null_bitmasks;
  } parall_scan_cookie_t;

  // a partition loading task with results
  typedef struct PartitionLoadTask {
    uint part_id;
    std::string part_key;
    int result;
    uint64_t rows_loaded;
    std::string error_msg;
    PartitionLoadTask() : part_id(0), result(0), rows_loaded(0) {}
  } partition_load_task_t;

  // imcs instance
  static Imcs *m_instance;

  // initialization flag, only once.
  static std::once_flag one;

  // initialization flag.
  std::atomic<uint8> m_inited{0};

  static std::unique_ptr<boost::asio::thread_pool> m_imcs_pool;

  std::shared_mutex m_table_mutex;
  // loaded tables. key format: schema_name + ":" + table_name.
  std::unordered_map<std::string, std::unique_ptr<RapidTable>> m_tables;

  // loaded partitioned tables. key format: schema_name + ":" + table_name.
  std::unordered_map<std::string, std::unique_ptr<RapidTable>> m_parttables;

  // the current version of imcs.
  uint m_version{SHANNON_RPD_VERSION};

  const char *m_magic = "SHANNON_MAGIC_IMCS";
};

/**
 * Multi-threaded Partition Loading Design
 * ========================================
 *
 * Overview:
 * This implementation enables parallel loading of partitioned InnoDB tables by creating
 * independent thread contexts that share metadata while maintaining separate cursors.
 *
 * Architecture:
 *
 * Main Thread (Secondary Load Context)
 * ┌─────────────────────────────────────────────────────────────┐
 * │ context->m_table                                            │
 * │   ├─ s: TABLE_SHARE (metadata) ← Shared by all threads     │
 * │   ├─ file: ha_innopart (opened) ← Source for cloning       │
 * │   └─ read_set/write_set ← Copied to worker threads         │
 * └─────────────────────────────────────────────────────────────┘
 *                           │
 *                           │ Spawn worker threads
 *                           ↓
 * ┌─────────────────────────────────────────────────────────────┐
 * │ Worker Thread 1              Worker Thread 2       ...      │
 * ├─────────────────────────────────────────────────────────────┤
 * │ PartitionLoadThreadContext   PartitionLoadThreadContext     │
 * │ ├─ m_thd (independent)       ├─ m_thd (independent)         │
 * │ ├─ m_table (independent)     ├─ m_table (independent)       │
 * │ │  ├─ s → TABLE_SHARE        │  ├─ s → TABLE_SHARE         │
 * │ │  └─ file: m_handler        │  └─ file: m_handler         │
 * │ └─ m_handler (cloned)        └─ m_handler (cloned)         │
 * │    └─ Partitions 0,3,6...       └─ Partitions 1,4,7...     │
 * └─────────────────────────────────────────────────────────────┘
 *                           │
 *                           │ Parallel partition scanning
 *                           ↓
 * ┌─────────────────────────────────────────────────────────────┐
 * │ InnoDB Storage Engine                                       │
 * │ Partition 0  Partition 1  Partition 2  Partition 3  ...    │
 * └─────────────────────────────────────────────────────────────┘
 *
 * Key Design Principles:
 *
 * 1. Shared Metadata (TABLE_SHARE):
 *    - All worker threads share the same TABLE_SHARE from the main thread
 *    - This saves memory and ensures consistent schema information
 *    - Shared via open_table_from_share() with SKIP_NEW_HANDLER flag
 *
 * 2. Independent Thread Context (THD):
 *    - Each worker creates its own THD for thread-local storage
 *    - Enables independent transaction contexts and memory management
 *    - Each THD has its own mem_root for automatic memory cleanup
 *
 * 3. Independent TABLE Objects:
 *    - Each worker has its own TABLE struct for cursor state
 *    - TABLE objects share the TABLE_SHARE but have independent:
 *      * read_set/write_set bitmaps
 *      * handler pointers
 *      * buffer spaces
 *
 * 4. Cloned Handlers (ha_innopart):
 *    - Each worker clones the main handler using handler::clone()
 *    - Cloned handlers inherit the opened state from the source handler
 *    - No explicit ha_open() needed because:
 *      a) ha_innopart::clone() inherits partition handlers' open state
 *      b) change_table_ptr() updates internal pointers to new TABLE
 *      c) Partition-level operations (rnd_*_in_part) work directly
 *    - Each cloned handler maintains independent cursors for concurrent reads
 *
 * 5. Resource Management:
 *    - Handler and TABLE memory allocated on THD's mem_root
 *    - closefrm() automatically closes handlers and frees TABLE resources
 *    - Deleting THD releases all mem_root allocations (handlers, TABLE, etc.)
 *    - No manual delete/free needed for mem_root-allocated objects
 *
 * Thread Safety:
 * - clone_mutex: Protects handler cloning (some internal states may not be thread-safe)
 * - Each thread reads from different partitions to avoid contention
 * - Shared TABLE_SHARE is read-only after initialization
 *
 * Workflow:
 * 1. Main thread initializes context with opened table
 * 2. Each worker thread:
 *    a) Creates independent THD
 *    b) Creates TABLE sharing the main TABLE_SHARE (SKIP_NEW_HANDLER)
 *    c) Clones handler from main thread's handler
 *    d) Associates cloned handler with new TABLE via change_table_ptr()
 * 3. Workers process assigned partitions in parallel
 * 4. Cleanup: closefrm() + delete THD handles all resource release
 */

// Manages THD lifecycle, table opening, and handler cloning.
// Ensures THD globals are restored properly on destruction, preventing
// thread-local state corruption in multi-threaded parallel loading
class PartitionLoadThreadContext {
 public:
  PartitionLoadThreadContext() : m_thd(nullptr), m_handler(nullptr), m_table(nullptr) { my_thread_init(); }

  ~PartitionLoadThreadContext() {
    cleanup();
    my_thread_end();
  }

  /**
   * Initializes the thread context for parallel partition loading.
   *
   * This method creates an isolated execution environment for a worker thread by:
   * 1. Creating a new THD (Thread Descriptor) with daemon privileges
   * 2. Setting up thread-specific global variables
   * 3. Creating a private TABLE instance from the shared table definition
   * 4. Configuring the table for read operations with proper column sets
   *
   * Each worker thread needs its own THD and TABLE instances to avoid conflicts
   * when accessing MySQL internals and to maintain proper isolation during
   * parallel data loading operations.
   *
   * @param context The rapid load context containing table and schema information
   * @return true if initialization failed, false if successful
   */
  bool initialize(const Rapid_load_context *context);

  /**
   * Clones the partition handler for exclusive use by this worker thread.
   *
   * This method creates a private copy of the partition handler (ha_innopart)
   * to enable concurrent data scanning from multiple partitions without
   * resource contention. Handler cloning is thread-sensitive and must be
   * protected by a mutex since the original handler's state may be modified
   * during the cloning process.
   *
   * Key benefits:
   * - Each thread gets independent handler instance avoiding lock contention
   * - Isolated cursor positions and internal states for parallel scanning
   * - Thread-safe access to underlying storage engine structures
   *
   * @param file The source partition handler to clone from
   * @param context The rapid load context with table information
   * @param clone_mutex Mutex to protect the cloning operation (thread-sensitive)
   * @return true if cloning failed, false if successful
   */
  bool clone_handler(ha_innopart *file, const Rapid_load_context *context, std::mutex &clone_mutex);

  inline THD *thd() { return m_thd; }
  inline ha_innopart *handler() { return m_handler; }
  inline TABLE *table() { return m_table; }

  // Prevent copying
  PartitionLoadThreadContext(const PartitionLoadThreadContext &) = delete;
  PartitionLoadThreadContext &operator=(const PartitionLoadThreadContext &) = delete;

  inline void set_error() { m_error.store(true); }

  int end_transactions();
  void cleanup();

 private:
  std::atomic<bool> m_error{false};
  bool m_transactions_ended{false};
  THD *m_thd;
  ha_innopart *m_handler;
  TABLE *m_table;
};

// Manages InnoDB transaction state by acquiring/releasing external locks.
// 1. ha_external_lock(F_RDLCK) starts the transaction before reading data
// 2. ha_external_lock(F_UNLCK) releases the lock on destruction
class PartitionLoadHandlerLock {
 public:
  PartitionLoadHandlerLock(handler *h, THD *thd, int lock_type) : m_handler(h), m_thd(thd), m_locked(false) {
    if (m_handler && m_handler->ha_external_lock(m_thd, lock_type) == 0) {
      m_locked = true;
    }
  }

  virtual ~PartitionLoadHandlerLock() {
    if (m_locked && m_handler) {
      m_handler->ha_external_lock(m_thd, F_UNLCK);
    }
  }

  inline bool is_locked() const { return m_locked; }

  // Prevent copying
  PartitionLoadHandlerLock(const PartitionLoadHandlerLock &) = delete;
  PartitionLoadHandlerLock &operator=(const PartitionLoadHandlerLock &) = delete;

 private:
  handler *m_handler;
  THD *m_thd;
  bool m_locked;
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_IMCS_H__