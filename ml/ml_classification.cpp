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

#include "ml_classification.h"

#include <iostream>
#include <memory>
#include <set>

#include "extra/lightgbm/LightGBM/include/LightGBM/c_api.h"  //LightGBM

#include "include/my_inttypes.h"
#include "include/thr_lock.h"     //TL_READ
#include "sql-common/json_dom.h"  //Json_wrapper.
#include "sql/current_thd.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"   //Field
#include "sql/handler.h"
#include "sql/sql_class.h"  //THD
#include "sql/table.h"

#include "storage/innobase/include/ut0dbg.h"            //for ut_a
#include "storage/rapid_engine/include/rapid_status.h"  //loaded table.

#include "auto_ml.h"
#include "ml_info.h"
#include "ml_utils.h"  //ml utils

namespace ShannonBase {
namespace ML {

ML_classification::ML_classification() {}

ML_classification::~ML_classification() {}

// returuns the # of record read successfully, otherwise 0 read failed.
int ML_classification::read_data(TABLE *table, std::vector<double> &train_data, std::string &label_name,
                                 std::vector<float> &label_data) {
  THD *thd = current_thd;
  auto n_read{0u};

  const dd::Table *table_obj{nullptr};
  const dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  if (!table_obj && table->s->table_category != TABLE_UNKNOWN_CATEGORY) {
    if (thd->dd_client()->acquire(table->s->db.str, table->s->table_name.str, &table_obj)) {
      return n_read;
    }
  }

  // must read from secondary engine.
  unique_ptr_destroy_only<handler> sec_tb_handler(Utils::get_secondary_handler(table));
  /* Read the traning data into train_data vector from rapid engine. here, we use training data
  as lablels too */
  my_bitmap_map *old_map = tmp_use_all_columns(table, table->read_set);
  sec_tb_handler->ha_open(table, table->s->normalized_path.str, O_RDONLY, HA_OPEN_IGNORE_IF_LOCKED, table_obj);
  if (sec_tb_handler && sec_tb_handler->ha_external_lock(thd, F_RDLCK)) {
    sec_tb_handler->ha_close();
    return n_read;
  }

  if (sec_tb_handler->ha_rnd_init(true)) {
    sec_tb_handler->ha_external_lock(thd, F_UNLCK);
    sec_tb_handler->ha_close();
    return n_read;
  }

  // read the training data from target table.
  std::map<std::string, std::set<std::string>> txt2numeric;
  for (auto field_id = 0u; field_id < table->s->fields; field_id++) {
    Field *field_ptr = *(table->field + field_id);
    txt2numeric[field_ptr->field_name];
  }

  while (sec_tb_handler->ha_rnd_next(table->record[0]) == 0) {
    for (auto field_id = 0u; field_id < table->s->fields; field_id++) {
      Field *field_ptr = *(table->field + field_id);
      // if (field_ptr->is_real_null())
      auto data_val{0.0};
      String buf;
      my_decimal dval;
      switch (field_ptr->type()) {
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
          data_val = field_ptr->val_real();
          break;
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
          field_ptr->val_decimal(&dval);
          my_decimal2double(10, &dval, &data_val);
          break;
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:  // convert txt string to numeric
          buf.set_charset(field_ptr->charset());
          field_ptr->val_str(&buf);
          txt2numeric[field_ptr->field_name].insert(buf.c_ptr_safe());
          assert(txt2numeric[field_ptr->field_name].find(buf.c_ptr_safe()) != txt2numeric[field_ptr->field_name].end());
          data_val = std::distance(txt2numeric[field_ptr->field_name].begin(),
                                   txt2numeric[field_ptr->field_name].find(buf.c_ptr_safe()));
          break;
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIME:
          data_val = field_ptr->val_real();
        default:
          break;
      }

      if (likely(strcmp(field_ptr->field_name, label_name.c_str()))) {
        train_data.push_back(data_val);
      } else {  // is label data.
        label_data.push_back(data_val);
      }
    }  // for
    n_read++;
  }  // while

  if (old_map) tmp_restore_column_map(table->read_set, old_map);

  sec_tb_handler->ha_rnd_end();
  sec_tb_handler->ha_external_lock(thd, F_UNLCK);
  // to close the secondary engine table.
  sec_tb_handler->ha_close();

  return n_read;
}

int ML_classification::train() {
  THD *thd = current_thd;
  std::string user_name(thd->security_context()->user().str);
  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " NOT loaded into rapid engine for ML";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto source_table_ptr = Utils::open_table_by_name(m_sch_name, m_table_name, TL_READ);
  if (!source_table_ptr) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " open failed for ML";
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  std::vector<double> train_data;
  std::vector<float> label_data;
  auto n_sample = read_data(source_table_ptr, train_data, m_target_name, label_data);
  Utils::close_table(source_table_ptr);

  // if it's a multi-target, then minus the size of target columns.
  auto n_feature = source_table_ptr->s->fields - 1;
  std::string mode_params =
      "task = train "
      "objective = binary "
      "metric = auc "
      "num_leaves = 31 "
      "learning_rate = 0.1 "
      "num_iterations = 100";
  std::string model_content;
  // clang-format off
  if (!(m_handler = Utils::ML_train(mode_params,
                                    C_API_DTYPE_FLOAT64,
                                    reinterpret_cast<const void *>(train_data.data()),
                                    n_sample,
                                    n_feature,
                                    C_API_DTYPE_FLOAT32,
                                    reinterpret_cast<const void *>(label_data.data()),
                                    model_content)))
    return HA_ERR_GENERIC;

  // the definition of this table, ref: `ml_train.sql`
  std::string mode_type("classification"), oper_type("train");
  if (Utils::store_model_catalog(mode_type,
                                 oper_type,
                                 m_options,
                                 user_name,
                                 m_handler_name,
                                 model_content,
                                 m_target_name,
                                 m_table_name))
    return HA_ERR_GENERIC;
  // clang-format on
  return 0;
}

int ML_classification::predict() { return 0; }

// load the model from model_content.
int ML_classification::load(std::string &model_content) {
  assert(model_content.length() && m_handler_name.length());

  // insert the model content into the loaded map.
  Loaded_models[m_handler_name] = model_content;
  return 0;
}

int ML_classification::load_from_file(std::string modle_file_full_path [[maybe_unused]],
                                      std::string model_handle_name [[maybe_unused]]) {
  return 0;
}

int ML_classification::unload(std::string model_handle_name [[maybe_unused]]) { return 0; }

int ML_classification::import(std::string model_handle_name [[maybe_unused]], std::string user_name [[maybe_unused]],
                              std::string &content [[maybe_unused]]) {
  return 0;
}

double ML_classification::score() { return 0; }

int ML_classification::explain_row() { return 0; }

int ML_classification::explain_table() { return 0; }

int ML_classification::predict_row() { return 0; }

int ML_classification::predict_table() { return 0; }

ML_TASK_TYPE ML_classification::type() { return ML_TASK_TYPE::CLASSIFICATION; }

}  // namespace ML
}  // namespace ShannonBase