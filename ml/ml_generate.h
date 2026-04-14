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

   Copyright (c) 2023-, Shannon Data AI and/or its affiliates. */
#ifndef __SHANNONBASE_ML_GENERATE_H__
#define __SHANNONBASE_ML_GENERATE_H__

#include <memory>
#include <string>
#include <vector>

#include "sql-common/json_dom.h"  //Json_wrapper.

#include "infra_component/pii_detector.h"
#include "ml_algorithm.h"

class Json_wrapper;
namespace ShannonBase {
namespace ML {
class ML_generate : public ML_algorithm {
 public:
  ML_generate() = default;
  virtual ~ML_generate() override = default;

  /**
   * Options accepted in the JSON option blob (in_model_option):
   *
   * Common options (all tasks):
   *   'model_id'     Model identifier string (directory name under llm-models/)
   *
   * LLM generation tasks ('generation' | 'summarization'):
   *   'task'              Task type.  Default: 'generation'.
   *   'context'           Additional context string.
   *   'language'          Language hint, e.g. 'en', 'zh'.
   *   'temperature'       Sampling temperature [0.0, 2.0].
   *   'max_tokens'        Maximum tokens to generate.
   *   'top_k'             Top-K sampling parameter.
   *   'top_p'             Nucleus sampling threshold.
   *   'repeat_penalty'    Repetition penalty.
   *   'frequency_penalty' Frequency penalty.
   *   'presence_penalty'  Presence penalty.
   *   'stop_sequences'    JSON_ARRAY of stop strings.
   *   'speculative_decoding'  'true'|'false'.
   *   'image'             Base64-encoded image (multimodal models).
   *
   * PII detection task ('pii_detect'):
   *   'task'          'pii_detect'
   *   'model_id'      NER model name (e.g. 'pii-ner-model').
   *   'output_format' Currently only 'json' is supported.
   *   'label_names'   JSON_ARRAY of BIO label strings matching model output
   *                   class indices.  Defaults to dslim/bert-base-NER scheme:
   *                   ["O","B-PER","I-PER","B-ORG","I-ORG","B-LOC","I-LOC",
   *                    "B-MISC","I-MISC"]
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
  virtual int train(THD *, Json_wrapper &, Json_wrapper &) override { return false; }
  virtual int load(THD *, std::string &) override { return false; }
  virtual int load_from_file(THD *, std::string &, std::string &) override { return false; }
  virtual int unload(THD *, std::string &) override { return false; }
  virtual int import(THD *, Json_wrapper &, Json_wrapper &, std::string &) override { return false; }
  virtual double score(THD *, std::string &, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return 0.0f;
  }

  virtual int explain(THD *, std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
  virtual int explain_row(THD *, Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) override {
    assert(false);
    return HA_ERR_GENERIC;
  }
  virtual int explain_table(THD *) override { return false; }
  virtual int predict_row(THD *, Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) override {
    return false;
  }
  virtual int predict_table(THD *, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return false;
  }

  std::string text_generation_task(const std::string &text, const ShannonBase::ML::OPTION_VALUE_T &opt_values,
                                   const std::string &task_str, const std::string &model_path,
                                   const std::string &token_path);

  PIIDetector::MaskingOptions parse_masking_options(const ShannonBase::ML::OPTION_VALUE_T &opt_values);
  std::string pii_task(const std::string &text, const ShannonBase::ML::OPTION_VALUE_T &opt_values,
                       const std::string &task_str, const std::string &model_path, const std::string &token_path);
};

class ML_generate_table : public ML_generate {
 public:
  ML_generate_table() = default;
  virtual ~ML_generate_table() override = default;

  virtual ML_TASK_TYPE_T type() override { return ML_TASK_TYPE_T::GENERATE; }

  virtual std::string Generate(std::string &text, Json_wrapper &option) override;

 private:
  virtual int train(THD *, Json_wrapper &, Json_wrapper &) override { return false; }
  virtual int load(THD *, std::string &) override { return false; }
  virtual int load_from_file(THD *, std::string &, std::string &) override { return false; }
  virtual int unload(THD *, std::string &) override { return false; }
  virtual int import(THD *, Json_wrapper &, Json_wrapper &, std::string &) override { return false; }
  virtual double score(THD *, std::string &, std::string &, std::string &, std::string &, Json_wrapper &) override {
    return 0.0f;
  }

  virtual int explain(THD *, std::string &, std::string &, std::string &, Json_wrapper &) override { return false; }
  virtual int explain_row(THD *, Json_wrapper &, std::string &, Json_wrapper &, Json_wrapper &) override {
    assert(false);
    return HA_ERR_GENERIC;
  }
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
#endif  //__SHANNONBASE_ML_GENERATE_H__