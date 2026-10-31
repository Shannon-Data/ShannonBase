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
#ifndef __SHANNONBASE_DECIMAL64_H__
#define __SHANNONBASE_DECIMAL64_H__

#include <cstddef>
#include <cstdint>

namespace ShannonBase {
namespace Utils {
// Decode MySQL's packed NEWDECIMAL directly to an exact scaled integer. Up to
// 18 digits fit for both signs. Wider decimals keep the arbitrary-precision path.
// This does not restore a SQL Field or construct a my_decimal for each value.
inline bool DecodePackedDecimal64(const uint8_t *data, size_t bytes, unsigned precision, unsigned scale, int64_t *out) {
  static constexpr unsigned dig_bytes[] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  static constexpr uint32_t powers[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
  if (data == nullptr || out == nullptr || precision == 0 || precision > 18 || scale > precision) return false;

  const unsigned integers = precision - scale;
  const size_t expected = (integers / 9 + scale / 9) * 4 + dig_bytes[integers % 9] + dig_bytes[scale % 9];

  if (bytes != expected) return false;

  const bool negative = (data[0] & 0x80) == 0;
  const uint8_t mask = negative ? 0xff : 0;
  size_t offset{0};
  uint64_t value{0};
  const auto group = [&](unsigned digits) {
    uint32_t part{0};
    for (unsigned i = 0; i < dig_bytes[digits]; ++i) {
      const uint8_t sign = offset == 0 ? 0x80 : 0;
      part = (part << 8) | (data[offset++] ^ mask ^ sign);
    }

    if (part >= powers[digits]) return false;

    value = value * powers[digits] + part;
    return true;
  };

  if (integers % 9 && !group(integers % 9)) return false;

  for (unsigned i = 0; i < integers / 9; ++i) {
    if (!group(9)) return false;
  }

  for (unsigned i = 0; i < scale / 9; ++i) {
    if (!group(9)) return false;
  }

  if (scale % 9 && !group(scale % 9)) return false;

  *out = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
  return true;
}
}  // namespace Utils
}  // namespace ShannonBase
#endif
