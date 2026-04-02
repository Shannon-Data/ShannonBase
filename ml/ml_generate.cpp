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
#include <map>
#include <string>

#include "include/my_inttypes.h"
#include "include/mysqld_error.h"
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

extern char mysql_home[FN_REFLEN];
extern char mysql_llm_home[FN_REFLEN];

namespace ShannonBase {
namespace ML {
std::string ML_generate_table::Generate(std::string &, Json_wrapper &) {
  std::string result;
  return result;
}

std::string ML_generate_row::Generate(std::string &text, Json_wrapper &option) {
  std::string result;
  ShannonBase::ML::OPTION_VALUE_T opt_values;
  std::string keystr;
  if (ShannonBase::ML::Utils::parse_json(option, opt_values, keystr, 0)) {
    my_error(ER_ML_FAIL, MYF(0), "can not parse the option");
    return result;
  }

  std::string model_name("Llama-3.2-3B-Instruct");  // default model used.
  keystr = "model_id";
  if (opt_values.find(keystr) != opt_values.end()) model_name = opt_values[keystr].size() ? opt_values[keystr][0] : "";

  std::string home_path(mysql_llm_home);
  if (!home_path.length()) home_path.append(mysql_home);

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

  std::string task_str("generation");
  keystr = "task";
  if (opt_values.find(keystr) != opt_values.end() && !opt_values[keystr].empty()) {
    task_str = opt_values[keystr][0];
    std::transform(task_str.begin(), task_str.end(), task_str.begin(), ::tolower);
  }

  if (task_str == "pii_detect" || task_str == "pii_mask")  // PII detection or masking task
    return pii_task(text, opt_values, task_str, model_path, token_path);
  else  // Text generation task
    return text_generation_task(text, opt_values, task_str, model_path, token_path);
  return result;
}

std::string ML_generate_row::text_generation_task(const std::string &text,
                                                  const ShannonBase::ML::OPTION_VALUE_T &opt_values,
                                                  const std::string &task_str, const std::string &model_path,
                                                  const std::string &token_path) {
  auto get_opt = [&](const std::string &key) -> std::string {
    auto it = opt_values.find(key);
    return (it != opt_values.end() && !it->second.empty()) ? it->second[0] : "";
  };

  LLM_Generate::GenerationOptions gen_options;

  if (std::string lang = get_opt("language"); !lang.empty()) {
    std::transform(lang.begin(), lang.end(), lang.begin(), ::tolower);
    gen_options.setLanguage(lang);
  }

  gen_options.model_id = get_opt("model_id");
  gen_options.setModelDefaults(gen_options.model_id);
  gen_options.optimizeForModelSize();
  gen_options.task = task_str;

  if (std::string ctx = get_opt("context"); !ctx.empty()) {
    std::transform(ctx.begin(), ctx.end(), ctx.begin(), ::tolower);
    gen_options.context = ctx;
  }

  if (std::string v = get_opt("temperature"); !v.empty()) gen_options.temperature = std::atof(v.c_str());
  if (std::string v = get_opt("max_tokens"); !v.empty()) gen_options.max_tokens = std::atoi(v.c_str());
  if (std::string v = get_opt("top_k"); !v.empty()) gen_options.top_k = std::atoi(v.c_str());
  if (std::string v = get_opt("top_p"); !v.empty()) gen_options.top_p = std::atof(v.c_str());
  if (std::string v = get_opt("repeat_penalty"); !v.empty()) gen_options.repeat_penalty = std::atof(v.c_str());
  if (std::string v = get_opt("presence_penalty"); !v.empty()) gen_options.presence_penalty = std::atof(v.c_str());
  if (std::string v = get_opt("frequency_penalty"); !v.empty()) gen_options.frequency_penalty = std::atof(v.c_str());

  if (auto it = opt_values.find("stop_sequences"); it != opt_values.end()) gen_options.stop_sequences = it->second;

  if (std::string v = get_opt("speculative_decoding"); !v.empty()) {
    std::transform(v.begin(), v.end(), v.begin(), ::tolower);
    gen_options.speculative_decoding = (v == "true");
  }

  auto tg = std::make_unique<LLM_Generate::TextGenerator>(model_path, token_path, gen_options);
  return tg->Generate(text).output;
}

std::string ML_generate_row::pii_task(const std::string &text, const ShannonBase::ML::OPTION_VALUE_T &opt_values,
                                      const std::string &task_str, const std::string &model_path,
                                      const std::string &token_path) {
  std::string result;

  PIIDetector::Options det_opts;
  det_opts.model_id = "multilang-pii-ner-ONNX";  // default PII detection model
  auto mid_it = opt_values.find("model_id");
  if (mid_it != opt_values.end() && !mid_it->second.empty()) det_opts.model_id = mid_it->second[0];

  auto fmt_it = opt_values.find("output_format");
  if (fmt_it != opt_values.end() && !fmt_it->second.empty()) det_opts.output_format = fmt_it->second[0];

  auto lbl_it = opt_values.find("label_names");
  if (lbl_it != opt_values.end() && !lbl_it->second.empty()) det_opts.label_names = lbl_it->second;

  auto conf_it = opt_values.find("min_confidence");
  if (conf_it != opt_values.end() && !conf_it->second.empty())
    det_opts.min_confidence = static_cast<float>(std::atof(conf_it->second[0].c_str()));

  auto detector = std::make_unique<PIIDetector>(model_path, token_path, det_opts);

  if (!detector->Initialized()) {
    std::string err("PII model failed to initialize: ");
    err.append(det_opts.model_id);
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return result;
  }

  if (task_str == "pii_detect") {
    result = detector->Detect(text);
  } else {
    // pii_mask
    PIIDetector::MaskingOptions mask_opts = parse_masking_options(opt_values);
    result = detector->Mask(text, mask_opts);
  }

  return result;
}

PIIDetector::MaskingOptions ML_generate_row::parse_masking_options(const ShannonBase::ML::OPTION_VALUE_T &opt_values) {
  PIIDetector::MaskingOptions opts;

  auto policy_it = opt_values.find("policy");
  if (policy_it == opt_values.end() || policy_it->second.empty()) {
    // Default: replace_with_type
    opts.policy = PIIDetector::MaskingPolicy::REPLACE_WITH_TYPE;
    return opts;
  }

  std::string policy_str = policy_it->second[0];
  std::transform(policy_str.begin(), policy_str.end(), policy_str.begin(), ::tolower);

  if (policy_str == "replace_with_type") {
    opts.policy = PIIDetector::MaskingPolicy::REPLACE_WITH_TYPE;
  } else if (policy_str == "replace_with_mask") {
    opts.policy = PIIDetector::MaskingPolicy::REPLACE_WITH_MASK;
  } else if (policy_str == "hash") {
    opts.policy = PIIDetector::MaskingPolicy::HASH;
  } else if (policy_str == "preserve_format") {
    opts.policy = PIIDetector::MaskingPolicy::PRESERVE_FORMAT;
  } else if (policy_str == "custom") {
    opts.policy = PIIDetector::MaskingPolicy::CUSTOM;
    auto rules_it = opt_values.find("rules");
    if (rules_it != opt_values.end()) {
      for (const auto &kv : opt_values) {
        const std::string prefix = "rules.";
        if (kv.first.rfind(prefix, 0) == 0 && !kv.second.empty()) {
          opts.custom_rules[kv.first.substr(prefix.size())] = kv.second[0];
        }
      }
      (void)rules_it;
    }
  } else {
    // Unknown policy — fall back to replace_with_type.
    opts.policy = PIIDetector::MaskingPolicy::REPLACE_WITH_TYPE;
  }
  return opts;
}
}  // namespace ML
}  // namespace ShannonBase