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

  m_opts.endpoint = resolve_endpoint(m_opts);
  if (m_opts.endpoint.empty()) {
    m_error_string = "[OllamaGenerator] endpoint could not be resolved";
    return;
  }

  bool is_cloud = (m_opts.provider != MLProvider::OLLAMA);
  if (is_cloud && m_opts.endpoint.rfind("https://", 0) != 0) {
    m_error_string = "[OllamaGenerator] Cloud provider endpoint must use HTTPS: " + m_opts.endpoint;
    return;
  }

  std::string ep = m_opts.endpoint;
  if (!ep.empty() && ep.back() == '/') ep.pop_back();
  m_generate_url = ep + "/api/generate";

  /* A cloud round trip needs longer than the local default, but only the
     default may be raised: a caller that named a timeout gets the one it
     named. Raising an explicit value silently is how a turn deadline
     derived from the caller's own budget turned back into two minutes. */
  if (m_opts.provider != MLProvider::OLLAMA && m_opts.provider != MLProvider::ONNX_LOCAL &&
      !m_opts.http_timeout_explicit && m_opts.http_timeout_ms <= 30000) {
    m_opts.http_timeout_ms = 120000;
  }

  m_initialized = true;
}

/*
 * BuildRequestBody — Ollama /api/generate format (raw prompt)
 * Used only when provider == OLLAMA.**/
namespace {
void splice_extra_body(rapidjson::Writer<rapidjson::StringBuffer> &w, const std::string &extra_body) {
  if (extra_body.empty()) return;
  rapidjson::Document extra;
  extra.Parse(extra_body.c_str(), extra_body.size());
  if (extra.HasParseError() || !extra.IsObject()) return;

  for (auto it = extra.MemberBegin(); it != extra.MemberEnd(); ++it) {
    if (!it->name.IsString()) continue;
    rapidjson::StringBuffer vb;
    rapidjson::Writer<rapidjson::StringBuffer> vw(vb);
    it->value.Accept(vw);
    w.Key(it->name.GetString(), it->name.GetStringLength());
    w.RawValue(vb.GetString(), vb.GetSize(), it->value.GetType());
  }
}
}  // namespace

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
  splice_extra_body(w, m_opts.extra_body);
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

  splice_extra_body(w, m_opts.extra_body);
  w.EndObject();
  return sb.GetString();
}

std::string OllamaGenerator::BuildAnthropicRequestBody(const std::string &prompt, int maxNewTokens) const {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);

  w.StartObject();

  w.Key("model");
  w.String(m_opts.model_id.c_str());

  w.Key("max_tokens");
  w.Int(maxNewTokens > 0 ? maxNewTokens : m_opts.max_tokens);

  // Anthropic: system is a top-level field, NOT inside messages
  if (!m_opts.system_prompt.empty()) {
    w.Key("system");
    w.String(m_opts.system_prompt.c_str());
  }

  w.Key("messages");
  w.StartArray();
  w.StartObject();
  w.Key("role");
  w.String("user");
  w.Key("content");
  w.String(prompt.c_str());
  w.EndObject();
  w.EndArray();

  // Optional sampling params
  if (m_opts.temperature > 0.0f) {
    w.Key("temperature");
    w.Double(static_cast<double>(m_opts.temperature));
  }
  if (m_opts.top_p < 1.0f && m_opts.top_p > 0.0f) {
    w.Key("top_p");
    w.Double(static_cast<double>(m_opts.top_p));
  }

  splice_extra_body(w, m_opts.extra_body);
  w.EndObject();
  return sb.GetString();
}

/**
 * ParseResponse — handles all response formats:
 *   1. OpenAI /chat/completions  (openai / dashscope / qianfan / deepseek)
 *      {"choices":[{"message":{"content":"...","reasoning_content":"..."}}]}
 *   2. Ollama /api/generate
 *      {"response":"..."}
 *   3. Ollama /api/chat
 *      {"message":{"content":"..."}}
 *   4. Anthropic /v1/messages
 *      {"content":[{"type":"text","text":"..."}]}
 *
 * On failure: sets m_error_string and returns an empty string.
 * Does NOT call my_error — that is the caller's responsibility
 * (single point of error reporting at the UDF boundary).
 */
