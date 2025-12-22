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

#include <chrono>
#include <condition_variable>
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
#include "storage/rapid_engine/include/rapid_types.h"             // bit_array_t

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
  // open a table via schema name and table name.
  static TABLE *open_table_by_name(THD *thd, std::string schema_name, std::string table_name, thr_lock_type mode);

  // close a opened table.
  static int close_table(THD *thd, TABLE *table);

  // to check whether this type is supported or not.
  static bool is_support_type(enum_field_types type);

  template <typename T>
  static T get_field_numeric(Field *field, const uchar *data_ptr, const Compress::Dictionary *dict,
                             bool db_low_byte_first = false) {
    T data_val{};

    auto safe_cast = [](auto value) -> T {
      if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(value);
      } else {
        if (static_cast<T>(value) > std::numeric_limits<T>::max()) {
          return std::numeric_limits<T>::max();
        } else if (static_cast<T>(value) < std::numeric_limits<T>::lowest()) {
          return std::numeric_limits<T>::lowest();
        }
        return static_cast<T>(value);
      }
    };

    switch (field->type()) {
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_VARCHAR: {
        data_val = safe_cast(0);
      } break;

      case MYSQL_TYPE_TINY: {
        // Field_tiny::val_int() impl
        const int tmp = field->is_unsigned() ? (int)data_ptr[0] : (int)((signed char *)data_ptr)[0];
        data_val = safe_cast((longlong)tmp);
      } break;

      case MYSQL_TYPE_SHORT: {
        // Field_short::val_int() impl
        short j;
        if (db_low_byte_first)
          j = sint2korr(data_ptr);
        else
          j = shortget(data_ptr);
        longlong value = field->is_unsigned() ? (longlong)(unsigned short)j : (longlong)j;
        data_val = safe_cast(value);
      } break;

      case MYSQL_TYPE_INT24: {
        // Field_medium::val_int() impl
        const long j = field->is_unsigned() ? (long)uint3korr(data_ptr) : sint3korr(data_ptr);
        data_val = safe_cast((longlong)j);
      } break;

      case MYSQL_TYPE_LONG: {
        // Field_long::val_int() impl
        int32 j;
        if (db_low_byte_first)
          j = sint4korr(data_ptr);
        else
          j = longget(data_ptr);
        longlong value = field->is_unsigned() ? (longlong)(uint32)j : (longlong)j;
        data_val = safe_cast(value);
      } break;

      case MYSQL_TYPE_LONGLONG: {
        // Field_longlong::val_int() impl
        longlong value;
        if (db_low_byte_first)
          value = sint8korr(data_ptr);
        else
          value = longlongget(data_ptr);
        if (field->is_unsigned()) {
          data_val = safe_cast(static_cast<double>(static_cast<ulonglong>(value)));
        } else {
          data_val = safe_cast(value);
        }
      } break;

      case MYSQL_TYPE_FLOAT: {
        // Field_float::val_real() impl
        double value;
        if (db_low_byte_first)
          value = double{float4get(data_ptr)};
        else
          value = double{floatget(data_ptr)};
        data_val = safe_cast(value);
      } break;

      case MYSQL_TYPE_DOUBLE: {
        // Field_double::val_real() impl
        double value;
        if (db_low_byte_first)
          value = float8get(data_ptr);
        else
          value = doubleget(data_ptr);
        data_val = safe_cast(value);
      } break;

      case MYSQL_TYPE_DECIMAL: {
        double value;
        memcpy(&value, data_ptr, sizeof(value));
        data_val = safe_cast(value);
      } break;

      case MYSQL_TYPE_NEWDECIMAL: {
        auto new_field = down_cast<Field_new_decimal *>(field);
        int prec = new_field->precision;
        int scale = new_field->decimals();
        my_decimal dv;
        double tmp_val;
        auto ret = binary2my_decimal(E_DEC_FATAL_ERROR & ~E_DEC_OVERFLOW, data_ptr, &dv, prec, scale, true);
        if (ret == E_DEC_OK) {
          my_decimal2double(E_DEC_FATAL_ERROR, &dv, &tmp_val);
          data_val = safe_cast(tmp_val);
        } else {
          data_val = safe_cast(0);
        }
      } break;

      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_NEWDATE: {
        if (field->real_type() == MYSQL_TYPE_NEWDATE) {
          ulong j = uint3korr(data_ptr);
          j = (j % 32L) + (j / 32L % 16L) * 100L + (j / (16L * 32L)) * 10000L;
          data_val = safe_cast(j);
        } else {
          // TODO: impl other datetime type.
          data_val = safe_cast(0);
        }
      } break;

      case MYSQL_TYPE_TIME: {
        auto dec = down_cast<Field_timef *>(field)->decimals();
        MYSQL_TIME ltime;
        const longlong tmpl = my_time_packed_from_binary(data_ptr, dec);
        TIME_from_longlong_time_packed(&ltime, tmpl);
        const double tmp = TIME_to_double_time(ltime);
        data_val = safe_cast(ltime.neg ? -tmp : tmp);
      } break;

      case MYSQL_TYPE_YEAR: {
        int tmp = (int)data_ptr[0];
        if (tmp != 0) tmp += 1900;
        data_val = safe_cast((longlong)tmp);
      } break;

      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_TIMESTAMP2: {
        // TODO: timestamp.
        data_val = safe_cast(0);
      } break;

      default: {
        data_val = safe_cast(0);
      } break;
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
    ostr << field->table->s->db.str << "." << *field->table_name << "." << field->field_name;
    return ostr.str();
  }

  static inline std::string get_key_name(const char *db_name, const char *table_name, const char *field_name) {
    std::ostringstream ostr;
    ostr << db_name << "." << table_name << "." << field_name;
    return ostr.str();
  }

  // check Nth is 1 or not.
  static inline bool bit_array_get(bit_array_t *ba, size_t n) {
    if (!ba || !ba->data || n >= ba->size * 8) return false;

    size_t byte_index = n / 8;
    size_t bit_index = n % 8;
    return (ba->data[byte_index] & (1 << bit_index)) != 0;
  }

  // set Nth is 1.
  static inline void bit_array_set(bit_array_t *ba, size_t n) {
    if (!ba || !ba->data || n >= ba->size * 8) return;

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

  static inline std::string currenttime_to_string() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm = *std::localtime(&now_time_t);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "." << std::setfill('0') << std::setw(3) << now_ms.count();
    return oss.str();
  }
};

class ColumnMapGuard {
 public:
  enum class TYPE : uint8_t { READ, WRITE, ALL };
  ColumnMapGuard(TABLE *t, TYPE type = TYPE::READ);
  ~ColumnMapGuard();

  ColumnMapGuard(const ColumnMapGuard &) = delete;
  ColumnMapGuard &operator=(const ColumnMapGuard &) = delete;

 private:
  TYPE bit_type{TYPE::READ};
  TABLE *table{nullptr};
  my_bitmap_map *old_rmap{nullptr};
  my_bitmap_map *old_wmap{nullptr};
};

}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_H__