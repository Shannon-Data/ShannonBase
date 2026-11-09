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
#ifndef __SHANNONBASE_UTILS_TPP_H__
#define __SHANNONBASE_UTILS_TPP_H__

// Must be included at global scope: inside a namespace this probe lands there and
// the check below fails, instead of nesting ShannonBase::Utils under it.
struct shannonbase_utils_tpp_scope_probe {};
static_assert(sizeof(::shannonbase_utils_tpp_scope_probe) > 0,
              "utils.tpp must be included at global scope, not inside a namespace");

#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Utils {
/**
 * Load a value of type T from an arbitrary (possibly unaligned) address.
 *
 * Avoids the alignment / strict-aliasing UB of `*reinterpret_cast<T*>(p)`.
 * Compilers lower the fixed-length memcpy to an unaligned load on targets
 * where that is cheap (x86-64), and emit safe byte loads elsewhere (ARM).
 */
template <typename T>
inline T load_unaligned(const uchar *p) {
  T v{};
  std::memcpy(&v, p, sizeof(T));
  return v;
}

// The single STL allocation boundary. std::pmr containers must receive a
// throwing allocator (returning nullptr is undefined behavior). Keep exception
// conversion at individual container/object allocations, never around an
// operator body: callers name the failed buffer and choose spill or hard error.
// Bounded resources retain the reason independently of this STL adapter.
template <typename Allocate>
inline bool TryAllocation(Allocate &&allocate) noexcept {
  try {
    allocate();
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

template <typename Buffer>
inline bool Reserve(Buffer &buffer, size_t capacity) noexcept {
  if (capacity <= buffer.capacity()) return true;
  if (capacity > buffer.max_size()) return false;
  return TryAllocation([&] { buffer.reserve(capacity); });
}

// Retained append-only state needs geometric growth, rather than reserve(size+1)
// on every row. Charge both old and new buffers through the original allocator.
template <typename Buffer>
inline bool Grow(Buffer &buffer, size_t required) noexcept {
  if (required <= buffer.capacity()) return true;
  const size_t old = buffer.capacity();
  const size_t grown = old > buffer.max_size() / 2 ? buffer.max_size() : std::max<size_t>(1, old * 2);
  return Reserve(buffer, std::max(required, grown));
}

template <typename Container, typename... Args>
inline bool TryEmplace(Container &container, Args &&...args) noexcept {
  return TryAllocation([&] { container.emplace(std::forward<Args>(args)...); });
}

template <typename Container, typename... Args>
inline bool TryEmplaceAt(Container &container, typename Container::iterator *result, Args &&...args) noexcept {
  return TryAllocation([&] { *result = container.emplace(std::forward<Args>(args)...); });
}

// POD byte I/O shared by every persistence path; trivial types only.
template <typename T>
inline void append_pod(std::string &out, const T &v) {
  out.append(reinterpret_cast<const char *>(&v), sizeof(T));
}

template <typename T>
inline bool write_pod(std::ostream &out, const T &v) {
  out.write(reinterpret_cast<const char *>(&v), sizeof(T));
  return out.good();
}

template <typename T>
inline bool read_pod(std::istream &in, T &v) {
  in.read(reinterpret_cast<char *>(&v), sizeof(T));
  return in.good();
}

template <typename T>
T Util::get_field_numeric(Field *field, const uchar *data_ptr, const Compress::Dictionary *dict,
                          bool db_low_byte_first) {
  // field is only dereferenced once data_ptr is known to be non-NULL, as before: a NULL SQL value carries neither.
  if (data_ptr == nullptr || field == nullptr) return T{};
  // Only the integer branches read signedness, and for those (all Field_num)
  // UNSIGNED_FLAG is set exactly when is_unsigned() is true. Taking the flag
  // costs a bitmask test instead of the virtual call this wrapper used to pay
  // per cell on every scan, DECIMAL and temporal included.
  return get_field_numeric_typed<T>(field, field->type(), field->is_flag_set(UNSIGNED_FLAG), data_ptr, dict,
                                    db_low_byte_first);
}

template <typename T>
T Util::get_field_numeric_typed(Field *field, enum_field_types field_type, bool is_unsigned, const uchar *data_ptr,
                                const Compress::Dictionary *dict [[maybe_unused]], bool db_low_byte_first) {
  T data_val{};

  // a NULL SQL value has no data pointer.
  if (!data_ptr) return data_val;

  auto safe_cast = [](auto value) -> T {
    using S = decltype(value);
    if constexpr (std::is_floating_point_v<T>) {
      return static_cast<T>(value);
    } else if constexpr (std::is_integral_v<S>) {
      // Clamp in the source domain. Casting to T first and only then range
      // checking is a no-op whenever T is narrower than S: the truncation has
      // already happened, so the comparison can never fail.
      if constexpr (std::is_signed_v<S> == std::is_signed_v<T>) {
        if (value > static_cast<S>(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
        if (value < static_cast<S>(std::numeric_limits<T>::lowest())) return std::numeric_limits<T>::lowest();
      } else if constexpr (std::is_signed_v<S>) {
        // Signed source, unsigned T: everything below zero saturates to zero.
        if (value < 0) return std::numeric_limits<T>::lowest();
        using U = std::make_unsigned_t<S>;
        if (static_cast<U>(value) > static_cast<U>(std::numeric_limits<T>::max())) {
          return std::numeric_limits<T>::max();
        }
      } else {
        // Unsigned source, signed T: only the upper bound can be exceeded.
        if (value > static_cast<S>(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
      }
      return static_cast<T>(value);
    } else {
      // Floating-point source. Compare as long double so that the integral
      // bounds survive the promotion, and reject NaN via the negated test.
      const long double v = static_cast<long double>(value);
      if (!(v >= static_cast<long double>(std::numeric_limits<T>::lowest()))) {
        return std::numeric_limits<T>::lowest();
      }
      if (v >= static_cast<long double>(std::numeric_limits<T>::max())) {
        return std::numeric_limits<T>::max();
      }
      return static_cast<T>(value);
    }
  };

  switch (field_type) {
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR: {
      if (field->real_type() == MYSQL_TYPE_ENUM || field->real_type() == MYSQL_TYPE_SET) {
        longlong ival = 0;
        switch (field->pack_length()) {
          case 1:
            ival = static_cast<longlong>(data_ptr[0]);
            break;
          case 2:
            ival = static_cast<longlong>(uint2korr(data_ptr));
            break;
          case 3:
            ival = static_cast<longlong>(uint3korr(data_ptr));
            break;
          case 4:
            ival = static_cast<longlong>(uint4korr(data_ptr));
            break;
          default:
            ival = static_cast<longlong>(data_ptr[0]);
            break;
        }
        data_val = safe_cast(ival);
      } else
        data_val = safe_cast(0);  // For non-enum/set strings, return 0 as numeric value
    } break;
    case MYSQL_TYPE_TINY: {
      // Field_tiny::val_int() impl
      const int tmp = is_unsigned ? static_cast<int>(data_ptr[0])
                                  : static_cast<int>(reinterpret_cast<const signed char *>(data_ptr)[0]);
      data_val = safe_cast((longlong)tmp);
    } break;
    case MYSQL_TYPE_SHORT: {
      // Field_short::val_int() impl
      short j = db_low_byte_first ? sint2korr(data_ptr) : shortget(data_ptr);
      longlong value = is_unsigned ? (longlong)(unsigned short)j : (longlong)j;
      data_val = safe_cast(value);
    } break;
    case MYSQL_TYPE_INT24: {
      // Field_medium::val_int() impl
      const long j = is_unsigned ? (long)uint3korr(data_ptr) : sint3korr(data_ptr);
      data_val = safe_cast((longlong)j);
    } break;
    case MYSQL_TYPE_LONG: {
      // Field_long::val_int() impl
      int32 j = (db_low_byte_first) ? sint4korr(data_ptr) : longget(data_ptr);
      longlong value = is_unsigned ? (longlong)(uint32)j : (longlong)j;
      data_val = safe_cast(value);
    } break;
    case MYSQL_TYPE_LONGLONG: {
      // Field_longlong::val_int() impl
      longlong value = (db_low_byte_first) ? sint8korr(data_ptr) : longlongget(data_ptr);
      // Never route a 64-bit integer through double: it has 53 mantissa bits,
      // so every BIGINT UNSIGNED above 2^53 would be rounded, and the ART key
      // built from it would collide with its neighbours or wrap to 0.
      data_val = (is_unsigned) ? safe_cast(static_cast<ulonglong>(value)) : safe_cast(value);
    } break;
    case MYSQL_TYPE_FLOAT: {
      // Field_float::val_real() impl
      double value = (db_low_byte_first) ? double{float4get(data_ptr)} : double{floatget(data_ptr)};
      data_val = safe_cast(value);
    } break;
    case MYSQL_TYPE_DOUBLE: {
      // Field_double::val_real() impl
      double value = (db_low_byte_first) ? float8get(data_ptr) : doubleget(data_ptr);
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
      } else if (field->real_type() == MYSQL_TYPE_DATETIME2) {
        uint8 dec = down_cast<Field_datetimef *>(field)->decimals();
        longlong packed = my_datetime_packed_from_binary(data_ptr, dec);
        MYSQL_TIME ltime;
        TIME_from_longlong_datetime_packed(&ltime, packed);
        double tmp = TIME_to_double_datetime(ltime);
        data_val = safe_cast(tmp);
      } else if (field->real_type() == MYSQL_TYPE_DATETIME) {
        longlong j = db_low_byte_first ? sint8korr(data_ptr) : longlongget(data_ptr);
        data_val = safe_cast(j);
      } else {
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
      if (field->real_type() == MYSQL_TYPE_TIMESTAMP2) {
        uint8 dec = down_cast<Field_timestampf *>(field)->decimals();
        my_timeval tm;
        my_timestamp_from_binary(&tm, data_ptr, dec);
        double res = (double)tm.m_tv_sec + (double)tm.m_tv_usec / 1000000.0;
        data_val = safe_cast(res);
      } else {
        uint32 seconds;
        seconds = db_low_byte_first ? uint4korr(data_ptr) : longget(data_ptr);
        data_val = safe_cast((longlong)seconds);
      }
    } break;
    default: {
      data_val = safe_cast(0);
    } break;
  }
  return data_val;
}
}  // namespace Utils
}  // namespace ShannonBase
#endif  //__SHANNONBASE_UTILS_TPP_H__
