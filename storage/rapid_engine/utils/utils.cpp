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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/utils/utils.h"

#include "include/decimal.h"  //my_decimal
#include "include/my_bitmap.h"
#include "sql/sql_base.h"

#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/types/table.h"
#include "sql/opt_trace.h"
#include "sql/sql_class.h"     //Secondary_engine_statement_context
#include "sql/sql_executor.h"  //QEP_TBA
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"                //JOIN
#include "sql/table.h"                        //TABLE
#include "sql/transaction.h"                  // trans_commit_stmt
#include "storage/innobase/include/ut0dbg.h"  //ut_a

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/ml/ml.h"
#include "storage/rapid_engine/populate/log_populate.h"

namespace ShannonBase {
namespace Utils {
// open table by name. return table ptr, otherwise return nullptr.
TABLE *Util::open_table_by_name(THD *thd, std::string schema_name, std::string table_name, thr_lock_type lk_mode) {
  /**
   * due to in function, `select xxxx`, when the statment executed, it enter lock table mode
   * but there's not even a opened table, so that, here we try to open a table, it failed before
   * exiting the lock table mode. such as executing `selecct ml_predict_row(xxx) int xx;`
   * if you only get a TABLE object, you can
   *    Table_ref table_list;
   *     table_list.db = context.m_schema_name.c_str();
   *     table_list.db_length = context.m_schema_name.length();
   *     table_list.table_name = context.m_table_name.c_str();
   *     table_list.table_name_length = context.m_table_name.length();
   *     table_list.alias = context.m_table_name.c_str();
   *     table_list.set_lock({TL_READ, THR_DEFAULT});
   *     MDL_REQUEST_INIT(&table_list.mdl_request,
   *                      MDL_key::TABLE,                 // namespace
   *                      context.m_schema_name.c_str(),  // db
   *                      context.m_table_name.c_str(),   // name
   *                      MDL_SHARED_READ,                // type
   *                      MDL_TRANSACTION);               // duration
   *
   *     Table_ref *table_list_ptr = &table_list;
   *     uint counter{0};
   *     if (open_tables(thd, &table_list_ptr, &counter, 0)) {
   *       failed.emplace(lsn, std::move(change_rec));
   *       continue;
   *    }
   *
   *    context.m_table = table_list.table;
   *    ..
   *  to close the opened tables.
   *    close_thread_tables(thd);
   */

  /*
  TABLE *table{nullptr};
  for (table = thd->open_tables; table; table = table->next) {
    auto db_flag = !strncmp(schema_name.c_str(), table->s->db.str, table->s->db.length);
    auto tb_flag = !strncmp(table_name.c_str(), table->s->table_name.str, table->s->table_name.length);
    if (table->s && table->file && (table->file->inited != handler::NONE) && db_flag && tb_flag) {
      return table;
    }
  }

  auto old_mode = thd->locked_tables_mode;
  if (thd->locked_tables_mode == LTM_PRELOCKED) {
    thd->locked_tables_mode = LTM_NONE;
  }

  Open_table_context table_ref_context(thd, MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK);

  Table_ref table_ref(schema_name.c_str(), table_name.c_str(), lk_mode);
  table_ref.open_strategy = Table_ref::OPEN_NORMAL;

  if (open_table(thd, &table_ref, &table_ref_context) || !table_ref.table->file) {
    sql_print_warning("Failed to open table %s.%s", schema_name, table_name);
    return nullptr;
  }

  auto table_ptr = table_ref.table;
  if (!table_ptr->next_number_field)  // in case.
    table_ptr->next_number_field = table_ptr->found_next_number_field;
  thd->locked_tables_mode = old_mode;

  if (!table_ptr->file) return nullptr;
  if (table_ptr->file->ha_external_lock(thd, F_WRLCK)) {
    return nullptr;
  }
*/
  Table_ref table_list;
  table_list.db = schema_name.c_str();
  table_list.db_length = schema_name.length();
  table_list.table_name = table_name.c_str();
  table_list.table_name_length = table_name.length();
  table_list.alias = table_name.c_str();
  table_list.set_lock({TL_READ, THR_DEFAULT});
  MDL_REQUEST_INIT(&table_list.mdl_request,
                   MDL_key::TABLE,       // namespace
                   schema_name.c_str(),  // db
                   table_name.c_str(),   // name
                   MDL_SHARED_READ,      // type
                   MDL_TRANSACTION);     // duration

  Table_ref *table_list_ptr = &table_list;
  uint counter{0};
  uint flags = MYSQL_OPEN_GET_NEW_TABLE | MYSQL_OPEN_IGNORE_FLUSH;
  if (open_tables(thd, &table_list_ptr, &counter, flags)) return nullptr;
  if (table_list.table->file->ha_external_lock(thd, F_WRLCK)) return nullptr;

  return table_list.table;
}

int Util::close_table(THD *thd, TABLE *table) {
  // it will close in close_thread_tables(). so here do nothing.
  if (table) table->file->ha_external_lock(thd, F_UNLCK);

  // Transaction will be open in openning stage implicitly.
  if (thd->get_transaction()->is_active(Transaction_ctx::STMT)) trans_commit_stmt(thd);

  return SHANNON_SUCCESS;
}

std::map<std::string, std::unique_ptr<Compress::Dictionary>> loaded_dictionaries;
bool Util::is_support_type(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_TYPED_ARRAY:
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_SET: {
      return false;
    } break;
    default:
      return true;
  }
  return false;
}

int Util::get_range_value(enum_field_types type, const Compress::Dictionary *dictionary, const key_range *min_key,
                          const key_range *max_key, double &minkey, double &maxkey) {
  switch (type) {
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT: {
      minkey = min_key ? *reinterpret_cast<const int *>(min_key->key) : SHANNON_MIN_DOUBLE;
      maxkey = max_key ? *reinterpret_cast<const int *>(max_key->key) : SHANNON_MIN_DOUBLE;
    } break;
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      minkey = min_key ? *reinterpret_cast<const int *>(min_key->key) : SHANNON_MIN_DOUBLE;
      maxkey = max_key ? *reinterpret_cast<const int *>(max_key->key) : SHANNON_MAX_DOUBLE;
    } break;
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_FLOAT: {
      minkey = min_key ? *reinterpret_cast<const double *>(min_key->key) : SHANNON_MIN_DOUBLE;
      maxkey = max_key ? *reinterpret_cast<const double *>(max_key->key) : SHANNON_MAX_DOUBLE;
    } break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      minkey = min_key ? *reinterpret_cast<const double *>(min_key->key) : SHANNON_MIN_DOUBLE;
      maxkey = max_key ? *reinterpret_cast<const double *>(max_key->key) : SHANNON_MAX_DOUBLE;
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIME2: {
      minkey = maxkey = SHANNON_MIN_DOUBLE;
      if (min_key) {
        Field_datetimef datetime_min(const_cast<uchar *>(min_key->key), nullptr, 0, 0, "start_datetime", 6);
        minkey = datetime_min.val_real();
      }
      if (max_key) {
        Field_datetimef datetime_max(const_cast<uchar *>(max_key->key), nullptr, 0, 0, "start_datetime", 6);
        maxkey = datetime_max.val_real();
      }
    } break;
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      minkey = 0;
      maxkey = dictionary->content_size();
    } break;
    default:
      break;
  }
  return SHANNON_SUCCESS;
}

