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
#ifndef __SHANNONBASE_INDEX_ENCODER_H__
#define __SHANNONBASE_INDEX_ENCODER_H__

/**
 * Order-preserving key encoders for ART: memcmp() order of the encoded bytes
 * equals numeric order of the values.
 *
 *  - unsigned integers: big-endian.
 *  - signed integers:   sign bit flipped, then big-endian.
 *  - float / double:    IEEE bits transformed to a monotonic unsigned value,
 *                       big-endian.  +0 and -0 share one key; -inf < finite
 *                       < +inf < NaN (all NaNs share one key).
 *
 *   unsigned char key[sizeof(T)];
 *   Encoder<T>::Encode(value, key);
 *   T decoded = Encoder<T>::Decode(key);
 *
 * Everything is defined inline here (it used to be explicit specialisations
 * in encoder.cpp that this header never declared, which is ill-formed NDR and
 * kept the compiler from inlining a few shifts on the index-build hot path).
 * Any integral type other than bool works, not only the fixed-width aliases.
 */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace ShannonBase {
namespace Imcs {
namespace Index {
namespace encoder_detail {
template <typename UInt>
inline void StoreBigEndian(UInt value, unsigned char *key) {
  for (size_t i = 0; i < sizeof(UInt); ++i)
    key[i] = static_cast<unsigned char>((value >> ((sizeof(UInt) - 1 - i) * 8)) & 0xFF);
}

template <typename UInt>
inline UInt LoadBigEndian(const unsigned char *key) {
  UInt v = 0;
  for (size_t i = 0; i < sizeof(UInt); ++i) v = static_cast<UInt>((v << 8) | key[i]);
  return v;
}

template <typename UInt>
constexpr UInt SignBit() {
  return static_cast<UInt>(UInt(1) << (sizeof(UInt) * 8 - 1));
}

template <typename Float, typename UInt>
inline void EncodeFloat(Float value, unsigned char *key) {
  static_assert(sizeof(Float) == sizeof(UInt), "width mismatch");
  UInt encoded;
  if (value == Float(0)) {
    encoded = SignBit<UInt>();  // canonical +0/-0
  } else if (std::isnan(value)) {
    encoded = std::numeric_limits<UInt>::max();
  } else if (std::isinf(value)) {
    encoded = value > 0 ? static_cast<UInt>(std::numeric_limits<UInt>::max() - 1) : UInt(0);
  } else {
    std::memcpy(&encoded, &value, sizeof(value));
    encoded = (encoded & SignBit<UInt>()) ? static_cast<UInt>(~encoded) : static_cast<UInt>(encoded ^ SignBit<UInt>());
  }
  StoreBigEndian(encoded, key);
}

template <typename Float, typename UInt>
inline Float DecodeFloat(const unsigned char *key) {
  const UInt encoded = LoadBigEndian<UInt>(key);
  if (encoded == std::numeric_limits<UInt>::max()) return std::numeric_limits<Float>::quiet_NaN();
  if (encoded == static_cast<UInt>(std::numeric_limits<UInt>::max() - 1)) return std::numeric_limits<Float>::infinity();
  if (encoded == 0) return -std::numeric_limits<Float>::infinity();

  const UInt bits =
      (encoded & SignBit<UInt>()) ? static_cast<UInt>(encoded ^ SignBit<UInt>()) : static_cast<UInt>(~encoded);
  Float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
}  // namespace encoder_detail

template <typename T>
class Encoder {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>, "Encoder<T>: unsupported key type");
  using U = std::make_unsigned_t<T>;

 public:
  static inline void Encode(T value, unsigned char *key) {
    U u = static_cast<U>(value);
    if constexpr (std::is_signed_v<T>) u = static_cast<U>(u ^ encoder_detail::SignBit<U>());
    encoder_detail::StoreBigEndian(u, key);
  }

  static inline T Decode(const unsigned char *key) {
    U u = encoder_detail::LoadBigEndian<U>(key);
    if constexpr (std::is_signed_v<T>) u = static_cast<U>(u ^ encoder_detail::SignBit<U>());
    return static_cast<T>(u);
  }
};

template <>
class Encoder<float> {
 public:
  static inline void Encode(float value, unsigned char *key) {
    encoder_detail::EncodeFloat<float, uint32_t>(value, key);
  }
  static inline float Decode(const unsigned char *key) { return encoder_detail::DecodeFloat<float, uint32_t>(key); }
};

template <>
class Encoder<double> {
 public:
  static inline void Encode(double value, unsigned char *key) {
    encoder_detail::EncodeFloat<double, uint64_t>(value, key);
  }
  static inline double Decode(const unsigned char *key) { return encoder_detail::DecodeFloat<double, uint64_t>(key); }
};
}  // namespace Index
}  // namespace Imcs
}  // namespace ShannonBase
#endif  // __SHANNONBASE_INDEX_ENCODER_H__