namespace {
/*
  Providers spell the same four outcomes four different ways. Mapping them
  here, once, is what lets the agent loop reason about "was this truncated"
  without a branch per provider -- and what stops a new provider from
  silently introducing a fifth spelling that every caller then has to learn.
  An unrecognised value maps to empty, i.e. "unknown", never to "stop".
*/
std::string normalise_finish_reason(const std::string &raw) {
  if (raw == "stop" || raw == "end_turn" || raw == "eos") return "stop";
  if (raw == "length" || raw == "max_tokens" || raw == "model_length") return "length";
  if (raw == "tool_calls" || raw == "tool_use" || raw == "function_call") return "tool_call";
  if (raw == "content_filter" || raw == "refusal") return "content_filter";
  if (raw == "stop_sequence") return "stop_sequence";
  return "";
}

/* usage is reported at the top level by every provider that reports it, but
   under three different key spellings. */
void extract_usage(const rapidjson::Document &doc, ShannonBase::ML::LLM_Generate::OllamaGenerator::Result &meta) {
  if (doc.HasMember("usage") && doc["usage"].IsObject()) {
    const auto &u = doc["usage"];
    /* OpenAI-compatible */
    if (u.HasMember("prompt_tokens") && u["prompt_tokens"].IsInt64())
      meta.prompt_tokens = u["prompt_tokens"].GetInt64();
    if (u.HasMember("completion_tokens") && u["completion_tokens"].IsInt64())
      meta.completion_tokens = u["completion_tokens"].GetInt64();
    /* Anthropic */
    if (u.HasMember("input_tokens") && u["input_tokens"].IsInt64()) meta.prompt_tokens = u["input_tokens"].GetInt64();
    if (u.HasMember("output_tokens") && u["output_tokens"].IsInt64())
      meta.completion_tokens = u["output_tokens"].GetInt64();
  }
  /* Ollama /api/generate reports counts at the top level instead. */
  if (doc.HasMember("prompt_eval_count") && doc["prompt_eval_count"].IsInt64())
    meta.prompt_tokens = doc["prompt_eval_count"].GetInt64();
  if (doc.HasMember("eval_count") && doc["eval_count"].IsInt64()) meta.completion_tokens = doc["eval_count"].GetInt64();
}
}  // namespace

std::string OllamaGenerator::ParseResponse(const std::string &raw, Result &meta) {
  rapidjson::Document doc;
  doc.Parse(raw.c_str(), raw.size());
  if (doc.HasParseError()) {
    m_error_string = "[OllamaGenerator] JSON parse error: " + raw.substr(0, 256);
    return "";
  }

  extract_usage(doc, meta);

  /* Anthropic puts it at the top level; OpenAI inside the choice; Ollama
     calls it done_reason. Read all three before the format branches below,
     because each of those returns as soon as it finds its text. */
  if (doc.HasMember("stop_reason") && doc["stop_reason"].IsString())
    meta.finish_reason = normalise_finish_reason(doc["stop_reason"].GetString());
  if (doc.HasMember("done_reason") && doc["done_reason"].IsString())
    meta.finish_reason = normalise_finish_reason(doc["done_reason"].GetString());
  if (doc.HasMember("choices") && doc["choices"].IsArray() && doc["choices"].Size() > 0) {
    const auto &c0 = doc["choices"][0];
    if (c0.IsObject() && c0.HasMember("finish_reason") && c0["finish_reason"].IsString())
      meta.finish_reason = normalise_finish_reason(c0["finish_reason"].GetString());
  }

  // Anthropic /v1/messages
  // {"content":[{"type":"text","text":"..."}], "model":"claude-..."}
  if (doc.HasMember("content") && doc["content"].IsArray() && doc["content"].Size() > 0) {
    for (rapidjson::SizeType i = 0; i < doc["content"].Size(); i++) {
      const auto &block = doc["content"][i];
      if (block.IsObject() && block.HasMember("type") && std::string(block["type"].GetString()) == "text" &&
          block.HasMember("text") && block["text"].IsString())
        return block["text"].GetString();
    }
  }

  // OpenAI-compatible (openai/dashscope/qianfan/deepseek)
  // {"choices":[{"message":{"content":"..."}}]}
  if (doc.HasMember("choices") && doc["choices"].IsArray() && doc["choices"].Size() > 0) {
    const auto &choice = doc["choices"][0];
    if (choice.IsObject() && choice.HasMember("message")) {
      const auto &msg = choice["message"];

      // DeepSeek thinking mode: log reasoning_content
      if (msg.HasMember("reasoning_content") && msg["reasoning_content"].IsString()) {
        DBUG_PRINT("info", ("[DeepSeek] reasoning_content: %.300s", msg["reasoning_content"].GetString()));
      }

      if (msg.HasMember("content") && msg["content"].IsString()) return msg["content"].GetString();
    }
  }

  // Ollama /api/generate
  if (doc.HasMember("response") && doc["response"].IsString()) return doc["response"].GetString();

  // Ollama /api/chat
  if (doc.HasMember("message") && doc["message"].IsObject()) {
    const auto &msg = doc["message"];
    if (msg.HasMember("content") && msg["content"].IsString()) return msg["content"].GetString();
  }

  // Error responses — set m_error_string, return empty (failure signal)
  if (doc.HasMember("error")) {
    // Anthropic: {"error":{"type":"...","message":"..."}}
    // OpenAI:    {"error":{"message":"..."}}
    if (doc["error"].IsObject() && doc["error"].HasMember("message") && doc["error"]["message"].IsString()) {
      m_error_string = std::string("[OllamaGenerator] API error: ") + doc["error"]["message"].GetString();
      return "";
    }
    if (doc["error"].IsString()) {
      m_error_string = std::string("[OllamaGenerator] API error: ") + doc["error"].GetString();
      return "";
    }
  }

  m_error_string = "[OllamaGenerator] Unknown response format: " + raw.substr(0, 256);
  return "";
}

