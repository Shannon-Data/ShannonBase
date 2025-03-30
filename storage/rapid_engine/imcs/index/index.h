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
#include <vector>

#include "storage/rapid_engine/imcs/index/art/art.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {

class ART;

template <typename key_t, typename value_t>
class Index {
 public:
  enum class IndexType { ART = 0, B_TREE };

  explicit Index() : m_inited(false), m_type(IndexType::ART) {
    if (m_type != IndexType::ART) return;

    m_impl = std::make_unique<ART>();

    if (!m_inited) {
      m_impl->ART_tree_init();
      m_inited = m_impl->Art_initialized();
    }
  }

  explicit Index(std::string name) : m_inited(false), m_name(name), m_type(IndexType::ART) {
    if (m_type != IndexType::ART) return;

    m_impl = std::make_unique<ART>();

    if (!m_inited) {
      m_impl->ART_tree_init();
      m_inited = m_impl->Art_initialized();
    }
  }

  explicit Index(std::string name, IndexType type) : m_inited(false), m_name(name), m_type(type) {
    if (m_type != IndexType::ART) return;
    m_impl = std::make_unique<ART>();

    if (!m_inited) {
      m_impl->ART_tree_init();
      m_inited = m_impl->Art_initialized();
    }
  }

  virtual ~Index() {
    if (!m_impl.get()) return;
    m_impl->ART_tree_destroy();
    m_impl.reset(nullptr);
  }

  Index(Index &&) = delete;
  Index &operator=(Index &&) = delete;
  inline bool initialized() { return m_inited; }

  inline void set_name(std::string &name) { m_name = name; }

  // the root entry.
  inline ART::Art_node *root() const { return m_impl->root(); }

  // gets impl
  inline ART *impl() const { return m_impl.get(); }

  int insert(key_t *key, size_t key_len, value_t *value, size_t value_len) {
    if (!initialized()) return 1;

    m_impl->ART_insert(key, key_len, value, value_len);
    return 0;
  }

  int remove(key_t *key, size_t key_len) {
    if (!initialized()) return 1;

    auto value_ptr = m_impl->ART_delete(key, key_len);
    if (!value_ptr) return 1;
    return 0;
  }

  value_t *lookup(key_t *key, size_t key_len) {
    if (!initialized() || !key || !key_len) return nullptr;

    return reinterpret_cast<value_t *>(m_impl->ART_search(key, key_len));
  }

  // gets the maximun value. return 0 success, 1 failed.
  // the maximum value stores into value param.
  int maximum(key_t *value, size_t value_len, uint ind) {
    if (!initialized()) return 1;

    ART::Art_leaf *l = m_impl->ART_maximum();
    if (!l->values) return 1;

    if (ind >= l->vcount) return 1;
    *value = *reinterpret_cast<value_t *>(l->values[ind]);
    return 0;
  }

  // gets the minimum value. return 0 success, 1 failed.
  // the minimum value stores into value param.
  int minimum(key_t *value, size_t value_len, uint ind) {
    if (!initialized()) return 1;

    ART::Art_leaf *l = m_impl->ART_minimum();
    if (!l->values) return 1;

    if (ind >= l->vcount) return 1;
    *value = *reinterpret_cast<value_t *>(l->values[ind]);
    return 0;
  }

  IndexType type() { return m_type; }

 private:
  bool m_inited{false};
  std::string m_name;
  IndexType m_type{IndexType::ART};
  std::unique_ptr<ART> m_impl{nullptr};
  bool m_start_scan{false};
};

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_INDEX_H__