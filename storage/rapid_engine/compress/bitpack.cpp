/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   The fundamental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#include "storage/rapid_engine/compress/bitpack.h"

#include <algorithm>
#include <cstring>

namespace ShannonBase {
namespace Compress {

size_t BitPackedColumn::BytesFor(size_t rows, uint8_t width) {
  if (width == 0 || width > kMaxWidth) return 0;
  if (rows > SIZE_MAX / width) return 0;
  return (rows * width + 7) / 8;
}

uint8_t BitPackedColumn::WidthFor(int64_t min_value, int64_t max_value) {
  if (max_value < min_value) return 0;
  // Unsigned subtraction gives the span even across the sign boundary
  // (-100..100 -> 200); a span that wraps to a huge value is a domain wider
  // than the codec supports and is rejected below.
  const uint64_t span = static_cast<uint64_t>(max_value) - static_cast<uint64_t>(min_value);
  if (span == 0) return 1;

  uint8_t bits = 0;
  for (uint64_t v = span; v != 0; v >>= 1) ++bits;
  return bits > kMaxWidth ? 0 : bits;
}

bool BitPackedColumn::Attach(uint8_t *buffer, size_t bytes, uint8_t width, int64_t bias, size_t rows) {
  if (buffer == nullptr || width == 0 || width > kMaxWidth) return false;
  if (bytes < BytesFor(rows, width)) return false;

  m_buffer = buffer;
  m_bytes = bytes;
  m_width = width;
  m_bias = bias;
  m_rows = rows;
  return true;
}

bool BitPackedColumn::GetCode(size_t row, uint64_t *code) const {
  if (!valid() || code == nullptr || row >= m_rows) return false;

  const size_t bit = row * m_width;
  size_t byte = bit >> 3;
  unsigned shift = static_cast<unsigned>(bit & 7u);
  unsigned left = m_width;
  uint64_t value = 0;
  unsigned done = 0;
  while (left > 0) {
    const unsigned take = std::min<unsigned>(left, 8u - shift);
    const uint8_t mask = static_cast<uint8_t>((1u << take) - 1u);
    value |= static_cast<uint64_t>((m_buffer[byte] >> shift) & mask) << done;
    done += take;
    left -= take;
    ++byte;
    shift = 0;
  }

  *code = value;
  return true;
}

bool BitPackedColumn::SetCode(size_t row, uint64_t code) {
  if (!valid() || row >= m_rows || code > max_code()) return false;

  const size_t bit = row * m_width;
  size_t byte = bit >> 3;
  unsigned shift = static_cast<unsigned>(bit & 7u);
  unsigned left = m_width;
  unsigned done = 0;
  while (left > 0) {
    const unsigned take = std::min<unsigned>(left, 8u - shift);
    const uint8_t mask = static_cast<uint8_t>(((1u << take) - 1u) << shift);
    const uint8_t bits = static_cast<uint8_t>(((code >> done) & ((1u << take) - 1u)) << shift);
    m_buffer[byte] = static_cast<uint8_t>((m_buffer[byte] & static_cast<uint8_t>(~mask)) | bits);
    done += take;
    left -= take;
    ++byte;
    shift = 0;
  }
  return true;
}

bool BitPackedColumn::GetValue(size_t row, int64_t *value) const {
  uint64_t code = 0;
  if (value == nullptr || !GetCode(row, &code)) return false;
  // The bias is the column minimum, so code + bias cannot leave int64.
  *value = static_cast<int64_t>(code) + m_bias;
  return true;
}

bool BitPackedColumn::SetValue(size_t row, int64_t value) {
  const uint64_t code = static_cast<uint64_t>(value) - static_cast<uint64_t>(m_bias);
  if (code > max_code()) return false;
  return SetCode(row, code);
}

bool BitPackedColumn::MatchValueRange(size_t row_begin, size_t count, int64_t low, int64_t high, uint8_t *bits,
                                     size_t bits_bytes) const {
  if (!valid() || bits == nullptr || bits_bytes < (count + 7) / 8) return false;
  if (count == 0) return true;
  if (row_begin >= m_rows) return false;

  const size_t rows = std::min(count, m_rows - row_begin);
  // Unsigned wrap makes a bound outside the column domain match nothing,
  // without a separate range check.
  const uint64_t low_code = static_cast<uint64_t>(low) - static_cast<uint64_t>(m_bias);
  const uint64_t high_code = static_cast<uint64_t>(high) - static_cast<uint64_t>(m_bias);
  const auto set_if_match = [&](size_t i, uint64_t code) {
    if (code >= low_code && code <= high_code) bits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7u));
  };

  // 8/16/32 bits per code land on whole bytes, so the stream is an array of
  // little-endian codes of that width.
  const size_t byte = (row_begin * m_width) >> 3;
  if (m_width == 8) {
    for (size_t i = 0; i < rows; ++i) set_if_match(i, m_buffer[byte + i]);
    return true;
  }
  if (m_width == 16 || m_width == 32) {
    const size_t step = m_width / 8;
    for (size_t i = 0; i < rows; ++i) {
      const uint8_t *src = m_buffer + byte + i * step;
      uint64_t code = 0;
      if (m_width == 16) {
        uint16_t word = 0;
        std::memcpy(&word, src, sizeof(word));
        code = word;
      } else {
        uint32_t word = 0;
        std::memcpy(&word, src, sizeof(word));
        code = word;
      }
      set_if_match(i, code);
    }
    return true;
  }

  for (size_t i = 0; i < rows; ++i) {
    uint64_t code = 0;
    if (!GetCode(row_begin + i, &code)) return false;
    set_if_match(i, code);
  }
  return true;
}

}  // namespace Compress
}  // namespace ShannonBase
