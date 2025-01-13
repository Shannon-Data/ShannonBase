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

#include "sql/join_optimizer/finalize_plan.h"
#include "sql/my_decimal.h"  //my_decimal
#include "sql/opt_trace.h"
#include "sql/sql_class.h"     //Secondary_engine_statement_context
#include "sql/sql_executor.h"  //QEP_TBA
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"  //JOIN
#include "sql/table.h"          //TABLE

#include "storage/innobase/include/ut0dbg.h"  //ut_a

#include "storage/rapid_engine/compress/dictionary/dictionary.h"  //Dictionary
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/ml/ml.h"
#include "storage/rapid_engine/populate/populate.h"

namespace ShannonBase {
namespace Utils {

std::map<std::string, std::unique_ptr<Compress::Dictionary>> loaded_dictionaries;
bool Util::is_support_type(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_TYPED_ARRAY:
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET: {
      return false;
    } break;
    default:
      return true;
  }
  return false;
}

double Util::get_field_value(Field *&field, Compress::Dictionary *&dictionary) {
  ut_ad(field && dictionary);
  double data_val{0};
  if (!field->is_real_null()) {  // not null
    switch (field->type()) {
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_VARCHAR: {
        String buf;
        buf.set_charset(field->charset());
        field->val_str(&buf);
        data_val = dictionary->store((uchar *)buf.c_ptr(), buf.length());
      } break;
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE: {
        data_val = field->val_real();
      } break;
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL: {
        my_decimal dval;
        field->val_decimal(&dval);
        my_decimal2double(10, &dval, &data_val);
      } break;
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIME: {
        data_val = field->val_real();
      } break;
      default:
        data_val = field->val_real();
    }
  }
  return data_val;
}

double Util::get_field_value(enum_field_types type, const uchar *buf, uint len, Compress::Dictionary *dictionary,
                             CHARSET_INFO *charset) {
  ut_ad(buf && dictionary);
  double data_val{0};
  switch (type) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR: {
      data_val = dictionary->store(buf, len);
    } break;
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      data_val = *(int *)buf;
      break;
    case MYSQL_TYPE_FLOAT:
      data_val = *(float *)buf;
      break;
    case MYSQL_TYPE_DOUBLE:
      data_val = *(double *)buf;
      break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      const int prec = 60;
      const int scale = 0;
      my_decimal dv;
      auto ret = binary2my_decimal(E_DEC_FATAL_ERROR & ~E_DEC_OVERFLOW, buf, &dv, prec, scale);
      if (!ret) {
        my_decimal2double(0, &dv, &data_val);
      }
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIME: {
      MYSQL_TIME ltime;
      TIME_from_longlong_datetime_packed(&ltime, my_datetime_packed_from_binary(buf, 0));
      data_val = TIME_to_ulonglong_datetime(ltime);
    } break;
    default:
      ut_a(false);
  }

  return data_val;
}

