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

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/
#include "ml_retrieve_schema_metadata.h"

#if !defined(_WIN32)
#include <pthread.h>
#else
#include <Windows.h>
#endif

#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "ml_utils.h"
#include "my_thread.h"
#include "mysql/service_mysql_alloc.h"
#include "sql/thd_raii.h"

#include "sql/current_thd.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/impl/types/column_impl.h"
#include "sql/dd/impl/types/index_element_impl.h"
#include "sql/dd/types/column.h"
#include "sql/dd/types/foreign_key_element.h"
#include "sql/dd/types/table.h"
#include "sql/log.h"
#include "sql/sql_thd_internal_api.h"
#include "sql/statement/ed_connection.h"
#include "sql/statement/protocol_local.h"
#include "sql/statement/statement_runnable.h"

#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table_cache.h"
#include "sql/transaction.h"

#include "storage/rapid_engine/utils/utils.h"
extern bool opt_initialize;
namespace ShannonBase {
namespace ML {
std::once_flag EmbeddingManager::s_once;
EmbeddingManager *EmbeddingManager::s_instance{nullptr};
std::atomic<embedding_state_t> EmbeddingManager::m_state{embedding_state_t::EMBEDDING_STATE_EXIT};
std::condition_variable EmbeddingManager::m_manager_cv;
std::mutex EmbeddingManager::m_manager_mutex;

struct ScopedInternalTHD {
  THD *thd{nullptr};

  ScopedInternalTHD() {
    thd = create_internal_thd();
    if (!thd) return;

    thd->system_thread = SYSTEM_THREAD_BACKGROUND;
    thd->security_context()->skip_grants();
    thd->variables.lock_wait_timeout = 5;
    thd->variables.option_bits |= OPTION_LOG_OFF;
    thd->mark_plugin_fake_ddl(true);
    thd->set_time();

    THD *expected = nullptr;
    EmbeddingManager::instance()->m_current_thd.compare_exchange_strong(expected, thd, std::memory_order_acq_rel,
                                                                        std::memory_order_relaxed);
  }

  ~ScopedInternalTHD() {
    if (!thd) return;
    THD *expected = thd;
    EmbeddingManager::instance()->m_current_thd.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel,
                                                                        std::memory_order_relaxed);
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    close_thread_tables(thd);
    thd->mdl_context.release_transactional_locks();

    if (EmbeddingManager::is_running()) {
      tdc_remove_table(thd, TDC_RT_REMOVE_UNUSED, ML_META_SCHEMA, ML_SCHEMA_EMBEDDINGS_TABLE, false);
    }
    ha_close_connection(thd);
    thd->killed.store(THD::NOT_KILLED, std::memory_order_relaxed);
    destroy_internal_thd(thd);
    thd = nullptr;
  }

  ScopedInternalTHD(const ScopedInternalTHD &) = delete;
  ScopedInternalTHD &operator=(const ScopedInternalTHD &) = delete;
  ScopedInternalTHD(ScopedInternalTHD &&) = delete;
  ScopedInternalTHD &operator=(ScopedInternalTHD &&) = delete;

  explicit operator bool() const { return thd != nullptr; }
};

struct MysqlThreadGuard {
  MysqlThreadGuard() { my_thread_init(); }
  ~MysqlThreadGuard() { my_thread_end(); }
  MysqlThreadGuard(const MysqlThreadGuard &) = delete;
  MysqlThreadGuard &operator=(const MysqlThreadGuard &) = delete;
};

static inline void store_current_timestamp(THD * /*thd*/, Field *field) {
  my_timeval tv;
  my_micro_time_to_timeval(my_micro_time(), &tv);
  if (field->decimals() == 0) tv.m_tv_usec = 0;
  field->set_notnull();
  static_cast<Field_timestamp *>(field)->store_timestamp(&tv);
}

/**
 * Here, we dont use such as to do this
 *   const std::string sql_str =
 *     "SELECT schema_name, table_name FROM mysql.schema_embeddings"
 *     " WHERE status IN (0, 2)";
 * LEX_STRING sql_lex = {const_cast<char *>(sql_str.c_str()), sql_str.length()};
 * Statement_runnable run(sql_lex);
 * Ed_connection conn(thd);
 * if (conn.execute_direct(&run)) return;
 * taking these following consider:
 * 1: too expensive, lex/syntax parsing, optimization, execution, innodb opers.
 * 2: it affects some sql status variables, will misleading use monitor system.
 */
static int insert_schema_embedding_item(THD *thd, TABLE *table_ptr, const std::string &schema, const std::string &table,
                                        const std::string &doc) {
  if (thd->killed.load(std::memory_order_relaxed)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: thread killed before insert for %s.%s", schema.c_str(), table.c_str()));
    return HA_ERR_GENERIC;
  }

  table_ptr->file->ha_reset();
  table_ptr->file->start_stmt(thd, TL_WRITE);
  ShannonBase::Utils::ColumnMapGuard column_guard(table_ptr, ShannonBase::Utils::ColumnMapGuard::TYPE::ALL);
  memset(table_ptr->record[0], 0, table_ptr->s->reclength);

  auto *schema_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
  schema_field->set_notnull();
  schema_field->store(schema.c_str(), schema.length(), &my_charset_utf8mb3_general_ci);

  auto *table_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];
  table_field->set_notnull();
  table_field->store(table.c_str(), table.length(), &my_charset_utf8mb3_general_ci);

