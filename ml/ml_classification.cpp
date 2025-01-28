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
#include <vector>

#include "include/my_inttypes.h"
#include "include/thr_lock.h"     //TL_READ
#include "sql-common/json_dom.h"  //Json_wrapper.
#include "sql/current_thd.h"
#include "sql/derror.h"  //ER_TH
#include "sql/field.h"   //Field
#include "sql/handler.h"
#include "sql/mysqld.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"  //THD
#include "sql/table.h"

#include "extra/lightgbm/LightGBM/include/LightGBM/c_api.h"  //LightGBM
#include "ml_utils.h"                                        //ml utils
#include "storage/innobase/include/ut0dbg.h"                 //for ut_a
#include "storage/rapid_engine/include/rapid_status.h"       //loaded table.

namespace ShannonBase {
namespace ML {

ML_classification::ML_classification() {}

ML_classification::~ML_classification() {}

int ML_classification::train() {
#if 0
  THD *thd = current_thd;
  std::string user_name(thd->security_context()->user().str);
  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " NOT loaded into rapid engine.";
    my_error(ER_SECONDARY_ENGINE_DDL, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  TABLE *source_table_ptr{nullptr};
  if (Utils::open_table_by_name(m_sch_name, m_table_name, TL_READ, &source_table_ptr)) return HA_ERR_GENERIC;

  // create sample data set
  //auto n_sample = source_table_ptr->file->stats.records;
  //auto n_feature = source_table_ptr->s->fields;

  //the label of sample data.
  std::vector<const char *> feature_name_vec(n_feature);
  for (auto idx = 0u; idx < n_feature; idx++) {
    auto field_ptr = *(source_table_ptr->s->field + idx);
    feature_name_vec.emplace_back(field_ptr->field_name);
  }

  unique_ptr_destroy_only<handler> tb_handler(Utils::get_secondary_handler(source_table_ptr));
  /* Read the traning data into train_data vector from rapid engine. here, we use training data
  as lablels too */
  my_bitmap_map *old_map = tmp_use_all_columns(source_table_ptr, source_table_ptr->read_set);
  tb_handler->ha_open(source_table_ptr, source_table_ptr->s->table_name.str, O_RDONLY, HA_OPEN_IGNORE_IF_LOCKED,
                      source_table_ptr->s->tmp_table_def);
  if (tb_handler && tb_handler->ha_external_lock(thd, F_RDLCK)) return HA_ERR_GENERIC;
  if (tb_handler->ha_rnd_init(true)) {
    return HA_ERR_GENERIC;
  }
#endif
  return 0;
}

int ML_classification::predict() { return 0; }

int ML_classification::load(std::string &model_content [[maybe_unused]]) { return 0; }

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