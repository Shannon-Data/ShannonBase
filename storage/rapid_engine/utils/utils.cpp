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
#include "storage/rapid_engine/utils/utils.h"

#include <iostream>
#include <iomanip>
#include <sstream>

#include "sql/field.h"                        //Field
#include "sql/my_decimal.h"                   //my_decimal
#include "sql/table.h"                        //TABLE

#include "storage/innobase/include/ut0dbg.h"  //ut_a

#include "storage/rapid_engine/compress/dictionary/dictionary.h"  //Dictionary
#include "storage/rapid_engine/include/rapid_const.h"

namespace ShannonBase {
namespace Utils {

bool Util::is_support_type(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_TYPED_ARRAY:
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET: {
      return false;
    } break;
    default:
      return true;
  }
  return false;
}
double Util::get_value_mysql_type(enum_field_types &types,
                                  Compress::Dictionary *&dictionary,
                                  const uchar *key, uint key_len) {
  double val{0};
  if (!key || !key_len || !dictionary) return val;

  switch (types) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      val = *(int *)key;
    } break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      val = *(double *)key;
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIME2: {
      Field_datetimef datetime(const_cast<uchar *>(key), nullptr, 0, 0,
                               "temp_datetime", 6);
      val = datetime.val_real();
    } break;
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      uchar *key_ptr = const_cast<uchar *>(key);
      val = dictionary->lookup(key_ptr);
    } break;
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_FLOAT: {
      val = *(double *)key;
    } break;
    default:
      break;
  }
  return val;
}
double Util::get_field_value(Field *&field, Compress::Dictionary *&dictionary) {
  ut_ad(field && dictionary);
  double data_val{0};
  if (!field->is_real_null()) {  // not null
    switch (field->type()) {
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_VARCHAR: {
        String buf;
        buf.set_charset(field->charset());
        field->val_str(&buf);
        data_val = dictionary->store(buf);
      } break;
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_FLOAT:
      case MYSQL_TYPE_DOUBLE: {
        data_val = field->val_real();
      } break;
      case MYSQL_TYPE_DECIMAL:
      case MYSQL_TYPE_NEWDECIMAL: {
        my_decimal dval;
        field->val_decimal(&dval);
        my_decimal2double(10, &dval, &data_val);
      } break;
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_TIME: {
        data_val = field->val_real();
      } break;
      default:
        data_val = field->val_real();
    }
  }
  return data_val;
}

double Util::get_field_value(enum_field_types type, const uchar* buf, uint len,
                            Compress::Dictionary *dictionary, CHARSET_INFO* charset) {
  ut_ad(buf && dictionary);
  double data_val{0};
  switch (type) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR: {
      String str_str((const char*)buf, len, charset);
      data_val = dictionary->store(str_str);
    } break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      data_val = *(int*)buf;
      break;
    case MYSQL_TYPE_FLOAT:
      data_val = *(float*) buf;
      break;
    case MYSQL_TYPE_DOUBLE:
      data_val = *(double*)buf;
      break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      data_val = *(double*)buf;
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIME: {
      data_val = *(double*)buf;
    } break;
    default:
      data_val = *(double*)buf;
  }

  return data_val;
}

double Util::store_field_value(TABLE *&table, Field *&field,
                               Compress::Dictionary *&dictionary,
                               double &value) {
  ut_a(field && dictionary);
  my_bitmap_map *old_map = tmp_use_all_columns(table, table->write_set);
  switch (field->type()) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR: {  // if string, stores its stringid, and gets from
                                // local dictionary.
      String str;
      dictionary->get(value, str,
                      *const_cast<CHARSET_INFO *>(field->charset()));
      field->store(str.c_ptr(), str.length(), &my_charset_bin);
    } break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: {
      field->store(value);
    } break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      field->store(value);
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_DATETIME: {
      field->store(value);
    } break;
    default:
      field->store(value);
  }
  if (old_map) tmp_restore_column_map(table->write_set, old_map);
  return value;
}

double Util::store_field_value(TABLE *&table, Field *&field,
                               Compress::Dictionary *&dictionary,
                               const uchar *key, uint key_len) {
  double val{0};
  if (!key || !key_len || !dictionary) return val;
  my_bitmap_map *old_map = tmp_use_all_columns(table, table->write_set);
  switch (field->type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      val = *(int *)key;
      field->store(val);
    } break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      val = *(double *)key;
      field->store(val);
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIME2: {
      Field_datetimef datetime(const_cast<uchar *>(key), nullptr, 0, 0,
                               "temp_datetime", 6);
      val = datetime.val_real();
      field->store(val);
    } break;
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      uchar *key_ptr = const_cast<uchar *>(key);
      val = dictionary->lookup(key_ptr);
      field->store(val);
    } break;
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_FLOAT: {
      val = *(double *)key;
      field->store(val);
    } break;
    default:
      break;
  }
  if (old_map) tmp_restore_column_map(table->write_set, old_map);
  return 0;
}
int Util::get_range_value(enum_field_types type,
                          Compress::Dictionary *&dictionary, key_range *min_key,
                          key_range *max_key, double &minkey, double &maxkey) {
  switch (type) {
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT: {
      minkey = min_key ? *(int *)min_key->key : SHANNON_LOWEST_DOUBLE;
      maxkey = max_key ? *(int *)max_key->key : SHANNON_LOWEST_DOUBLE;
    } break;
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      minkey = min_key ? *(int *)min_key->key : SHANNON_LOWEST_DOUBLE;
      maxkey = max_key ? *(int *)max_key->key : SHANNON_LOWEST_DOUBLE;
    } break;
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_FLOAT: {
      minkey = min_key ? *(double *)min_key->key : SHANNON_LOWEST_DOUBLE;
      maxkey = max_key ? *(double *)max_key->key : SHANNON_LOWEST_DOUBLE;
    } break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL: {
      minkey = min_key ? *(double *)min_key->key : SHANNON_LOWEST_DOUBLE;
      maxkey = max_key ? *(double *)max_key->key : SHANNON_LOWEST_DOUBLE;
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIME2: {
      minkey = maxkey = SHANNON_LOWEST_DOUBLE;
      if (min_key) {
        Field_datetimef datetime_min(const_cast<uchar *>(min_key->key), nullptr,
                                     0, 0, "start_datetime", 6);
        minkey = datetime_min.val_real();
      }
      if (max_key) {
        Field_datetimef datetime_max(const_cast<uchar *>(max_key->key), nullptr,
                                     0, 0, "start_datetime", 6);
        maxkey = datetime_max.val_real();
      }
    } break;
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      minkey = 0;
      maxkey = dictionary->content_size();
    } break;
    default:
      break;
  }
  return 0;
}

int Util::mem2string (uchar* buff, uint length, std::string& result) {
  const char* data = static_cast<const char*>((char*)buff);
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');

  for (size_t i = 0; i < length; ++i) {
      oss << std::setw(2) << static_cast<unsigned>(static_cast<unsigned char>(data[i]));
  }
  result = oss.str();
  return 0;
}

}  // namespace Utils
}  // namespace ShannonBase