  auto *doc_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::DOC_TEXT)];
  doc_field->set_notnull();
  doc_field->store(doc.c_str(), doc.length(), &my_charset_utf8mb3_general_ci);

  auto *status_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::STATUS)];
  status_field->set_notnull();
  status_field->store(static_cast<int64_t>(EmbeddingStatus::PENDING), /*unsigned=*/true);

  auto *updated_at_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::UPDATED_AT)];
  updated_at_field->set_notnull();
  store_current_timestamp(thd, updated_at_field);

  auto *embedding_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::EMBEDDING)];
  embedding_field->set_null();

  auto auto_inc_guard = create_scope_guard([&] { table_ptr->next_number_field = nullptr; });
  table_ptr->next_number_field = table_ptr->found_next_number_field;
  int ret = table_ptr->file->ha_write_row(table_ptr->record[0]);
  table_ptr->file->ha_release_auto_increment();
  if (ret) {
    DBUG_PRINT("ml",
               ("ML EmbeddingManager: ha_write_row failed for %s.%s, error=%d", schema.c_str(), table.c_str(), ret));
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    return ret;
  }

  if (trans_commit_stmt(thd) || trans_commit(thd)) {
    DBUG_PRINT("ml",
               ("ML EmbeddingManager: insert transaction commit failed for %s.%s", schema.c_str(), table.c_str()));
    return HA_ERR_GENERIC;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

static int update_schema_embedding_embedding(THD *thd, TABLE *table_ptr, const std::string &schema,
                                             const std::string &table, const std::vector<float> &embedding) {
  if (thd->killed.load(std::memory_order_relaxed)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: thread killed before updating embedding for %s.%s", schema.c_str(),
                      table.c_str()));
    return HA_ERR_GENERIC;
  }

  constexpr uint UNIQUE_SCHEMA_TABLE_KEY = 1;
  table_ptr->file->ha_reset();
  table_ptr->file->start_stmt(thd, TL_WRITE);

  ShannonBase::Utils::ColumnMapGuard column_guard(table_ptr, ShannonBase::Utils::ColumnMapGuard::TYPE::ALL);
  memset(table_ptr->record[0], 0, table_ptr->s->reclength);

  auto *schema_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
  schema_field->set_notnull();
  schema_field->store(schema.c_str(), schema.length(), &my_charset_utf8mb3_general_ci);

  auto *table_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];
  table_field->set_notnull();
  table_field->store(table.c_str(), table.length(), &my_charset_utf8mb3_general_ci);

  uchar key_buf[MAX_KEY_LENGTH] = {};
  key_copy(key_buf, table_ptr->record[0], &table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY],
           table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY].key_length);

  int ret = table_ptr->file->ha_index_init(UNIQUE_SCHEMA_TABLE_KEY, /*sorted=*/true);
  if (ret) {
    DBUG_PRINT("ml",
               ("ML EmbeddingManager: ha_index_init failed for %s.%s, error=%d", schema.c_str(), table.c_str(), ret));
    return ret;
  }

  auto index_guard = create_scope_guard([&] { table_ptr->file->ha_index_end(); });
  ret = table_ptr->file->ha_index_read_map(table_ptr->record[0], key_buf, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: row not found for %s.%s, error=%d", schema.c_str(), table.c_str(), ret));
    return ret;
  }

  store_record(table_ptr, record[1]);

  auto *embedding_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::EMBEDDING)];
  embedding_field->set_notnull();
  embedding_field->store(reinterpret_cast<const char *>(embedding.data()), embedding.size() * sizeof(float),
                         &my_charset_bin);

  auto *status_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::STATUS)];
  status_field->set_notnull();
  status_field->store(static_cast<int64_t>(EmbeddingStatus::PROCESSED), /*unsigned=*/true);

  auto *updated_at_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::UPDATED_AT)];
  updated_at_field->set_notnull();
  store_current_timestamp(thd, updated_at_field);

  ret = table_ptr->file->ha_update_row(table_ptr->record[1], table_ptr->record[0]);
  if (ret && ret != HA_ERR_RECORD_IS_THE_SAME) {
    DBUG_PRINT("ml",
               ("ML EmbeddingManager: ha_update_row failed for %s.%s, error=%d", schema.c_str(), table.c_str(), ret));
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    return ret;
  }

  if (trans_commit_stmt(thd) || trans_commit(thd)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: transaction commit failed for %s.%s", schema.c_str(), table.c_str()));
    return HA_ERR_GENERIC;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

static int try_update_doc_text_in_table(THD *thd, TABLE *table_ptr, const std::string &schema, const std::string &table,
                                        const std::string &doc) {
  constexpr uint UNIQUE_SCHEMA_TABLE_KEY = 1;

  table_ptr->file->ha_reset();
  table_ptr->file->start_stmt(thd, TL_WRITE);

  ShannonBase::Utils::ColumnMapGuard column_guard(table_ptr, ShannonBase::Utils::ColumnMapGuard::TYPE::ALL);
  memset(table_ptr->record[0], 0, table_ptr->s->reclength);

  auto *schema_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
  schema_field->set_notnull();
  schema_field->store(schema.c_str(), schema.length(), &my_charset_utf8mb3_general_ci);

  auto *table_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];
  table_field->set_notnull();
  table_field->store(table.c_str(), table.length(), &my_charset_utf8mb3_general_ci);

  uchar key_buf[MAX_KEY_LENGTH] = {};
  key_copy(key_buf, table_ptr->record[0], &table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY],
           table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY].key_length);

  int ret = table_ptr->file->ha_index_init(UNIQUE_SCHEMA_TABLE_KEY, /*sorted=*/true);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: try_update_doc_text ha_index_init failed for %s.%s, error=%d",
                      schema.c_str(), table.c_str(), ret));
    return ret;
  }
  auto index_guard = create_scope_guard([&] { table_ptr->file->ha_index_end(); });

  ret = table_ptr->file->ha_index_read_map(table_ptr->record[0], key_buf, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  if (ret) {
    // Row not found – let caller decide whether to INSERT.
    DBUG_PRINT("ml", ("ML EmbeddingManager: try_update_doc_text row not found for %s.%s (will insert)", schema.c_str(),
                      table.c_str()));
    return ret;  // typically HA_ERR_KEY_NOT_FOUND
  }

  store_record(table_ptr, record[1]);

  auto *doc_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::DOC_TEXT)];
  doc_field->set_notnull();
  doc_field->store(doc.c_str(), doc.length(), &my_charset_utf8mb3_general_ci);

  auto *status_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::STATUS)];
  status_field->set_notnull();
  status_field->store(static_cast<int64_t>(EmbeddingStatus::PENDING), /*unsigned=*/true);

  auto *embedding_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::EMBEDDING)];
  embedding_field->set_null();

  auto *updated_at_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::UPDATED_AT)];
  updated_at_field->set_notnull();
  store_current_timestamp(thd, updated_at_field);

  ret = table_ptr->file->ha_update_row(table_ptr->record[1], table_ptr->record[0]);
  if (ret && ret != HA_ERR_RECORD_IS_THE_SAME) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: try_update_doc_text ha_update_row failed for %s.%s, error=%d",
                      schema.c_str(), table.c_str(), ret));
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    return ret;
  }

  if (trans_commit_stmt(thd) || trans_commit(thd)) {
    DBUG_PRINT("ml",
               ("ML EmbeddingManager: try_update_doc_text commit failed for %s.%s", schema.c_str(), table.c_str()));
    return HA_ERR_GENERIC;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

static int update_schema_embedding_doc_text(THD *thd, TABLE *table_ptr, const std::string &schema,
                                            const std::string &table, const std::string &doc) {
  if (thd->killed.load(std::memory_order_relaxed)) {
    DBUG_PRINT("ml",
               ("ML EmbeddingManager: thread killed before doc-text update for %s.%s", schema.c_str(), table.c_str()));
    return HA_ERR_GENERIC;
  }

  int ret = try_update_doc_text_in_table(thd, table_ptr, schema, table, doc);
  if (ret == HA_ERR_KEY_NOT_FOUND || ret == HA_ERR_END_OF_FILE) {
    // Row does not yet exist; fall through to INSERT.
    return insert_schema_embedding_item(thd, table_ptr, schema, table, doc);
  }
  return ret;
}

static int mark_schema_embedding_error(THD *thd, TABLE *table_ptr, const std::string &schema,
                                       const std::string &table) {
  if (thd->killed.load(std::memory_order_relaxed)) {
    DBUG_PRINT("ml",
               ("ML EmbeddingManager: thread killed before marking error for %s.%s", schema.c_str(), table.c_str()));
    return HA_ERR_GENERIC;
  }

  constexpr uint UNIQUE_SCHEMA_TABLE_KEY = 1;
  table_ptr->file->ha_reset();
  table_ptr->file->start_stmt(thd, TL_WRITE);

  ShannonBase::Utils::ColumnMapGuard column_guard(table_ptr, ShannonBase::Utils::ColumnMapGuard::TYPE::ALL);
  memset(table_ptr->record[0], 0, table_ptr->s->reclength);

  auto *schema_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
  schema_field->set_notnull();
  schema_field->store(schema.c_str(), schema.length(), &my_charset_utf8mb3_general_ci);

  auto *table_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];
  table_field->set_notnull();
  table_field->store(table.c_str(), table.length(), &my_charset_utf8mb3_general_ci);

  uchar key_buf[MAX_KEY_LENGTH] = {};
  key_copy(key_buf, table_ptr->record[0], &table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY],
           table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY].key_length);

  int ret = table_ptr->file->ha_index_init(UNIQUE_SCHEMA_TABLE_KEY, /*sorted=*/true);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: mark_error ha_index_init failed for %s.%s, error=%d", schema.c_str(),
                      table.c_str(), ret));
    return ret;
  }

  auto index_guard = create_scope_guard([&] { table_ptr->file->ha_index_end(); });
  ret = table_ptr->file->ha_index_read_map(table_ptr->record[0], key_buf, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: mark_error row not found for %s.%s, error=%d", schema.c_str(),
                      table.c_str(), ret));
    return ret;
  }

  store_record(table_ptr, record[1]);

  auto *status_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::STATUS)];
  status_field->set_notnull();
  status_field->store(static_cast<int64_t>(EmbeddingStatus::ERROR), /*unsigned=*/true);

  auto *updated_at_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::UPDATED_AT)];
  updated_at_field->set_notnull();
  store_current_timestamp(thd, updated_at_field);

  ret = table_ptr->file->ha_update_row(table_ptr->record[1], table_ptr->record[0]);
  if (ret && ret != HA_ERR_RECORD_IS_THE_SAME) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: mark_error ha_update_row failed for %s.%s, error=%d", schema.c_str(),
                      table.c_str(), ret));
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    return ret;
  }

  if (trans_commit_stmt(thd) || trans_commit(thd)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: mark_error commit failed for %s.%s", schema.c_str(), table.c_str()));
    return HA_ERR_GENERIC;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

static int delete_schema_embedding_item(THD *thd, TABLE *table_ptr, const std::string &schema,
                                        const std::string &table) {
  constexpr uint UNIQUE_SCHEMA_TABLE_KEY = 1;
  table_ptr->file->ha_reset();
  table_ptr->file->start_stmt(thd, TL_WRITE);

  ShannonBase::Utils::ColumnMapGuard column_guard(table_ptr, ShannonBase::Utils::ColumnMapGuard::TYPE::ALL);
  memset(table_ptr->record[0], 0, table_ptr->s->reclength);

  auto *schema_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
  schema_field->set_notnull();
  schema_field->store(schema.c_str(), schema.length(), &my_charset_utf8mb3_general_ci);

  auto *table_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];
  table_field->set_notnull();
  table_field->store(table.c_str(), table.length(), &my_charset_utf8mb3_general_ci);

  uchar key_buf[MAX_KEY_LENGTH] = {};
  key_copy(key_buf, table_ptr->record[0], &table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY],
           table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY].key_length);

  int ret = table_ptr->file->ha_index_init(UNIQUE_SCHEMA_TABLE_KEY, /*sorted=*/true);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: delete ha_index_init failed for %s.%s, error=%d", schema.c_str(),
                      table.c_str(), ret));
    return ret;
  }
  auto index_guard = create_scope_guard([&] { table_ptr->file->ha_index_end(); });

  ret = table_ptr->file->ha_index_read_map(table_ptr->record[0], key_buf, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  if (ret) {
    // Row already gone — benign for a delete.
    if (ret == HA_ERR_KEY_NOT_FOUND || ret == HA_ERR_END_OF_FILE) return ShannonBase::SHANNON_SUCCESS;
    DBUG_PRINT("ml", ("ML EmbeddingManager: delete ha_index_read_map failed for %s.%s, error=%d", schema.c_str(),
                      table.c_str(), ret));
    return ret;
  }

  ret = table_ptr->file->ha_delete_row(table_ptr->record[0]);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: delete ha_delete_row failed for %s.%s, error=%d", schema.c_str(),
                      table.c_str(), ret));
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    return ret;
  }

  if (trans_commit_stmt(thd) || trans_commit(thd)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: delete commit failed for %s.%s", schema.c_str(), table.c_str()));
    return HA_ERR_GENERIC;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

std::string serialize_from_dd_table(const std::string &schema_name, const std::string &table_name,
                                    const dd::Table *table_obj, SerializeMode mode) {
  if (!table_obj) return "";

  const bool with_comments = (mode == SerializeMode::WITH_COMMENTS);
  std::ostringstream doc;
  doc << "Table: " << schema_name << "." << table_name << "\n";
  if (with_comments && !table_obj->comment().empty()) doc << "Comment: " << table_obj->comment().c_str() << "\n";

  std::unordered_set<std::string> pk_cols;
  for (const dd::Index *idx : table_obj->indexes()) {
    if (idx->type() != dd::Index::IT_PRIMARY) continue;
    for (const dd::Index_element *ie : idx->elements()) {
      if (ie->is_hidden()) continue;
      pk_cols.emplace(ie->column().name().c_str());
    }
  }

  std::unordered_map<std::string, std::string> fk_map;
  for (const dd::Foreign_key *fk : table_obj->foreign_keys()) {
    const std::string ref_schema = fk->referenced_table_schema_name().c_str();
    const std::string ref_table = fk->referenced_table_name().c_str();
    for (const dd::Foreign_key_element *fke : fk->elements()) {
      std::string ref_label;
      if (ref_schema != schema_name) ref_label = ref_schema + ".";
      ref_label += ref_table + "." + std::string(fke->referenced_column_name().c_str());
      fk_map[fke->column().name().c_str()] = std::move(ref_label);
    }
  }

  auto dd_column_type_str = [&](const dd::Column *col) -> std::string {
    if (!col) return "unknown";
    return col->column_type_utf8().c_str();
  };

  doc << "Columns:\n";
  for (const dd::Column *col : table_obj->columns()) {
    if (col->is_se_hidden()) continue;
    if (col->is_virtual() && col->hidden() != dd::Column::enum_hidden_type::HT_VISIBLE) continue;

    const std::string col_name = col->name().c_str();
    doc << "  " << col_name << " " << dd_column_type_str(col);
    if (pk_cols.count(col_name)) doc << " [PK]";
    if (col->is_auto_increment()) doc << " [AUTO_INCREMENT]";
    if (!col->is_nullable() && !pk_cols.count(col_name)) doc << " [NOT NULL]";
    auto fk_it = fk_map.find(col_name);
    if (fk_it != fk_map.end()) doc << " [FK->" << fk_it->second << "]";
    if (with_comments && !col->comment().empty()) doc << " : " << col->comment().c_str();
    doc << "\n";
  }

  bool has_uk = false;
  for (const dd::Index *idx : table_obj->indexes()) {
    if (idx->type() != dd::Index::IT_UNIQUE) continue;
    if (!has_uk) {
      doc << "Unique indexes:\n";
      has_uk = true;
    }
    doc << "  " << idx->name().c_str() << "(";
    bool first = true;
    for (const dd::Index_element *ie : idx->elements()) {
      if (ie->is_hidden()) continue;
      if (!first) doc << ", ";
      doc << ie->column().name().c_str();
      first = false;
    }
    doc << ")\n";
  }
  return doc.str();
}

template <typename Fn>
static bool with_mdl_shared_read(THD *thd, const std::string &schema, const std::string &table, Fn &&fn) {
  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, schema.c_str(), table.c_str(), MDL_SHARED_READ, MDL_STATEMENT);
  if (thd->mdl_context.acquire_lock(&mdl_request, thd->variables.lock_wait_timeout)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: failed to acquire MDL for %s.%s", schema.c_str(), table.c_str()));
    return false;
  }

  // Run the caller's work while we hold the lock.
  fn();

  // Release this statement's MDL locks immediately so they do not accumulate.
  thd->mdl_context.release_statement_locks();
  return true;
}

