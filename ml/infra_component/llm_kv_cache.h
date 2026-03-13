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

   The fundmental code for imcs.
   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/

// ml/infra_component/kv_cache.h
#ifndef __SHANNONBASE_KV_CACHE_H__
#define __SHANNONBASE_KV_CACHE_H__

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ShannonBase {
namespace ML {
namespace LLM_Generate {
template <typename T>
struct KVLayerCache {
  std::vector<T> key;
  std::vector<T> value;

  int heads = 0;
  int head_dim = 0;
  int max_seq = 0;
  int seq = 0;

  KVLayerCache() = default;

  void init(int h, int dim, int max_seq_len) {
    heads = h;
    head_dim = dim;
    max_seq = max_seq_len;

    size_t total = (size_t)heads * max_seq * head_dim;

    try {
      key.resize(total);
      value.resize(total);
    } catch (const std::bad_alloc &e) {
      fprintf(stderr, "[ERROR] Failed to allocate cache memory: %s\n", e.what());
      throw;
    }

    if (key.size() != total || value.size() != total) {
      fprintf(stderr, "[ERROR] Cache allocation failed: expected=%zu, key=%zu, value=%zu\n", total, key.size(),
              value.size());
    }

    seq = 0;
  }

  inline size_t token_stride() const { return (size_t)heads * head_dim; }

  inline T *key_ptr(int token_index) { return key.data() + token_index * token_stride(); }

  inline T *value_ptr(int token_index) { return value.data() + token_index * token_stride(); }

  inline const T *key_ptr(int token_index) const { return key.data() + token_index * token_stride(); }

  inline const T *value_ptr(int token_index) const { return value.data() + token_index * token_stride(); }

  void append(const T *new_key, const T *new_value, int token_count) {
    if (token_count <= 0) return;
    if (new_key == nullptr || new_value == nullptr) return;

    int available = max_seq - seq;
    if (available <= 0) {
      return;
    }

    int actual_tokens = token_count;
    if (actual_tokens > available) actual_tokens = available;

    size_t stride = token_stride();
    size_t copy_bytes = stride * actual_tokens * sizeof(T);

    T *key_dst = key_ptr(seq);
    T *value_dst = value_ptr(seq);

    memcpy(key_dst, new_key, copy_bytes);
    memcpy(value_dst, new_value, copy_bytes);

    seq += actual_tokens;
  }

  void set_from_onnx_layout(const T *key_src, const T *value_src, int token_count) {
    assert(token_count > 0 && token_count <= max_seq);
    assert(key_src != nullptr);
    assert(value_src != nullptr);

    size_t total_elements = (size_t)heads * token_count * head_dim;

    if (key.size() < total_elements) {
      key.resize(total_elements);
    }
    if (value.size() < total_elements) {
      value.resize(total_elements);
    }

    memcpy(key.data(), key_src, total_elements * sizeof(T));
    memcpy(value.data(), value_src, total_elements * sizeof(T));

    seq = token_count;
  }

  bool is_valid() const {
    return heads > 0 && head_dim > 0 && max_seq > 0 && !key.empty() && !value.empty() &&
           key.size() == (size_t)heads * max_seq * head_dim && value.size() == (size_t)heads * max_seq * head_dim;
  }

  void clear() { seq = 0; }
};

template <typename T>
class KVCacheManager {
 public:
  int num_layers = 0;
  int num_heads = 0;
  int head_dim = 0;
  int max_seq_len = 0;

  std::vector<KVLayerCache<T>> layers;

 public:
  KVCacheManager() = default;

  void init(int layers_, int heads_, int dim_, int max_seq_) {
    num_layers = layers_;
    num_heads = heads_;
    head_dim = dim_;
    max_seq_len = max_seq_;

    layers.resize(num_layers);
    for (auto &l : layers) {
      l.init(num_heads, head_dim, max_seq_len);
    }
  }

  // Check if cache is initialized
  bool is_initialized() const { return !layers.empty() && layers[0].heads > 0; }

  void clear() {
    for (auto &l : layers) l.clear();
  }

  int seq_len() const {
    if (layers.empty()) return 0;
    return layers[0].seq;
  }

  void init_layer_from_onnx(int layer, const T *key, const T *value, int token_count) {
    if (layer < 0 || layer >= num_layers) {
      throw std::out_of_range("Layer index out of range");
    }
    layers[layer].set_from_onnx_layout(key, value, token_count);
  }

  // Append new tokens to specific layer
  void append_layer(int layer, const T *key, const T *value, int token_count) {
    if (layer < 0 || layer >= num_layers) {
      throw std::out_of_range("Layer index out of range");
    }
    layers[layer].append(key, value, token_count);
  }

  // Get key tensor for all tokens (for ONNX tensor creation)
  T *key_data(int layer) { return layers[layer].key.data(); }

  T *value_data(int layer) { return layers[layer].value.data(); }

  const T *key_data(int layer) const { return layers[layer].key.data(); }

  const T *value_data(int layer) const { return layers[layer].value.data(); }

  // Get pointer to specific token's keys
  T *key_ptr(int layer, int token_index) { return layers[layer].key_ptr(token_index); }

  T *value_ptr(int layer, int token_index) { return layers[layer].value_ptr(token_index); }

  // Get current sequence length for a layer
  int layer_seq(int layer) const {
    if (layer < 0 || layer >= num_layers) return 0;
    return layers[layer].seq;
  }

  // Get total elements per layer
  size_t layer_elements(int layer) const { return (size_t)layers[layer].seq * layers[layer].token_stride(); }
};

// Type alias for variant support (to maintain compatibility with existing code)
template <typename T>
using layer_cache_t = std::vector<T>;

template <typename T>
using full_cache_t = std::vector<layer_cache_t<T>>;
}  // namespace LLM_Generate
}  // namespace ML
}  // namespace ShannonBase
#endif  // __SHANNONBASE_KV_CACHE_H__