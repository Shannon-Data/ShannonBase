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

   The fundamental code for LLM inference via remote HTTP backends.
   Supports Ollama (local), OpenAI-compatible cloud APIs (DashScope,
   Qianfan, DeepSeek, OpenAI) through a unified HTTP interface.

   Copyright (c) 2023 - , Shannon Data AI and/or its affiliates.
*/
#include "ml/infra_component/llm_generate_ollama.h"

#include <sstream>
#include <stdexcept>

#include <my_rapidjson_size_t.h>  // IWYU pragma: keep

#include <assert.h>
#include <rapidjson/document.h>
#include <rapidjson/error/error.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/reader.h>
#include <rapidjson/schema.h>
#include <rapidjson/stringbuffer.h>
#include "include/mysqld_error.h"

#include <curl/curl.h>
#include "include/my_dbug.h"
#include "include/mysql/components/services/log_builtins.h"
namespace ShannonBase {
namespace ML {
namespace LLM_Generate {
static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *buf = static_cast<std::string *>(userdata);
  buf->append(ptr, size * nmemb);
  return size * nmemb;
}

/*
 * Helper: resolve the effective endpoint for a given provider.
 * Called from the constructor so that m_generate_url is always consistent.
 **/
static std::string resolve_endpoint(const GenerationOptions &opts) {
  // Explicit endpoint always wins
  if (!opts.endpoint.empty() && opts.endpoint != "http://localhost:11434") return opts.endpoint;

  switch (opts.provider) {
    case MLProvider::DASHSCOPE:
      if (!opts.workspace_id.empty()) {
        std::string region = opts.region.empty() ? "cn-beijing" : opts.region;
        return "https://" + opts.workspace_id + "." + region + ".maas.aliyuncs.com/compatible-mode/v1";
      }
      return "https://dashscope.aliyuncs.com/compatible-mode/v1";
    case MLProvider::QIANFAN:
      return "https://qianfan.baidubce.com/v2";
    case MLProvider::DEEPSEEK:
      return "https://api.deepseek.com/v1";
    case MLProvider::OPENAI_COMPAT:
      return "https://api.openai.com/v1";
    case MLProvider::OLLAMA:
    default:
      return opts.endpoint.empty() ? "http://localhost:11434" : opts.endpoint;
  }
}

OllamaGenerator::OllamaGenerator(const GenerationOptions &opts) : m_opts(opts) {
  if (m_opts.model_id.empty()) {
    m_error_string = "[OllamaGenerator] model_id must not be empty";
    return;
  }

  // Resolve and cache the effective endpoint
  m_opts.endpoint = resolve_endpoint(m_opts);
  if (m_opts.endpoint.empty()) {
    m_error_string = "[OllamaGenerator] endpoint could not be resolved";
    return;
  }

  // Normalise: strip trailing slash, build Ollama-specific generate URL
  std::string ep = m_opts.endpoint;
  if (!ep.empty() && ep.back() == '/') ep.pop_back();
  m_generate_url = ep + "/api/generate";

  // Adjust default timeout for cloud providers
  if (m_opts.provider != MLProvider::OLLAMA && m_opts.provider != MLProvider::ONNX_LOCAL &&
      m_opts.http_timeout_ms <= 30000) {
    m_opts.http_timeout_ms = 120000;  // 120 s for cloud APIs
  }

  m_initialized = true;

  DBUG_PRINT("info", ("[OllamaGenerator] initialized: provider=%d endpoint=%s model=%s",
                      static_cast<int>(m_opts.provider), m_opts.endpoint.c_str(), m_opts.model_id.c_str()));
}

/*
 * BuildRequestBody — Ollama /api/generate format (raw prompt)
 * Used only when provider == OLLAMA.**/
std::string OllamaGenerator::BuildRequestBody(const std::string &prompt, int maxNewTokens) const {
  const int num_predict = (maxNewTokens > 0) ? maxNewTokens : m_opts.max_tokens;

  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);

