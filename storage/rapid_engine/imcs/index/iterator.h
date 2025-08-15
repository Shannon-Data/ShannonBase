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
#ifndef __SHANNONBASE_INDEX_ITERATOR_H__
#define __SHANNONBASE_INDEX_ITERATOR_H__

#include <memory>
#include <vector>

#include "my_inttypes.h"

#include "storage/rapid_engine/imcs/index/art/art.h"
#include "storage/rapid_engine/imcs/index/art/iterator.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {

class Iterator {
 public:
  explicit Iterator(ART *) {}
  virtual ~Iterator() = default;

  /* initialize the scan.
   startkey: the start key of the scan.
   startkey_len: the length of the start key.
   start_inclusive: whether the start key is inclusive.
   endkey: the end key of the scan.
   endkey_len: the length of the end key.
   end_inclusive: whether the end key is inclusive.
   if startkey is nullptr, then the scan starts from the first key.
   if endkey is nullptr, then the scan ends at the last key.
   if start_inclusive is true, then the start key is included in the scan.
   if end_inclusive is true, then the end key is included in the scan.
   */
  virtual void init_scan(const uchar *startkey, int startkey_len, bool start_inclusive, const uchar *endkey,
                         int endkey_len, bool end_inclusive) = 0;

  virtual bool next(const uchar **key_out, uint32_t *key_len_out, void *value_out) = 0;

  virtual bool initialized() = 0;
};

class Art_Iterator : public Iterator {
 public:
  explicit Art_Iterator(ART *art) : Iterator(art), m_tree(art ? art->tree() : nullptr) {
    if (art) {
      m_art_iter = std::make_unique<ARTIterator<uchar, row_id_t>>(art);
    }
  }
  ~Art_Iterator() = default;

  void init_scan(const uchar *startkey, int startkey_len, bool start_inclusive, const uchar *endkey, int endkey_len,
                 bool end_inclusive) override {
    if (m_art_iter) {
      m_art_iter->init_scan(startkey, startkey_len, start_inclusive, endkey, endkey_len, end_inclusive);
      m_initialized = true;
    }
  }

  bool next(const uchar **key_out, uint32_t *key_len_out, void *value_out) override {
    if (!m_art_iter) return false;

    const uchar *temp_key;
    row_id_t temp_value;

    bool result = m_art_iter->next(&temp_key, key_len_out, &temp_value);

    if (result) {
      *key_out = temp_key;
      *static_cast<row_id_t *>(value_out) = temp_value;
    }

    return result;
  }

  bool initialized() override { return m_initialized; }

 private:
  ART::Art_tree *m_tree;
  bool m_initialized{false};
  std::unique_ptr<ARTIterator<uchar, row_id_t>> m_art_iter;
};

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //#define __SHANNONBASE_INDEX_ITERATOR_H__