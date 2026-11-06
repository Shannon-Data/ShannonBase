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

}  // namespace Compress
}  // namespace ShannonBase
