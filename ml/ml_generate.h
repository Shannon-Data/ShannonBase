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

#include "sql-common/json_dom.h"  // Json_wrapper

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
   * Common options (all tasks)
   *
   *   'model_id'     Model identifier string.
   *                  - Local ONNX : directory name under llm-models/
   *                    e.g. 'Qwen2.5-0.5B-Instruct'
   *                  - Ollama local: model tag served by Ollama
   *                    e.g. 'qwen2.5:7b', 'llama3.1:8b'
   *                  - Cloud API  : model name accepted by the provider
   *                    e.g. 'qwen-max', 'ernie-4.5-turbo-128k',
   *                         'deepseek-v4-pro', 'gpt-4o'
   *
   * LLM generation tasks ('generation' | 'summarization')
   *
   *   'task'               Task type.  Default: 'generation'.
   *   'context'            Additional context string.
   *   'language'           Language hint, e.g. 'en', 'zh'.
   *   'temperature'        Sampling temperature [0.0, 2.0].
   *   'max_tokens'         Maximum tokens to generate.
   *                        Local ONNX     : capped at 8192.
   *                        Remote providers: up to 131072 (provider-dependent).
   *   'top_k'              Top-K sampling parameter.
   *   'top_p'              Nucleus sampling threshold [0.0, 1.0].
   *   'repeat_penalty'     Repetition penalty (0.0, 2.0].
   *   'frequency_penalty'  Frequency penalty [-2.0, 2.0].
   *   'presence_penalty'   Presence penalty  [-2.0, 2.0].
   *   'stop_sequences'     JSON_ARRAY of stop strings.
   *   'speculative_decoding'  'true' | 'false'.
   *   'image'              Base64-encoded image (multimodal models).
   *
   * Backend selection (generation tasks only; ignored for pii_*)
   *
   *   'provider'   Backend engine:
   *                  'onnx'      (default) Local ONNX Runtime inference.
   *                              Requires model files under llm-models/.
   *                  'ollama'    Local Ollama service (HTTP).
   *                              Default endpoint: http://localhost:11434
   *                  'openai'    OpenAI API or any OpenAI-compatible endpoint.
   *                              Default endpoint: https://api.openai.com/v1
   *                  'dashscope' Alibaba Cloud Tongyi Qianwen (DashScope).
   *                              Default endpoint (global):
   *                                https://dashscope.aliyuncs.com/compatible-mode/v1
   *                              Regional endpoint (requires 'workspace_id'):
   *                                https://{workspace_id}.{region}.maas.aliyuncs.com/compatible-mode/v1
   *                              Supported models: qwen-max, qwen-plus,
   *                                qwen-turbo, qwen-long, etc.
   *                  'qianfan'   Baidu Wenxin (Qianfan).
   *                              Default endpoint: https://qianfan.baidubce.com/v2
   *                              Supported models: ernie-4.5-turbo-128k, etc.
   *                  'deepseek'  DeepSeek Cloud API.
   *                              Default endpoint: https://api.deepseek.com/v1
   *                              Recommended models:
   *                                'deepseek-v4-flash'  fast, non-thinking
   *                                'deepseek-v4-pro'    extended reasoning
   *                              Deprecated (alias to v4-flash after 2026/07/24):
   *                                'deepseek-chat', 'deepseek-reasoner'
   *
   *   'endpoint'   Base URL for remote provider.
   *                Overrides the provider default when specified.
   *                Explicit endpoint always takes precedence over
   *                workspace_id/region auto-construction.
   *
   *   'api_key'    Bearer token for cloud API authentication.
   *                Required for: openai, dashscope, qianfan, deepseek.
   *                Not used for: onnx, ollama.
   *                ⚠ Avoid passing api_key in plain SQL — it will appear
   *                  in general_log. Prefer 'api_config' (see below).
   *
   *   'api_config' Name of a pre-configured entry in mysql.shannon_api_configs.
   *                When set, provider / endpoint / api_key are loaded from
   *                the config table (api_key stored encrypted).
   *                Takes precedence over inline 'api_key'.
   *                e.g. JSON_OBJECT('api_config', 'qwen_prod', ...)
   *
   *   'workspace_id'
   *                DashScope WorkspaceId for regional endpoint construction.
   *                Required when using cn-beijing, ap-southeast-1 (Singapore),
   *                or eu-central-1 (Frankfurt) regions.
   *                Obtain from DashScope console → 业务空间管理.
   *                Ignored when 'endpoint' is explicitly provided.
   *
   *   'region'     Cloud region for workspace_id-based endpoint construction.
   *                  'cn-beijing'     (default when workspace_id is set)
   *                  'ap-southeast-1' Singapore
   *                  'eu-central-1'   Frankfurt
   *                Ignored when 'endpoint' is explicitly provided.
   *
   *   'timeout_ms' Per-request HTTP timeout in milliseconds.
   *                Default: 30000 (Ollama local), 120000 (cloud providers).
   *
   * DeepSeek extended reasoning
   *
   *   'deepseek_thinking'
   *                'true' | 'false'.  Enable chain-of-thought thinking mode.
   *                Effective only when provider='deepseek' and model supports
   *                it (deepseek-v4-pro).  The reasoning_content returned by
   *                the API is logged at DEBUG level and stripped from the
   *                result text.  Default: 'false' (auto-set to 'true' for
   *                deepseek-v4-pro via setModelDefaults).
   *
   *   'reasoning_effort'
   *                'low' | 'medium' | 'high'.
   *                Controls the thinking token budget when
   *                deepseek_thinking=true.  Default: 'medium'.
   *
   * Examples
   *
   * -- Local ONNX (default, no provider needed):
   *   SELECT sys.ML_GENERATE('Hello',
   *     JSON_OBJECT('model_id',   'Qwen2.5-0.5B-Instruct',
   *                 'language',   'zh',
   *                 'max_tokens', 512));
   *
   * -- Local Ollama:
   *   SELECT sys.ML_GENERATE('Hello',
   *     JSON_OBJECT('provider', 'ollama',
   *                 'model_id', 'qwen2.5:7b',
   *                 'endpoint', 'http://192.168.1.100:11434'));
   *
   * -- Tongyi Qianwen cloud, global endpoint:
   *   SELECT sys.ML_GENERATE('分析2024年收入',
   *     JSON_OBJECT('provider',   'dashscope',
   *                 'model_id',   'qwen-plus',
   *                 'api_key',    'sk-xxxxxxxx',
   *                 'language',   'zh',
   *                 'max_tokens', 4000));
   *
   * -- Tongyi Qianwen cloud, Beijing regional endpoint:
   *   SELECT sys.ML_GENERATE('分析2024年收入',
   *     JSON_OBJECT('provider',      'dashscope',
   *                 'model_id',      'qwen-max',
   *                 'api_key',       'sk-xxxxxxxx',
   *                 'workspace_id',  'ws-xxxxxxxxxxxxxxxx',
   *                 'region',        'cn-beijing',
   *                 'language',      'zh',
   *                 'max_tokens',    4000));
   *
   * -- DeepSeek v4-pro with extended reasoning:
   *   SELECT sys.ML_GENERATE('预测未来6个月收入走势',
   *     JSON_OBJECT('provider',           'deepseek',
   *                 'model_id',           'deepseek-v4-pro',
   *                 'api_key',            'sk-xxxxxxxx',
   *                 'deepseek_thinking',  'true',
   *                 'reasoning_effort',   'high',
   *                 'language',           'zh',
   *                 'max_tokens',         8192));
   *
   * -- Using api_config (recommended for production, avoids key in log):
   *   SELECT sys.ML_GENERATE('分析数据',
   *     JSON_OBJECT('api_config', 'qwen_prod',
   *                 'max_tokens', 2000,
   *                 'language',   'zh'));
   *
   * PII detection task ('pii_detect')
   *
   *   'task'           'pii_detect'
   *   'model_id'       NER model name (e.g. 'multilang-pii-ner-ONNX').
   *   'output_format'  Currently only 'json' is supported.
   *   'label_names'    JSON_ARRAY of BIO label strings matching model output
   *                    class indices.  Defaults to dslim/bert-base-NER scheme:
   *                    ["O","B-PER","I-PER","B-ORG","I-ORG","B-LOC","I-LOC",
   *                     "B-MISC","I-MISC"]
   *   'min_confidence'        Global minimum confidence threshold [0.0, 1.0].
   *   'confidence_<TYPE>'     Per-type threshold, e.g. 'confidence_PERSON'.
   *                           Supported types: PERSON, ORGANIZATION, LOCATION,
   *                           MISC, EMAIL, PHONE_NUMBER, CREDIT_CARD, SSN,
   *                           IP_ADDRESS, DATE_OF_BIRTH, DATETIME,
   *                           STREET_ADDRESS.
   *
   * PII masking task ('pii_mask')
   *
   *   All pii_detect options above, plus:
   *   'policy'         Masking policy:
   *                      'replace_with_type'  (default) Entity type tag.
   *                      'replace_with_mask'  Fixed mask string.
   *                      'hash'               SHA-256 hash.
   *                      'preserve_format'    Format-preserving masking.
   *                      'custom'             Rule-based via 'rules.<TYPE>'.
   *   'rules.<TYPE>'   Custom replacement string for a specific entity type.
   *                    e.g. JSON_OBJECT('rules.PERSON', '***')
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
    return 0.0;
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

  /**
   * text_generation_task — handles 'generation' and 'summarization' tasks.
   * Parses opt_values into GenerationOptions, then dispatches to:
   *   - OllamaGenerator  for all remote providers (ollama/openai/dashscope/
   *                      qianfan/deepseek)
   *   - TextGenerator    for local ONNX inference (default)
   */
  std::string text_generation_task(const std::string &text, const ShannonBase::ML::OPTION_VALUE_T &opt_values,
                                   const std::string &task_str, const std::string &model_path,
                                   const std::string &token_path);

  /**
   * pii_task — handles 'pii_detect' and 'pii_mask' tasks.
   * Always uses local ONNX (PIIDetector); provider setting is ignored.
   */
  std::string pii_task(const std::string &text, const ShannonBase::ML::OPTION_VALUE_T &opt_values,
                       const std::string &task_str, const std::string &model_path, const std::string &token_path);

  /**
   * parse_masking_options — extracts PIIDetector::MaskingOptions from the
   * parsed JSON kv map (policy, rules.<TYPE>, etc.).
   */
  PIIDetector::MaskingOptions parse_masking_options(const ShannonBase::ML::OPTION_VALUE_T &opt_values);
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
    return 0.0;
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
#endif  // __SHANNONBASE_ML_GENERATE_H__