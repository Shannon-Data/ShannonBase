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
#include "sql-common/my_decimal.h"
#include "sql/field.h"                                            //Field
#include "sql/tztime.h"                                           //timzone
#include "storage/rapid_engine/compress/dictionary/dictionary.h"  //Dictionary
#include "storage/rapid_engine/include/rapid_object.h"            // bit_array_t

class THD;
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
  template <typename T>
  static T get_field_numeric(const Field *field, const Compress::Dictionary *dictionary) {
    T data_val;
    auto old_map = tmp_use_all_columns(field->table, field->table->read_set);

    if (!field->is_real_null()) {  // not null
      switch (field->type()) {
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_VARCHAR: {
          String buf;
          buf.set_charset(field->charset());
          field->val_str(&buf);
          std::string str(buf.ptr(), buf.length());
          auto vdata_val = dictionary ? const_cast<Compress::Dictionary *>(dictionary)->get(str) : -1;
          data_val = static_cast<T>(vdata_val);
        } break;
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE: {
          auto vdata_val = field->val_real();
          data_val = static_cast<T>(vdata_val);
        } break;
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
          my_decimal dval;
          double v;
          field->val_decimal(&dval);
          my_decimal2double(10, &dval, &v);
          data_val = static_cast<T>(v);
        } break;
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_TIME: {
          auto vdata_val = field->val_real();
          data_val = static_cast<T>(vdata_val);
        } break;
        default: {
          auto vdata_val = field->val_real();
          data_val = static_cast<T>(vdata_val);
        }
      }
    }
    if (old_map) tmp_restore_column_map(field->table->read_set, old_map);
    return data_val;
  }

  template <typename T>
  static T get_field_numeric(Field *field, const uchar *data_ptr, const Compress::Dictionary *dict) {
    T data_val{};
    switch (field->type()) {
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_VARCHAR: {       // in LogParser::parse_rec_fields, the string field has been processed.
        data_val = static_cast<T>(0);  // stored in dictionary, and represented by a string id.
      } break;
      case MYSQL_TYPE_TINY: {
        data_val = static_cast<T>(*(int8 *)data_ptr);
      } break;
      case MYSQL_TYPE_SHORT: {
        data_val = static_cast<T>(*(int16 *)data_ptr);
      } break;
      case MYSQL_TYPE_INT24: {
        const long j = field->is_unsigned() ? (long)uint3korr(data_ptr) : sint3korr(data_ptr);
        data_val = static_cast<T>(j);
      } break;
      case MYSQL_TYPE_LONG: {
        int32 v;
        memcpy(&v, data_ptr, sizeof(v));
        data_val = (field->is_unsigned()) ? static_cast<T>((uint32)v) : static_cast<T>(v);
      } break;
      case MYSQL_TYPE_LONGLONG: {
        longlong v;
        memcpy(&v, data_ptr, sizeof(v));
        data_val =
            (field->is_unsigned()) ? static_cast<T>(ulonglong2double(static_cast<ulonglong>(v))) : static_cast<T>(v);
      } break;
      case MYSQL_TYPE_FLOAT: {
        // in fact, same with impl of float4get, floatget
        float v;
        memcpy(&v, data_ptr, sizeof(v));
        data_val = static_cast<T>(v);
      } break;
      case MYSQL_TYPE_DOUBLE: {
        data_val = static_cast<T>(*(double *)data_ptr);
      } break;
      case MYSQL_TYPE_DECIMAL: {
        double v;
        memcpy(&v, data_ptr, sizeof(v));
        data_val = static_cast<T>(v);
      } break;
      case MYSQL_TYPE_NEWDECIMAL: {
        auto new_filed = down_cast<Field_new_decimal *>(field);
        int prec = new_filed->precision;
        int scale = new_filed->decimals();
        my_decimal dv;
        double tdata_val;
        auto ret = binary2my_decimal(E_DEC_FATAL_ERROR & ~E_DEC_OVERFLOW, data_ptr, &dv, prec, scale, true);
        if (ret == E_DEC_OK) {
          my_decimal2double(E_DEC_FATAL_ERROR, &dv, &tdata_val);
        }
        data_val = static_cast<T>(tdata_val);
      } break;
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_NEWDATE: {
        if (field->real_type() == MYSQL_TYPE_NEWDATE) {
          ulong j = uint3korr(data_ptr);
          j = (j % 32L) + (j / 32L % 16L) * 100L + (j / (16L * 32L)) * 10000L;
          data_val = static_cast<T>(j);
        } else {  // TODO: impl it.
        }
      } break;
      case MYSQL_TYPE_TIME: {
        auto dec = down_cast<Field_timef *>(field)->decimals();
        MYSQL_TIME ltime;
        const longlong tmpl = my_time_packed_from_binary(data_ptr, dec);
        TIME_from_longlong_time_packed(&ltime, tmpl);

        const double tmp = TIME_to_double_time(ltime);
        data_val = static_cast<T>(ltime.neg ? -tmp : tmp);
      } break;
      case MYSQL_TYPE_YEAR: {
        int tmp = (int)data_ptr[0];
        if (tmp != 0) tmp += 1900;
        data_val = static_cast<T>((longlong)tmp);
      } break;
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_TIMESTAMP2: {  // convert double value to timestamp. TODO: impl it.
        data_val = static_cast<T>(0);
      } break;
      default:
        data_val = static_cast<T>(field->val_real());
    }
    return data_val;
  }

  static int get_range_value(enum_field_types, const Compress::Dictionary *, const key_range *, const key_range *,
                             double &, double &);
  static int mem2string(uchar *buff, uint length, std::string &result);

  // convert a string padding format(with padding). if the length is less then
  // pack length, then padding the string with `space`(0x20) with dest charset.
  static uchar *pack_str(uchar *from, size_t length, const CHARSET_INFO *from_cs, uchar *to, size_t to_length,
                         const CHARSET_INFO *to_cs);

  static inline std::string get_key_name(Field *field) {
    std::ostringstream ostr;
    ostr << field->table->s->db.str << ":" << *field->table_name << ":" << field->field_name;
    return ostr.str();
  }

  static inline std::string get_key_name(const char *db_name, const char *table_name, const char *field_name) {
    std::ostringstream ostr;
    ostr << db_name << ":" << table_name << ":" << field_name;
    return ostr.str();
  }

  // check Nth is 1 or not.
  static inline bool bit_array_get(bit_array_t *ba, size_t n) {
    if (!ba) return false;

    size_t byte_index = n / 8;
    size_t bit_index = n % 8;
    return (ba->data[byte_index] & (1 << bit_index)) != 0;
  }

  // set Nth is 1.
  static inline void bit_array_set(bit_array_t *ba, size_t n) {
    size_t byte_index = n / 8;
    size_t bit_index = n % 8;
    ba->data[byte_index] |= (1 << bit_index);
  }

  // reset Nth is 0.
  static inline void bit_array_reset(bit_array_t *ba, size_t n) {
    size_t byte_index = n / 8;
    size_t bit_index = n % 8;
    ba->data[byte_index] &= ~(1 << bit_index);
  }

  static inline bool is_blob(enum_field_types type) {
    return (type == MYSQL_TYPE_BLOB || type == MYSQL_TYPE_TINY_BLOB || type == MYSQL_TYPE_MEDIUM_BLOB ||
            type == MYSQL_TYPE_LONG_BLOB)
               ? true
               : false;
  }

  static inline bool is_varstring(enum_field_types type) {
    /**if this is a string type, it will be use local dictionary encoding, therefore,
     * using stringid as field value. */
    return ((type == MYSQL_TYPE_VARCHAR || type == MYSQL_TYPE_VAR_STRING) ? true : false);
  }

  static inline bool is_string(enum_field_types type) {
    return (type == MYSQL_TYPE_VARCHAR || type == MYSQL_TYPE_VAR_STRING || type == MYSQL_TYPE_STRING) ? true : false;
  }

  // use cost info to determine which engine should be used.
  static bool standard_cost_threshold_classifier(THD *thd);

  // use decision tree to determine which engine should be used.
  static bool decision_tree_classifier(THD *thd);

  static bool dynamic_feature_normalization(THD *thd);

  static bool check_dict_encoding_projection(THD *thd);

  static void write_trace_reason(THD *thd, const char *text, const char *reason);

  static std::vector<std::string> split(const std::string &str, char delimiter);

  static uint normalized_length(const Field *field);
};

template <typename T>
class Encoder {
 public:
  static void EncodeFloat(T value, unsigned char *key);

  static T DecodeFloat(const unsigned char *key);
};

class ColumnMapGuard {
  TABLE *table;
  my_bitmap_map *old_map;

 public:
  ColumnMapGuard(TABLE *t);
  ~ColumnMapGuard();
};

}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_H__