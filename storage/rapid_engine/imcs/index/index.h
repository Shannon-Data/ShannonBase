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
#include "storage/rapid_engine/imcs/index/art.h"

namespace ShannonBase {
namespace Imcs {
class Art_index;

class Index {
public:
  enum class IndexType{
    ART = 0,
    B_TREE
  };
  explicit Index(IndexType);
  ~Index();
  Index(Index&&) = delete;
  Index& operator=(Index&&) = delete;

  int insert(uchar* key, uint key_len, uchar* value);
  int remove(uchar* key, uint key_len);
  int lookup(uchar* key, uint key_len, uchar* value, uint value_len);
  int maximum(uchar* value, uint value_len);
  int minimum(uchar* value, uint value_len);
  int next(Art_index::ART_Func& func, uchar* out);
  void* next_fast(uint key_offset, unsigned char* key, uint key_len);
  int next_prefix();

  void reset_pos();
  IndexType type() { return m_type; }
private:
  IndexType m_type {IndexType::ART};
  std::unique_ptr<Art_index> m_impl {nullptr};
  bool m_start_scan {false};
};

} //ns:imcs
} //ns:shannonbase
#endif //__SHANNONBASE_INDEX_H__