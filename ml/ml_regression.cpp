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
#include "ml_regression.h"

#include <string>

#include "include/thr_lock.h" //TL_READ
#include "include/my_inttypes.h"

#include "sql/current_thd.h"
#include "sql/sql_base.h"
#include "sql/table.h"
#include "sql/handler.h"

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
// clang-format off
//if move this head file to upperstair, will issue an `unlikely` error.
#include "lightgbm/include/LightGBM/application.h"
// clang-format on

namespace ShannonBase {
namespace ML {

ML_regression::ML_regression(std::string sch_name, std::string table_name,
                             std::string target_name) : m_sch_name(sch_name),
                                                        m_table_name(table_name),
                                                        m_target_name(target_name) {
}

ML_regression::~ML_regression() {
}

int ML_regression::train() {
  //will moved to my.cnf
  const char* config[1] = {"train.conf"};
  m_app = std::make_unique<LightGBM::Application>(1, const_cast<char**>(config));

  THD* thd = current_thd;
  Open_table_context table_ctx(thd, 0);
  thr_lock_type lock_mode (TL_READ);

  auto share = ShannonBase::shannon_loaded_tables->get(m_sch_name.c_str(), m_table_name.c_str());
  if (!share) {
    std::ostringstream err;
    err << m_sch_name.c_str() << "." << m_table_name.c_str() << " NOT loaded into rapid engine.";
    my_error(0, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  Table_ref table_ref(m_sch_name.c_str(), strlen(m_sch_name.c_str()),
                   m_table_name.c_str(), strlen( m_table_name.c_str()),
                   m_table_name.c_str(), lock_mode);

  MDL_REQUEST_INIT(&table_ref.mdl_request, MDL_key::TABLE,
                  m_sch_name.c_str(),  m_table_name.c_str(),
                  (lock_mode > TL_READ) ? MDL_SHARED_WRITE : MDL_SHARED_READ,
                  MDL_TRANSACTION);

  if (open_table(thd, &table_ref, &table_ctx)) {
    return HA_ERR_GENERIC;
  }

  //read the traning data from rapid engine, then do train.
  auto tb_handler = table_ref.table->file;
  if (!tb_handler) return 1;

  if (tb_handler->inited == handler::NONE && tb_handler->ha_rnd_init(true)) {
    return HA_ERR_GENERIC;
  }
  //table full scan to train the model.
  while (tb_handler->ha_rnd_next(table_ref.table->record[0])) {
  }

  tb_handler->ha_rnd_end();
  return 0;
}

int ML_regression::predict() {
  return 0;
}

} //ML
} //shannonbase