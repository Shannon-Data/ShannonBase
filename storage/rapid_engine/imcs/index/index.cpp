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

#include "storage/rapid_engine/imcs/index/index.h"
#include "storage/rapid_engine/imcs/index/art.h"
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase{
namespace Imcs {

Index::Index(IndexType type) :m_type(type) {
  if (m_type != IndexType::ART) return;

  m_impl = std::make_unique<Art_index>();
  m_impl->ART_tree_init();
}
Index::~Index() {
  if (!m_impl.get()) return;

  m_impl->ART_tree_destroy();
}

void Index::reset_pos() {
  m_impl->ART_reset_cursor();
}

int Index::insert(uchar* key, uint key_len, uchar* value) {
  if (!m_impl->Art_initialized()) return 1;

  m_impl->ART_insert(key, key_len, reinterpret_cast<void*>(value));
  return 0;
}

int Index::remove(uchar* key, uint key_len)
{
  if (!m_impl->Art_initialized()) return 1;
  auto value_ptr = m_impl->ART_delete(key, key_len);
  if (!value_ptr) return 1;
  return 0;
}

int Index::lookup(uchar* key, uint key_len, uchar* value, uint value_len) {
  if (!m_impl->Art_initialized() || !key || !key_len) return 1;
  
  auto value_ptr = reinterpret_cast<uchar*> (m_impl->ART_search(key, key_len));
  if (!value_ptr) return 1;
  memcpy(value, value_ptr, value_len);

  return 0;
}

int Index::maximum(uchar* value, uint value_len) {
  if (!m_impl->Art_initialized()) return 1;

  Art_index::Art_leaf *l = m_impl->ART_maximum();
  if (!l->value) return 1;

  memcpy(value, l->value, value_len);
  return 0;
}

int Index::minimum(uchar* value, uint value_len) {
  if (!m_impl->Art_initialized()) return 1;

  Art_index::Art_leaf *l = m_impl->ART_minimum();
  if (!l->value) return 1;

  memcpy(value, l->value, value_len);
  return 0;
}

int Index::next(Art_index::ART_Func& func, uchar* out) {
  if (!m_start_scan) {
    m_impl->ART_reset_cursor();
    m_start_scan = true;
  }
  if (!m_impl->Art_initialized()) return 1;
  return m_impl->ART_iter(func, out, ShannonBase::SHANNON_ROW_TOTAL_LEN);
}

int Index::next_prefix() {
  
  return 0;
}

} //ns:imcs
} //ns:shannonbase