/*
 * HttpPost — libcurl synchronous POST with Bearer auth support
 **/
std::string OllamaGenerator::HttpPost(const std::string &url, const std::string &body) {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) {
    m_error_string = "[OllamaGenerator] curl_easy_init() failed";
    return "";
  }

  bool is_cloud_provider = (m_opts.provider != MLProvider::OLLAMA);
  if (is_cloud_provider) {
    if (url.rfind("https://", 0) != 0) {
      m_error_string =
          "[OllamaGenerator] Refusing to send credentials over plain HTTP "
          "to a cloud provider endpoint: " +
          url;
      return "";
    }

    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
  } else {
    if (url.rfind("https://", 0) == 0) {
      curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
      curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    }
  }

  struct curl_slist *headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  if (m_opts.provider == MLProvider::ANTHROPIC) {
    if (!m_opts.api_key.empty()) {
      std::string xkey = "x-api-key: " + m_opts.api_key;
      headers = curl_slist_append(headers, xkey.c_str());
    }
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");
  } else {
    if (!m_opts.api_key.empty()) {
      std::string auth = "Authorization: Bearer " + m_opts.api_key;
      headers = curl_slist_append(headers, auth.c_str());
    }
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

  if (is_cloud_provider) {
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
  } else {
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 3L);
  }

  CURLcode rc = curl_easy_perform(curl.get());
  curl_slist_free_all(headers);

  if (rc != CURLE_OK) {
    m_error_string = std::string("[OllamaGenerator] curl error: ") + curl_easy_strerror(rc);
    return "";
  }

  long http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code != 200) {
    m_error_string = "[OllamaGenerator] HTTP " + std::to_string(http_code) + " from " + url +
                     "\nBody: " + response_body.substr(0, 512);
    return "";
  }
  return response_body;
}

/** Generate — main entry point
 * Routes to Ollama /api/generate  or  OpenAI /chat/completions
 * depending on the resolved provider.
 **/
OllamaGenerator::Result OllamaGenerator::Generate(const std::string &prompt, int maxNewTokens) {
  Result result;

  if (!m_initialized) {
    // m_error_string already carries the init failure reason
    // (e.g. "model_id must not be empty" / "endpoint could not be resolved")
    result.output = "";
    return result;
  }

  std::string ep = m_opts.endpoint;
  if (!ep.empty() && ep.back() == '/') ep.pop_back();

  std::string url, body;
  if (m_opts.provider == MLProvider::OLLAMA) {
    url = m_generate_url;  // ep + "/api/generate"
    body = BuildRequestBody(prompt, maxNewTokens);
  } else if (m_opts.provider == MLProvider::ANTHROPIC) {
    url = ep + "/messages";  // https://api.anthropic.com/v1/messages
    body = BuildAnthropicRequestBody(prompt, maxNewTokens);
  } else {
    // openai / dashscope / qianfan / deepseek → OpenAI /chat/completions
    url = ep + "/chat/completions";
    body = BuildOpenAIRequestBody(prompt, maxNewTokens);
  }

  DBUG_PRINT("info", ("[OllamaGenerator] Generate → provider=%d url=%s model=%s", static_cast<int>(m_opts.provider),
                      url.c_str(), m_opts.model_id.c_str()));

  const std::string raw = HttpPost(url, body);
  if (raw.empty()) {
    // m_error_string was set inside HttpPost
    result.output = "";
    return result;
  }

  if (m_opts.want_raw_response) result.raw_response = raw;

  const std::string text = ParseResponse(raw, result);
  if (text.empty() && !m_error_string.empty()) {
    // m_error_string was set inside ParseResponse
    result.output = "";
    return result;
  }

  result.output = text;
  return result;
}
}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase