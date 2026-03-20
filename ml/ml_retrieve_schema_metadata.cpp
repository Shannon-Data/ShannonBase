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
#include "sql/table_cache.h"  // tdc_remove_table()
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

std::condition_variable EmbeddingManager::m_fully_stopped_cv;
std::mutex EmbeddingManager::m_fully_stopped_mutex;
bool EmbeddingManager::m_fully_stopped{true};

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
  }

  ~ScopedInternalTHD() {
    if (!thd) return;
    thd->killed.store(THD::NOT_KILLED, std::memory_order_relaxed);

    trans_rollback_stmt(thd);
    trans_rollback(thd);
    close_thread_tables(thd);
    thd->mdl_context.release_transactional_locks();

    if (EmbeddingManager::is_running()) {
      tdc_remove_table(thd, TDC_RT_REMOVE_UNUSED, ML_META_SCHEMA, ML_SCHEMA_EMBEDDINGS_TABLE, false);
    }

    ha_close_connection(thd);

    // to make sure the opers dont affect the statistics, this only happens internally.
    destroy_internal_thd(thd);
    thd = nullptr;
  }

  ScopedInternalTHD(const ScopedInternalTHD &) = delete;
  ScopedInternalTHD &operator=(const ScopedInternalTHD &) = delete;
  ScopedInternalTHD(ScopedInternalTHD &&) = delete;
  ScopedInternalTHD &operator=(ScopedInternalTHD &&) = delete;

  explicit operator bool() const { return thd != nullptr; }
};

static inline std::string sql_esc(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '\'') out += '\'';
    out += c;
  }
  return out;
}

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
    DBUG_PRINT("ml", ("ML EmbeddingManager: thread killed before updating embedding for %s.%s", schema.c_str(),
                      table.c_str()));
    return HA_ERR_GENERIC;
  }

  table_ptr->file->ha_reset();
  table_ptr->file->start_stmt(thd, TL_WRITE);
  ShannonBase::Utils::ColumnMapGuard column_guard(table_ptr, ShannonBase::Utils::ColumnMapGuard::TYPE::ALL);
  memset(table_ptr->record[0], 0, table_ptr->s->reclength);

  Field *schema_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
  schema_field->set_notnull();
  schema_field->store(schema.c_str(), schema.length(), &my_charset_utf8mb3_general_ci);

  Field *table_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];
  table_field->set_notnull();
  table_field->store(table.c_str(), table.length(), &my_charset_utf8mb3_general_ci);

  Field *doc_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::DOC_TEXT)];
  doc_field->set_notnull();
  doc_field->store(doc.c_str(), doc.length(), &my_charset_utf8mb3_general_ci);

  Field *status_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::STATUS)];
  status_field->set_notnull();
  status_field->store(0LL, true);

  Field *updated_at_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::UPDATED_AT)];
  updated_at_field->set_notnull();
  store_current_timestamp(thd, updated_at_field);

  Field *embedding_field =
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
    DBUG_PRINT("ml", ("ML EmbeddingManager: transaction commit failed"));
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

  // Based on DDL key order: PRIMARY KEY (idx=0), UNIQUE KEY unique_schema_table (idx=1)
  constexpr uint UNIQUE_SCHEMA_TABLE_KEY = 1;
  table_ptr->file->ha_reset();
  table_ptr->file->start_stmt(thd, TL_WRITE);

  ShannonBase::Utils::ColumnMapGuard column_guard(table_ptr, ShannonBase::Utils::ColumnMapGuard::TYPE::ALL);
  memset(table_ptr->record[0], 0, table_ptr->s->reclength);

  Field *schema_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
  schema_field->set_notnull();
  schema_field->store(schema.c_str(), schema.length(), &my_charset_utf8mb3_general_ci);

  Field *table_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];
  table_field->set_notnull();
  table_field->store(table.c_str(), table.length(), &my_charset_utf8mb3_general_ci);

  // copy key fields → key buffer
  uchar key_buf[MAX_KEY_LENGTH] = {};
  key_copy(key_buf, table_ptr->record[0], &table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY],
           table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY].key_length);

  // open index and locate the row
  auto index_guard = create_scope_guard([&] { table_ptr->file->ha_index_end(); });
  int ret = table_ptr->file->ha_index_init(UNIQUE_SCHEMA_TABLE_KEY, /*sorted=*/true);
  if (ret) {
    DBUG_PRINT("ml",
               ("ML EmbeddingManager: ha_index_init failed for %s.%s, error=%d", schema.c_str(), table.c_str(), ret));
    return ret;
  }

  ret = table_ptr->file->ha_index_read_map(table_ptr->record[0], key_buf, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: row not found for %s.%s, error=%d", schema.c_str(), table.c_str(), ret));
    return ret;  // HA_ERR_KEY_NOT_FOUND if the row doesn't exist yet
  }

  store_record(table_ptr, record[1]);

  Field *embedding_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::EMBEDDING)];
  embedding_field->set_notnull();
  embedding_field->store(reinterpret_cast<const char *>(embedding.data()), embedding.size() * sizeof(float),
                         &my_charset_bin);

  Field *status_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::STATUS)];
  status_field->set_notnull();
  status_field->store(1LL, /*unsigned=*/true);  // 1 = processed

  Field *updated_at_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::UPDATED_AT)];
  updated_at_field->set_notnull();
  store_current_timestamp(thd, updated_at_field);

  ret = table_ptr->file->ha_update_row(table_ptr->record[1], table_ptr->record[0]);

  // HA_ERR_RECORD_IS_THE_SAME means nothing actually changed — treat as success
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

