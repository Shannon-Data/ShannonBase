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

   The fundmental code for GenAI.

   The ML_RAG routine performs retrieval-augmented generation (RAG) by:
   Taking a natural-language query.
   Retrieving context from relevant documents using semantic search.
   Generating a response that integrates information from the retrieved documents.

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_ML_RETRIEVE_SCHEMA_METADATA_H__
#define __SHANNONBASE_ML_RETRIEVE_SCHEMA_METADATA_H__

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "ml/ml_embedding.h"
#include "my_thread.h"

class THD;
class TABLE;
namespace dd {
class Column;
class Table;
}  // namespace dd

namespace ShannonBase {
namespace ML {
class ML_embedding_row;
static constexpr const char *ML_META_SCHEMA = "mysql";
static constexpr const char *ML_SCHEMA_EMBEDDINGS_TABLE = "schema_embeddings";
static constexpr const char *ML_DEFAULT_EMBED_MODEL = "all-MiniLM-L12-v2";

enum class DDLEventType { CREATE, ALTER, DROP };
enum class SerializeMode { WITH_COMMENTS, WITHOUT_COMMENTS };
enum class EmbeddingStatus : int64_t { PENDING = 0, PROCESSED = 1, ERROR = 2 };

struct DDLEvent {
  DDLEventType type;
  std::string schema_name;
  std::string table_name;
  std::string doc;
};

struct EmbedTask {
  DDLEventType type;
  std::string schema_name;
  std::string table_name;
  std::string doc;       // non-empty for CREATE/ALTER; empty means re-serialize
  std::string model_id;  // embedding model, default: all-MiniLM-L12-v2
};

struct EmbedResult {
  std::string schema_name;
  std::string table_name;
  std::vector<float> embedding;  // empty on failure
  bool success{false};
};

enum class embedding_state_t {
  EMBEDDING_STATE_INIT,
  EMBEDDING_STATE_RUN,
  EMBEDDING_STATE_STOP,
  EMBEDDING_STATE_EXIT,
  EMBEDDING_STATE_DISABLED
};

/**
 * Per-table worker context.
 *
 * One instance is created lazily for each (schema, table) pair the first time
 * a DDL event targeting that table is dispatched.  The worker thread owns a
 * dedicated THD and processes the queue sequentially, so events for the same
 * table are always serialized while events for different tables are parallel.
 */
struct TableWorkerContext {
  std::string key;              // "schema_name.table_name"
  std::deque<EmbedTask> tasks;  // guarded by mutex
  std::mutex mutex;
  std::condition_variable cv;
  my_thread_handle thread{};

  void start_job(DDLEventType type, const std::string &schema, const std::string &table, std::string doc,
                 const std::string &model_id) {
    EmbedTask task;
    task.type = type;
    task.schema_name = schema;
    task.table_name = table;
    task.doc = std::move(doc);
    task.model_id = model_id;
    {
      std::lock_guard<std::mutex> lk(this->mutex);
      this->tasks.push_back(std::move(task));
    }
    this->cv.notify_one();
  }
};

class EmbeddingManager {
 public:
  enum class SCHEMA_EMBEDDINGS_FIELD_INDEX {
    ID = 0,
    SCHEMA_NAME = 1,
    TABLE_NAME = 2,
    DOC_TEXT = 3,
    EMBEDDING = 4,
    STATUS = 5,
    UPDATED_AT = 6
  };

  static EmbeddingManager *instance() {
    std::call_once(s_once, []() { s_instance = new EmbeddingManager(); });
    return s_instance;
  }

  static void start() {
    auto *mgr = instance();
    if (mgr && EmbeddingManager::is_running()) return;
    instance()->start_impl();
  }

  static void shutdown() {
    auto *mgr = instance();
    if (mgr && !EmbeddingManager::is_running()) return;
    instance()->shutdown_impl();
  }

  static void initiate_shutdown();

  static bool wait_until_fully_stopped(std::chrono::milliseconds timeout = std::chrono::seconds(30));

  bool initialized() const { return m_initialized.load(); }

  static inline bool is_running() noexcept {
    return m_state.load(std::memory_order_acquire) == embedding_state_t::EMBEDDING_STATE_RUN;
  }

  void enqueue_ddl_event(const DDLEvent &ev);

  void recover_pending_tasks(THD *thd);

  static std::atomic<embedding_state_t> m_state;
  static std::condition_variable m_manager_cv;
  static std::mutex m_manager_mutex;

  std::atomic<uint32_t> m_ddl_pending_count{0};
  std::atomic<uint32_t> m_result_pending_count{0};

  static constexpr uint32_t MAX_DDL_QUEUE_DEPTH = 4096;
  static constexpr uint32_t MAX_WORKER_THREADS = 16;

  std::deque<std::string> m_ddl_order;                  // insertion-ordered keys
  std::unordered_map<std::string, DDLEvent> m_ddl_map;  // key -> latest event
  std::mutex m_ddl_mutex;

  std::deque<EmbedResult> m_result_queue;
  std::mutex m_result_mutex;

  /* Per-table worker pool */
  std::unordered_map<std::string, std::unique_ptr<TableWorkerContext>> m_table_workers;
  std::mutex m_table_workers_mutex;

  std::unique_ptr<ML_embedding_row> m_embedder{nullptr};
  std::mutex m_embedder_mutex;

  std::atomic<THD *> m_current_thd{nullptr};  // THD inside open_and_lock_tables

  static std::condition_variable m_fully_stopped_cv;
  static std::mutex m_fully_stopped_mutex;
  static bool m_fully_stopped;

  TableWorkerContext *get_or_create_worker(const std::string &key);
  void consume_results(THD *thd, TABLE *schema_embedding_table_ptr);
  void on_thread_exiting();

 private:
  EmbeddingManager() { m_initialized.store(true); }
  ~EmbeddingManager() { m_initialized.store(false); }
  EmbeddingManager(const EmbeddingManager &) = delete;
  EmbeddingManager &operator=(const EmbeddingManager &) = delete;

  void start_impl();
  void initiate_shutdown_impl();
  void shutdown_impl();

  my_thread_handle m_manager_thread{};
  std::atomic<bool> m_initialized{false};
  std::atomic<int> m_active_thread_count{0};

  static std::once_flag s_once;
  static EmbeddingManager *s_instance;
};

void shannon_ml_on_ddl_event(const DDLEvent &event);

std::string serialize_from_dd_table(const std::string &schema_name, const std::string &table_name,
                                    const dd::Table *table_obj, SerializeMode mode);
}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ML_RETRIEVE_SCHEMA_METADATA_H__