static void *embedding_table_worker_func(void *arg) {
  MysqlThreadGuard thread_guard;
  auto *ctx = static_cast<TableWorkerContext *>(arg);
#if !defined(_WIN32)
  {
    const std::string &k = ctx->key;
    std::string tname = "ml_tw_" + (k.size() > 9 ? k.substr(k.size() - 9) : k);
    pthread_setname_np(pthread_self(), tname.c_str());
  }
#else
  SetThreadDescription(GetCurrentThread(), L"ml_table_worker");
#endif

  auto *mgr = EmbeddingManager::instance();
  DBUG_PRINT("ml", ("ML TableWorker [%s]: started", ctx->key.c_str()));

  while (true) {
    EmbedTask task;
    {
      std::unique_lock<std::mutex> lk(ctx->mutex);
      ctx->cv.wait(lk, [ctx]() {
        return !ctx->tasks.empty() ||
               EmbeddingManager::m_state.load(std::memory_order_acquire) != embedding_state_t::EMBEDDING_STATE_RUN;
      });

      if (ctx->tasks.empty()) break;  // shutting down

      task = std::move(ctx->tasks.front());
      ctx->tasks.pop_front();
    }

    EmbedResult res;
    res.schema_name = task.schema_name;
    res.table_name = task.table_name;

    if (task.doc.empty()) {
      DBUG_PRINT("ml", ("ML TableWorker [%s]: received empty doc, marking error.", ctx->key.c_str()));
      res.success = false;
    } else {
      const std::string &mid = task.model_id.empty() ? ML_DEFAULT_EMBED_MODEL : task.model_id;

      auto *json_obj = new (std::nothrow) Json_object();
      if (!json_obj) {
        res.success = false;
      } else {
        json_obj->add_alias("model_id", new (std::nothrow) Json_string(mid));
        Json_wrapper opt(json_obj);  // takes ownership
        std::string doc_copy = task.doc;
        try {
          std::lock_guard<std::mutex> emb_lk(mgr->m_embedder_mutex);
          if (mgr->m_embedder) {
            res.embedding = mgr->m_embedder->GenerateEmbedding(doc_copy, opt);
            res.success = !res.embedding.empty();
          } else {
            res.success = false;
          }
        } catch (const std::exception &e) {
          DBUG_PRINT("ml", ("ML TableWorker [%s]: exception in GenerateEmbedding for %s.%s: %s", ctx->key.c_str(),
                            task.schema_name.c_str(), task.table_name.c_str(), e.what()));
          res.success = false;
        } catch (...) {
          DBUG_PRINT("ml", ("ML TableWorker [%s]: unknown exception in GenerateEmbedding for %s.%s", ctx->key.c_str(),
                            task.schema_name.c_str(), task.table_name.c_str()));
          res.success = false;
        }
      }
    }

    {
      std::lock_guard<std::mutex> res_lk(mgr->m_result_mutex);
      mgr->m_result_queue.push_back(std::move(res));
      mgr->m_result_pending_count.fetch_add(1, std::memory_order_relaxed);
    }
    {
      std::lock_guard<std::mutex> mgr_lk(EmbeddingManager::m_manager_mutex);
      EmbeddingManager::m_manager_cv.notify_one();
    }
  }

  DBUG_PRINT("ml", ("ML TableWorker [%s]: exiting", ctx->key.c_str()));
  return nullptr;
}