int Util::mem2string(uchar *buff, uint length, std::string &result) {
  const char *data = static_cast<const char *>((char *)buff);
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');

  for (size_t i = 0; i < length; ++i) oss << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(data[i]));
  result = oss.str();
  return SHANNON_SUCCESS;
}

uchar *Util::pack_str(uchar *from, size_t length, const CHARSET_INFO *from_cs, uchar *to, size_t to_length,
                      const CHARSET_INFO *to_cs) {
  size_t copy_length;
  const char *well_formed_error_pos;
  const char *cannot_convert_error_pos;
  const char *from_end_pos;

  copy_length = well_formed_copy_nchars(to_cs, (char *)to, to_length, &my_charset_bin, (char *)from, length, to_length,
                                        &well_formed_error_pos, &cannot_convert_error_pos, &from_end_pos);
  /* Append spaces if the string was shorter than the field. */
  if (copy_length < to_length)
    to_cs->cset->fill(to_cs, (char *)to + copy_length, to_length - copy_length, to_cs->pad_char);
  return to;
}

void Util::write_trace_reason(THD *thd, const char *text, const char *reason) {
  Opt_trace_context *const trace = &thd->opt_trace;
  if (unlikely(trace->is_started())) {
    const Opt_trace_object wrapper(trace);
    Opt_trace_object oto(trace, text);
    oto.add_alnum("reason", reason);
  }
}

