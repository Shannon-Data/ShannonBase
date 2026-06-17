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

#ifndef __SHANNONBASE_RAPID_LLM_GENERATE_OLLAMA_H__
#define __SHANNONBASE_RAPID_LLM_GENERATE_OLLAMA_H__
#include <string>
#include <vector>

#include "ml/infra_component/llm_generate_onnx.h"

namespace ShannonBase {
namespace ML {
namespace LLM_Generate {
/*
* OllamaGenerator: a drop-in companion to TextGenerator that routes
   ML_GENERATE calls to a running Ollama HTTP service instead of the
   embedded ONNX Runtime. which providers an alternative implementation of
   the same GenerationOptions / Result interface contract, but with a
   different underlying implementation.

   Interface contract:
     • Same GenerationOptions struct as TextGenerator (provider/endpoint
       fields carry the Ollama-specific configuration).
     • Same Result struct:   { std::string output; std::vector<int64_t> tokens; }
       tokens is empty for Ollama (the HTTP API does not expose token IDs).
     • Same Generate(prompt, maxNewTokens) signature.
     • Initialized() / constructor error semantics match TextGenerator.

   Call-site usage (no existing call-site changes required):

     GenerationOptions opts;
     opts.setModelDefaults("qwen2.5:0.5b");
     opts.provider  = MLProvider::OLLAMA;
     opts.endpoint  = "http://localhost:11434";   // optional, this is default

     OllamaGenerator gen(opts);
     if (!gen.Initialized()) { ... handle error ... }
     auto result = gen.Generate("What is 2+2?");
     // result.output  → "4" (or longer model response)
*/

class OllamaGenerator {
 public:
  using Result = TextGenerator::Result;
  explicit OllamaGenerator(const GenerationOptions &opts);
  ~OllamaGenerator() = default;

  OllamaGenerator(const OllamaGenerator &) = delete;
  OllamaGenerator &operator=(const OllamaGenerator &) = delete;
  OllamaGenerator(OllamaGenerator &&) = default;
  OllamaGenerator &operator=(OllamaGenerator &&) = default;

  inline bool Initialized() const { return m_initialized; }
  inline const std::string &LastError() const { return m_error_string; }

  Result Generate(const std::string &userPrompt, int maxNewTokens = -1);

 private:
  std::string BuildRequestBody(const std::string &prompt, int maxNewTokens) const;
  std::string BuildOpenAIRequestBody(const std::string &prompt, int maxNewTokens) const;
  std::string ParseResponse(const std::string &raw_json);
  std::string HttpPost(const std::string &url, const std::string &body) const;

  GenerationOptions m_opts;
  bool m_initialized{false};
  std::string m_error_string;

  // Cached full generate URL:  endpoint + "/api/generate"
  std::string m_generate_url;
};
}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase
#endif  // __SHANNONBASE_RAPID_LLM_GENERATE_OLLAMA_H__