static int update_schema_embedding_doc_text(THD *thd, TABLE *table_ptr, const std::string &schema,
                                            const std::string &table, const std::string &doc) {
  if (thd->killed.load(std::memory_order_relaxed)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: thread killed before updating embedding for %s.%s", schema.c_str(),
                      table.c_str()));
    return HA_ERR_GENERIC;
  }

  constexpr uint UNIQUE_SCHEMA_TABLE_KEY = 1;
  bool need_insert = false;
  int final_ret = ShannonBase::SHANNON_SUCCESS;

  [&]() {
    table_ptr->file->ha_reset();
    table_ptr->file->start_stmt(thd, TL_WRITE);

    ShannonBase::Utils::ColumnMapGuard column_guard(table_ptr, ShannonBase::Utils::ColumnMapGuard::TYPE::ALL);
    memset(table_ptr->record[0], 0, table_ptr->s->reclength);

    Field *schema_field =
        table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
    schema_field->set_notnull();
    schema_field->store(schema.c_str(), schema.length(), &my_charset_utf8mb3_general_ci);

    Field *table_field =
        table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];
    table_field->set_notnull();
    table_field->store(table.c_str(), table.length(), &my_charset_utf8mb3_general_ci);

    uchar key_buf[MAX_KEY_LENGTH] = {};
    key_copy(key_buf, table_ptr->record[0], &table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY],
             table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY].key_length);

    auto index_guard = create_scope_guard([&] { table_ptr->file->ha_index_end(); });
    int ret = table_ptr->file->ha_index_init(UNIQUE_SCHEMA_TABLE_KEY, /*sorted=*/true);
    if (ret) {
      DBUG_PRINT("ml",
                 ("ML EmbeddingManager: update_schema_embedding_doc_text ha_index_init failed for %s.%s, error=%d",
                  schema.c_str(), table.c_str(), ret));
      final_ret = ret;
      return;  // exits lambda → both guards destruct cleanly
    }

    ret = table_ptr->file->ha_index_read_map(table_ptr->record[0], key_buf, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
    if (ret) {
      DBUG_PRINT("ml", ("ML EmbeddingManager: update_schema_embedding_doc_text row not found for %s.%s, will insert",
                        schema.c_str(), table.c_str()));
      need_insert = true;
      return;  // exits lambda → index_guard: ha_index_end(), column_guard: bitmap restored
    }

    store_record(table_ptr, record[1]);

    Field *doc_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::DOC_TEXT)];
    doc_field->set_notnull();
    doc_field->store(doc.c_str(), doc.length(), &my_charset_utf8mb3_general_ci);

    Field *status_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::STATUS)];
    status_field->set_notnull();
    status_field->store(0LL, /*unsigned=*/true);

    Field *embedding_field =
        table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::EMBEDDING)];
    embedding_field->set_null();

    Field *updated_at_field =
        table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::UPDATED_AT)];
    updated_at_field->set_notnull();
    store_current_timestamp(thd, updated_at_field);

    ret = table_ptr->file->ha_update_row(table_ptr->record[1], table_ptr->record[0]);
    if (ret && ret != HA_ERR_RECORD_IS_THE_SAME) {
      DBUG_PRINT("ml",
                 ("ML EmbeddingManager: update_schema_embedding_doc_text ha_update_row failed for %s.%s, error=%d",
                  schema.c_str(), table.c_str(), ret));
      trans_rollback_stmt(thd);
      trans_rollback(thd);
      final_ret = ret;
      return;
    }

    if (trans_commit_stmt(thd) || trans_commit(thd)) {
      DBUG_PRINT("ml", ("ML EmbeddingManager: update_schema_embedding_doc_text commit failed for %s.%s", schema.c_str(),
                        table.c_str()));
      final_ret = HA_ERR_GENERIC;
      return;
    }
  }();

  if (need_insert) {
    return insert_schema_embedding_item(thd, table_ptr, schema, table, doc);
  }

  return final_ret;
}

