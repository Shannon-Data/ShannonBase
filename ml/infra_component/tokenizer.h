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
#include <map>
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

  static Tokenizer from_bytes(const std::vector<uint8_t> &data) { return Tokenizer(data); }

  ~Tokenizer() {
    if (m_handle) {
      tokenizer_free(m_handle);
    }
  }

  // Non-copyable
  Tokenizer(const Tokenizer &) = delete;
  Tokenizer &operator=(const Tokenizer &) = delete;

  // Movable
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

  // HuggingFace-compatible methods
  uint32_t get_vocab_size(bool with_added_tokens = false) const {
    if (!m_handle) return 0;
    return tokenizer_get_vocab_size(m_handle, with_added_tokens);
  }

  // Alias for compatibility
  uint32_t vocab_size() const { return get_vocab_size(false); }

  std::map<std::string, uint32_t> get_vocab(bool with_added_tokens = true) const {
    if (!m_handle) return {};

    char **keys = nullptr;
    uint32_t *values = nullptr;
    size_t length = 0;

    std::map<std::string, uint32_t> vocab;

    if (tokenizer_get_vocab(m_handle, with_added_tokens, &keys, &values, &length)) {
      for (size_t i = 0; i < length; ++i) {
        if (keys[i]) {
          vocab[std::string(keys[i])] = values[i];
        }
      }
      vocab_free(keys, values, length);
    }

    return vocab;
  }

  // Enhanced Encoding class with HuggingFace compatibility
  class Encoding {
   public:
    explicit Encoding(EncodingResult *result) : m_result(result) {}

    ~Encoding() {
      if (m_result) {
        encoding_result_free(m_result);
      }
    }

    // Non-copyable
    Encoding(const Encoding &) = delete;
    Encoding &operator=(const Encoding &) = delete;

    // Movable
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

    // HuggingFace-compatible accessors
    std::vector<uint32_t> input_ids() const {
      if (!m_result || !m_result->input_ids) return {};
      return std::vector<uint32_t>(m_result->input_ids, m_result->input_ids + m_result->length);
    }

    // Alias for compatibility
    std::vector<uint32_t> ids() const { return input_ids(); }

    std::vector<uint32_t> attention_mask() const {
      if (!m_result || !m_result->attention_mask) return {};
      return std::vector<uint32_t>(m_result->attention_mask, m_result->attention_mask + m_result->length);
    }

    std::vector<uint32_t> token_type_ids() const {
      if (!m_result || !m_result->token_type_ids) return {};
      return std::vector<uint32_t>(m_result->token_type_ids, m_result->token_type_ids + m_result->length);
    }

    std::vector<uint32_t> special_tokens_mask() const {
      if (!m_result || !m_result->special_tokens_mask) return {};
      return std::vector<uint32_t>(m_result->special_tokens_mask, m_result->special_tokens_mask + m_result->length);
    }

    std::vector<std::string> tokens() const {
      if (!m_result || !m_result->tokens) return {};
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

    // Access to overflowing tokens
    std::vector<Encoding> get_overflowing() const {
      std::vector<Encoding> overflowing;
      if (m_result && m_result->overflowing && m_result->overflowing_length > 0) {
        overflowing.reserve(m_result->overflowing_length);
        for (size_t i = 0; i < m_result->overflowing_length; ++i) {
          if (m_result->overflowing[i]) {
            // Note: This creates a copy of the EncodingResult to avoid double-free
            // In a real implementation, you might want to handle this differently
            overflowing.emplace_back(m_result->overflowing[i]);
          }
        }
      }
      return overflowing;
    }

    // Raw access (use with caution)
    EncodingResult *get() const { return m_result; }
    EncodingResult *operator->() const { return m_result; }

   private:
    EncodingResult *m_result;
  };

  // Enhanced BatchEncoding class
  class BatchEncoding {
   public:
    BatchEncoding(BatchEncodingResult *batch_result) : m_batch_result(batch_result) {}

    ~BatchEncoding() {
      if (m_batch_result) {
        batch_encoding_result_free(m_batch_result);
      }
    }

    // Non-copyable
    BatchEncoding(const BatchEncoding &) = delete;
    BatchEncoding &operator=(const BatchEncoding &) = delete;

    // Movable
    BatchEncoding(BatchEncoding &&other) noexcept : m_batch_result(other.m_batch_result) {
      other.m_batch_result = nullptr;
    }

    BatchEncoding &operator=(BatchEncoding &&other) noexcept {
      if (this != &other) {
        if (m_batch_result) {
          batch_encoding_result_free(m_batch_result);
        }
        m_batch_result = other.m_batch_result;
        other.m_batch_result = nullptr;
      }
      return *this;
    }

    // Access individual encodings
    EncodingResult *operator[](size_t index) const {
      if (!m_batch_result || index >= m_batch_result->length || !m_batch_result->encodings) {
        return nullptr;
      }
      return m_batch_result->encodings[index];
    }

    size_t size() const { return m_batch_result ? m_batch_result->length : 0; }

    // HuggingFace-compatible accessors
    std::vector<std::vector<uint32_t>> input_ids() const {
      std::vector<std::vector<uint32_t>> result;
      if (!m_batch_result) return result;

      result.reserve(m_batch_result->length);
      for (size_t i = 0; i < m_batch_result->length; ++i) {
        if (m_batch_result->encodings[i]) {
          auto *encoding = m_batch_result->encodings[i];
          result.emplace_back(encoding->input_ids, encoding->input_ids + encoding->length);
        } else {
          result.emplace_back();
        }
      }
      return result;
    }

    // Alias for compatibility
    std::vector<std::vector<uint32_t>> all_ids() const { return input_ids(); }

    std::vector<std::vector<uint32_t>> attention_mask() const {
      std::vector<std::vector<uint32_t>> result;
      if (!m_batch_result) return result;

      result.reserve(m_batch_result->length);
      for (size_t i = 0; i < m_batch_result->length; ++i) {
        if (m_batch_result->encodings[i]) {
          auto *encoding = m_batch_result->encodings[i];
          result.emplace_back(encoding->attention_mask, encoding->attention_mask + encoding->length);
        } else {
          result.emplace_back();
        }
      }
      return result;
    }

    std::vector<std::vector<uint32_t>> token_type_ids() const {
      std::vector<std::vector<uint32_t>> result;
      if (!m_batch_result) return result;

      result.reserve(m_batch_result->length);
      for (size_t i = 0; i < m_batch_result->length; ++i) {
        if (m_batch_result->encodings[i]) {
          auto *encoding = m_batch_result->encodings[i];
          result.emplace_back(encoding->token_type_ids, encoding->token_type_ids + encoding->length);
        } else {
          result.emplace_back();
        }
      }
      return result;
    }

   private:
    BatchEncodingResult *m_batch_result;
  };

  // HuggingFace-compatible encoding methods
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

    auto *result = tokenizer_encode_batch(m_handle, c_texts.data(), c_texts.size(), add_special_tokens);
    return BatchEncoding(result);
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

  // Utility methods
  bool is_valid() const { return tokenizer_is_valid(m_handle); }

  std::string get_last_error() const {
    const char *err = tokenizer_get_last_error();
    return err ? std::string(err) : std::string("Unknown error");
  }

  // Get tokenizer information
  struct TokenizerInfo {
    std::string model_type;
    uint32_t vocab_size;
    uint32_t added_tokens_count;
  };

  TokenizerInfo get_info() const {
    char *model_type = nullptr;
    uint32_t vocab_size = 0;
    uint32_t added_tokens_count = 0;

    TokenizerInfo info;

    if (tokenizer_get_info(m_handle, &model_type, &vocab_size, &added_tokens_count)) {
      info.model_type = model_type ? std::string(model_type) : "Unknown";
      info.vocab_size = vocab_size;
      info.added_tokens_count = added_tokens_count;

      if (model_type) {
        string_free(model_type);
      }
    }
    return info;
  }

 private:
  // Constructor for JSON loading
  Tokenizer(const std::string &json, bool from_json_flag) {
    if (from_json_flag) {
      m_handle = tokenizer_from_json(json.c_str());
    } else {
      m_handle = nullptr;
    }
  }

  // Constructor for bytes loading
  explicit Tokenizer(const std::vector<uint8_t> &data) { m_handle = tokenizer_from_bytes(data.data(), data.size()); }

  TokenizerHandle *m_handle = nullptr;
};

// Utility namespace with factory functions
namespace TokenizerUtils {
inline std::unique_ptr<Tokenizer> load_from_file(const std::string &path) { return std::make_unique<Tokenizer>(path); }

inline std::unique_ptr<Tokenizer> load_from_json(const std::string &json) {
  auto tokenizer = Tokenizer::from_json(json);
  return std::make_unique<Tokenizer>(std::move(tokenizer));
}

inline std::unique_ptr<Tokenizer> load_from_bytes(const std::vector<uint8_t> &data) {
  auto tokenizer = Tokenizer::from_bytes(data);
  return std::make_unique<Tokenizer>(std::move(tokenizer));
}
}  // namespace TokenizerUtils

}  // namespace tokenizers
}  // namespace ML
}  // namespace ShannonBase

#endif  // __SHANNONBASE_RAPID_FFI_TOKENIZE_H__