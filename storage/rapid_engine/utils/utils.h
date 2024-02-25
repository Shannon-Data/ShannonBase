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

#include <string>

#include "include/field_types.h"
#include "include/my_inttypes.h"

class TABLE;
class Field;
class key_range;
namespace ShannonBase {
namespace Compress {
class Dictionary;
}

namespace Utils {
class Util {
 public:
  static bool is_support_type(enum_field_types type);
  static double get_value_mysql_type(enum_field_types &,
                                     Compress::Dictionary *&, const uchar *,
                                     uint);
  static double get_field_value(Field *&, Compress::Dictionary *&);
  static double store_field_value(TABLE *&table, Field *&,
                                  Compress::Dictionary *&, double &);
  static double store_field_value(TABLE *&table, Field *&,
                                  Compress::Dictionary *&, const uchar *, uint);
  static int get_range_value(enum_field_types, Compress::Dictionary *&,
                             key_range *, key_range *, double &, double &);
  static int mem2string (uchar* buff, uint length, std::string& result);
};

}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_H__