  w.StartObject();
  w.Key("model");
  w.String(m_opts.model_id.c_str());
  w.Key("prompt");
  w.String(prompt.c_str());
  w.Key("stream");
  w.Bool(false);  // synchronous — simpler for UDF context
  w.Key("options");
  w.StartObject();
  w.Key("temperature");
  w.Double(static_cast<double>(m_opts.temperature));
  w.Key("top_k");
  w.Int(m_opts.top_k);
  w.Key("top_p");
  w.Double(static_cast<double>(m_opts.top_p));
  w.Key("repeat_penalty");
  w.Double(static_cast<double>(m_opts.repeat_penalty));
  w.Key("num_predict");
  w.Int(num_predict);
  w.EndObject();
  w.EndObject();

  return sb.GetString();
}

/*
 * BuildOpenAIRequestBody — OpenAI /chat/completions format
 * Used for all cloud providers: openai, dashscope, qianfan, deepseek.
 */
std::string OllamaGenerator::BuildOpenAIRequestBody(const std::string &prompt, int maxNewTokens) const {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);

  w.StartObject();
  w.Key("model");
  w.String(m_opts.model_id.c_str());
  w.Key("stream");
  w.Bool(false);

  // messages array
  w.Key("messages");
  w.StartArray();

  // Optional system turn
  if (!m_opts.system_prompt.empty()) {
    w.StartObject();
    w.Key("role");
    w.String("system");
    w.Key("content");
    w.String(m_opts.system_prompt.c_str());
    w.EndObject();
  }

  // User turn — full agent prompt goes here
  w.StartObject();
  w.Key("role");
  w.String("user");
  w.Key("content");
  w.String(prompt.c_str());
  w.EndObject();

  w.EndArray();  // messages

  // Common generation parameters
  w.Key("temperature");
  w.Double(static_cast<double>(m_opts.temperature));
  w.Key("max_tokens");
  w.Int(maxNewTokens > 0 ? maxNewTokens : m_opts.max_tokens);
  w.Key("top_p");
  w.Double(static_cast<double>(m_opts.top_p));

  // DeepSeek-specific: thinking / reasoning_effort
  // Only injected when provider is deepseek and thinking is enabled.
  // deepseek-v4-pro supports extended reasoning; deepseek-v4-flash does not.
  if (m_opts.provider == MLProvider::DEEPSEEK && m_opts.deepseek_thinking) {
    w.Key("thinking");
    w.StartObject();
    w.Key("type");
    w.String("enabled");
    w.EndObject();

    w.Key("reasoning_effort");
    w.String(m_opts.reasoning_effort.empty() ? "medium" : m_opts.reasoning_effort.c_str());
  }

  w.EndObject();
  return sb.GetString();
}

/* * ParseResponse — handles all response formats:
 *   1. OpenAI /chat/completions  (openai / dashscope / qianfan / deepseek)
 *      {"choices":[{"message":{"content":"...","reasoning_content":"..."}}]}
 *   2. Ollama /api/generate
 *      {"response":"..."}
 *   3. Ollama /api/chat
 *      {"message":{"content":"..."}}
 * */
