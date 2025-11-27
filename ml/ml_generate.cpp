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

   The ML_GENERATE routine uses the specified large language model (LLM) to
   generate text-based content as a response for the given natural-language
   query.

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates.
*/
#include "ml_generate.h"

#include <chrono>
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

#include "infra_component/llm_generate.h"
#include "storage/rapid_engine/include/rapid_status.h"  //loaded table.

extern char mysql_home[FN_REFLEN];
extern char mysql_llm_home[FN_REFLEN];
namespace ShannonBase {
namespace ML {
std::string ML_generate_row::Generate(std::string &text, Json_wrapper &option) {
  std::string result;

  ShannonBase::ML::OPTION_VALUE_T opt_values;
  std::string keystr;
  if (ShannonBase::ML::Utils::parse_json(option, opt_values, keystr, 0)) {
    std::string err("can not parse the option");
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return result;
  }

  std::string model_name("Llama-3.2-3B-Instruct");  // default model used.
  keystr = "model_id";
  if (opt_values.find(keystr) != opt_values.end()) model_name = opt_values[keystr].size() ? opt_values[keystr][0] : "";

  std::string home_path(mysql_llm_home);
  if (!home_path.length()) {
    home_path.append(mysql_home);
  }

  std::string model_path(home_path);
  model_path.append("llm-models/").append(model_name).append("/onnx/");
  if (!std::filesystem::exists(model_path)) {
    std::string err("can not find the model:");
    err.append(model_name);
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return result;
  }

  std::string token_path(home_path);
  token_path.append("llm-models/").append(model_name).append("/");
  if (!std::filesystem::exists(token_path)) {
    std::string err("can not find the token path:");
    err.append(model_name);
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return result;
  }

  std::string oper_type;
  LLM_Generate::GenerationOptions gen_options;
  keystr = "language";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    std::transform(oper_type.begin(), oper_type.end(), oper_type.begin(), ::tolower);
    gen_options.setLanguage(oper_type);
  }
  gen_options.model_id = model_name;

  gen_options.setModelDefaults(model_name);
  gen_options.optimizeForModelSize();

  // If user specify its value, then use user input value.
  keystr = "task";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    std::transform(oper_type.begin(), oper_type.end(), oper_type.begin(), ::tolower);
    gen_options.task = oper_type;
  }
  keystr = "context";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    std::transform(oper_type.begin(), oper_type.end(), oper_type.begin(), ::tolower);
    gen_options.context = oper_type;
  }

  keystr = "temperature";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    gen_options.temperature = std::atof(oper_type.c_str());
  }

  keystr = "max_tokens";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    gen_options.max_tokens = std::atoi(oper_type.c_str());
  }

  keystr = "top_k";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    gen_options.top_k = std::atoi(oper_type.c_str());
  }

  keystr = "top_p";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    gen_options.top_p = std::atof(oper_type.c_str());
  }

  keystr = "repeat_penalty";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    gen_options.repeat_penalty = std::atof(oper_type.c_str());
  }

  keystr = "presence_penalty";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    gen_options.presence_penalty = std::atof(oper_type.c_str());
  }

  keystr = "frequency_penalty";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    gen_options.frequency_penalty = std::atof(oper_type.c_str());
  }

  keystr = "stop_sequences";
  if (opt_values.find(keystr) != opt_values.end()) {
    gen_options.stop_sequences = opt_values[keystr];
  }

  keystr = "speculative_decoding";
  if (opt_values.find(keystr) != opt_values.end()) {
    oper_type = opt_values[keystr][0];
    std::transform(oper_type.begin(), oper_type.end(), oper_type.begin(), ::tolower);
    gen_options.speculative_decoding = (oper_type == "true") ? true : false;
  }

  auto tg = std::make_unique<LLM_Generate::TextGenerator>(model_path, token_path, gen_options);
  LLM_Generate::TextGenerator::Result gen_res = tg->Generate(text);
  result = gen_res.output;
  return result;
}

std::string ML_generate_table::Generate(std::string &, Json_wrapper &) {
  std::string result;
  return result;
}

}  // namespace ML
}  // namespace ShannonBase