// cost threshold classifier for determining which engine should to go.
// returns true goes to secondary engine, otherwise, false go to innodb.
bool Util::standard_cost_threshold_classifier(THD *thd) {
  if (current_thd->variables.use_secondary_engine == SECONDARY_ENGINE_FORCED) return true;

  auto stmt_context = thd->secondary_engine_statement_context();
  assert(stmt_context);

  ShannonBase::ML::Query_arbitrator::WHERE2GO where{ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_PRIMARY};
  std::string text, reason, threshold_str(std::to_string(thd->variables.secondary_engine_cost_threshold));

  if (stmt_context->get_primary_cost() > thd->variables.secondary_engine_cost_threshold) {
    reason = "The estimated query cost does exceed secondary_engine_cost_threshold, goes to secondary engine.";
    reason.append("cost: ").append(std::to_string(thd->m_current_query_cost)).append(", threshold: ");
    reason.append(threshold_str);
    text = "secondary_engine_used";
    where = ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_SECONDARY;
  } else {
    reason = "The estimated query cost does not exceed secondary_engine_cost_threshold, goes to primary engine.";
    reason.append("cost: ").append(std::to_string(thd->m_current_query_cost)).append(", threshold: ");
    reason.append(threshold_str);
    text = "secondary_engine_not_used";
    where = ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_PRIMARY;
  }

  write_trace_reason(thd, text.c_str(), reason.c_str());

  return (where == ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_SECONDARY) ? true : false;
}

//  decision tree classifier for determining which engine should to go.
// returns true goes to secondary engine, otherwise, false go to innodb.
bool Util::decision_tree_classifier(THD *thd) {
  std::string text, reason;
  // here to use trained decision tree to classify the query.

  ShannonBase::ML::Query_arbitrator qa;
  std::string mode_path = "./shannon_rapid_classifier.onnx";
  qa.load_model(mode_path);
  auto where = qa.predict(thd->lex->unit->first_query_block()->join);
  if (where == ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_SECONDARY) {
    text = "secondary_engine_used";
    reason = "The Query_arbitrator do the prediction, goes to secondary engine.";
  } else {
    text = "secondary_engine_not_used";
    reason = "The Query_arbitrator do the prediction, goes to primary engine.";
  }
  write_trace_reason(thd, text.c_str(), reason.c_str());

  return (where == ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_SECONDARY) ? true : false;
}

// dynamic feature normalization for determining which engine should to go.
// returns true goes to secondary engine, otherwise, false go to innodb.
bool Util::dynamic_feature_normalization(THD *thd) {
  auto stmt_context = thd->secondary_engine_statement_context();
  assert(stmt_context);

  // If queue is too long or CP is too long, this mechanism wants to progressively start
  // shifting queries to mysql, moving gradually towards the heavier queries
  if (ShannonBase::Populate::Populator::active() &&
      ShannonBase::Populate::sys_pop_buff.size() * ShannonBase::SHANNON_TO_MUCH_POP_THRESHOLD_RATIO >
          ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE) {
    return false;
  }

  // to checkts whether query involves tables are still in pop queue. if yes, go innodb.
  if (thd->variables.use_secondary_engine != SECONDARY_ENGINE_FORCED) {
    for (auto &table_ref : stmt_context->get_query_tables()) {
      auto share = ShannonBase::shannon_loaded_tables->get(table_ref->db, table_ref->table_name);
      auto table_id = share ? share->m_tableid : 0;
      {
        std::shared_lock lk(ShannonBase::Populate::sys_pop_buff_mutex);
        if (ShannonBase::Populate::sys_pop_buff.find(table_id) != ShannonBase::Populate::sys_pop_buff.end()) {
          return false;  // still in propation processing.
        }
      }
      if (ShannonBase::Populate::Populator::mark_table_required(table_id)) return false;
    }
  }

  return false;
}

