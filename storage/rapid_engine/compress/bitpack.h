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
#ifndef __SHANNONBASE_BITPACK_H__
#define __SHANNONBASE_BITPACK_H__

#include <cstddef>
#include <cstdint>

namespace ShannonBase {
namespace Compress {

/**
 * Fixed-width bit packing for integer columns: row i holds (value - bias) at
 * bit offset i * width, least significant bit first.
 *
 * NULL has no representation here; the column's null mask stays authoritative,
 * so a packed slot always carries a value. The buffer is caller-owned, which
 * keeps the codec free of a memory pool and lets a CU account the bytes it
 * uses.
 */
class BitPackedColumn {
 public:
  /// Widest domain this codec packs; wider ones keep their native slot.
  static constexpr uint8_t kMaxWidth = 32;

  /// Bytes needed to hold `rows` codes of `width` bits.
  static size_t BytesFor(size_t rows, uint8_t width);

  /// Bits needed for [min_value, max_value] and the bias to apply, or 0 when the
  /// domain is wider than kMaxWidth.
  static uint8_t WidthFor(int64_t min_value, int64_t max_value);

  BitPackedColumn() = default;

  /// Takes ownership of nothing: `buffer` must outlive the column.
  bool Attach(uint8_t *buffer, size_t bytes, uint8_t width, int64_t bias, size_t rows);

  bool valid() const { return m_buffer != nullptr && m_width != 0; }
  uint8_t width() const { return m_width; }
  int64_t bias() const { return m_bias; }
  size_t rows() const { return m_rows; }
  size_t byte_size() const { return m_bytes; }
  uint8_t *data() { return m_buffer; }
  const uint8_t *data() const { return m_buffer; }

  /// Packed code (value - bias) and the value itself.
  bool GetCode(size_t row, uint64_t *code) const;
  bool SetCode(size_t row, uint64_t code);
  bool GetValue(size_t row, int64_t *value) const;
  bool SetValue(size_t row, int64_t value);

  /// Largest code the configured width can hold.
  uint64_t max_code() const { return m_width == 64 ? UINT64_MAX : ((uint64_t{1} << m_width) - 1); }

 private:
  uint8_t *m_buffer{nullptr};
  size_t m_rows{0};
  size_t m_bytes{0};
  uint8_t m_width{0};
  int64_t m_bias{0};
};

}  // namespace Compress
}  // namespace ShannonBase
#endif  // __SHANNONBASE_BITPACK_H__
