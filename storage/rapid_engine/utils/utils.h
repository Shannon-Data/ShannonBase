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
#ifndef __SHANNONBASE_UTILS_H__
#define __SHANNONBASE_UTILS_H__

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "include/field_types.h"
#include "include/my_inttypes.h"
#include "sql/field.h"  //Field

#include "storage/rapid_engine/include/rapid_object.h"  // bit_array_t

class TABLE;
class Field;
class key_range;
class CHARSET_INFO;
namespace ShannonBase {
namespace Compress {
class Dictionary;
}

namespace Utils {
class Util {
 public:
  // to check whether this type is supported or not.
  static bool is_support_type(enum_field_types type);

  // get the data value.
  static double get_value_mysql_type(enum_field_types &, Compress::Dictionary *&, const uchar *, uint);
  static double get_field_value(Field *&, Compress::Dictionary *&);

  static double get_field_value(enum_field_types, const uchar *, uint, Compress::Dictionary *, CHARSET_INFO *charset);

  static double store_field_value(TABLE *&table, Field *&, Compress::Dictionary *&, double &);
  static double store_field_value(TABLE *&table, Field *&, Compress::Dictionary *&, const uchar *, uint);
  static int get_range_value(enum_field_types, Compress::Dictionary *&, key_range *, key_range *, double &, double &);
  static int mem2string(uchar *buff, uint length, std::string &result);

  // convert a string padding format(with padding). if the length is less then pack
  // length, then padding the string with `space`(0x20) with dest charset.
  static uchar *pack_str(uchar *from, size_t length, const CHARSET_INFO *from_cs, uchar *to, size_t to_length,
                         const CHARSET_INFO *to_cs);

  inline static std::string get_key_name(Field *field) {
    std::ostringstream ostr;
    ostr << field->table->s->db.str << ":" << *field->table_name << ":" << field->field_name;
    return ostr.str();
  }

  inline static std::string get_key_name(const char *db_name, const char *table_name, const char *field_name) {
    std::ostringstream ostr;
    ostr << db_name << ":" << table_name << ":" << field_name;
    return ostr.str();
  }

  // check Nth is 1 or not.
  inline static bool bit_array_get(bit_array_t *ba, size_t n) {
    if (!ba) return false;

    size_t byte_index = n / 8;
    size_t bit_index = n % 8;
    return (ba->data[byte_index] & (1 << bit_index)) != 0;
  }

  // set Nth is 1.
  inline static void bit_array_set(bit_array_t *ba, size_t n) {
    size_t byte_index = n / 8;
    size_t bit_index = n % 8;
    ba->data[byte_index] |= (1 << bit_index);
  }
};

}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_H__