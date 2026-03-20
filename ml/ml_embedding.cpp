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
#include <filesystem>
#include <string>

#include "include/field_types.h"
#include "include/my_inttypes.h"
#include "include/mysqld_error.h"
#include "sql/current_thd.h"
#include "sql/derror.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/mysqld.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"
#include "sql/table.h"

#include "ml_info.h"
#include "ml_utils.h"
#include "storage/innobase/include/ut0dbg.h"
#include "storage/rapid_engine/include/rapid_const.h"

extern char mysql_home[FN_REFLEN];
extern char mysql_llm_home[FN_REFLEN];
namespace ShannonBase {
namespace ML {
static std::string build_model_dir(const std::string &model_id) {
  std::string base(mysql_llm_home);
  if (base.empty()) base = mysql_home;
  return base + "llm-models/" + model_id;
}

bool ML_embedding_row::init_embedder(const std::string &model_id) {
  const std::string model_dir = build_model_dir(model_id);

  const std::string onnx_path = model_dir + "/onnx/";
  if (!std::filesystem::exists(onnx_path)) {
    std::string err("cannot find ONNX model for: ");
    err.append(model_id);
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return false;
  }

  const std::string tokenizer = model_dir + "/tokenizer.json";
  if (!std::filesystem::exists(tokenizer)) {
    std::string err("cannot find tokenizer.json for: ");
    err.append(model_id);
    my_error(ER_ML_FAIL, MYF(0), err.c_str());
    return false;
  }

  m_embedder = std::make_unique<SentenceTransform::MiniLMEmbedding>(onnx_path, tokenizer);
  m_cached_model_id = model_id;
  return true;
}

bool ML_embedding_row::WarmUp(const std::string &model_id) {
  if (m_embedder && m_cached_model_id == model_id) return true;  // already warm
  return init_embedder(model_id);
}

int ML_embedding_row::TerminateTask() { return m_embedder && m_embedder->TerminateTask(); }

// It's a time-consuming operation, therefore, we need to poll the cancel token to cancel this operation if needed.
ML_embedding::EmbeddingVector ML_embedding_row::GenerateEmbedding(std::string &text, Json_wrapper &option) {
  ML_embedding::EmbeddingVector result;
  ShannonBase::ML::OPTION_VALUE_T opt_values;
  std::string keystr;
  if (ShannonBase::ML::Utils::parse_json(option, opt_values, keystr, 0)) {
    my_error(ER_ML_FAIL, MYF(0), "cannot parse embedding option JSON");
    return result;
  }

  std::string model_id("all-MiniLM-L12-v2");
  keystr = "model_id";
  if (opt_values.find(keystr) != opt_values.end() && !opt_values[keystr].empty()) model_id = opt_values[keystr][0];

  if (!m_embedder || m_cached_model_id != model_id) {
    if (!init_embedder(model_id)) return result;
  }

  auto embed_result = m_embedder->EmbedText(text);
  if (embed_result.confidence <= 0.0 || embed_result.embedding.empty()) return result;
  result = std::move(embed_result.embedding);
  return result;
}

int ML_embedding_row::GenerateTableEmbedding(std::string &, std::string &, Json_wrapper &) {
  assert(false);
  return 0;
}

/* ML_embedding_table is a stub — cancel parameter accepted but unused. */
ML_embedding::EmbeddingVector ML_embedding_table::GenerateEmbedding(std::string &, Json_wrapper &) { return {}; }

int ML_embedding_table::GenerateTableEmbedding(std::string &, std::string &, Json_wrapper &) {
  return ShannonBase::SHANNON_SUCCESS;
}

int ML_embedding_table::TerminateTask() { return ShannonBase::SHANNON_SUCCESS; }
}  // namespace ML
}  // namespace ShannonBase