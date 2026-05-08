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

   The fundmental code for LLM inference.
   Copyright (c) 2023 - , Shannon Data AI and/or its affiliates.
*/

#ifndef __SHANNONBASE_RAPID_LLM_GENERATE_H__
#define __SHANNONBASE_RAPID_LLM_GENERATE_H__

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include "ml/infra_component/llm_model_detector.h"

namespace ShannonBase {
namespace ML {
namespace LLM_Generate {
using ShannonBase::ML::CPUDetector;
using ShannonBase::ML::Device;
using ShannonBase::ML::ModelSelection;
using ShannonBase::ML::Precision;
using ShannonBase::ML::select_model_variant;

// ONNX_LOCAL  : original embedded ONNX-Runtime path (default, unchanged).
// OLLAMA      : remote Ollama HTTP service  (http://host:11434/api/generate).
// OPENAI_COMPAT: any OpenAI-compatible /v1/chat/completions endpoint (future).
enum class MLProvider : uint8_t {
  ONNX_LOCAL = 0,
  OLLAMA = 1,
  OPENAI_COMPAT = 2,
};

typedef struct {
  std::string task{"generation"};  // 'generation' or 'summarization'
  std::string model_id;            // Model identifier, llma, mistral, phi, etc.
  std::string context;             // Additional context
  std::string language{"en"};      // Language setting

  MLProvider provider{MLProvider::ONNX_LOCAL};
  std::string endpoint{"http://localhost:11434"};  // Ollama default
  uint32_t http_timeout_ms{30000};                 // per-request timeout

  float temperature{0.7f};  // Sampling temperature (0.0-2.0)
  int max_tokens{100};      // Maximum tokens to generate
  int top_k{50};            // Top-k sampling
  float top_p{0.9f};        // Nucleus sampling

  float repeat_penalty{1.0f};     // Repetition penalty
  float frequency_penalty{0.0f};  // Frequency penalty
  float presence_penalty{0.0f};   // Presence penalty

  int min_new_tokens{8};  // Suppress EOS / stop sequences until at least this many tokens are generated.

  std::vector<std::string> stop_sequences;  // Stop sequences
  bool speculative_decoding{false};         // Speculative decoding
  std::string image_base64;                 // Base64 encoded image

  void setLanguage(const std::string &lang) {
    std::string lower_lang = lang;
    std::transform(lower_lang.begin(), lower_lang.end(), lower_lang.begin(), ::tolower);

    static const std::unordered_map<std::string, std::string> languageMap = {
        {"en", "english"},
        {"english", "english"},
        {"zh", "chinese"},
        {"cn", "chinese"},
        {"chinese", "chinese"},
        {"es", "spanish"},
        {"spanish", "spanish"},
        {"fr", "french"},
        {"french", "french"},
        {"de", "german"},
        {"german", "german"},
        {"ja", "japanese"},
        {"japanese", "japanese"},
        {"ko", "korean"},
        {"korean", "korean"},
        {"ru", "russian"},
        {"russian", "russian"},
        {"ar", "arabic"},
        {"arabic", "arabic"},
        {"pt", "portuguese"},
        {"portuguese", "portuguese"},
        {"it", "italian"},
        {"italian", "italian"},
        {"nl", "dutch"},
        {"dutch", "dutch"},
        {"hi", "hindi"},
        {"hindi", "hindi"},
        {"tr", "turkish"},
        {"turkish", "turkish"},
        {"vi", "vietnamese"},
        {"vietnamese", "vietnamese"},
        {"th", "thai"},
        {"thai", "thai"},
        {"id", "indonesian"},
        {"indonesian", "indonesian"},
        {"pl", "polish"},
        {"polish", "polish"},
        {"uk", "ukrainian"},
        {"ukrainian", "ukrainian"},
        // more....
    };

    auto it = languageMap.find(lower_lang);
    language = (it != languageMap.end()) ? it->second : lang;
  }

  void setModelDefaults(const std::string &model_id_) {
    this->model_id = model_id_;

    std::string lower_model = model_id_;
    std::transform(lower_model.begin(), lower_model.end(), lower_model.begin(), ::tolower);

    // defaults
    temperature = 0.7f;
    max_tokens = 512;
    top_k = 50;
    top_p = 0.9f;
    repeat_penalty = 1.1f;
    frequency_penalty = 0.0f;
    presence_penalty = 0.0f;
    stop_sequences.clear();

    // recommended params value.
    if (lower_model.find("qwen") != std::string::npos) {
      temperature = 0.8f;
      max_tokens = 50;
      top_k = 50;
      top_p = 0.95f;
      repeat_penalty = 1.05f;
      stop_sequences = {"<|im_end|>"};
      if (language == "chinese") {
        temperature = 0.6f;
        max_tokens = 100;
      }
    } else if (lower_model.find("llama") != std::string::npos) {
      temperature = 0.7f;
      max_tokens = 1024;
      top_k = 40;
      top_p = 0.95f;
      repeat_penalty = 1.1f;
      stop_sequences = {"</s>", "<|eot_id|>"};
    } else if (lower_model.find("mistral") != std::string::npos) {
      temperature = 0.5f;
      max_tokens = 1024;
      top_p = 0.9f;
      repeat_penalty = 1.15f;
      stop_sequences = {"</s>", "[INST]", "[/INST]"};
    } else if (lower_model.find("phi") != std::string::npos) {
      temperature = 0.6f;
      max_tokens = 512;
      top_p = 0.88f;
      repeat_penalty = 1.08f;
      stop_sequences = {"<|endoftext|>"};
    } else if (lower_model.find("gemma") != std::string::npos) {
      temperature = 0.7f;
      max_tokens = 1024;
      top_p = 0.9f;
      repeat_penalty = 1.1f;
      stop_sequences = {"<end_of_turn>", "<eos>"};
    } else if (lower_model.find("chatglm") != std::string::npos) {
      temperature = 0.5f;
      max_tokens = 1024;
      top_p = 0.85f;
      repeat_penalty = 1.05f;
      stop_sequences = {"<|endoftext|>"};
      if (language == "chinese") {
        temperature = 0.4f;
        max_tokens = 512;
      }
    } else if (lower_model.find("yi") != std::string::npos) {
      temperature = 0.3f;
      max_tokens = 2048;
      top_p = 0.85f;
      repeat_penalty = 1.05f;
      stop_sequences = {"<|im_end|>", "<|im_start|>"};
    }

    if (task == "summarization") {
      temperature = 0.3f;
      max_tokens = 300;
      repeat_penalty = 1.2f;
      if (language == "chinese")
        max_tokens = 200;
      else if (language == "japanese")
        max_tokens = 250;
    }
  }

  void optimizeForModelSize() {
    std::string lower_model = model_id;
    std::transform(lower_model.begin(), lower_model.end(), lower_model.begin(), ::tolower);

    if (lower_model.find("0.5b") != std::string::npos || lower_model.find("1b") != std::string::npos ||
        lower_model.find("1.5b") != std::string::npos) {
      temperature = std::min(temperature, 0.1f);
      max_tokens = std::max(max_tokens, 512);
      top_k = std::min(top_k, 10);
      repeat_penalty = std::min(repeat_penalty, 1.15f);
    } else if (lower_model.find("70b") != std::string::npos || lower_model.find("72b") != std::string::npos) {
      temperature = std::min(temperature + 0.1f, 1.0f);
      max_tokens = std::max(max_tokens, 2048);
    }
  }

  bool validate() const {
    bool task_ok = (task == "generation" || task == "summarization" || task == "pii_detect" || task == "pii_mask");
    if (!task_ok) return false;
    if (task == "pii_detect" || task == "pii_mask") return true;
    return temperature >= 0.0f && temperature <= 5.0f && max_tokens > 0 && max_tokens <= 8192 && top_k >= 0 &&
           top_k <= 1000 && top_p >= 0.0f && top_p <= 1.0f && repeat_penalty > 0.0f && repeat_penalty <= 2.0f &&
           frequency_penalty >= -2.0f && frequency_penalty <= 2.0f && presence_penalty >= -2.0f &&
           presence_penalty <= 2.0f;
  }

} GenerationOptions;

struct ModelConfig {
  size_t num_layers = 0;       // 0 means auto detect
  size_t num_query_heads = 0;  // 0 means auto detect
  size_t num_kv_heads = 0;     // 0 means auto detect
  size_t head_dim = 0;         // 0 means auto detect
  std::string attention_type;  // "standard" or "gqa"
};
}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase
#endif  // __SHANNONBASE_RAPID_LLM_GENERATE_H__