static int mark_schema_embedding_error(THD *thd, TABLE *table_ptr, const std::string &schema,
                                       const std::string &table) {
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

  Field *schema_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
  schema_field->set_notnull();
  schema_field->store(schema.c_str(), schema.length(), &my_charset_utf8mb3_general_ci);

  Field *table_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];
  table_field->set_notnull();
  table_field->store(table.c_str(), table.length(), &my_charset_utf8mb3_general_ci);

  uchar key_buf[MAX_KEY_LENGTH] = {};
  key_copy(key_buf, table_ptr->record[0], &table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY],
           table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY].key_length);

  auto index_guard = create_scope_guard([&] { table_ptr->file->ha_index_end(); });
  int ret = table_ptr->file->ha_index_init(UNIQUE_SCHEMA_TABLE_KEY, /*sorted=*/true);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: mark_schema_embedding_error ha_index_init failed for %s.%s, error=%d",
                      schema.c_str(), table.c_str(), ret));
    return ret;
  }
  ret = table_ptr->file->ha_index_read_map(table_ptr->record[0], key_buf, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: mark_schema_embedding_error row not found for %s.%s, error=%d",
                      schema.c_str(), table.c_str(), ret));
    return ret;
  }

  store_record(table_ptr, record[1]);

  Field *status_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::STATUS)];
  status_field->set_notnull();
  status_field->store(2LL, /*unsigned=*/true);  // 2 = error

  Field *updated_at_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::UPDATED_AT)];
  updated_at_field->set_notnull();
  store_current_timestamp(thd, updated_at_field);

  ret = table_ptr->file->ha_update_row(table_ptr->record[1], table_ptr->record[0]);
  if (ret && ret != HA_ERR_RECORD_IS_THE_SAME) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: mark_schema_embedding_error ha_update_row failed for %s.%s, error=%d",
                      schema.c_str(), table.c_str(), ret));
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    return ret;
  }

  if (trans_commit_stmt(thd) || trans_commit(thd)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: mark_schema_embedding_error commit failed for %s.%s", schema.c_str(),
                      table.c_str()));
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

  Field *schema_field =
      table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::SCHEMA_NAME)];
  schema_field->set_notnull();
  schema_field->store(schema.c_str(), schema.length(), &my_charset_utf8mb3_general_ci);

  Field *table_field = table_ptr->field[static_cast<int>(EmbeddingManager::SCHEMA_EMBEDDINGS_FIELD_INDEX::TABLE_NAME)];
  table_field->set_notnull();
  table_field->store(table.c_str(), table.length(), &my_charset_utf8mb3_general_ci);

  uchar key_buf[MAX_KEY_LENGTH] = {};
  key_copy(key_buf, table_ptr->record[0], &table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY],
           table_ptr->key_info[UNIQUE_SCHEMA_TABLE_KEY].key_length);

  auto index_guard = create_scope_guard([&] { table_ptr->file->ha_index_end(); });
  int ret = table_ptr->file->ha_index_init(UNIQUE_SCHEMA_TABLE_KEY, /*sorted=*/true);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: delete_schema_embedding_item ha_index_init failed for %s.%s, error=%d",
                      schema.c_str(), table.c_str(), ret));
    return ret;
  }

  ret = table_ptr->file->ha_index_read_map(table_ptr->record[0], key_buf, HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  if (ret) {
    // HA_ERR_KEY_NOT_FOUND is benign for a delete — row already gone
    DBUG_PRINT("ml", ("ML EmbeddingManager: delete_schema_embedding_item ha_index_read_map failed for %s.%s, error=%d",
                      schema.c_str(), table.c_str(), ret));
    if (ret == HA_ERR_KEY_NOT_FOUND || ret == HA_ERR_END_OF_FILE) {
      return ShannonBase::SHANNON_SUCCESS;
    }
    return ret;
  }

  ret = table_ptr->file->ha_delete_row(table_ptr->record[0]);
  if (ret) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: delete_schema_embedding_item ha_delete_row failed for %s.%s, error=%d",
                      schema.c_str(), table.c_str(), ret));
    trans_rollback_stmt(thd);
    trans_rollback(thd);
    return ret;
  }

  if (trans_commit_stmt(thd) || trans_commit(thd)) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: delete_schema_embedding_item commit failed for %s.%s", schema.c_str(),
                      table.c_str()));
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

