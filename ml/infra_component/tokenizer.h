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

struct ChatMessage {
  std::string role;
  std::string content;

  ChatMessage(const std::string &r, const std::string &c) : role(r), content(c) {}
  ChatMessage(const char *r, const char *c) : role(r ? r : ""), content(c ? c : "") {}
};

class Tokenizer {
 public:
  explicit Tokenizer(const char *path) {
    m_handle = tokenizer_from_file(path);
    if (!m_handle && tokenizer_has_error()) {  // Failed to load tokenizer
      m_handle = nullptr;
    }
  }

  explicit Tokenizer(const std::string &path) : Tokenizer(path.c_str()) {}

  static Tokenizer from_json(const std::string &json) { return Tokenizer(json, true); }

  static Tokenizer from_bytes(const std::vector<uint8_t> &data) { return Tokenizer(data); }

  ~Tokenizer() {
    if (m_handle) tokenizer_free(m_handle);
  }

  // Non-copyable
  Tokenizer(const Tokenizer &) = delete;
  Tokenizer &operator=(const Tokenizer &) = delete;

  // Movable
  Tokenizer(Tokenizer &&other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }

  Tokenizer &operator=(Tokenizer &&other) noexcept {
    if (this != &other) {
      if (m_handle) tokenizer_free(m_handle);
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
        if (keys[i]) vocab[std::string(keys[i])] = values[i];
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
      if (m_result) encoding_result_free(m_result);
    }

    // Non-copyable
    Encoding(const Encoding &) = delete;
    Encoding &operator=(const Encoding &) = delete;

    // Movable
    Encoding(Encoding &&other) noexcept : m_result(other.m_result) { other.m_result = nullptr; }

    Encoding &operator=(Encoding &&other) noexcept {
      if (this != &other) {
        if (m_result) encoding_result_free(m_result);
        m_result = other.m_result;
        other.m_result = nullptr;
      }
      return *this;
    }

    // Check if encoding is valid
    bool is_valid() const { return m_result != nullptr; }

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
        if (m_result->tokens[i]) result.emplace_back(m_result->tokens[i]);
      }
      return result;
    }

    size_t length() const { return m_result ? m_result->length : 0; }

    // Access to overflowing tokens - Fixed to avoid double-free
    std::vector<std::unique_ptr<Encoding>> get_overflowing() const {
      std::vector<std::unique_ptr<Encoding>> overflowing;
      if (m_result && m_result->overflowing && m_result->overflowing_length > 0) {
        overflowing.reserve(m_result->overflowing_length);
        for (size_t i = 0; i < m_result->overflowing_length; ++i) {
          if (m_result->overflowing[i]) {
            // Create a new Encoding that takes ownership
            overflowing.emplace_back(std::make_unique<Encoding>(m_result->overflowing[i]));
            // Set to nullptr to avoid double-free
            m_result->overflowing[i] = nullptr;
          }
        }
        // Clear the overflowing array to prevent cleanup issues
        m_result->overflowing_length = 0;
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
      if (m_batch_result) batch_encoding_result_free(m_batch_result);
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
        if (m_batch_result) batch_encoding_result_free(m_batch_result);
        m_batch_result = other.m_batch_result;
        other.m_batch_result = nullptr;
      }
      return *this;
    }

    // Check if batch encoding is valid
    bool is_valid() const { return m_batch_result != nullptr; }

    // Access individual encodings - Return by value to avoid ownership issues
    std::unique_ptr<Encoding> get_encoding(size_t index) const {
      if (!m_batch_result || index >= m_batch_result->length || !m_batch_result->encodings)
        return std::make_unique<Encoding>(nullptr);

      // Create a copy of the EncodingResult to avoid double-free
      EncodingResult *original = m_batch_result->encodings[index];
      if (!original) return std::make_unique<Encoding>(nullptr);

      // Note: This is a simplified approach. In a production environment,
      // you might want to implement proper deep copying of EncodingResult
      return std::make_unique<Encoding>(original);
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
    if (!m_handle) return Encoding(nullptr);
    auto result = tokenizer_encode(m_handle, text.c_str(), add_special_tokens);
    return Encoding(result);
  }

  BatchEncoding encode_batch(const std::vector<std::string> &texts, bool add_special_tokens = true) const {
    if (!m_handle || texts.empty()) return BatchEncoding(nullptr);

    std::vector<const char *> c_texts;
    c_texts.reserve(texts.size());
    for (const auto &text : texts) c_texts.push_back(text.c_str());

    auto result = tokenizer_encode_batch(m_handle, c_texts.data(), c_texts.size(), add_special_tokens);
    return BatchEncoding(result);
  }

  std::string decode(const std::vector<uint32_t> &ids, bool skip_special_tokens = true) const {
    if (!m_handle || ids.empty()) return "";
    auto s = tokenizer_decode(m_handle, ids.data(), ids.size(), skip_special_tokens);
    if (!s) return "";
    std::string result(s);
    string_free(s);
    return result;
  }

  /**
   * Apply chat template to a conversation - Enhanced with better error handling
   * @param messages Vector of ChatMessage objects
   * @param add_generation_prompt Whether to add generation prompt at the end
   * @param custom_template Optional custom template (empty string uses default)
   * @return Formatted conversation string
   */
  std::string apply_chat_template(const std::vector<ChatMessage> &messages, bool add_generation_prompt = true,
                                  const std::string &custom_template = "") const {
    if (!m_handle || messages.empty()) return "";

    // Validate message roles before proceeding
    for (const auto &msg : messages) {
      if (msg.role != "system" && msg.role != "user" &&
          msg.role != "assistant") {  // Invalid role Must be 'system', 'user', or 'assistant
        return "";
      }
    }

    // Convert C++ ChatMessage to C ChatMessage
    std::vector<::ChatMessage> c_messages;
    std::vector<std::string> role_storage, content_storage;  // Keep strings alive

    c_messages.reserve(messages.size());
    role_storage.reserve(messages.size());
    content_storage.reserve(messages.size());

    for (const auto &msg : messages) {
      role_storage.push_back(msg.role);
      content_storage.push_back(msg.content);

      ::ChatMessage c_msg;
      c_msg.role = role_storage.back().c_str();
      c_msg.content = content_storage.back().c_str();
      c_messages.push_back(c_msg);
    }

    const char *template_ptr = custom_template.empty() ? nullptr : custom_template.c_str();

    char *formatted = tokenizer_apply_chat_template(m_handle, c_messages.data(), c_messages.size(),
                                                    add_generation_prompt, template_ptr);

    if (!formatted) {  // Failed to apply chat template
      std::string error_msg = "Failed to apply chat template";
      return error_msg;
    }

    std::string result(formatted);
    string_free(formatted);
    return result;
  }

  /**
   * Apply chat template and encode in one step - Enhanced with better error handling
   */
  Encoding apply_chat_template_and_encode(const std::vector<ChatMessage> &messages, bool add_generation_prompt = true,
                                          const std::string &custom_template = "",
                                          bool add_special_tokens = true) const {
    if (!m_handle || messages.empty()) return Encoding(nullptr);

    // Validate message roles before proceeding
    for (const auto &msg : messages) {
      if (msg.role != "system" && msg.role != "user" && msg.role != "assistant") return Encoding(nullptr);
    }

    // Convert C++ ChatMessage to C ChatMessage
    std::vector<::ChatMessage> c_messages;
    std::vector<std::string> role_storage, content_storage;

    c_messages.reserve(messages.size());
    role_storage.reserve(messages.size());
    content_storage.reserve(messages.size());

    for (const auto &msg : messages) {
      role_storage.push_back(msg.role);
      content_storage.push_back(msg.content);

      ::ChatMessage c_msg;
      c_msg.role = role_storage.back().c_str();
      c_msg.content = content_storage.back().c_str();
      c_messages.push_back(c_msg);
    }

    const char *template_ptr = custom_template.empty() ? nullptr : custom_template.c_str();

    auto *result = tokenizer_apply_chat_template_and_encode(m_handle, c_messages.data(), c_messages.size(),
                                                            add_generation_prompt, template_ptr, add_special_tokens);

    if (!result && tokenizer_has_error()) {  // Failed to apply chat template and encode
      return Encoding(nullptr);
    }
    return Encoding(result);
  }

  /**
   * Check if tokenizer has chat template support
   * @return True if chat templates are supported
   */
  bool has_chat_template() const {
    if (!m_handle) return false;
    return tokenizer_has_chat_template(m_handle);
  }

  /**
   * Get the current chat template string
   * @return Chat template string, or empty string if not available
   */
  std::string get_chat_template() const {
    if (!m_handle) return "";

    char *template_str = tokenizer_get_chat_template(m_handle);
    if (!template_str) return "";

    std::string result(template_str);
    string_free(template_str);
    return result;
  }

  /**
   * Apply chat template to multiple conversations
   * @param conversations Vector of conversation (each conversation is a vector of ChatMessage)
   * @param add_generation_prompt Whether to add generation prompt at the end
   * @param custom_template Optional custom template (empty string uses default)
   * @return Vector of formatted conversation strings
   */
  std::vector<std::string> apply_chat_template_batch(const std::vector<std::vector<ChatMessage>> &conversations,
                                                     bool add_generation_prompt = true,
                                                     const std::string &custom_template = "") const {
    std::vector<std::string> results;
    results.reserve(conversations.size());

    for (const auto &conversation : conversations) {
      try {
        results.push_back(apply_chat_template(conversation, add_generation_prompt, custom_template));
      } catch (const std::exception &) {
        results.push_back("");  // Return empty string on error
      }
    }

    return results;
  }

  /**
   * Apply chat template and encode multiple conversations
   * @param conversations Vector of conversation (each conversation is a vector of ChatMessage)
   * @param add_generation_prompt Whether to add generation prompt at the end
   * @param custom_template Optional custom template (empty string uses default)
   * @param add_special_tokens Whether to add special tokens during encoding
   * @return Vector of Encoding results
   */
  std::vector<Encoding> apply_chat_template_and_encode_batch(const std::vector<std::vector<ChatMessage>> &conversations,
                                                             bool add_generation_prompt = true,
                                                             const std::string &custom_template = "",
                                                             bool add_special_tokens = true) const {
    std::vector<Encoding> results;
    results.reserve(conversations.size());

    for (const auto &conversation : conversations) {
      try {
        results.push_back(
            apply_chat_template_and_encode(conversation, add_generation_prompt, custom_template, add_special_tokens));
      } catch (const std::exception &) {
        results.emplace_back(nullptr);  // Return invalid encoding on error
      }
    }

    return results;
  }

  /**
   * Create a simple user-assistant conversation and apply template
   * @param user_message The user's message
   * @param system_message Optional system message (empty string to skip)
   * @param add_generation_prompt Whether to add generation prompt
   * @return Formatted conversation string
   */
  std::string format_chat(const std::string &user_message, const std::string &system_message = "",
                          bool add_generation_prompt = true) const {
    std::vector<ChatMessage> messages;

    if (!system_message.empty()) {
      messages.emplace_back("system", system_message);
    }
    messages.emplace_back("user", user_message);

    return apply_chat_template(messages, add_generation_prompt);
  }

  /**
   * Create and encode a simple user-assistant conversation
   * @param user_message The user's message
   * @param system_message Optional system message (empty string to skip)
   * @param add_generation_prompt Whether to add generation prompt
   * @param add_special_tokens Whether to add special tokens during encoding
   * @return Encoding result
   */
  Encoding encode_chat(const std::string &user_message, const std::string &system_message = "",
                       bool add_generation_prompt = true, bool add_special_tokens = true) const {
    std::vector<ChatMessage> messages;

    if (!system_message.empty()) {
      messages.emplace_back("system", system_message);
    }
    messages.emplace_back("user", user_message);

    return apply_chat_template_and_encode(messages, add_generation_prompt, "", add_special_tokens);
  }

  /**
   * Add an assistant response to a conversation and reformat
   * @param messages Existing conversation
   * @param assistant_response The assistant's response to add
   * @param add_generation_prompt Whether to add generation prompt after assistant response
   * @return Updated formatted conversation
   */
  std::string continue_chat(std::vector<ChatMessage> &messages, const std::string &assistant_response,
                            bool add_generation_prompt = false) const {
    messages.emplace_back("assistant", assistant_response);
    return apply_chat_template(messages, add_generation_prompt);
  }

  // Utility methods
  bool is_valid() const { return m_handle && tokenizer_is_valid(m_handle); }

  std::string get_last_error() const {
    const char *err = tokenizer_get_last_error();
    return err ? std::string(err) : std::string("");
  }

  void clear_error() const { tokenizer_clear_error(); }

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

    if (m_handle && tokenizer_get_info(m_handle, &model_type, &vocab_size, &added_tokens_count)) {
      info.model_type = model_type ? std::string(model_type) : "Unknown";
      info.vocab_size = vocab_size;
      info.added_tokens_count = added_tokens_count;

      if (model_type) {
        string_free(model_type);
      }
    } else {
      info.model_type = "Unknown";
      info.vocab_size = 0;
      info.added_tokens_count = 0;
    }
    return info;
  }

 private:
  // Constructor for JSON loading
  Tokenizer(const std::string &json, bool from_json_flag) {
    if (from_json_flag) {
      m_handle = tokenizer_from_json(json.c_str());
      if (!m_handle && tokenizer_has_error()) {  // Failed to load tokenizer from JSON
        m_handle = nullptr;
      }
    } else {
      m_handle = nullptr;
    }
  }

  // Constructor for bytes loading
  explicit Tokenizer(const std::vector<uint8_t> &data) {
    m_handle = tokenizer_from_bytes(data.data(), data.size());
    if (!m_handle && tokenizer_has_error()) {
      m_handle = nullptr;
    }
  }

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

// Helper function to create ChatMessage easily
inline ChatMessage make_message(const std::string &role, const std::string &content) {
  return ChatMessage(role, content);
}

// Helper function to create a simple conversation
inline std::vector<ChatMessage> make_conversation(const std::string &user_msg, const std::string &system_msg = "") {
  std::vector<ChatMessage> messages;
  if (!system_msg.empty()) {
    messages.emplace_back("system", system_msg);
  }
  messages.emplace_back("user", user_msg);
  return messages;
}

}  // namespace TokenizerUtils

}  // namespace tokenizers
}  // namespace ML
}  // namespace ShannonBase

#endif  // __SHANNONBASE_RAPID_FFI_TOKENIZE_H__