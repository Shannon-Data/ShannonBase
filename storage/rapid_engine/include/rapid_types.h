/*
   Copyright (c) 2014, 2023, Oracle and/or its affiliates.

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

   Shannon Data AI.
*/
#ifndef __SHANNONBASE_RPD_TYPES_H__
#define __SHANNONBASE_RPD_TYPES_H__
#include <cstring>

#include "field_types.h"          //for MYSQL_TYPE_XXX
#include "include/my_alloc.h"     // MEM_ROOT
#include "include/my_inttypes.h"  //uint8_t
#include "sql/field.h"

#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {

// used for SHANNON_DB_TRX_ID FIELD.
class Mock_field_trxid : public Field_longlong {
 public:
  Mock_field_trxid()
      : Field_longlong(nullptr,                    // ptr_arg
                       8,                          // len_arg
                       &Field::dummy_null_buffer,  // null_ptr_arg
                       1,                          // null_bit_arg
                       Field::NONE,                // auto_flags_arg
                       SHANNON_DB_TRX_ID,          // field_name_arg
                       false,                      // zero_arg
                       true)                       // unsigned_arg
  {}

  void make_writable() { bitmap_set_bit(table->write_set, field_index()); }
  void make_readable() { bitmap_set_bit(table->read_set, field_index()); }
};

extern MEM_ROOT rapid_mem_root;
// the memor object for all rapid engine.
class MemoryObject {};

typedef struct alignas(CACHE_LINE_SIZE) BitArray {
  BitArray() = delete;

  explicit BitArray(size_t rows) {
    size = (rows + 7) / 8;
    data = new uint8_t[size];
    std::memset(data, 0x0, size);
  }

  BitArray(const BitArray &other) {
    size = other.size;
    if (size > 0) {
      data = new uint8_t[size];
      std::memcpy(data, other.data, size);
    }
  }

  BitArray(BitArray &&other) noexcept : data(std::exchange(other.data, nullptr)), size(std::exchange(other.size, 0)) {}

  ~BitArray() { delete[] data; }

  BitArray &operator=(const BitArray &other) {
    if (this != &other) {
      BitArray tmp(other);
      swap(tmp);
    }
    return *this;
  }

  BitArray &operator=(BitArray &&other) noexcept {
    if (this != &other) {
      delete[] data;
      data = std::exchange(other.data, nullptr);
      size = std::exchange(other.size, 0);
    }
    return *this;
  }

  void swap(BitArray &other) noexcept {
    std::swap(data, other.data);
    std::swap(size, other.size);
  }

  // data of BA, where to store the real bitmap.
  uint8_t *data{nullptr};
  // size of BA.
  size_t size{0};
} BitArray_t;

using bit_array_t = BitArray_t;

using mysql_field_t = struct mysql_field_info {
  // whether field nullable or not.
  bool has_nullbit{false};
  // if is nullable, true is null, or none-null.
  bool is_null{false};

  // type in mysql type. DATA_MISSING = 0.
  uint mtype{0u};

  // mysql field lenght.
  size_t mlength{0};

  // physical length(innodb).
  size_t plength{0};
  // field data.
  std::unique_ptr<uchar[]> data{nullptr};
};

using key_info_t = std::pair<uint, std::unique_ptr<uchar[]>>;

}  // namespace ShannonBase
#endif  //__SHANNONBASE_RPD_TYPES_H__