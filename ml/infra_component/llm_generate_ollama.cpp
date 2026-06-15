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

OllamaGenerator::OllamaGenerator(const GenerationOptions &opts) : m_opts(opts) {
  if (m_opts.model_id.empty()) {
    m_error_string = "[OllamaGenerator] model_id must not be empty";
    return;
  }
  if (m_opts.endpoint.empty()) {
    m_error_string = "[OllamaGenerator] endpoint must not be empty";
    return;
  }

  // Normalise endpoint — strip trailing slash once, append /api/generate.
  std::string ep = m_opts.endpoint;
  if (!ep.empty() && ep.back() == '/') ep.pop_back();
  m_generate_url = ep + "/api/generate";

  m_initialized = true;
}

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

std::string OllamaGenerator::ParseResponse(const std::string &raw_json) const {
  rapidjson::Document doc;
  doc.Parse(raw_json.c_str(), raw_json.size());

  if (doc.HasParseError()) {
    std::ostringstream oss;
    oss << "[OllamaGenerator] JSON parse error at offset " << doc.GetErrorOffset() << ": "
        << rapidjson::GetParseError_En(doc.GetParseError()) << "\nRaw response: " << raw_json.substr(0, 256);
    throw std::runtime_error(oss.str());
  }

  // /api/generate path
  if (doc.HasMember("response") && doc["response"].IsString()) return doc["response"].GetString();

  // /api/chat path
  if (doc.HasMember("message") && doc["message"].IsObject()) {
    const auto &msg = doc["message"];
    if (msg.HasMember("content") && msg["content"].IsString()) return msg["content"].GetString();
  }

  // Ollama error response:  { "error": "model not found" }
  if (doc.HasMember("error") && doc["error"].IsString()) {
    throw std::runtime_error(std::string("[OllamaGenerator] Ollama error: ") + doc["error"].GetString());
  }

  throw std::runtime_error(
      "[OllamaGenerator] Unexpected Ollama response format.\n"
      "Raw (first 256 bytes): " +
      raw_json.substr(0, 256));
}

std::string OllamaGenerator::HttpPost(const std::string &url, const std::string &body) const {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) {
    throw std::runtime_error("[OllamaGenerator] curl_easy_init() failed");
  }

  std::unique_ptr<struct curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
  headers.reset(curl_slist_append(headers.get(), "Content-Type: application/json"));
  if (!headers) {
    throw std::runtime_error("[OllamaGenerator] curl_slist_append() failed");
  }

  std::string response_body;
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write_cb);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(m_opts.http_timeout_ms));
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 3L);

  CURLcode rc = curl_easy_perform(curl.get());
  if (rc != CURLE_OK) {
    throw std::runtime_error(std::string("[OllamaGenerator] curl error: ") + curl_easy_strerror(rc));
  }

  long http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
  if (http_code != 200) {
    std::ostringstream oss;
    oss << "[OllamaGenerator] HTTP " << http_code << " from " << url << "\nBody: " << response_body.substr(0, 512);
    throw std::runtime_error(oss.str());
  }

  return response_body;
}

OllamaGenerator::Result OllamaGenerator::Generate(const std::string &userPrompt, int maxNewTokens) {
  if (!m_initialized) throw std::runtime_error("[OllamaGenerator] Not initialized: " + m_error_string);

  DBUG_PRINT("info",
             ("[OllamaGenerator] Generate → model=%s endpoint=%s", m_opts.model_id.c_str(), m_generate_url.c_str()));

  const std::string req_body = BuildRequestBody(userPrompt, maxNewTokens);
  const std::string raw_resp = HttpPost(m_generate_url, req_body);
  const std::string text = ParseResponse(raw_resp);

  Result result;
  result.output = text;
  // result.tokens stays empty — Ollama HTTP API does not expose token IDs.
  // If token-level access is needed, switch to Ollama's /api/generate with
  // "stream":true and parse each streamed token object.
  return result;
}
}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase