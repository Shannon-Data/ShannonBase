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

/**
 * Integer Encoders for ART (Adaptive Radix Tree) Index
 *
 * These template specializations provide order-preserving encoding for integer types,
 * enabling them to be used as keys in ART trees while maintaining correct sort order
 * when compared using memcmp().
 *
 * Key Features:
 * - Signed integers: XOR with sign bit to convert negative values to smaller unsigned values
 * - Unsigned integers: Direct big-endian encoding (natural lexicographic ordering)
 * - All encodings use big-endian byte order for consistent memcmp() behavior
 * - Preserves numerical ordering: if a < b, then encoded(a) < encoded(b) lexicographically
 *
 * Usage:
 *   unsigned char key[sizeof(T)];
 *   Encoder<T>::EncodeFloat(value, key);
 *   T decoded = Encoder<T>::DecodeFloat(key);
 */

#include "storage/rapid_engine/imcs/index/encoder.h"
#include <cmath>
#include <cstring>
#include <limits>
#include "include/my_inttypes.h"

namespace ShannonBase {
namespace Imcs {
namespace Index {

template <>
void Encoder<float>::EncodeData(float value, unsigned char *key) {
  uint32_t val;
  std::memcpy(&val, &value, sizeof(float));

  if (val & (1u << 31)) {
    val = ~val;
  } else {
    val ^= (1u << 31);
  }

  key[0] = (val >> 24) & 0xFF;
  key[1] = (val >> 16) & 0xFF;
  key[2] = (val >> 8) & 0xFF;
  key[3] = val & 0xFF;
}

template <>
void Encoder<double>::EncodeData(double value, unsigned char *key) {
  uint64_t u;
  std::memcpy(&u, &value, sizeof(double));

  if (std::isnan(value)) {
    uint64_t nan_value = std::numeric_limits<uint64_t>::max();
    for (int i = 0; i < 8; i++) {
      key[i] = static_cast<unsigned char>((nan_value >> (56 - i * 8)) & 0xFF);
    }
    return;
  }

  if (std::isinf(value)) {
    uint64_t inf_value;
    if (value > 0) {
      inf_value = std::numeric_limits<uint64_t>::max() - 1;  // +Inf
    } else {
      inf_value = std::numeric_limits<uint64_t>::max() - 2;  // -Inf
    }
    for (int i = 0; i < 8; i++) {
      key[i] = static_cast<unsigned char>((inf_value >> (56 - i * 8)) & 0xFF);
    }
    return;
  }

  if (u & (1ULL << 63)) {
    // neg value.
    u = ~u;
  } else {
    // positive.
    u ^= (1ULL << 63);
  }

  // to big-endian.
  for (int i = 0; i < 8; i++) {
    key[i] = static_cast<unsigned char>((u >> (56 - i * 8)) & 0xFF);
  }
}

template <>
float Encoder<float>::DecodeData(const unsigned char *key) {
  uint32_t val = 0;

  val |= (uint32_t)key[0] << 24;
  val |= (uint32_t)key[1] << 16;
  val |= (uint32_t)key[2] << 8;
  val |= (uint32_t)key[3];

  if (val & (1u << 31)) {
    val ^= (1u << 31);
  } else {
    val = ~val;
  }

  float result;
  std::memcpy(&result, &val, sizeof(float));
  return result;
}

template <>
double Encoder<double>::DecodeData(const unsigned char *key) {
  uint64_t u = 0;
  for (int i = 0; i < 8; i++) {
    u = (u << 8) | key[i];
  }

  if (u == std::numeric_limits<uint64_t>::max()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (u == std::numeric_limits<uint64_t>::max() - 1) {
    return std::numeric_limits<double>::infinity();
  }
  if (u == std::numeric_limits<uint64_t>::max() - 2) {
    return -std::numeric_limits<double>::infinity();
  }

  if (u & (1ULL << 63)) {
    u ^= (1ULL << 63);
  } else {
    u = ~u;
  }

  double value;
  std::memcpy(&value, &u, sizeof(double));
  return value;
}

template <>
void Encoder<int8_t>::EncodeData(int8_t value, unsigned char *key) {
  uint8_t val = static_cast<uint8_t>(value) ^ (1u << 7);
  key[0] = val & 0xFF;
}

template <>
int8_t Encoder<int8_t>::DecodeData(const unsigned char *key) {
  uint8_t val = key[0] ^ (1u << 7);
  return static_cast<int8_t>(val);
}

template <>
void Encoder<int16_t>::EncodeData(int16_t value, unsigned char *key) {
  uint16_t val = static_cast<uint16_t>(value) ^ (1u << 15);

  key[0] = (val >> 8) & 0xFF;
  key[1] = val & 0xFF;
}

template <>
int16_t Encoder<int16_t>::DecodeData(const unsigned char *key) {
  uint16_t val = 0;

  val |= (uint16_t)key[0] << 8;
  val |= (uint16_t)key[1];

  val ^= (1u << 15);
  return static_cast<int16_t>(val);
}

template <>
void Encoder<int32_t>::EncodeData(int32_t value, unsigned char *key) {
  uint32_t val = static_cast<uint32_t>(value) ^ (1u << 31);

  key[0] = (val >> 24) & 0xFF;
  key[1] = (val >> 16) & 0xFF;
  key[2] = (val >> 8) & 0xFF;
  key[3] = val & 0xFF;
}

template <>
int32_t Encoder<int32_t>::DecodeData(const unsigned char *key) {
  uint32_t val = 0;

  val |= (uint32_t)key[0] << 24;
  val |= (uint32_t)key[1] << 16;
  val |= (uint32_t)key[2] << 8;
  val |= (uint32_t)key[3];

  val ^= (1u << 31);
  return static_cast<int32_t>(val);
}

template <>
void Encoder<int64_t>::EncodeData(int64_t value, unsigned char *key) {
  uint64_t val = static_cast<uint64_t>(value) ^ (1ULL << 63);

  key[0] = (val >> 56) & 0xFF;
  key[1] = (val >> 48) & 0xFF;
  key[2] = (val >> 40) & 0xFF;
  key[3] = (val >> 32) & 0xFF;
  key[4] = (val >> 24) & 0xFF;
  key[5] = (val >> 16) & 0xFF;
  key[6] = (val >> 8) & 0xFF;
  key[7] = val & 0xFF;
}

template <>
int64_t Encoder<int64_t>::DecodeData(const unsigned char *key) {
  uint64_t val = 0;

  val |= (uint64_t)key[0] << 56;
  val |= (uint64_t)key[1] << 48;
  val |= (uint64_t)key[2] << 40;
  val |= (uint64_t)key[3] << 32;
  val |= (uint64_t)key[4] << 24;
  val |= (uint64_t)key[5] << 16;
  val |= (uint64_t)key[6] << 8;
  val |= (uint64_t)key[7];

  val ^= (1ULL << 63);
  return static_cast<int64_t>(val);
}

// big-endian
template <>
void Encoder<uint8_t>::EncodeData(uint8_t value, unsigned char *key) {
  key[0] = value & 0xFF;
}

template <>
uint8_t Encoder<uint8_t>::DecodeData(const unsigned char *key) {
  return key[0];
}

template <>
void Encoder<uint16_t>::EncodeData(uint16_t value, unsigned char *key) {
  key[0] = (value >> 8) & 0xFF;
  key[1] = value & 0xFF;
}

template <>
uint16_t Encoder<uint16_t>::DecodeData(const unsigned char *key) {
  uint16_t val = 0;
  val |= (uint16_t)key[0] << 8;
  val |= (uint16_t)key[1];
  return val;
}

template <>
void Encoder<uint32_t>::EncodeData(uint32_t value, unsigned char *key) {
  key[0] = (value >> 24) & 0xFF;
  key[1] = (value >> 16) & 0xFF;
  key[2] = (value >> 8) & 0xFF;
  key[3] = value & 0xFF;
}

template <>
uint32_t Encoder<uint32_t>::DecodeData(const unsigned char *key) {
  uint32_t val = 0;
  val |= (uint32_t)key[0] << 24;
  val |= (uint32_t)key[1] << 16;
  val |= (uint32_t)key[2] << 8;
  val |= (uint32_t)key[3];
  return val;
}

template <>
void Encoder<uint64_t>::EncodeData(uint64_t value, unsigned char *key) {
  key[0] = (value >> 56) & 0xFF;
  key[1] = (value >> 48) & 0xFF;
  key[2] = (value >> 40) & 0xFF;
  key[3] = (value >> 32) & 0xFF;
  key[4] = (value >> 24) & 0xFF;
  key[5] = (value >> 16) & 0xFF;
  key[6] = (value >> 8) & 0xFF;
  key[7] = value & 0xFF;
}

template <>
uint64_t Encoder<uint64_t>::DecodeData(const unsigned char *key) {
  uint64_t val = 0;
  val |= (uint64_t)key[0] << 56;
  val |= (uint64_t)key[1] << 48;
  val |= (uint64_t)key[2] << 40;
  val |= (uint64_t)key[3] << 32;
  val |= (uint64_t)key[4] << 24;
  val |= (uint64_t)key[5] << 16;
  val |= (uint64_t)key[6] << 8;
  val |= (uint64_t)key[7];
  return val;
}

}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase