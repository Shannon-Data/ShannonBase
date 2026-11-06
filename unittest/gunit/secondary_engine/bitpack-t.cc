/* Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "storage/rapid_engine/compress/bitpack.h"

namespace ShannonBase {
namespace Compress {

TEST(BitPackedColumnTest, WidthForCoversTheDomain) {
  EXPECT_EQ(1, BitPackedColumn::WidthFor(0, 0));
  EXPECT_EQ(1, BitPackedColumn::WidthFor(7, 7));
  EXPECT_EQ(1, BitPackedColumn::WidthFor(0, 1));
  EXPECT_EQ(2, BitPackedColumn::WidthFor(0, 3));
  EXPECT_EQ(3, BitPackedColumn::WidthFor(0, 4));
  EXPECT_EQ(8, BitPackedColumn::WidthFor(0, 255));
  EXPECT_EQ(9, BitPackedColumn::WidthFor(0, 256));
  EXPECT_EQ(8, BitPackedColumn::WidthFor(-100, 100));  // span 200
  EXPECT_EQ(32, BitPackedColumn::WidthFor(0, UINT32_MAX));
  EXPECT_EQ(32, BitPackedColumn::WidthFor(INT32_MIN, INT32_MAX));
  // Wider than the codec packs, or an inverted domain, is refused.
  EXPECT_EQ(0, BitPackedColumn::WidthFor(0, uint64_t{1} << 32));
  EXPECT_EQ(0, BitPackedColumn::WidthFor(INT64_MIN, INT64_MAX));
  EXPECT_EQ(0, BitPackedColumn::WidthFor(10, -10));
}

TEST(BitPackedColumnTest, BytesForRoundsUpToWholeBytes) {
  EXPECT_EQ(0U, BitPackedColumn::BytesFor(0, 8));
  EXPECT_EQ(1U, BitPackedColumn::BytesFor(1, 8));
  EXPECT_EQ(3U, BitPackedColumn::BytesFor(3, 8));
  EXPECT_EQ(3U, BitPackedColumn::BytesFor(10, 2));
  EXPECT_EQ(5U, BitPackedColumn::BytesFor(3, 12));  // 36 bits
  EXPECT_EQ(0U, BitPackedColumn::BytesFor(4, 0));
  EXPECT_EQ(0U, BitPackedColumn::BytesFor(4, 33));
}

TEST(BitPackedColumnTest, RoundTripsEveryWidth) {
  constexpr size_t kRows = 37;  // odd, so the last code straddles bytes
  for (uint8_t width = 1; width <= BitPackedColumn::kMaxWidth; ++width) {
    std::vector<uint8_t> buffer(BitPackedColumn::BytesFor(kRows, width), 0);
    BitPackedColumn column;
    ASSERT_TRUE(column.Attach(buffer.data(), buffer.size(), width, /*bias=*/0, kRows)) << "width " << int(width);

    for (size_t row = 0; row < kRows; ++row) {
      const uint64_t code = (static_cast<uint64_t>(row) * 2654435761u) & column.max_code();
      ASSERT_TRUE(column.SetCode(row, code));
    }
    for (size_t row = 0; row < kRows; ++row) {
      uint64_t code = 0;
      const uint64_t expected = (static_cast<uint64_t>(row) * 2654435761u) & column.max_code();
      ASSERT_TRUE(column.GetCode(row, &code)) << "width " << int(width) << " row " << row;
      EXPECT_EQ(expected, code) << "width " << int(width) << " row " << row;
    }
  }
}

TEST(BitPackedColumnTest, SignedDomainUsesTheBias) {
  std::vector<uint8_t> buffer(BitPackedColumn::BytesFor(8, 8), 0);
  BitPackedColumn column;
  ASSERT_TRUE(column.Attach(buffer.data(), buffer.size(), 8, /*bias=*/-100, 8));

  const int64_t values[] = {-100, -99, -1, 0, 1, 50, 99, 100};
  for (size_t row = 0; row < 8; ++row) ASSERT_TRUE(column.SetValue(row, values[row]));
  for (size_t row = 0; row < 8; ++row) {
    int64_t value = 0;
    ASSERT_TRUE(column.GetValue(row, &value));
    EXPECT_EQ(values[row], value) << "row " << row;
  }
  // Outside [bias, bias + max_code] is refused rather than truncated.
  EXPECT_FALSE(column.SetValue(0, -101));
  EXPECT_FALSE(column.SetValue(0, 156));
}

TEST(BitPackedColumnTest, OverwriteClearsStaleBits) {
  std::vector<uint8_t> buffer(BitPackedColumn::BytesFor(4, 12), 0xFF);  // all bits set
  BitPackedColumn column;
  ASSERT_TRUE(column.Attach(buffer.data(), buffer.size(), 12, /*bias=*/0, 4));

  for (size_t row = 0; row < 4; ++row) ASSERT_TRUE(column.SetCode(row, 0));
  for (size_t row = 0; row < 4; ++row) {
    uint64_t code = 1;
    ASSERT_TRUE(column.GetCode(row, &code));
    EXPECT_EQ(0U, code) << "row " << row;
  }
  // The padding bits past the last code stay untouched.
  ASSERT_TRUE(column.SetCode(3, column.max_code()));
  uint64_t code = 0;
  ASSERT_TRUE(column.GetCode(3, &code));
  EXPECT_EQ(column.max_code(), code);
}

TEST(BitPackedColumnTest, AttachValidatesArguments) {
  std::vector<uint8_t> buffer(16, 0);
  BitPackedColumn column;
  EXPECT_FALSE(column.Attach(nullptr, 16, 8, 0, 8));
  EXPECT_FALSE(column.Attach(buffer.data(), 16, 0, 0, 8));
  EXPECT_FALSE(column.Attach(buffer.data(), 16, 33, 0, 8));
  EXPECT_FALSE(column.Attach(buffer.data(), 7, 8, 0, 8));  // 8 rows need 8 bytes
  EXPECT_TRUE(column.Attach(buffer.data(), 8, 8, 0, 8));

  // Out-of-range rows are rejected instead of reading past the buffer.
  uint64_t code = 0;
  EXPECT_FALSE(column.GetCode(8, &code));
  EXPECT_FALSE(column.SetCode(8, 1));
}

}  // namespace Compress
}  // namespace ShannonBase
