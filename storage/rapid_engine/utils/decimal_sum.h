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
#ifndef __SHANNONBASE_DECIMAL_SUM_H__
#define __SHANNONBASE_DECIMAL_SUM_H__

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include "sql-common/my_decimal.h"
#include "storage/rapid_engine/utils/decimal64.h"

namespace ShannonBase {
namespace Utils {
// Packed values may have leading zero groups even when their declared precision
// exceeds 38. Reject only values that cannot be represented exactly in int128.
inline bool DecodePackedDecimal128(const uint8_t *data, size_t bytes, unsigned precision, unsigned scale,
                                   __int128 *out) {
  if (!data || !out || precision == 0 || precision > DECIMAL_MAX_PRECISION || scale > precision ||
      scale > DECIMAL_MAX_SCALE)
    return false;

  if (precision <= 18) {
    int64_t value;
    if (!DecodePackedDecimal64(data, bytes, precision, scale, &value)) return false;
    *out = value;
    return true;
  }

  static constexpr unsigned kDigitBytes[] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  static constexpr uint32_t kPowers[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};

  const unsigned integers = precision - scale;
  const size_t expected = (integers / 9 + scale / 9) * 4 + kDigitBytes[integers % 9] + kDigitBytes[scale % 9];

  if (bytes != expected) return false;

  const bool negative = (data[0] & 0x80) == 0;
  const uint8_t mask = negative ? 0xff : 0;
  using Unsigned = unsigned __int128;
  const Unsigned limit = (Unsigned{1} << 127) - (negative ? 0 : 1);
  Unsigned value{0};
  size_t offset{0};
  const auto group = [&](unsigned digits) {
    uint32_t part{0};
    for (unsigned i = 0; i < kDigitBytes[digits]; ++i) {
      const uint8_t sign = offset == 0 ? 0x80 : 0;
      part = (part << 8) | (data[offset++] ^ mask ^ sign);
    }

    if (part >= kPowers[digits]) return false;

    // Valid DECIMAL(38) digits always fit int128. A 64-bit prefix multiplied
    // by at most 10^9 also fits, including leading-zero wide expressions.
    if (precision <= std::numeric_limits<__int128>::digits10 || (value >> std::numeric_limits<uint64_t>::digits) == 0) {
      value = value * kPowers[digits] + part;
      return true;
    }

    Unsigned product;
    return !__builtin_mul_overflow(value, static_cast<Unsigned>(kPowers[digits]), &product) &&
           !__builtin_add_overflow(product, static_cast<Unsigned>(part), &value) && value <= limit;
  };

  if (integers % 9 && !group(integers % 9)) return false;

  for (unsigned i = 0; i < integers / 9; ++i) {
    if (!group(9)) return false;
  }
  for (unsigned i = 0; i < scale / 9; ++i) {
    if (!group(9)) return false;
  }

  if (scale % 9 && !group(scale % 9)) return false;

  *out = negative && value != 0 ? -static_cast<__int128>(value - 1) - 1 : static_cast<__int128>(value);
  return true;
}

// A group stays in exact scaled integers until overflow or a wide decimal
// requires promotion. Promotion is permanent; no per-row decimal round trips.
class DecimalSum {
 public:
  DecimalSum() = default;
  DecimalSum(const DecimalSum &other) noexcept { Copy(other); }
  DecimalSum &operator=(const DecimalSum &other) noexcept {
    if (this != &other) {
      Destroy();
      Copy(other);
    }
    return *this;
  }

  ~DecimalSum() { Destroy(); }

  bool scaled() const { return !m_decimal; }

  bool AddScaled(__int128 value, unsigned scale) {
    if (scale > DECIMAL_MAX_SCALE) return true;
    if (m_scale == kUnsetScale) m_scale = scale;
    __int128 result;
    if (!m_decimal && m_scale == scale && !__builtin_add_overflow(m_value.scaled, value, &result)) {
      m_value.scaled = result;
      return false;
    }
    my_decimal decimal;
    if (Convert(value, scale, &decimal)) return true;
    return Add(decimal);
  }

  bool Add(const my_decimal &value) {
    if (!m_decimal && m_scale == kUnsetScale) {
      Set(value);
      return false;
    }
    if (!m_decimal) {
      my_decimal initial;
      if (Get(&initial)) return true;
      new (&m_value.decimal) my_decimal(initial);
      m_decimal = true;
    }
    my_decimal result;
    if (my_decimal_add(E_DEC_FATAL_ERROR, &result, &m_value.decimal, &value) > 1) return true;
    m_value.decimal = result;
    return false;
  }

  void Set(const my_decimal &value) {
    Destroy();
    new (&m_value.decimal) my_decimal(value);
    m_decimal = true;
  }

  bool Get(my_decimal *result) const {
    if (m_decimal) {
      *result = m_value.decimal;
      return false;
    }
    return Convert(m_value.scaled, m_scale == kUnsetScale ? 0 : m_scale, result);
  }

 private:
  static constexpr unsigned kUnsetScale = DECIMAL_MAX_SCALE + 1;

  static bool Convert(__int128 value, unsigned scale, my_decimal *result) {
    // int128 has at most 39 digits; padding to DECIMAL_MAX_SCALE is smaller.
    char text[DECIMAL_MAX_PRECISION + 3];
    char *end = text + sizeof(text), *cursor = end;
    const bool negative = value < 0;
    using Unsigned = unsigned __int128;
    Unsigned magnitude = negative ? Unsigned{0} - static_cast<Unsigned>(value) : static_cast<Unsigned>(value);
    unsigned digits{0};
    do {
      if (scale != 0 && digits == scale) *--cursor = '.';
      *--cursor = '0' + magnitude % 10;
      magnitude /= 10;
      ++digits;
    } while (magnitude != 0 || digits <= scale);
    if (negative) *--cursor = '-';
    return str2my_decimal(E_DEC_FATAL_ERROR, cursor, end - cursor, &my_charset_latin1, result) > 1;
  }

  void Destroy() {
    if (m_decimal) m_value.decimal.~my_decimal();
  }

  void Copy(const DecimalSum &other) {
    m_decimal = other.m_decimal;
    m_scale = other.m_scale;
    if (m_decimal)
      new (&m_value.decimal) my_decimal(other.m_value.decimal);
    else
      m_value.scaled = other.m_value.scaled;
  }

  union Value {
    Value() : scaled(0) {}
    ~Value() {}
    __int128 scaled;
    my_decimal decimal;
  } m_value;
  unsigned m_scale{kUnsetScale};
  bool m_decimal{false};
};
}  // namespace Utils
}  // namespace ShannonBase
#endif
