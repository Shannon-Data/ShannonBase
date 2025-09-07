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
   To uses the specified embedding model to encode the specified text or query
   into a vector embedding.

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/
#include "ml_embedding.h"

#include <chrono>
#include <ostream>
#include <string>

#include "include/field_types.h"  //MYSQL_TYPE_VECTOR
#include "include/my_inttypes.h"
#include "include/thr_lock.h"  //TL_READ
#include "sql/current_thd.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"   //Field
#include "sql/handler.h"
#include "sql/mysqld.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"  //THD
#include "sql/table.h"

#include "ml_info.h"
#include "ml_utils.h"                         //ml utils
#include "storage/innobase/include/ut0dbg.h"  //for ut_a

#include "storage/rapid_engine/include/rapid_status.h"  //loaded table.

extern char mysql_home[FN_REFLEN];
extern char mysql_llm_home[FN_REFLEN];
namespace ShannonBase {
namespace ML {

ML_embedding_row::ML_embedding_row() {}

ML_embedding::EmbeddingVector ML_embedding_row::GenerateEmbedding(std::string &text, Json_wrapper &option) {
  ML_embedding::EmbeddingVector result;

  ShannonBase::ML::OPTION_VALUE_T opt_values;
  std::string keystr;
  if (ShannonBase::ML::Utils::parse_json(option, opt_values, keystr, 0)) {
    std::string err("can not parse the option");
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return result;
  }

  std::string model_name("all-MiniLM-L12-v2");
  keystr = "model_id";
  if (opt_values.find(keystr) != opt_values.end()) model_name = opt_values[keystr].size() ? opt_values[keystr][0] : "";

  std::string path_path(mysql_llm_home);
  if (!path_path.length()) {
    path_path.append(mysql_home);
  }
  path_path.append("/llm-models/").append(model_name).append("/onnx/");
  if (!std::filesystem::exists(path_path)) {
    std::string err("can not find the model:");
    err.append(model_name);
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return result;
  }
  SentenceTransform::DocumentEmbeddingManager doc_manger(path_path);
  if (doc_manger.ProcessText(text, 1024)) {
    return result;
  }

  auto embeded_res = doc_manger.Results();
  if (!embeded_res.size()) {
    return result;
  }

  result = std::move(embeded_res[0].embedding);
  return result;
}

int ML_embedding_row::GenerateTableEmbedding(std::string &, std::string &, Json_wrapper &) {
  assert(false);
  return 0;
}

ML_embedding_table::ML_embedding_table() {}

ML_embedding::EmbeddingVector ML_embedding_table::GenerateEmbedding(std::string &, Json_wrapper &) {
  ML_embedding::EmbeddingVector result;
  return result;
}

int ML_embedding_table::GenerateTableEmbedding(std::string &InputTableColumn, std::string &OutputTableColumn,
                                               Json_wrapper &option) {
  std::string model_name;
  bool truncate{false};
  int batch_size{0};
  std::string details_column;

  ShannonBase::ML::OPTION_VALUE_T opt_values;
  std::string keystr;
  ShannonBase::ML::Utils::parse_json(option, opt_values, keystr, 0);

  keystr = "model_id";
  if (opt_values.find(keystr) != opt_values.end()) model_name = opt_values[keystr].size() ? opt_values[keystr][0] : "";

  keystr = "truncate";
  if (opt_values.find(keystr) != opt_values.end()) {
    auto truncate_str = opt_values[keystr].size() ? opt_values[keystr][0] : "";
    truncate = (truncate_str.length()) ? ((truncate_str == "true") ? true : false) : false;
  }

  keystr = "batch_size";
  if (opt_values.find(keystr) != opt_values.end()) {
    auto batch_size_str = opt_values[keystr].size() ? opt_values[keystr][0] : "";
    batch_size = (batch_size_str.length()) ? std::stoi(batch_size_str) : 0;
  }

  keystr = "details_column";
  if (opt_values.find(keystr) != opt_values.end())
    details_column = opt_values[keystr].size() ? opt_values[keystr][0] : "";

  size_t firstDot = InputTableColumn.find('.');
  size_t secondDot = InputTableColumn.find('.', firstDot + 1);
  std::string db, table, column;
  if (firstDot != std::string::npos && secondDot != std::string::npos) {
    db = InputTableColumn.substr(0, firstDot);
    table = InputTableColumn.substr(firstDot + 1, secondDot - firstDot - 1);
    column = InputTableColumn.substr(secondDot + 1);
  } else {
    // Handle invalid format - you might want to throw an exception instead
    db = InputTableColumn;  // Default to treating entire string as DBName
    table = "";
    column = "";
  }

  firstDot = OutputTableColumn.find('.');
  secondDot = OutputTableColumn.find('.', firstDot + 1);
  std::string odb, otable, ocolumn;
  if (firstDot != std::string::npos && secondDot != std::string::npos) {
    odb = OutputTableColumn.substr(0, firstDot);
    otable = OutputTableColumn.substr(firstDot + 1, secondDot - firstDot - 1);
    ocolumn = OutputTableColumn.substr(secondDot + 1);
  } else {
    // Handle invalid format - you might want to throw an exception instead
    otable = OutputTableColumn;  // Default to treating entire string as DBName
    otable = "";
    ocolumn = "";
  }

  auto source_table_ptr = Utils::open_table_by_name(db, table, thr_lock_type::TL_READ);
  if (!source_table_ptr) return HA_ERR_GENERIC;
  TableGuard source_tb(source_table_ptr);
  Field *field_ptr{nullptr};
  bool found{false};
  for (auto index = 0u; source_table_ptr->s->fields; index++) {
    field_ptr = *(source_table_ptr->field + index);
    if (field_ptr->field_name != column) continue;
    found = true;
  }
  if (found) {
    if (field_ptr->type() != MYSQL_TYPE_VARCHAR || field_ptr->type() != MYSQL_TYPE_STRING ||
        field_ptr->type() != MYSQL_TYPE_VAR_STRING) {
      std::string err("Target colummn Type is NOT supported");
      my_error(ER_ML_FAIL, MYF(0), err.c_str());
      return HA_ERR_GENERIC;  // the target column is not vector type.
    }
  } else {
    std::string err("Target colummn you specified NOT exists");
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return HA_ERR_GENERIC;  // the target column is not exists.
  }

  auto target_table_ptr = Utils::open_table_by_name(odb, otable, thr_lock_type::TL_WRITE);
  if (!target_table_ptr) return HA_ERR_GENERIC;
  TableGuard target_tb(target_table_ptr);
  field_ptr = nullptr;
  found = false;
  for (auto index = 0u; target_table_ptr->s->fields; index++) {
    field_ptr = *(target_table_ptr->field + index);
    if (field_ptr->field_name != ocolumn) continue;
    found = true;
  }

  if (found) {
    if (field_ptr->type() != MYSQL_TYPE_VECTOR) {
      std::string err("Target colummn Type is NOT vector");
      my_error(ER_ML_FAIL, MYF(0), err.c_str());
      return HA_ERR_GENERIC;  // the target column is not vector type.
    }
  } else {
    std::string err("Target colummn you specified NOT exists");
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return HA_ERR_GENERIC;  // the target column is not exists.
  }

  if (source_table_ptr->file->inited == handler::NONE && source_table_ptr->file->ha_rnd_init(true)) {
    source_table_ptr->file->ha_rnd_end();
    return HA_ERR_GENERIC;
  }

  int tmp{HA_ERR_GENERIC};
  while ((tmp = source_table_ptr->file->ha_rnd_next(source_table_ptr->record[0])) != HA_ERR_END_OF_FILE) {
  }

  return 0;
}

}  // namespace ML
}  // namespace ShannonBase