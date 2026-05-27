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
#ifndef __SHANNONBASE_CRC_H__
#define __SHANNONBASE_CRC_H__
#include <array>
#include <cstdint>
#include <cstring>

#include "storage/rapid_engine/include/rapid_const.h"
namespace ShannonBase {
namespace Utils {
#if !defined(SHANNON_CRC32_HW_SUPPORTED)
// Software table (CRC32C)
static const std::array<uint32_t, 256> &crc32c_table() {
  static const std::array<uint32_t, 256> table = []() {
    std::array<uint32_t, 256> t{};
    constexpr uint32_t kPoly = 0x82F63B78u;  // CRC32C

    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t crc = i;
      for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (kPoly & -(crc & 1u));
      t[i] = crc;
    }
    return t;
  }();
  return table;
}
#endif

#if defined(SHANNON_CRC32_HW_SUPPORTED)
__attribute__((target("sse4.2")))
#endif
uint32_t
crc32c_compute(const void *data, size_t len, uint32_t seed) {
  const uint8_t *p = static_cast<const uint8_t *>(data);
  uint32_t crc = ~seed;

#if defined(SHANNON_CRC32_HW_SUPPORTED)
  // ---- 64-bit fast path ----
#if defined(SHANNON_CRC32_U64_SUPPORTED)
  uint64_t crc64 = crc;

  while (len >= 8) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    crc64 = _mm_crc32_u64(crc64, v);
    p += 8;
    len -= 8;
  }

  crc = static_cast<uint32_t>(crc64);
#endif
  // ---- 32-bit ----
  while (len >= 4) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    crc = _mm_crc32_u32(crc, v);
    p += 4;
    len -= 4;
  }

  while (len--) {
    crc = _mm_crc32_u8(crc, *p++);
  }

#else
  const auto &table = crc32c_table();
  while (len--) {
    crc = (crc >> 8) ^ table[(crc ^ *p++) & 0xFF];
  }
#endif
  return ~crc;
}
}  // namespace Utils
}  // namespace ShannonBase
#endif  // __SHANNONBASE_CRC_H__