int Util::get_range_value(enum_field_types type, Compress::Dictionary *&dictionary, key_range *min_key,
                          key_range *max_key, double &minkey, double &maxkey) {
  switch (type) {
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT: {
      minkey = min_key ? *(int *)min_key->key : SHANNON_LOWEST_DOUBLE;
      maxkey = max_key ? *(int *)max_key->key : SHANNON_LOWEST_DOUBLE;
    } break;
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      minkey = min_key ? *(int *)min_key->key : SHANNON_LOWEST_DOUBLE;
      maxkey = max_key ? *(int *)max_key->key : SHANNON_LOWEST_DOUBLE;
    } break;
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_FLOAT: {
      minkey = min_key ? *(double *)min_key->key : SHANNON_LOWEST_DOUBLE;
      maxkey = max_key ? *(double *)max_key->key : SHANNON_LOWEST_DOUBLE;
    } break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      minkey = min_key ? *(double *)min_key->key : SHANNON_LOWEST_DOUBLE;
      maxkey = max_key ? *(double *)max_key->key : SHANNON_LOWEST_DOUBLE;
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIME2: {
      minkey = maxkey = SHANNON_LOWEST_DOUBLE;
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
  return 0;
}

int Util::mem2string(uchar *buff, uint length, std::string &result) {
  const char *data = static_cast<const char *>((char *)buff);
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');

  for (size_t i = 0; i < length; ++i) {
    oss << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(data[i]));
  }
  result = oss.str();
  return 0;
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

static inline void write_trace_reason(THD *thd, const char *text, const char *reason) {
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
  std::ostringstream oss;
  std::string text;
  if (stmt_context->get_primary_cost() > thd->variables.secondary_engine_cost_threshold) {
    oss << "The estimated query cost does exceed secondary_engine_cost_threshold, goes to secondary engine.";
    oss << "cost: " << thd->m_current_query_cost << ", threshold: " << thd->variables.secondary_engine_cost_threshold;
    text = "secondary_engine_used";
    where = ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_SECONDARY;
  } else {
    oss << "The estimated query cost does not exceed secondary_engine_cost_threshold, goes to primary engine.";
    oss << "cost: " << thd->m_current_query_cost << ", threshold: " << thd->variables.secondary_engine_cost_threshold;
    text = "secondary_engine_not_used";
    where = ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_PRIMARY;
  }

  write_trace_reason(thd, text.c_str(), oss.str().c_str());

  return (where == ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_SECONDARY) ? true : false;
}

//  decision tree classifier for determining which engine should to go.
// returns true goes to secondary engine, otherwise, false go to innodb.
bool Util::decision_tree_classifier(THD *thd) {
  std::ostringstream oss;
  std::string text;
  // here to use trained decision tree to classify the query.
  if (is_very_fast_query(thd)) {
    text = "secondary_engine_not_used";
    oss << "The estimated query is a very fast query, goes to primary engine.";
    write_trace_reason(thd, text.c_str(), oss.str().c_str());
    return false;
  }

  ShannonBase::ML::Query_arbitrator qa;
  qa.load_model();
  auto where = qa.predict(thd->lex->unit->first_query_block()->join);
  if (where == ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_SECONDARY) {
    text = "secondary_engine_used";
    oss << "The Query_arbitrator do the prediction, goes to secondary engine.";
  } else {
    text = "secondary_engine_not_used";
    oss << "The Query_arbitrator do the prediction, goes to primary engine.";
  }
  write_trace_reason(thd, text.c_str(), oss.str().c_str());

  return (where == ShannonBase::ML::Query_arbitrator::WHERE2GO::TO_SECONDARY) ? true : false;
}

// dynamic feature normalization for determining which engine should to go.
// returns true goes to secondary engine, otherwise, false go to innodb.
bool Util::dynamic_feature_normalization(THD *thd) {
  auto stmt_context = thd->secondary_engine_statement_context();
  assert(stmt_context);

  // If queue is too long or CP is too long, this mechanism wants to progressively start
  // shifting queries to mysql, moving gradually towards the heavier queries
  if (ShannonBase::Populate::Populator::log_pop_thread_is_active() &&
      ShannonBase::Populate::sys_pop_buff.size() * ShannonBase::SHANNON_TO_MUCH_POP_THRESHOLD_RATIO >
          ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE) {
    return false;
  }

  // to checkts whether query involves tables are still in pop queue. if yes, go innodb.
  for (auto &table_ref : stmt_context->get_query_tables()) {
    std::string table_name(table_ref->db);
    table_name += ":";
    table_name += table_ref->table_name;
    if (ShannonBase::Populate::Populator::check_population_status(table_name)) return false;
  }

  return false;
}

// check whether the dictionary encoding projection is supported or not.
// returns true if supported to innodb, otherwise, false to secondary engine.
bool Util::check_dict_encoding_projection(THD *thd) {
  auto imcs_instance = ShannonBase::Imcs::Imcs::instance();
  if (!imcs_instance) return true;

  std::string key_part;
  auto table_ref = thd->lex->unit->first_query_block()->leaf_tables;
  for (; table_ref; table_ref = table_ref->next_leaf) {
    if (table_ref->is_view_or_derived()) continue;

    key_part += table_ref->db;
    key_part += ":";
    key_part += table_ref->table_name;
    key_part += ":";

    for (auto j = 0u; j < table_ref->table->s->fields; j++) {
      auto field_ptr = *(table_ref->table->field + j);
      if (field_ptr->is_flag_set(NOT_SECONDARY_FLAG)) continue;

      std::string key = key_part + field_ptr->field_name;
      auto cu_header = imcs_instance->get_cus()[key]->header();
      assert(cu_header);
      // to test all cu infos.
    }
  }

  return false;
}

}  // namespace Utils
}  // namespace ShannonBase