static void process_ddl_events(EmbeddingManager *mgr, std::deque<DDLEvent> &ddl_batch) {
  ScopedInternalTHD scope;
  if (!scope) return;

  // Binlog must be disabled for internal DDL metadata writes.
  Disable_binlog_guard binlog_guard(scope.thd);
  Table_ref tl(ML_META_SCHEMA, strlen(ML_META_SCHEMA), ML_SCHEMA_EMBEDDINGS_TABLE, strlen(ML_SCHEMA_EMBEDDINGS_TABLE),
               ML_SCHEMA_EMBEDDINGS_TABLE, TL_WRITE);
  tl.open_type = OT_BASE_ONLY;
  uint flags = MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY | MYSQL_OPEN_IGNORE_FLUSH;
  if (open_and_lock_tables(scope.thd, &tl, flags)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: cannot open mysql.schema_embeddings table."));
    return;
  }

  if (scope.thd->killed != THD::NOT_KILLED) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: process_ddl_events: THD killed after open — aborting batch."));
    return;
  }

  TABLE *schema_embedding_table_ptr = tl.table;
  if (!schema_embedding_table_ptr || !schema_embedding_table_ptr->file) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: schema_embeddings table handler is null — aborting DDL batch."));
    return;
  }

  for (auto &ev : ddl_batch) {
    if (!EmbeddingManager::is_running()) break;

    const std::string key = ev.schema_name + "." + ev.table_name;
    if (ev.type == DDLEventType::DROP) {
      delete_schema_embedding_item(scope.thd, schema_embedding_table_ptr, ev.schema_name, ev.table_name);
      continue;
    }

    std::string doc;
    if (ev.type == DDLEventType::ALTER) {
      bool acquired = with_mdl_shared_read(scope.thd, ev.schema_name, ev.table_name, [&]() {
        dd::cache::Dictionary_client *dc = scope.thd->dd_client();
        const dd::cache::Dictionary_client::Auto_releaser releaser(dc);
        const dd::Table *table_obj = nullptr;
        if (dc && !dc->acquire(ev.schema_name.c_str(), ev.table_name.c_str(), &table_obj) && table_obj) {
          doc = serialize_from_dd_table(ev.schema_name, ev.table_name, table_obj, SerializeMode::WITH_COMMENTS);
        }
      });
      if (!acquired || doc.empty()) {
        DBUG_PRINT("ml", ("ML EmbeddingManager: cannot get doc for ALTER %s — skipping.", key.c_str()));
        continue;
      }
    } else {
      // CREATE: prefer the pre-serialized doc from the hook; fall back to a
      // fresh DD read if it was empty (e.g. when replaying from recovery).
      doc = std::move(ev.doc);
      if (doc.empty()) {
        with_mdl_shared_read(scope.thd, ev.schema_name, ev.table_name, [&]() {
          dd::cache::Dictionary_client *dc = scope.thd->dd_client();
          const dd::cache::Dictionary_client::Auto_releaser releaser(dc);
          const dd::Table *table_obj = nullptr;
          if (dc && !dc->acquire(ev.schema_name.c_str(), ev.table_name.c_str(), &table_obj) && table_obj) {
            doc = serialize_from_dd_table(ev.schema_name, ev.table_name, table_obj, SerializeMode::WITH_COMMENTS);
          }
        });
      }
      if (doc.empty()) {
        DBUG_PRINT("ml", ("ML EmbeddingManager: empty doc for CREATE %s — skipping.", key.c_str()));
        continue;
      }
    }

    int db_ret =
        update_schema_embedding_doc_text(scope.thd, schema_embedding_table_ptr, ev.schema_name, ev.table_name, doc);
    if (db_ret) {
      DBUG_PRINT("ml",
                 ("ML EmbeddingManager: upsert failed for %s, error=%d — skipping embedding.", key.c_str(), db_ret));
      continue;
    }

    TableWorkerContext *worker = mgr->get_or_create_worker(key);
    if (!worker) continue;

    std::string model_id = ML_DEFAULT_EMBED_MODEL;
    worker->start_job(ev.type, ev.schema_name, ev.table_name, std::move(doc), model_id);
    DBUG_PRINT("ml", ("ML EmbeddingManager: dispatched %s for %s",
                      (ev.type == DDLEventType::CREATE ? "CREATE" : "ALTER"), key.c_str()));
  }
}