static void *embedding_table_worker_func(void *arg) {
  my_thread_init();
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

      if (ctx->tasks.empty()) break;

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
  mgr->on_thread_exiting();  // decrement live count; signal CV when last thread exits
  my_thread_end();
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
    DBUG_PRINT("ml", ("ML EmbeddingManager: cannot open mysql.schema_embedding table."));
    return;
  }
  TABLE *schema_embedding_table_ptr = tl.table;
  if (!schema_embedding_table_ptr || !schema_embedding_table_ptr->file) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: schema_embeddings table handler is null — aborting DDL batch."));
    return;
  }

  for (auto &ev : ddl_batch) {
    // Honour shutdown request inside a long batch.
    if (!EmbeddingManager::is_running()) break;

    const std::string key = ev.schema_name + "." + ev.table_name;
    if (ev.type == DDLEventType::DROP) {
      delete_schema_embedding_item(scope.thd, schema_embedding_table_ptr, ev.schema_name, ev.table_name);
      continue;
    }

    std::string doc;
    if (ev.type == DDLEventType::ALTER) {
      // Always re-fetch for ALTER — ignore ev.doc regardless of whether it's set
      MDL_request mdl_request;
      MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, ev.schema_name.c_str(), ev.table_name.c_str(), MDL_SHARED_READ,
                       MDL_STATEMENT);
      if (scope.thd->mdl_context.acquire_lock(&mdl_request, scope.thd->variables.lock_wait_timeout)) {
        DBUG_PRINT("ml", ("ML EmbeddingManager: failed to acquire MDL for ALTER %s.%s", ev.schema_name.c_str(),
                          ev.table_name.c_str()));
        continue;
      }
      dd::cache::Dictionary_client *dc = scope.thd->dd_client();
      const dd::cache::Dictionary_client::Auto_releaser releaser(dc);
      const dd::Table *table_obj = nullptr;
      if (!dc || dc->acquire(ev.schema_name.c_str(), ev.table_name.c_str(), &table_obj) || !table_obj) {
        DBUG_PRINT("ml", ("ML EmbeddingManager: DD acquire failed for ALTER %s.%s", ev.schema_name.c_str(),
                          ev.table_name.c_str()));
        continue;
      }
      doc = serialize_from_dd_table(ev.schema_name, ev.table_name, table_obj, SerializeMode::WITH_COMMENTS);
    } else {
      // CREATE: ev.doc was just serialized from the hook, use it;
      doc = std::move(ev.doc);
      if (doc.empty()) {
        MDL_request mdl_request;
        MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, ev.schema_name.c_str(), ev.table_name.c_str(), MDL_SHARED_READ,
                         MDL_STATEMENT);
        if (scope.thd->mdl_context.acquire_lock(&mdl_request, scope.thd->variables.lock_wait_timeout)) {
          DBUG_PRINT("ml", ("ML EmbeddingManager: failed to acquire MDL for %s.%s", ev.schema_name.c_str(),
                            ev.table_name.c_str()));
          continue;
        }
        dd::cache::Dictionary_client *dc = scope.thd->dd_client();
        const dd::cache::Dictionary_client::Auto_releaser releaser(dc);
        const dd::Table *table_obj = nullptr;
        if (dc && !dc->acquire(ev.schema_name.c_str(), ev.table_name.c_str(), &table_obj) && table_obj) {
          doc = serialize_from_dd_table(ev.schema_name, ev.table_name, table_obj, SerializeMode::WITH_COMMENTS);
        }
      }
    }

    if (doc.empty()) {
      DBUG_PRINT("ml", ("ML EmbeddingManager: empty doc for %s, skipping.", key.c_str()));
      continue;
    }

    int db_ret =
        (ev.type == DDLEventType::CREATE)
            ? insert_schema_embedding_item(scope.thd, schema_embedding_table_ptr, ev.schema_name, ev.table_name, doc)
            : update_schema_embedding_doc_text(scope.thd, schema_embedding_table_ptr, ev.schema_name, ev.table_name,
                                               doc);

    if (db_ret) continue;

    TableWorkerContext *workers = mgr->get_or_create_worker(key);
    if (!workers) continue;

    std::string model_id = ML_DEFAULT_EMBED_MODEL;
    workers->start_job(ev.type, ev.schema_name, ev.table_name, doc, model_id);

    DBUG_PRINT("ml", ("ML EmbeddingManager: dispatched %s for %s",
                      (ev.type == DDLEventType::CREATE ? "CREATE" : "ALTER"), key.c_str()));
  }
}