// check whether the dictionary encoding projection is supported or not.
// returns true if supported to innodb, otherwise, false to secondary engine.
// RAPID info such as rapid base table cardinality,
//   |     dict encoding projection, varlen projection size, rapid queue
//   |     size in to decide if query should be offloaded to RAPID.
bool Util::check_dict_encoding_projection(THD *thd) {
  auto imcs_instance = ShannonBase::Imcs::Imcs::instance();
  if (!imcs_instance) return true;  // To InnoDB.

  std::string key_part;
  auto table_ref = thd->lex->unit->first_query_block()->leaf_tables;
  for (; table_ref; table_ref = table_ref->next_leaf) {
    if (table_ref->is_view_or_derived()) continue;

    auto share = ShannonBase::shannon_loaded_tables->get(table_ref->db, table_ref->table_name);
    if (!share) return true;  // not loaded.

    auto table_id = share ? share->m_tableid : 0;
    auto is_part = table_ref->partition_names ? true : false;
    auto rpd_table = is_part ? ShannonBase::Imcs::Imcs::instance()->get_rpd_table(table_id)
                             : ShannonBase::Imcs::Imcs::instance()->get_rpd_parttable(table_id);
    for (auto j = 0u; j < table_ref->table->s->fields; j++) {
      auto field_ptr = *(table_ref->table->field + j);
      if (field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;
      auto dict_algo = rpd_table->meta().fields[j].dictionary.get()->get_algo();
      if (dict_algo == ShannonBase::Compress::Encoding_type::NONE) return true;
    }
  }

  return false;  // to offload RAPID.
}

std::vector<std::string> Util::split(const std::string &str, char delimiter) {
  std::vector<std::string> tokens;
  size_t start = 0;
  size_t end = str.find(delimiter);

  while (end != std::string::npos) {
    tokens.emplace_back(str.substr(start, end - start));
    start = end + 1;
    end = str.find(delimiter, start);
  }

  tokens.emplace_back(str.substr(start));
  return tokens;
}

uint Util::normalized_length(const Field *field) {
  return (Utils::Util::is_blob(field->type()) || Utils::Util::is_varstring(field->type()) ||
          Utils::Util::is_string(field->type()))
             ? ((field->real_type() == MYSQL_TYPE_ENUM) ? field->pack_length() : sizeof(uint32))
             : field->pack_length();
}

ColumnMapGuard::ColumnMapGuard(TABLE *t, TYPE type) : bit_type(type), table(t) {
  switch (bit_type) {
    case TYPE::READ:
      old_rmap = tmp_use_all_columns(table, table->read_set);
      break;
    case TYPE::WRITE:
      old_wmap = tmp_use_all_columns(table, table->write_set);
      break;
    case TYPE::ALL:
      old_rmap = tmp_use_all_columns(table, table->read_set);
      old_wmap = tmp_use_all_columns(table, table->write_set);
      break;
    default:
      assert(false);
  }
}

ColumnMapGuard::~ColumnMapGuard() {
  switch (bit_type) {
    case TYPE::READ:
      if (old_rmap) tmp_restore_column_map(table->read_set, old_rmap);
      break;
    case TYPE::WRITE:
      if (old_wmap) tmp_restore_column_map(table->write_set, old_wmap);
      break;
    case TYPE::ALL:
      if (old_rmap) tmp_restore_column_map(table->read_set, old_rmap);
      if (old_wmap) tmp_restore_column_map(table->write_set, old_wmap);
      break;
    default:
      break;
  }
}

}  // namespace Utils
}  // namespace ShannonBase