static void *embedding_manager_func(void *arg) {
  MysqlThreadGuard thread_guard;
#if !defined(_WIN32)
  pthread_setname_np(pthread_self(), "ml_embed_manager");
#else
  SetThreadDescription(GetCurrentThread(), L"ml_embed_manager");
#endif

  auto should_stop = []() -> bool { return !EmbeddingManager::is_running(); };
  if (!ShannonBase::Utils::Util::wait_for_server_bootup(10, should_stop)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: exits (shutdown or bootup timeout)."));
    return nullptr;
  }

  auto *mgr = static_cast<EmbeddingManager *>(arg);
  DBUG_PRINT("ml", ("ML EmbeddingManager: started, entering event loop."));
  while (EmbeddingManager::is_running()) {
    {
      std::unique_lock<std::mutex> lk(EmbeddingManager::m_manager_mutex);
      EmbeddingManager::m_manager_cv.wait(lk, [mgr]() {
        return mgr->m_ddl_pending_count.load(std::memory_order_acquire) > 0 ||
               mgr->m_result_pending_count.load(std::memory_order_acquire) > 0 || !EmbeddingManager::is_running();
      });
    }

    if (!EmbeddingManager::is_running()) break;

    // Drain the DDL queue.
    std::deque<DDLEvent> ddl_batch;
    {
      std::lock_guard<std::mutex> ddl_lk(mgr->m_ddl_mutex);
      std::lock_guard<std::mutex> mgr_lk(EmbeddingManager::m_manager_mutex);
      // Build the ordered event list from the O(1) index structures.
      for (const auto &k : mgr->m_ddl_order) {
        auto it = mgr->m_ddl_map.find(k);
        if (it != mgr->m_ddl_map.end()) {
          ddl_batch.push_back(std::move(it->second));
        }
      }
      mgr->m_ddl_order.clear();
      mgr->m_ddl_map.clear();
      mgr->m_ddl_pending_count.store(0, std::memory_order_relaxed);
    }

    if (!ddl_batch.empty()) {
      process_ddl_events(mgr, ddl_batch);
    }

    if (mgr->m_result_pending_count.load(std::memory_order_acquire) > 0) {
      ScopedInternalTHD scope;
      if (scope) {
        Table_ref tl(ML_META_SCHEMA, strlen(ML_META_SCHEMA), ML_SCHEMA_EMBEDDINGS_TABLE,
                     strlen(ML_SCHEMA_EMBEDDINGS_TABLE), ML_SCHEMA_EMBEDDINGS_TABLE, TL_WRITE);
        tl.open_type = OT_BASE_ONLY;
        uint flags = MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY | MYSQL_OPEN_IGNORE_FLUSH;
        if (open_and_lock_tables(scope.thd, &tl, flags)) {
          DBUG_PRINT("ml", ("ML EmbeddingManager: cannot open schema_embeddings for result consume."));
          continue;
        }
        TABLE *schema_embedding_table_ptr = tl.table;
        if (!schema_embedding_table_ptr || !schema_embedding_table_ptr->file) {
          DBUG_PRINT("ml", ("ML EmbeddingManager: schema_embeddings handler null — skipping consume."));
          continue;
        }
        Disable_binlog_guard bg(scope.thd);
        mgr->consume_results(scope.thd, schema_embedding_table_ptr);
      }
    }
  }

  DBUG_PRINT("ml", ("ML EmbeddingManager: event loop finished, exiting."));
  return nullptr;
}

