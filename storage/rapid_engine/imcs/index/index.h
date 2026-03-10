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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_INDEX_H__
#define __SHANNONBASE_INDEX_H__

#include <memory>
#include <string>
#include <vector>

#include "storage/rapid_engine/imcs/index/art/art.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {
template <typename key_t, typename value_t>
class Index {
 public:
  enum class IndexType { ART = 0, B_TREE };

  explicit Index() : m_type(IndexType::ART) { init_impl(); }

  explicit Index(std::string name) : m_name(std::move(name)), m_type(IndexType::ART) { init_impl(); }

  explicit Index(std::string name, IndexType type) : m_name(std::move(name)), m_type(type) {
    if (m_type == IndexType::ART) init_impl();
  }

  virtual ~Index() {
    if (m_impl) {
      m_impl->ART_tree_destroy();
      m_impl.reset();
    }
  }

  Index(Index &&) = delete;
  Index &operator=(Index &&) = delete;

  inline bool initialized() const { return m_inited; }

  inline void set_name(const std::string &name) { m_name = name; }

  inline ART::Art_node *root() const { return m_impl ? m_impl->root() : nullptr; }

  inline ART *impl() const { return m_impl.get(); }

  int insert(key_t *key, size_t key_len, value_t *value, size_t value_len) {
    if (!initialized()) return 1;
    m_impl->ART_insert(reinterpret_cast<const unsigned char *>(key), static_cast<int>(key_len), value,
                       static_cast<uint32_t>(value_len));
    return 0;
  }

  int remove(key_t *key, size_t key_len) {
    if (!initialized()) return 1;
    void *p = m_impl->ART_delete(reinterpret_cast<const unsigned char *>(key), static_cast<int>(key_len));
    return p ? 0 : 1;
  }

  value_t *lookup(key_t *key, size_t key_len) {
    if (!initialized() || !key || !key_len) return nullptr;
    return reinterpret_cast<value_t *>(
        m_impl->ART_search(reinterpret_cast<const unsigned char *>(key), static_cast<int>(key_len)));
  }

  /**
   * maximum — write the idx-th value of the maximum-key leaf into *out.
   * Returns 0 on success, 1 on failure.
   */
  int maximum(value_t *out, size_t /*value_len*/, uint idx) {
    if (!initialized() || !out) return 1;
    ART::Art_leaf *leaf = m_impl->ART_maximum();
    if (!leaf) return 1;
    // [H2] Use get_value_count() — vcount removed from Art_leaf.
    if (idx >= leaf->get_value_count()) return 1;
    auto *v = leaf->get_value(idx);
    if (!v || v->empty()) return 1;
    *out = *reinterpret_cast<const value_t *>(v->data());
    return 0;
  }

  /**
   * minimum — write the idx-th value of the minimum-key leaf into *out.
   * Returns 0 on success, 1 on failure.
   */
  int minimum(value_t *out, size_t /*value_len*/, uint idx) {
    if (!initialized() || !out) return 1;
    ART::Art_leaf *leaf = m_impl->ART_minimum();
    if (!leaf) return 1;
    if (idx >= leaf->get_value_count()) return 1;
    auto *v = leaf->get_value(idx);
    if (!v || v->empty()) return 1;
    *out = *reinterpret_cast<const value_t *>(v->data());
    return 0;
  }

  IndexType type() const { return m_type; }

 private:
  void init_impl() {
    if (m_type != IndexType::ART) return;
    m_impl = std::make_unique<ART>();
    m_impl->ART_tree_init();
    m_inited = m_impl->Art_initialized();
  }

  bool m_inited{false};
  std::string m_name;
  IndexType m_type{IndexType::ART};
  std::unique_ptr<ART> m_impl{nullptr};
};
}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  // __SHANNONBASE_INDEX_H__