static void *embedding_manager_func(void *arg) {
  struct MysqlThreadGuard {
    MysqlThreadGuard() { my_thread_init(); }
    ~MysqlThreadGuard() { my_thread_end(); }
    MysqlThreadGuard(const MysqlThreadGuard &) = delete;
    MysqlThreadGuard &operator=(const MysqlThreadGuard &) = delete;
  } thread_guard;

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

    std::deque<DDLEvent> ddl_batch;
    {
      std::lock_guard<std::mutex> ddl_lk(mgr->m_ddl_mutex);
      std::lock_guard<std::mutex> mgr_lk(EmbeddingManager::m_manager_mutex);
      ddl_batch.swap(mgr->m_ddl_queue);
      mgr->m_ddl_queue_keys.clear();
      mgr->m_ddl_pending_count.store(0, std::memory_order_relaxed);
    }

    if (!ddl_batch.empty()) {
      process_ddl_events(mgr, ddl_batch);
    } else if (mgr->m_result_pending_count.load(std::memory_order_acquire) > 0) {
      ScopedInternalTHD scope;
      if (scope) {
        Table_ref tl(ML_META_SCHEMA, strlen(ML_META_SCHEMA), ML_SCHEMA_EMBEDDINGS_TABLE,
                     strlen(ML_SCHEMA_EMBEDDINGS_TABLE), ML_SCHEMA_EMBEDDINGS_TABLE, TL_WRITE);
        tl.open_type = OT_BASE_ONLY;
        uint flags = MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK | MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY | MYSQL_OPEN_IGNORE_FLUSH;
        if (open_and_lock_tables(scope.thd, &tl, flags)) {
          DBUG_PRINT("ml", ("ML EmbeddingManager: cannot open mysql.schema_embedding table."));
          continue;
        }
        TABLE *schema_embedding_table_ptr = tl.table;
        if (!schema_embedding_table_ptr || !schema_embedding_table_ptr->file) {
          DBUG_PRINT("ml", ("ML EmbeddingManager: schema_embeddings table handler is null — skipping consume."));
          continue;
        }
        Disable_binlog_guard bg(scope.thd);
        mgr->consume_results(scope.thd, schema_embedding_table_ptr);
      }
    }
  }

  DBUG_PRINT("ml", ("ML EmbeddingManager: event loop finished, exiting."));
  mgr->on_thread_exiting();  // decrement live count; signal CV when last thread exits
  return nullptr;
}