void EmbeddingManager::start_impl() {
  if (!m_initialized.load()) return;
  embedding_state_t expected = embedding_state_t::EMBEDDING_STATE_EXIT;
  if (!m_state.compare_exchange_strong(expected, embedding_state_t::EMBEDDING_STATE_RUN, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    expected = embedding_state_t::EMBEDDING_STATE_STOP;
    if (!m_state.compare_exchange_strong(expected, embedding_state_t::EMBEDDING_STATE_RUN, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
      return;  // already RUN, or another thread won
    }
  }

  {
    std::lock_guard<std::mutex> lk(m_embedder_mutex);
    m_embedder = std::make_unique<ML_embedding_row>();
    if (!m_embedder->WarmUp(ML_DEFAULT_EMBED_MODEL)) {
      DBUG_PRINT("ml",
                 ("ML EmbeddingManager: WarmUp failed for '%s' — will retry on first use", ML_DEFAULT_EMBED_MODEL));
    }
  }

  my_thread_attr_t attr;
  my_thread_attr_init(&attr);
  my_thread_attr_setdetachstate(&attr, MY_THREAD_CREATE_JOINABLE);
  if (my_thread_create(&m_manager_thread, &attr, embedding_manager_func, this) != 0) {
    sql_print_error("[EmbeddingManager] start: failed to create coordinator thread");
    my_thread_attr_destroy(&attr);
    m_state.store(embedding_state_t::EMBEDDING_STATE_STOP, std::memory_order_release);
    return;
  }
  my_thread_attr_destroy(&attr);
}

void EmbeddingManager::shutdown_impl() {
  initiate_shutdown_impl();

  if (m_manager_thread.thread != 0) {
    my_thread_join(&m_manager_thread, nullptr);
    m_manager_thread = {};
  }

  {
    std::lock_guard<std::mutex> lk(m_table_workers_mutex);
    for (auto &[key, ctx] : m_table_workers) {
      if (ctx->thread.thread != 0) {
        my_thread_join(&ctx->thread, nullptr);
        ctx->thread = {};
      }
    }
    m_table_workers.clear();
  }

  m_state.store(embedding_state_t::EMBEDDING_STATE_EXIT, std::memory_order_release);
  m_initialized.store(false);
}

void EmbeddingManager::initiate_shutdown_impl() {
  embedding_state_t expected = embedding_state_t::EMBEDDING_STATE_RUN;
  if (!m_state.compare_exchange_strong(expected, embedding_state_t::EMBEDDING_STATE_STOP, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    return;
  }

  if (m_embedder) m_embedder->TerminateTask();

  THD *active = m_current_thd.load(std::memory_order_acquire);
  if (active != nullptr) {
    mysql_mutex_lock(&active->LOCK_thd_data);
    active->awake(THD::KILL_CONNECTION);
    mysql_mutex_unlock(&active->LOCK_thd_data);
  }

  {
    std::lock_guard<std::mutex> lk(m_ddl_mutex);
    m_ddl_order.clear();
    m_ddl_map.clear();
    m_ddl_pending_count.store(0, std::memory_order_relaxed);
  }

  {
    std::lock_guard<std::mutex> lk(m_table_workers_mutex);
    for (auto &[key, ctx] : m_table_workers) {
      {
        std::lock_guard<std::mutex> wlk(ctx->mutex);
        ctx->tasks.clear();
      }
      ctx->cv.notify_all();
    }
  }

  m_manager_cv.notify_all();
}

/**
 * When the pool is below MAX_WORKER_THREADS, spawn a new dedicated worker for
 * this (schema, table) key.  Once the pool is full, return the existing worker
 * that has the fewest queued tasks (work-stealing lite), avoiding unbounded
 * thread growth for schemas with thousands of tables.
 */
TableWorkerContext *EmbeddingManager::get_or_create_worker(const std::string &key) {
  std::lock_guard<std::mutex> lk(m_table_workers_mutex);

  // Fast path: dedicated worker already exists for this key.
  auto it = m_table_workers.find(key);
  if (it != m_table_workers.end()) return it->second.get();

  // Pool not yet full — spawn a new worker.
  if (m_table_workers.size() < MAX_WORKER_THREADS) {
    auto ctx = std::make_unique<TableWorkerContext>();
    ctx->key = key;

    my_thread_attr_t attr;
    my_thread_attr_init(&attr);
    my_thread_attr_setdetachstate(&attr, MY_THREAD_CREATE_JOINABLE);
    int rc = my_thread_create(&ctx->thread, &attr, embedding_table_worker_func, ctx.get());
    my_thread_attr_destroy(&attr);

    if (rc != 0) {
      DBUG_PRINT("ml", ("ML EmbeddingManager: failed to spawn worker for %s", key.c_str()));
      return nullptr;
    }

    TableWorkerContext *raw = ctx.get();
    m_table_workers[key] = std::move(ctx);
    DBUG_PRINT("ml", ("ML EmbeddingManager: spawned worker for %s", key.c_str()));
    return raw;
  }

  // Pool is full — route to the least-loaded existing worker.
  TableWorkerContext *best = nullptr;
  size_t min_depth = SIZE_MAX;
  for (auto &[k, ctx] : m_table_workers) {
    std::lock_guard<std::mutex> wlk(ctx->mutex);
    if (ctx->tasks.size() < min_depth) {
      min_depth = ctx->tasks.size();
      best = ctx.get();
    }
  }

  if (best) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: pool full — routing %s to worker %s (depth=%zu)", key.c_str(),
                      best->key.c_str(), min_depth));
  }
  return best;
}

