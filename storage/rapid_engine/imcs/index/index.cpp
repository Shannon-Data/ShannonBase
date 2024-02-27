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

#include <cstring>

#include "storage/rapid_engine/imcs/index/art.h"
#include "storage/rapid_engine/imcs/index/index.h"
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {
namespace Imcs {

Index::Index(IndexType type) : m_inited(false),m_type(type) {
  if (m_type != IndexType::ART) return;
  m_impl = std::make_unique<Art_index>();

  if (!m_inited) {
   m_impl->ART_tree_init();
   m_inited = true;
  }
}
Index::~Index() {
  if (!m_impl.get()) return;
  m_impl->ART_tree_destroy();
  m_impl.reset(nullptr);
}

void Index::reset_pos() { m_impl->ART_reset_cursor(); }

int Index::insert(uchar *key, uint key_len, uchar *value) {
  if (!initialized()) return 1;

  m_impl->ART_insert(key, key_len, reinterpret_cast<void *>(value));
  return 0;
}

int Index::remove(uchar *key, uint key_len) {
  if (!initialized()) return 1;

  auto value_ptr = m_impl->ART_delete(key, key_len);
  if (!value_ptr) return 1;
  return 0;
}

void* Index::lookup(uchar *key, uint key_len) {
  if (!initialized() || !key || !key_len)
    return nullptr;

  return reinterpret_cast<uchar *>(m_impl->ART_search(key, key_len));
}

int Index::maximum(uchar *value, uint value_len) {
  if (!initialized()) return 1;

  Art_index::Art_leaf *l = m_impl->ART_maximum();
  if (!l->value) return 1;

  memcpy(value, l->value, value_len);
  return 0;
}

int Index::minimum(uchar *value, uint value_len) {
  if (!initialized()) return 1;

  Art_index::Art_leaf *l = m_impl->ART_minimum();
  if (!l->value) return 1;

  memcpy(value, l->value, value_len);
  return 0;
}

void* Index::first(uint key_offset, uchar *key, uint key_len) {
  if (!initialized()) return nullptr;
  m_start_scan = true;
  m_impl->ART_reset_cursor();

  return m_impl->ART_iter_first(key_offset, key, key_len);
}

void* Index::next() {
  if (!initialized() || !m_start_scan) return nullptr;

  return m_impl->ART_iter_next();
}

void* Index::read_index(Art_index::ART_Func2 &func) {
  if (!initialized()) return nullptr;
  m_impl->ART_reset_cursor();
  m_start_scan = true;

  return m_impl->ART_iter(func);
}

}  // namespace Imcs
}  // namespace ShannonBase