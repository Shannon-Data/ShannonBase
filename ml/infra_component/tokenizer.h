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

   The fundmental code for imcs. Using `Llama-3.2-3B-Instruct` to generate
   response text.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/

#ifndef __SHANNONBASE_RAPID_FFI_TOKENIZE_H__
#define __SHANNONBASE_RAPID_FFI_TOKENIZE_H__

#include "extra/tokenizer-ffi/include/tokenizer_ffi.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace ShannonBase {
namespace ML {
namespace tokenizers {
class Tokenizer {
 public:
  explicit Tokenizer(const char *path) { m_handle = tokenizer_from_file(path); }

  explicit Tokenizer(const std::string &path) : Tokenizer(path.c_str()) {}

  static Tokenizer from_json(const std::string &json) { return Tokenizer(json, true); }

  ~Tokenizer() {
    if (m_handle) {
      tokenizer_free(m_handle);
    }
  }

  Tokenizer(const Tokenizer &) = delete;
  Tokenizer &operator=(const Tokenizer &) = delete;

  Tokenizer(Tokenizer &&other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }

  Tokenizer &operator=(Tokenizer &&other) noexcept {
    if (this != &other) {
      if (m_handle) {
        tokenizer_free(m_handle);
      }
      m_handle = other.m_handle;
      other.m_handle = nullptr;
    }
    return *this;
  }

  uint32_t vocab_size() const {
    uint32_t size = tokenizer_get_vocab_size(m_handle);
    if (size == 0) {
      return 0;
    }
    return size;
  }

  class Encoding {
   public:
    explicit Encoding(EncodingResult *result) : m_result(result) {}

    ~Encoding() {
      if (m_result) {
        encoding_result_free(m_result);
      }
    }

    Encoding(const Encoding &) = delete;
    Encoding &operator=(const Encoding &) = delete;

    Encoding(Encoding &&other) noexcept : m_result(other.m_result) { other.m_result = nullptr; }

    Encoding &operator=(Encoding &&other) noexcept {
      if (this != &other) {
        if (m_result) {
          encoding_result_free(m_result);
        }
        m_result = other.m_result;
        other.m_result = nullptr;
      }
      return *this;
    }

    EncodingResult *get() const { return m_result; }
    EncodingResult *operator->() const { return m_result; }

    std::vector<uint32_t> ids() const {
      if (!m_result) return {};
      return std::vector<uint32_t>(m_result->ids, m_result->ids + m_result->length);
    }

    std::vector<uint32_t> attention_mask() const {
      if (!m_result) return {};
      return std::vector<uint32_t>(m_result->attention_mask, m_result->attention_mask + m_result->length);
    }

    std::vector<std::string> tokens() const {
      if (!m_result) return {};
      std::vector<std::string> result;
      result.reserve(m_result->length);
      for (size_t i = 0; i < m_result->length; ++i) {
        if (m_result->tokens[i]) {
          result.emplace_back(m_result->tokens[i]);
        }
      }
      return result;
    }

    size_t length() const { return m_result ? m_result->length : 0; }

   private:
    EncodingResult *m_result;
  };

  class BatchEncoding {
   public:
    BatchEncoding(EncodingResult **results, size_t count) : m_results(results), m_count(count) {}

    ~BatchEncoding() {
      if (m_results) {
        encoding_result_array_free(m_results, m_count);
      }
    }

    BatchEncoding(const BatchEncoding &) = delete;
    BatchEncoding &operator=(const BatchEncoding &) = delete;

    BatchEncoding(BatchEncoding &&other) noexcept : m_results(other.m_results), m_count(other.m_count) {
      other.m_results = nullptr;
      other.m_count = 0;
    }

    BatchEncoding &operator=(BatchEncoding &&other) noexcept {
      if (this != &other) {
        if (m_results) {
          encoding_result_array_free(m_results, m_count);
        }
        m_results = other.m_results;
        m_count = other.m_count;
        other.m_results = nullptr;
        other.m_count = 0;
      }
      return *this;
    }

    EncodingResult *operator[](size_t index) const {
      if (index >= m_count || !m_results) {
        return nullptr;
      }
      return m_results[index];
    }

    size_t size() const { return m_count; }

    std::vector<std::vector<uint32_t>> all_ids() const {
      std::vector<std::vector<uint32_t>> result;
      result.reserve(m_count);
      for (size_t i = 0; i < m_count; ++i) {
        if (m_results[i]) {
          result.emplace_back(m_results[i]->ids, m_results[i]->ids + m_results[i]->length);
        } else {
          result.emplace_back();
        }
      }
      return result;
    }

   private:
    EncodingResult **m_results;
    size_t m_count;
  };

  Encoding encode(const std::string &text, bool add_special_tokens = true) const {
    auto *result = tokenizer_encode(m_handle, text.c_str(), add_special_tokens);
    return Encoding(result);
  }

  BatchEncoding encode_batch(const std::vector<std::string> &texts, bool add_special_tokens = true) const {
    std::vector<const char *> c_texts;
    c_texts.reserve(texts.size());
    for (const auto &text : texts) {
      c_texts.push_back(text.c_str());
    }

    auto *results = tokenizer_encode_batch(m_handle, c_texts.data(), c_texts.size(), add_special_tokens);
    return BatchEncoding(results, c_texts.size());
  }

  std::string decode(const std::vector<uint32_t> &ids, bool skip_special_tokens = true) const {
    if (ids.empty()) {
      return "";
    }

    auto *s = tokenizer_decode(m_handle, ids.data(), ids.size(), skip_special_tokens);
    std::string result(s);
    string_free(s);
    return result;
  }

  bool is_valid() const { return tokenizer_is_valid(m_handle); }

 private:
  Tokenizer(const std::string &json, bool from_json_flag) {
    if (from_json_flag) {
      m_handle = tokenizer_from_json(json.c_str());
    } else
      m_handle = nullptr;
  }

  static std::string get_last_error() {
    const char *err = tokenizer_get_last_error();
    if (!err) {
      return "Unknown error";
    }
    return std::string(err);
  }

  TokenizerHandle *m_handle = nullptr;
};

namespace TokenizerUtils {
inline std::unique_ptr<Tokenizer> load_from_file(const std::string &path) { return std::make_unique<Tokenizer>(path); }

inline std::unique_ptr<Tokenizer> load_from_json(const std::string &json) {
  auto tokenizer = Tokenizer::from_json(json);
  return std::make_unique<Tokenizer>(std::move(tokenizer));
}
}  // namespace TokenizerUtils

}  // namespace tokenizers
}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_LLM_GENERATE_H__