/**
 * If processing fails mid-batch, the unprocessed tail is placed back at the
 * front of m_result_queue so it will be retried on the next coordinator wake,
 * instead of being silently dropped.
 */
void EmbeddingManager::consume_results(THD *thd, TABLE *schema_embedding_table_ptr) {
  if (!schema_embedding_table_ptr || !schema_embedding_table_ptr->file || thd->killed != THD::NOT_KILLED) return;

  constexpr uint UNIQUE_SCHEMA_TABLE_KEY = 1;
  if (schema_embedding_table_ptr->s->keys <= UNIQUE_SCHEMA_TABLE_KEY) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: consume_results: schema_embeddings has only %u key(s), "
                      "expected >%u — skipping.",
                      schema_embedding_table_ptr->s->keys, UNIQUE_SCHEMA_TABLE_KEY));
    return;
  }

  std::deque<EmbedResult> batch;
  {
    std::lock_guard<std::mutex> lk(m_result_mutex);
    if (m_result_queue.empty()) return;
    batch.swap(m_result_queue);
    m_result_pending_count.store(0, std::memory_order_relaxed);
  }

  size_t processed = 0;
  for (auto &res : batch) {
    if (thd->killed != THD::NOT_KILLED) break;

    int ret = (res.success && !res.embedding.empty())
                  ? update_schema_embedding_embedding(thd, schema_embedding_table_ptr, res.schema_name, res.table_name,
                                                      res.embedding)
                  : mark_schema_embedding_error(thd, schema_embedding_table_ptr, res.schema_name, res.table_name);
    if (ret) {
      DBUG_PRINT("ml", ("ML EmbeddingManager: consume_results error %d for %s.%s — "
                        "returning unprocessed tail to queue for retry.",
                        ret, res.schema_name.c_str(), res.table_name.c_str()));
      break;
    }
    ++processed;
  }

  if (processed < batch.size()) {
    std::lock_guard<std::mutex> lk(m_result_mutex);
    for (auto it = batch.rbegin() + static_cast<ptrdiff_t>(processed); it != batch.rend(); ++it) {
      m_result_queue.push_front(std::move(*it));
    }
    m_result_pending_count.store(static_cast<uint32_t>(m_result_queue.size()), std::memory_order_relaxed);
  }
}

/**
 * Uses m_ddl_order (insertion-ordered key list) + m_ddl_map (key→event) for
 * O(1) ALTER coalescing and O(1) DROP eviction.
 *
 * The overflow guard now re-checks queue depth after eviction before pushing,
 * preventing size from exceeding MAX_DDL_QUEUE_DEPTH.
 */
void EmbeddingManager::enqueue_ddl_event(const DDLEvent &ev) {
  if (m_state.load(std::memory_order_acquire) != embedding_state_t::EMBEDDING_STATE_RUN) return;

  const std::string key = ev.schema_name + "." + ev.table_name;

  {
    std::unique_lock<std::mutex> lk(m_ddl_mutex);

    if (ev.type == DDLEventType::DROP) {
      // For DROP: remove any pending event for this table (O(1) via map).
      if (m_ddl_map.erase(key)) {
        // Also remove from the ordered list.
        m_ddl_order.erase(std::remove(m_ddl_order.begin(), m_ddl_order.end(), key), m_ddl_order.end());
        m_ddl_pending_count.fetch_sub(1, std::memory_order_relaxed);
      }
      // re-check size *after* eviction before pushing.
      if (m_ddl_map.size() >= MAX_DDL_QUEUE_DEPTH) {
        DBUG_PRINT(
            "ml", ("ML EmbeddingManager: DDL queue full even after DROP eviction — dropping DROP for %s", key.c_str()));
        return;
      }
      m_ddl_order.push_back(key);
      m_ddl_map[key] = ev;

    } else if (ev.type == DDLEventType::ALTER) {
      // re-check size *after* eviction before pushing.
      auto it = m_ddl_map.find(key);
      if (it != m_ddl_map.end()) {
        // Coalesce: update the existing entry, no new order slot needed.
        it->second = ev;
        // No counter bump, no wake — the coordinator already knows about this key.
        return;
      }
      if (m_ddl_map.size() >= MAX_DDL_QUEUE_DEPTH) {
        DBUG_PRINT("ml", ("ML EmbeddingManager: DDL queue full — dropping ALTER for %s", key.c_str()));
        return;
      }
      m_ddl_order.push_back(key);
      m_ddl_map[key] = ev;

    } else {  // CREATE
      if (m_ddl_map.size() >= MAX_DDL_QUEUE_DEPTH) {
        DBUG_PRINT("ml", ("ML EmbeddingManager: DDL queue full — dropping CREATE for %s", key.c_str()));
        return;
      }
      // For CREATE, if there's already a pending event (e.g. ALTER), overwrite it.
      if (!m_ddl_map.count(key)) m_ddl_order.push_back(key);
      m_ddl_map[key] = ev;
    }
  }

  {
    std::lock_guard<std::mutex> mgr_lk(m_manager_mutex);
    m_ddl_pending_count.fetch_add(1, std::memory_order_relaxed);
    m_manager_cv.notify_one();
  }
}

/**
 * Scans mysql.schema_embeddings for rows whose status is PENDING (0) or
 * ERROR (2) and re-enqueues them as ALTER events so the coordinator will
 * re-compute and store their embeddings.  Called once at startup from the
 * main thread (before the coordinator's event loop begins) or after a crash.
 */
void EmbeddingManager::recover_pending_tasks(THD *thd) {
  if (!thd || opt_initialize) return;

  ScopedInternalTHD scope;
  if (!scope) return;

  Table_ref tl(ML_META_SCHEMA, strlen(ML_META_SCHEMA), ML_SCHEMA_EMBEDDINGS_TABLE, strlen(ML_SCHEMA_EMBEDDINGS_TABLE),
               ML_SCHEMA_EMBEDDINGS_TABLE, TL_READ);
  tl.open_type = OT_BASE_ONLY;
  uint flags = MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY | MYSQL_OPEN_IGNORE_FLUSH;
  if (open_and_lock_tables(scope.thd, &tl, flags)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: recover_pending_tasks: cannot open schema_embeddings."));
    return;
  }
  TABLE *tbl = tl.table;
  if (!tbl || !tbl->file) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: recover_pending_tasks: table handler null."));
    return;
  }

  // Collect (schema, table) pairs where status IN (PENDING, ERROR).
  std::vector<std::pair<std::string, std::string>> to_recover;

  tbl->file->ha_rnd_init(/*scan=*/true);
  while (true) {
    int ret = tbl->file->ha_rnd_next(tbl->record[0]);
    if (ret == HA_ERR_END_OF_FILE) break;
    if (ret) {
      DBUG_PRINT("ml", ("ML EmbeddingManager: recover_pending_tasks: ha_rnd_next error %d.", ret));
      break;
    }

    auto *status_field = tbl->field[static_cast<int>(SCHEMA_EMBEDDINGS_FIELD_INDEX::STATUS)];
    int64_t status = status_field->val_int();
    if (status != static_cast<int64_t>(EmbeddingStatus::PENDING) &&
        status != static_cast<int64_t>(EmbeddingStatus::ERROR)) {
      continue;
    }

    auto *schema_field = tbl->field[static_cast<int>(SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
    auto *table_field = tbl->field[static_cast<int>(SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];

    String schema_val, table_val;
    schema_field->val_str(&schema_val);
    table_field->val_str(&table_val);

    to_recover.emplace_back(std::string(schema_val.c_ptr_safe(), schema_val.length()),
                            std::string(table_val.c_ptr_safe(), table_val.length()));
  }
  tbl->file->ha_rnd_end();

  close_thread_tables(scope.thd);
  scope.thd->mdl_context.release_transactional_locks();

  DBUG_PRINT("ml", ("ML EmbeddingManager: recover_pending_tasks: found %zu rows to recover.", to_recover.size()));

  // Re-enqueue each as an ALTER event (doc is empty; coordinator will re-fetch from DD).
  for (auto &[schema, table] : to_recover) {
    if (ShannonBase::ML::Utils::is_system_schema(schema.c_str())) continue;
    DDLEvent ev{DDLEventType::ALTER, schema, table, /*doc=*/""};
    enqueue_ddl_event(ev);
  }
}

void shannon_ml_on_ddl_event(const DDLEvent &ev) {
  auto *mgr = EmbeddingManager::instance();
  if (!mgr || !mgr->initialized() || opt_initialize) return;
  if (ShannonBase::ML::Utils::is_system_schema(ev.schema_name.c_str())) return;

  if (EmbeddingManager::m_state.load(std::memory_order_acquire) == embedding_state_t::EMBEDDING_STATE_EXIT) {
    mgr->start();
  }
  mgr->enqueue_ddl_event(ev);
}
}  // namespace ML
}  // namespace ShannonBase