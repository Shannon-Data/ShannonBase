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

   The fundmental code for ML.

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/

#include "ml_topic_modeling.h"

#include <chrono>
#include <sstream>

#include "include/my_base.h"
#include "include/mysqld_error.h"
#include "sql/field.h"
#include "sql/table.h"

#include "ml_info.h"
#include "ml_utils.h"
#include "storage/rapid_engine/include/rapid_config.h"

namespace ShannonBase {
namespace ML {

int ML_topic_modeling::train(THD *, Json_wrapper &model_object, Json_wrapper &model_metadata) {
  // Topic modeling requires document_column option
  OPTION_VALUE_T options;
  std::string keystr;
  if (!m_options.empty() && Utils::parse_json(m_options, options, keystr, 0)) return HA_ERR_GENERIC;

  std::string document_column;
  if (options.find(ML_KEYWORDS::document_column) != options.end())
    document_column = options[ML_KEYWORDS::document_column][0];

  if (document_column.empty()) {
    my_error(ER_ML_FAIL, MYF(0), "topic_modeling requires 'document_column' option");
    return HA_ERR_GENERIC;
  }

  // Validate table is loaded
  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name << "." << m_table_name << " NOT loaded into rapid engine";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto source_table_ptr = Utils::open_table_by_name(m_sch_name, m_table_name, TL_READ);
  if (!source_table_ptr) {
    std::ostringstream err;
    err << m_sch_name << "." << m_table_name << " open failed for ML";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  // Validate table size
  if (Utils::validate_table_size(source_table_ptr)) {
    Utils::close_table(source_table_ptr);
    return HA_ERR_GENERIC;
  }

  // Validate document column exists and is text type
  bool doc_col_found = false;
  for (uint i = 0; i < source_table_ptr->s->fields; i++) {
    Field *field = source_table_ptr->field[i];
    if (field->field_name == document_column) {
      doc_col_found = true;
      if (field->type() != MYSQL_TYPE_VARCHAR && field->type() != MYSQL_TYPE_VAR_STRING &&
          field->type() != MYSQL_TYPE_STRING && field->type() != MYSQL_TYPE_BLOB &&
          field->type() != MYSQL_TYPE_MEDIUM_BLOB && field->type() != MYSQL_TYPE_LONG_BLOB &&
          field->type() != MYSQL_TYPE_TINY_BLOB) {
        std::ostringstream err;
        err << "document_column '" << document_column << "' must be a text/string type";
        my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
        Utils::close_table(source_table_ptr);
        return HA_ERR_GENERIC;
      }
      break;
    }
  }
  if (!doc_col_found) {
    std::ostringstream err;
    err << "document_column '" << document_column << "' not found in table";
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    Utils::close_table(source_table_ptr);
    return HA_ERR_GENERIC;
  }

  // Read all data
  std::vector<double> train_data;
  std::vector<float> label_data;
  std::vector<std::string> features_name;
  int n_class{0};
  txt2numeric_map_t txt2num_dict;
  std::string empty_target;
  std::vector<std::string> include_cols, exclude_cols;
  auto n_sample = Utils::read_data(source_table_ptr, train_data, features_name, empty_target, label_data, n_class,
                                   txt2num_dict, &include_cols, &exclude_cols);
  Utils::close_table(source_table_ptr);

  if (n_sample == 0) {
    std::ostringstream err;
    err << "No data read from " << m_sch_name << "." << m_table_name;
    my_error(ER_ML_FAIL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  // Build metadata
  std::ostringstream oss;
  oss << m_sch_name << "." << m_table_name;
  std::string sch_tb_name(oss.str()), notes_str, opt_metrics, empty_str;

  auto start = std::chrono::steady_clock::now();
  auto end = std::chrono::steady_clock::now();
  auto train_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0f;

  auto content_dom = Json_dom::parse(
      "{}", 2, [](const char *, size_t) { assert(false); }, [] { assert(false); });
  if (!content_dom.get()) return HA_ERR_GENERIC;
  model_object = Json_wrapper(std::move(content_dom));

  // Build topic modeling params JSON string
  std::string training_params = "{\"document_column\":\"" + document_column + "\"}";

  auto meta_json = Utils::build_up_model_metadata(
      TASK_NAMES_MAP[type()], empty_str, sch_tb_name, features_name, nullptr, notes_str,
      MODEL_FORMATS_MAP[MODEL_FORMAT_T::VER_2], MODEL_STATUS_MAP[MODEL_STATUS_T::READY],
      MODEL_QUALITIES_MAP[MODEL_QUALITY_T::HIGH], train_duration, TASK_NAMES_MAP[type()], 0, n_sample,
      features_name.size(), n_sample, features_name.size(), opt_metrics, features_name, 0, &m_options, training_params,
      nullptr, nullptr, nullptr, 1, txt2num_dict);

  model_metadata = Json_wrapper(meta_json);
  return 0;
}

int ML_topic_modeling::load(THD *, std::string &model_content) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(model_content.length() && m_handler_name.length());
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_topic_modeling::load_from_file(THD *, std::string &model_file_full_path, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  if (!model_file_full_path.length() || !model_handle_name.length()) return HA_ERR_GENERIC;
  Loaded_models[model_handle_name] = Utils::read_file(model_file_full_path);
  return 0;
}

int ML_topic_modeling::unload(THD *, std::string &model_handle_name) {
  std::lock_guard<std::mutex> lock(models_mutex);
  assert(!Loaded_models.empty());
  auto cnt = Loaded_models.erase(model_handle_name);
  return (cnt == 1) ? 0 : HA_ERR_GENERIC;
}

int ML_topic_modeling::import(THD *, Json_wrapper &, Json_wrapper &, std::string &) {
  assert(false);
  return 0;
}

double ML_topic_modeling::score(THD *, std::string &, std::string &, std::string &, std::string &, Json_wrapper &) {
  return 0;
}

int ML_topic_modeling::explain(THD *, std::string &, std::string &, std::string &, Json_wrapper &) { return 0; }
int ML_topic_modeling::explain_row(THD *, Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) { return 0; }
int ML_topic_modeling::explain_table(THD *) { return 0; }
int ML_topic_modeling::predict_row(THD *, Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) { return 0; }
int ML_topic_modeling::predict_table(THD *, std::string &, std::string &, std::string &, Json_wrapper &) { return 0; }

}  // namespace ML
}  // namespace ShannonBase