std::string OllamaGenerator::ParseResponse(const std::string &raw) {
  rapidjson::Document doc;
  doc.Parse(raw.c_str(), raw.size());

  if (doc.HasParseError()) {
    m_error_string = "[OllamaGenerator] JSON parse error at offset " + std::to_string(doc.GetErrorOffset()) + ": " +
                     raw.substr(0, 256);
    my_error(ER_ML_FAIL, MYF(0), m_error_string);
    return m_error_string;
  }

  // Format 1: OpenAI-compatible
  if (doc.HasMember("choices") && doc["choices"].IsArray() && doc["choices"].Size() > 0) {
    const auto &choice = doc["choices"][0];
    if (!choice.IsObject() || !choice.HasMember("message") || !choice["message"].IsObject()) goto parse_error;

    const auto &msg = choice["message"];

    // DeepSeek thinking mode: reasoning_content holds the <think> chain.
    // We log it at DEBUG level and discard it from the returned text —
    // the ShannonBase agent already handles <think> stripping in JS.
    if (msg.HasMember("reasoning_content") && msg["reasoning_content"].IsString()) {
      DBUG_PRINT("info",
                 ("[DeepSeek] reasoning_content (first 300 chars): %.300s", msg["reasoning_content"].GetString()));
    }

    if (msg.HasMember("content") && msg["content"].IsString()) return msg["content"].GetString();
  }

  // Format 2: Ollama /api/generate
  if (doc.HasMember("response") && doc["response"].IsString()) return doc["response"].GetString();

  // Format 3: Ollama /api/chat
  if (doc.HasMember("message") && doc["message"].IsObject()) {
    const auto &msg = doc["message"];
    if (msg.HasMember("content") && msg["content"].IsString()) return msg["content"].GetString();
  }

  // Error responses
  if (doc.HasMember("error")) {
    // OpenAI style: {"error":{"message":"..."}}
    if (doc["error"].IsObject() && doc["error"].HasMember("message") && doc["error"]["message"].IsString()) {
      m_error_string = std::string("[OllamaGenerator] API error: ") + doc["error"]["message"].GetString();
      my_error(ER_ML_FAIL, MYF(0), m_error_string);
      return m_error_string;
    }
    // Ollama style: {"error":"..."}
    if (doc["error"].IsString()) {
      m_error_string = std::string("[OllamaGenerator] API error: ") + doc["error"].GetString();
      my_error(ER_ML_FAIL, MYF(0), m_error_string);
      return m_error_string;
    }
  }

parse_error:
  m_error_string =
      "[OllamaGenerator] Unknown or malformed response format.\n Raw (first 256 bytes): " + raw.substr(0, 256);
  my_error(ER_ML_FAIL, MYF(0), m_error_string);
  return m_error_string;
}

/*
 * HttpPost — libcurl synchronous POST with Bearer auth support
 **/
std::string OllamaGenerator::HttpPost(const std::string &url, const std::string &body) const {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) throw std::runtime_error("[OllamaGenerator] curl_easy_init() failed");

  // Build header list
  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  // Cloud providers require Bearer token
  if (!m_opts.api_key.empty()) {
    std::string auth = "Authorization: Bearer " + m_opts.api_key;
    headers = curl_slist_append(headers, auth.c_str());
  }

  std::string response_body;
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS,
                   static_cast<long>(m_opts.http_timeout_ms > 0 ? m_opts.http_timeout_ms : 120000L));
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 3L);

  CURLcode rc = curl_easy_perform(curl.get());
  curl_slist_free_all(headers);

  if (rc != CURLE_OK) throw std::runtime_error(std::string("[OllamaGenerator] curl error: ") + curl_easy_strerror(rc));

  long http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code != 200) {
    throw std::runtime_error("[OllamaGenerator] HTTP " + std::to_string(http_code) + " from " + url +
                             "\nBody: " + response_body.substr(0, 512));
  }

  return response_body;
}

/** Generate — main entry point
 * Routes to Ollama /api/generate  or  OpenAI /chat/completions
 * depending on the resolved provider.
 **/
OllamaGenerator::Result OllamaGenerator::Generate(const std::string &prompt, int maxNewTokens) {
  if (!m_initialized) throw std::runtime_error("[OllamaGenerator] Not initialized: " + m_error_string);

  std::string ep = m_opts.endpoint;
  if (!ep.empty() && ep.back() == '/') ep.pop_back();

  std::string url;
  std::string body;

  if (m_opts.provider == MLProvider::OLLAMA) {
    // Ollama local service — raw prompt mode
    url = m_generate_url;  // ep + "/api/generate"
    body = BuildRequestBody(prompt, maxNewTokens);
  } else {
    // All cloud providers use OpenAI /chat/completions
    url = ep + "/chat/completions";
    body = BuildOpenAIRequestBody(prompt, maxNewTokens);
  }

  DBUG_PRINT("info", ("[OllamaGenerator] Generate → url=%s model=%s", url.c_str(), m_opts.model_id.c_str()));

  const std::string raw = HttpPost(url, body);
  const std::string text = ParseResponse(raw);

  Result result;
  result.output = text;
  // result.tokens stays empty — remote APIs do not expose token IDs.
  return result;
}
}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase