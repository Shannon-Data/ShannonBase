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
#ifndef __SHANNONBASE_ML_EMBEDDING_H__
#define __SHANNONBASE_ML_EMBEDDING_H__

#include <memory>
#include <string>
#include <vector>

#include "sql-common/json_dom.h"  //Json_wrapper.

#include "ml/infra_component/sentence_transform.h"  //onnxruntime
#include "ml_algorithm.h"

class Json_wrapper;
namespace ShannonBase {
namespace ML {
class ML_embedding : public ML_algorithm {
 public:
  using EmbeddingVector = SentenceTransform::MiniLMEmbedding::EmbeddingVector;
  ML_embedding() = default;
  virtual ~ML_embedding() = default;

  /**
   * options: JSON_OBJECT(keyvalue[, keyvalue] ...)
  keyvalue:
  {
    'model_id', {'ModelID'}
    |'truncate', {true|false}
  }
   */
  virtual EmbeddingVector GenerateEmbedding(std::string &text, Json_wrapper &option) = 0;

  /** options. JSON
   * 'model_id', {'ModelID'}
    |'truncate', {true|false}
    |'batch_size', BatchSize
    |'details_column', 'ErrorDetailsColumnName'
  */
  virtual int GenerateTableEmbedding(std::string &InputTableColumn, std::string &OutputTableColumn,
                                     Json_wrapper &option) = 0;
};

class ML_embedding_row : public ML_embedding {
 public:
  ML_embedding_row();
  virtual ~ML_embedding_row() override {}
  virtual EmbeddingVector GenerateEmbedding(std::string &text, Json_wrapper &option) override;
  virtual int GenerateTableEmbedding(std::string &InputTableColumn, std::string &OutputTableColumn,
                                     Json_wrapper &option) override;
  virtual ML_TASK_TYPE_T type() override { return ML_TASK_TYPE_T::EMBEDDING; }

 private:
  virtual int train(THD *, Json_wrapper & /*model_object*/, Json_wrapper & /*model_metadata*/) override {
    return false;
  }
  virtual int load(THD *, std::string &) override { return false; }
  virtual int load_from_file(THD *, std::string &, std::string &) override { return false; }
  virtual int unload(THD *, std::string &) override { return false; }
  virtual int import(THD *, Json_wrapper &, Json_wrapper &, std::string &) override { return false; }
  virtual double score(THD *, std::string &, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return 0.0f;
  }

  virtual int explain(THD *, std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
  virtual int explain_row(THD *) override { return false; }
  virtual int explain_table(THD *) override { return false; }
  virtual int predict_row(THD *, Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) override {
    return false;
  }
  virtual int predict_table(THD *, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return false;
  }
};

class ML_embedding_table : public ML_embedding {
 public:
  ML_embedding_table();
  virtual ~ML_embedding_table() override {}
  virtual EmbeddingVector GenerateEmbedding(std::string &text, Json_wrapper &option) override;
  virtual int GenerateTableEmbedding(std::string &InputTableColumn, std::string &OutputTableColumn,
                                     Json_wrapper &option) override;
  virtual ML_TASK_TYPE_T type() override { return ML_TASK_TYPE_T::EMBEDDING; }

 private:
  virtual int train(THD *, Json_wrapper & /*model_object*/, Json_wrapper & /*model_metadata*/) override {
    return false;
  }
  virtual int load(THD *, std::string &) override { return false; }
  virtual int load_from_file(THD *, std::string &, std::string &) override { return false; }
  virtual int unload(THD *, std::string &) override { return false; }
  virtual int import(THD *, Json_wrapper &, Json_wrapper &, std::string &) override { return false; }
  virtual double score(THD *, std::string &, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return 0.0f;
  }

  virtual int explain(THD *, std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
  virtual int explain_row(THD *) override { return false; }
  virtual int explain_table(THD *) override { return false; }
  virtual int predict_row(THD *, Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) override {
    return false;
  }
  virtual int predict_table(THD *, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return false;
  }
};
}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ML_EMBEDDING_H__