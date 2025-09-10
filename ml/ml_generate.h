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
#ifndef __SHANNONBASE_ML_GENERATE_H__
#define __SHANNONBASE_ML_GENERATE_H__

#include <memory>
#include <string>
#include <vector>

#include "sql-common/json_dom.h"  //Json_wrapper.

#include "ml_algorithm.h"

class Json_wrapper;
namespace ShannonBase {
namespace ML {

class ML_generate : public ML_algorithm {
 public:
  ML_generate() = default;
  virtual ~ML_generate() override = default;

  /**options
   * 'task', {'generation'|'summarization'}
   * |'model_id', 'ModelID'
   * |'context', 'Context'
   * |'language', 'Language'
   * |'temperature', Temperature
   * |'max_tokens', MaxTokens
   * |'top_k', K
   * |'top_p', P
   * |'repeat_penalty', RepeatPenalty
   * |'frequency_penalty', FrequencyPenalty
   * |'presence_penalty', PresencePenalty
   * |'stop_sequences', JSON_ARRAY('StopSequence'[, 'StopSequence'] ...)
   * |'speculative_decoding', {true|false}
   * |'image', Base64ImageEncoding
   */
  virtual std::string Generate(std::string &text, Json_wrapper &option) = 0;
};

class ML_generate_row : public ML_generate {
 public:
  ML_generate_row() = default;
  virtual ~ML_generate_row() override = default;

  virtual ML_TASK_TYPE_T type() override { return ML_TASK_TYPE_T::GENERATE; }

  virtual std::string Generate(std::string &text, Json_wrapper &option) override;

 private:
  virtual int train() override { return false; }
  virtual int load(std::string &) override { return false; }
  virtual int load_from_file(std::string &, std::string &) override { return false; }
  virtual int unload(std::string &) override { return false; }
  virtual int import(Json_wrapper &, Json_wrapper &, std::string &) override { return false; }
  virtual double score(std::string &, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return 0.0f;
  }

  virtual int explain(std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
  virtual int explain_row() override { return false; }
  virtual int explain_table() override { return false; }
  virtual int predict_row(Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) override { return false; }
  virtual int predict_table(std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
};

class ML_generate_table : public ML_generate {
 public:
  ML_generate_table() = default;
  virtual ~ML_generate_table() override = default;

  virtual ML_TASK_TYPE_T type() override { return ML_TASK_TYPE_T::GENERATE; }

  virtual std::string Generate(std::string &text, Json_wrapper &option) override;

 private:
  virtual int train() override { return false; }
  virtual int load(std::string &) override { return false; }
  virtual int load_from_file(std::string &, std::string &) override { return false; }
  virtual int unload(std::string &) override { return false; }
  virtual int import(Json_wrapper &, Json_wrapper &, std::string &) override { return false; }
  virtual double score(std::string &, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return 0.0f;
  }

  virtual int explain(std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
  virtual int explain_row() override { return false; }
  virtual int explain_table() override { return false; }
  virtual int predict_row(Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) override { return false; }
  virtual int predict_table(std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
};

}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_ML_GENERATE_H__