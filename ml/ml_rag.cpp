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
#include "ml_rag.h"

#include <chrono>
#include <filesystem>
#include <string>

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

extern char mysql_home[FN_REFLEN];
extern char mysql_llm_home[FN_REFLEN];
namespace ShannonBase {
namespace ML {
std::string ML_RAG_row::ProcessRAG(const std::string &, const Json_wrapper &options) {
  std::string result;

  Json_wrapper rag_opt = options;
  ShannonBase::ML::OPTION_VALUE_T opt_values;
  std::string keystr;
  if (ShannonBase::ML::Utils::parse_json(rag_opt, opt_values, keystr, 0)) {
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

  return result;
}

std::string ML_RAG_table::ProcessRAG(const std::string &, const Json_wrapper &) {
  std::string result;

  return result;
}
}  // namespace ML
}  // namespace ShannonBase