void EmbeddingManager::start_impl() {
  if (!m_initialized.load()) return;
  embedding_state_t expected = embedding_state_t::EMBEDDING_STATE_EXIT;
  if (!m_state.compare_exchange_strong(expected, embedding_state_t::EMBEDDING_STATE_RUN, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    // Also allow starting from STOP state
    expected = embedding_state_t::EMBEDDING_STATE_STOP;
    if (!m_state.compare_exchange_strong(expected, embedding_state_t::EMBEDDING_STATE_RUN, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
      return;  // already RUN, or another thread won the race
    }
  }

  {
    std::lock_guard<std::mutex> lk(m_fully_stopped_mutex);
    m_fully_stopped = false;
  }
  // Account for the coordinator thread we are about to spawn.
  m_active_thread_count.fetch_add(1, std::memory_order_relaxed);

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
    m_active_thread_count.fetch_sub(1, std::memory_order_relaxed);
    m_state.store(embedding_state_t::EMBEDDING_STATE_STOP, std::memory_order_release);
    return;
  }
  my_thread_attr_destroy(&attr);
}

void EmbeddingManager::initiate_shutdown_impl() {
  embedding_state_t expected = embedding_state_t::EMBEDDING_STATE_RUN;
  if (!m_state.compare_exchange_strong(expected, embedding_state_t::EMBEDDING_STATE_STOP, std::memory_order_acq_rel,
                                       std::memory_order_acquire)) {
    return;  // already STOP, EXIT, or another thread won the race
  }

  THD *active = m_current_thd.load(std::memory_order_acquire);
  if (active) active->awake(THD::KILL_CONNECTION);

  // clear pending queues to unblock coordinator and workers waiting on them; they will check the state and exit
  // gracefully
  {
    std::lock_guard<std::mutex> lk(m_ddl_mutex);
    m_ddl_queue.clear();
    m_ddl_queue_keys.clear();
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

void EmbeddingManager::on_thread_exiting() {
  // fetch_sub returns the value *before* the decrement.
  int prev = m_active_thread_count.fetch_sub(1, std::memory_order_acq_rel);
  if (prev == 1) {
    // We are the last thread standing.
    std::lock_guard<std::mutex> lk(m_fully_stopped_mutex);
    m_fully_stopped = true;
    m_fully_stopped_cv.notify_all();
    DBUG_PRINT("ml", ("ML EmbeddingManager: all threads exited — shutdown gate open."));
  }
}

void EmbeddingManager::initiate_shutdown() {
  auto *mgr = instance();
  if (mgr && EmbeddingManager::is_running()) mgr->initiate_shutdown_impl();
}

bool EmbeddingManager::wait_until_fully_stopped(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lk(m_fully_stopped_mutex);
  return m_fully_stopped_cv.wait_for(lk, timeout, [] { return m_fully_stopped; });
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

  {
    std::lock_guard<std::mutex> lk(m_fully_stopped_mutex);
    m_fully_stopped = true;
  }
  m_fully_stopped_cv.notify_all();

  m_state.store(embedding_state_t::EMBEDDING_STATE_EXIT, std::memory_order_release);
  m_initialized.store(false);
}

TableWorkerContext *EmbeddingManager::get_or_create_worker(const std::string &key) {
  std::lock_guard<std::mutex> lk(m_table_workers_mutex);

  auto it = m_table_workers.find(key);
  if (it != m_table_workers.end()) return it->second.get();

  auto ctx = std::make_unique<TableWorkerContext>();
  ctx->key = key;

  my_thread_attr_t attr;
  my_thread_attr_init(&attr);
  my_thread_attr_setdetachstate(&attr, MY_THREAD_CREATE_JOINABLE);
  m_active_thread_count.fetch_add(1, std::memory_order_relaxed);
  int rc = my_thread_create(&ctx->thread, &attr, embedding_table_worker_func, ctx.get());
  my_thread_attr_destroy(&attr);

  if (rc != 0) {
    DBUG_PRINT("ml", ("ML EmbeddingManager: failed to spawn worker for %s", key.c_str()));
    m_active_thread_count.fetch_sub(1, std::memory_order_relaxed);
    return nullptr;
  }

  TableWorkerContext *raw = ctx.get();
  m_table_workers[key] = std::move(ctx);

  DBUG_PRINT("ml", ("ML EmbeddingManager: spawned worker for %s", key.c_str()));
  return raw;
}

void EmbeddingManager::consume_results(THD *thd, TABLE *schema_embedding_table_ptr) {
  if (!schema_embedding_table_ptr || !schema_embedding_table_ptr->file || thd->killed != THD::NOT_KILLED) return;

  std::deque<EmbedResult> batch;
  {
    std::lock_guard<std::mutex> lk(m_result_mutex);
    if (m_result_queue.empty()) return;
    batch.swap(m_result_queue);
    m_result_pending_count.store(0, std::memory_order_relaxed);
  }

  for (auto &res : batch) {
    if (thd->killed != THD::NOT_KILLED) break;
    int ret{ShannonBase::SHANNON_SUCCESS};
    ret = (res.success && !res.embedding.empty())
              ? update_schema_embedding_embedding(thd, schema_embedding_table_ptr, res.schema_name, res.table_name,
                                                  res.embedding)
              : mark_schema_embedding_error(thd, schema_embedding_table_ptr, res.schema_name, res.table_name);
    if (ret) {
      DBUG_PRINT("ml", ("ML EmbeddingManager: consume_results error %d for %s.%s — "
                        "stopping batch; remaining results will be retried",
                        ret, res.schema_name.c_str(), res.table_name.c_str()));
      break;
    }
  }
}

void EmbeddingManager::enqueue_ddl_event(const DDLEvent &ev) {
  if (EmbeddingManager::m_state.load(std::memory_order_acquire) != embedding_state_t::EMBEDDING_STATE_RUN) return;

  const std::string key = ev.schema_name + "." + ev.table_name;

  {
    std::unique_lock<std::mutex> lk(m_ddl_mutex);
    if (m_ddl_queue.size() >= MAX_DDL_QUEUE_DEPTH) {
      if (ev.type == DDLEventType::DROP) {
        m_ddl_queue.erase(
            std::remove_if(m_ddl_queue.begin(), m_ddl_queue.end(),
                           [&key](const DDLEvent &e) { return e.schema_name + "." + e.table_name == key; }),
            m_ddl_queue.end());
        m_ddl_queue_keys.erase(key);
      } else {
        return;
      }
    }

    if (ev.type == DDLEventType::ALTER && m_ddl_queue_keys.count(key)) {
      for (auto &e : m_ddl_queue) {
        if (e.schema_name + "." + e.table_name == key && e.type == DDLEventType::ALTER) {
          e.doc = ev.doc;
          return;  // coalesced — no counter bump, no wake
        }
      }
    }

    m_ddl_queue.push_back(ev);
    m_ddl_queue_keys.insert(key);
  }

  {
    std::lock_guard<std::mutex> mgr_lk(m_manager_mutex);
    m_ddl_pending_count.fetch_add(1, std::memory_order_relaxed);
    m_manager_cv.notify_one();
  }
}

void shannon_ml_on_ddl_event(const DDLEvent &ev) {
  auto *mgr = EmbeddingManager::instance();
  if (!mgr || !mgr->initialized() || opt_initialize) return;

  if (ShannonBase::ML::Utils::is_system_schema(ev.schema_name.c_str())) return;

  if (!EmbeddingManager::is_running()) mgr->start();
  mgr->enqueue_ddl_event(ev);
}
}  // namespace ML
}  // namespace ShannonBase