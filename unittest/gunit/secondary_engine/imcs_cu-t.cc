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
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>

#include "typelib.h"

#include "storage/rapid_engine/imcs/col0stats.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/imcs/table0persistence.h"
#include "storage/rapid_engine/utils/crc.h"
#include "storage/rapid_engine/utils/memory_pool.h"

namespace ShannonBase {
namespace Imcs {

namespace {
std::shared_ptr<Utils::MemoryPool> native_pool() {
  Utils::MemoryPool::Config config(64 * 1024 * 1024);
  config.small_pool_ratio = 0.5;
  config.auto_defragmentation = false;
  return std::make_shared<Utils::MemoryPool>(config);
}
FieldMetadata native_field() {
  FieldMetadata field;
  field.field_id = 0;
  field.type = MYSQL_TYPE_LONG;
  field.pack_length = 4;
  field.normalized_length = 4;
  field.is_secondary_field = true;
  field.encoding = Compress::ENCODING_TYPE::NONE;
  field.compression_level = Compress::COMPRESS_LEVEL::NONE;
  return field;
}
// Independent CU v2 record: two fixed-width integer slots and a valid CRC.
std::string native_record(bool empty = false) {
  std::string bytes;
  auto pod = [&](auto value) { bytes.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
  pod(CU_SERIAL_MAGIC);
  pod(uint16_t{2});
  pod(uint8_t{0});   // flags
  pod(uint32_t{0});  // column ID
  pod(static_cast<uint8_t>(MYSQL_TYPE_LONG));
  pod(static_cast<uint8_t>(Compress::ENCODING_TYPE::NONE));
  pod(uint8_t{0});  // no compression
  bytes.append(6, '\0');
  pod(uint64_t{4});  // packed width
  pod(uint64_t{4});  // slot width
  pod(uint64_t{empty ? 0u : 8u});
  pod(uint64_t{empty ? 0u : 2u});
  pod(double{0});
  pod(double{0});
  pod(double{0});
  pod(uint64_t{empty ? 0u : 8u});
  if (!empty) {
    pod(int32_t{11});
    pod(int32_t{-22});
  }
  pod(Utils::crc32c_compute(bytes.data(), bytes.size(), 0));
  return bytes;
}
void update_native_crc(std::string &bytes) {
  const auto crc = Utils::crc32c_compute(bytes.data(), bytes.size() - sizeof(uint32_t), 0);
  std::memcpy(bytes.data() + bytes.size() - sizeof(crc), &crc, sizeof(crc));
}

FieldMetadata native_bitpack_field(enum_field_types type, size_t slot_width) {
  auto field = native_field();
  field.type = type;
  field.pack_length = slot_width;
  field.normalized_length = slot_width;
  field.encoding = Compress::ENCODING_TYPE::BITPACK;
  return field;
}

// Independent CU v2 record for a bit-packable integer column. The payload is
// the canonical little-endian slot image, which is what a load writes before
// CU::pack() runs.
std::string native_packed_record(enum_field_types type, size_t slot_width, const std::vector<int64_t> &values) {
  std::string bytes;
  auto pod = [&](auto value) { bytes.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
  pod(CU_SERIAL_MAGIC);
  pod(uint16_t{2});
  pod(uint8_t{0});   // flags
  pod(uint32_t{0});  // column ID
  pod(static_cast<uint8_t>(type));
  pod(static_cast<uint8_t>(Compress::ENCODING_TYPE::BITPACK));
  pod(uint8_t{0});  // no compression
  bytes.append(6, '\0');
  pod(uint64_t{slot_width});
  pod(uint64_t{slot_width});
  pod(uint64_t{values.size() * slot_width});
  pod(uint64_t{values.size()});
  pod(double{0});
  pod(double{0});
  pod(double{0});
  pod(uint64_t{values.size() * slot_width});
  for (const auto value : values) {
    const auto bits = static_cast<uint64_t>(value);
    for (size_t i = 0; i < slot_width; ++i) bytes.push_back(static_cast<char>(bits >> (8 * i)));
  }
  pod(Utils::crc32c_compute(bytes.data(), bytes.size(), 0));
  return bytes;
}

FieldMetadata native_dictionary_field() {
  auto field = native_field();
  field.type = MYSQL_TYPE_VARCHAR;
  field.pack_length = 32;
  field.encoding = Compress::ENCODING_TYPE::SORTED;
  field.dictionary = std::make_shared<Compress::Dictionary>(field.encoding);
  return field;
}

std::string native_dictionary_record(const std::string &value = "alpha", bool dictionary = true,
                                     const std::string &extra = "") {
  auto bytes = native_record();
  bytes[6] = dictionary ? CU_FLAG_HAS_DICT : 0;
  bytes[11] = MYSQL_TYPE_VARCHAR;
  bytes[12] = static_cast<char>(Compress::ENCODING_TYPE::SORTED);
  const uint64_t pack_length = 32;
  std::memcpy(bytes.data() + 20, &pack_length, sizeof(pack_length));
  const uint32_t ids[] = {1, 2};
  std::memcpy(bytes.data() + 84, ids, sizeof(ids));
  bytes.resize(bytes.size() - sizeof(uint32_t));
  auto pod = [&](auto v) { bytes.append(reinterpret_cast<const char *>(&v), sizeof(v)); };
  if (dictionary) {
    pod(uint32_t{extra.empty() ? 3u : 4u});
    pod(uint64_t{value.size()});
    bytes.append(value);
    pod(uint64_t{0});  // Existing empty values have a non-null arena view.
    if (!extra.empty()) {
      pod(uint64_t{extra.size()});
      bytes.append(extra);
    }
  }
  pod(Utils::crc32c_compute(bytes.data(), bytes.size(), 0));
  return bytes;
}
}  // namespace

TEST(NativeCuPersistence, SharedDictionaryRestoresOnceAndKeepsArenaViews) {
  auto pool = native_pool();
  auto field = native_dictionary_field();
  CU first(nullptr, field, 0, 2, pool), second(nullptr, field, 0, 2, pool);
  CU::DictionaryRestoreContext dictionaries;
  std::istringstream a(native_dictionary_record()), b(native_dictionary_record());
  ASSERT_EQ(first.deserialize(&dictionaries, a, 2), SHANNON_SUCCESS);
  const auto original = field.dictionary->get_view(1).data();
  ASSERT_EQ(second.deserialize(&dictionaries, b, 2), SHANNON_SUCCESS);
  EXPECT_EQ(field.dictionary->get_view(1).data(), original);
  EXPECT_EQ(field.dictionary->get(1), "alpha");
  EXPECT_NE(field.dictionary->get_view(2).data(), nullptr);
  EXPECT_EQ(dictionaries.dictionaries_restored.load(), 1u);
  EXPECT_EQ(dictionaries.entries_restored.load(), 2u);
}

TEST(NativeCuPersistence, SharedDictionaryRejectsValidCrcConflictsAndMissingSections) {
  auto pool = native_pool();
  auto field = native_dictionary_field();
  CU first(nullptr, field, 0, 2, pool);
  CU::DictionaryRestoreContext dictionaries;
  std::istringstream good(native_dictionary_record());
  ASSERT_EQ(first.deserialize(&dictionaries, good, 2), SHANNON_SUCCESS);
  const auto original = field.dictionary->get_view(1).data();
  for (auto bytes : {native_dictionary_record("other"), native_dictionary_record("alpha", false)}) {
    CU second(nullptr, field, 0, 2, pool);
    std::istringstream input(bytes);
    EXPECT_NE(second.deserialize(&dictionaries, input, 2), SHANNON_SUCCESS);
    EXPECT_EQ(field.dictionary->get_view(1).data(), original);
    EXPECT_EQ(field.dictionary->get(1), "alpha");
  }
}

TEST(NativeCuPersistence, CorruptFirstDictionaryDoesNotPublishCanonicalState) {
  auto pool = native_pool();
  auto field = native_dictionary_field();
  CU first(nullptr, field, 0, 2, pool), second(nullptr, field, 0, 2, pool);
  CU::DictionaryRestoreContext dictionaries;
  auto bytes = native_dictionary_record();
  bytes.back() ^= 1;
  std::istringstream corrupt(bytes), good(native_dictionary_record());
  EXPECT_NE(first.deserialize(&dictionaries, corrupt, 2), SHANNON_SUCCESS);
  EXPECT_EQ(dictionaries.dictionaries_restored.load(), 0u);
  EXPECT_EQ(field.dictionary->size(), 1u);
  EXPECT_EQ(second.deserialize(&dictionaries, good, 2), SHANNON_SUCCESS);
  EXPECT_EQ(dictionaries.dictionaries_restored.load(), 1u);
}

TEST(NativeCuPersistence, ConcurrentChunksShareOneDictionaryInitialization) {
  auto pool = native_pool();
  auto field = native_dictionary_field();
  CU::DictionaryRestoreContext dictionaries;
  std::vector<std::unique_ptr<CU>> columns;
  std::vector<std::future<int>> results;
  for (size_t i = 0; i < 4; ++i) columns.push_back(std::make_unique<CU>(nullptr, field, 0, 2, pool));
  for (auto &column : columns) {
    results.push_back(std::async(std::launch::async, [cu = column.get(), &dictionaries] {
      std::istringstream input(native_dictionary_record());
      return cu->deserialize(&dictionaries, input, 2);
    }));
  }
  for (auto &result : results) EXPECT_EQ(result.get(), SHANNON_SUCCESS);
  EXPECT_EQ(dictionaries.dictionaries_restored.load(), 1u);
  EXPECT_EQ(dictionaries.entries_restored.load(), 2u);
}

TEST(NativeCuPersistence, LaterChunksCanExtendTheCanonicalDictionaryAfterValidation) {
  auto pool = native_pool();
  auto field = native_dictionary_field();
  CU first(nullptr, field, 0, 2, pool), later(nullptr, field, 0, 2, pool);
  CU::DictionaryRestoreContext dictionaries;
  auto extended = native_dictionary_record("alpha", true, "new");
  const uint32_t new_id = 3;
  std::memcpy(extended.data() + 88, &new_id, sizeof(new_id));
  update_native_crc(extended);
  std::istringstream a(native_dictionary_record()), b(extended);
  ASSERT_EQ(first.deserialize(&dictionaries, a, 2), SHANNON_SUCCESS);
  const auto original = field.dictionary->get_view(1).data();
  ASSERT_EQ(later.deserialize(&dictionaries, b, 2), SHANNON_SUCCESS);
  EXPECT_EQ(field.dictionary->get_view(1).data(), original);
  EXPECT_EQ(field.dictionary->get(3), "new");
  std::ostringstream output;
  ASSERT_EQ(later.serialize(output, 2, true), SHANNON_SUCCESS);
  uint32_t recovered_id = 0;
  const auto bytes = output.str();
  std::memcpy(&recovered_id, bytes.data() + 88, sizeof(recovered_id));
  EXPECT_EQ(recovered_id, new_id);
  EXPECT_EQ(dictionaries.dictionaries_restored.load(), 1u);
  EXPECT_EQ(dictionaries.entries_restored.load(), 3u);
  CU shorter(nullptr, field, 0, 2, pool);
  std::istringstream previous(native_dictionary_record());
  EXPECT_EQ(shorter.deserialize(&dictionaries, previous, 2), SHANNON_SUCCESS);
  EXPECT_EQ(field.dictionary->get(3), "new");
  EXPECT_EQ(dictionaries.entries_restored.load(), 3u);
}

TEST(NativeCuPersistence, CorruptExtensionDoesNotAddNewEntries) {
  auto pool = native_pool();
  auto field = native_dictionary_field();
  CU first(nullptr, field, 0, 2, pool), later(nullptr, field, 0, 2, pool);
  CU::DictionaryRestoreContext dictionaries;
  std::istringstream a(native_dictionary_record());
  ASSERT_EQ(first.deserialize(&dictionaries, a, 2), SHANNON_SUCCESS);
  auto bytes = native_dictionary_record("alpha", true, "new");
  bytes.back() ^= 1;
  std::istringstream b(bytes);
  EXPECT_NE(later.deserialize(&dictionaries, b, 2), SHANNON_SUCCESS);
  EXPECT_EQ(field.dictionary->get_view(3).data(), nullptr);
  EXPECT_EQ(dictionaries.entries_restored.load(), 2u);
}

TEST(NativeCuPersistence, ConcurrentConflictingExtensionsCannotOverwriteAnId) {
  auto pool = native_pool();
  auto field = native_dictionary_field();
  CU first(nullptr, field, 0, 2, pool), a(nullptr, field, 0, 2, pool), b(nullptr, field, 0, 2, pool);
  CU::DictionaryRestoreContext dictionaries;
  std::istringstream initial(native_dictionary_record());
  ASSERT_EQ(first.deserialize(&dictionaries, initial, 2), SHANNON_SUCCESS);
  auto apply = [&](CU &cu, const std::string &value) {
    std::istringstream input(native_dictionary_record("alpha", true, value));
    return cu.deserialize(&dictionaries, input, 2);
  };
  auto x = std::async(std::launch::async, [&] { return apply(a, "one"); });
  auto y = std::async(std::launch::async, [&] { return apply(b, "two"); });
  const auto xr = x.get(), yr = y.get();
  EXPECT_NE(xr == SHANNON_SUCCESS, yr == SHANNON_SUCCESS);
  EXPECT_EQ(field.dictionary->get(3), xr == SHANNON_SUCCESS ? "one" : "two");
  EXPECT_EQ(dictionaries.entries_restored.load(), 3u);
}

TEST(NativeCuPersistence, FixedWidthAndEmptyRecordsRoundTrip) {
  auto pool = native_pool();
  auto field = native_field();
  for (bool empty : {false, true}) {
    CU column(nullptr, field, 0, 2, pool);
    const auto bytes = native_record(empty);
    std::istringstream input(bytes);
    ASSERT_EQ(column.deserialize(nullptr, input, empty ? 0 : 2), SHANNON_SUCCESS);
    std::ostringstream output;
    ASSERT_EQ(column.serialize(output, empty ? 0 : 2, true), SHANNON_SUCCESS);
    EXPECT_EQ(output.str(), bytes);
  }
}
TEST(NativeCuPersistence, ValidCrcCannotHideShortRawPayload) {
  auto pool = native_pool();
  auto field = native_field();
  CU column(nullptr, field, 0, 2, pool);
  auto bytes = native_record();
  const uint64_t short_size = 4;
  std::memcpy(bytes.data() + 76, &short_size, sizeof(short_size));
  bytes.erase(88, 4);
  update_native_crc(bytes);
  std::istringstream input(bytes);
  EXPECT_NE(column.deserialize(nullptr, input, 2), SHANNON_SUCCESS);
}
TEST(NativeCuPersistence, ColumnRowsMustMatchContainingChunk) {
  auto pool = native_pool();
  auto field = native_field();
  CU column(nullptr, field, 0, 3, pool);
  std::istringstream input(native_record());
  EXPECT_NE(column.deserialize(nullptr, input, 3), SHANNON_SUCCESS);
}
TEST(NativeCuPersistence, ValidCrcCannotChangeEncodingOrFlags) {
  auto pool = native_pool();
  auto field = native_field();
  for (const size_t offset : {size_t{6}, size_t{12}}) {
    CU column(nullptr, field, 0, 2, pool);
    auto bytes = native_record();
    bytes[offset] = offset == 6 ? char{0x40} : static_cast<char>(Compress::ENCODING_TYPE::VARLEN);
    update_native_crc(bytes);
    std::istringstream input(bytes);
    EXPECT_NE(column.deserialize(nullptr, input, 2), SHANNON_SUCCESS);
  }
}
TEST(NativeCuPersistence, CompressedRecordNeedsAnExplicitCodec) {
  auto pool = native_pool();
  auto field = native_field();
  CU column(nullptr, field, 0, 2, pool);
  auto bytes = native_record();
  bytes[6] = CU_FLAG_COMPRESSED;
  update_native_crc(bytes);
  std::istringstream input(bytes);
  EXPECT_NE(column.deserialize(nullptr, input, 2), SHANNON_SUCCESS);
}
TEST(NativeCuPersistence, InputExceptionsReturnFailure) {
  auto pool = native_pool();
  auto field = native_field();
  CU column(nullptr, field, 0, 2, pool);
  std::istringstream input("");
  input.exceptions(std::ios::failbit | std::ios::badbit);
  EXPECT_NO_THROW(EXPECT_NE(column.deserialize(nullptr, input, 2), SHANNON_SUCCESS));
}
TEST(NativeCuPersistence, OutputExceptionsReturnFailure) {
  class ThrowingBuffer : public std::streambuf {
    std::streamsize xsputn(const char *, std::streamsize) override { throw std::bad_alloc(); }
  } buffer;
  auto pool = native_pool();
  auto field = native_field();
  CU column(nullptr, field, 0, 2, pool);
  std::ostream output(&buffer);
  output.exceptions(std::ios::badbit);
  EXPECT_NO_THROW(EXPECT_NE(column.serialize(output, 2, true), SHANNON_SUCCESS));
}

TEST(BitPackedCuStorage, PackRoundTripsCanonicalSlotsThroughVisibleCell) {
  auto pool = native_pool();
  auto field = native_bitpack_field(MYSQL_TYPE_LONG, 4);
  CU column(nullptr, field, 0, 8, pool);
  const std::vector<int64_t> values{1000, -1000, 0, 999};

  std::istringstream input(native_packed_record(MYSQL_TYPE_LONG, 4, values));
  ASSERT_EQ(column.deserialize(nullptr, input, values.size()), SHANNON_SUCCESS);
  ASSERT_FALSE(column.is_bitpacked());

  // -1000..0..1000 spans 2000 -> 11 bits per code instead of a 4-byte slot.
  EXPECT_EQ(column.pack(values.size()), 11);
  ASSERT_TRUE(column.is_bitpacked());
  EXPECT_EQ(column.bitpack_width(), 11);
  EXPECT_EQ(column.pack(values.size()), 11);  // already packed: no repack

  for (size_t row = 0; row < values.size(); ++row) {
    // This is the decode CU::get_visible_cell() runs for a packed cell (a
    // standalone CU has no owning IMCU to bound get_visible_cell by).
    std::array<uchar, 4> slot{};
    ASSERT_TRUE(column.decode_packed_slot(row, slot.data()));
    int32_t decoded = 0;
    std::memcpy(&decoded, slot.data(), sizeof(decoded));
    EXPECT_EQ(decoded, values[row]);
  }

  column.unpack();
  EXPECT_FALSE(column.is_bitpacked());
  // Unpacking restores the slot images, so packing again selects the same
  // domain and decodes back to the same values.
  EXPECT_EQ(column.pack(values.size()), 11);
  for (size_t row = 0; row < values.size(); ++row) {
    std::array<uchar, 4> slot{};
    ASSERT_TRUE(column.decode_packed_slot(row, slot.data()));
    int32_t decoded = 0;
    std::memcpy(&decoded, slot.data(), sizeof(decoded));
    EXPECT_EQ(decoded, values[row]);
  }
}

TEST(BitPackedCuStorage, PackKeepsNarrowIntegersBelowTheirSlotWidth) {
  auto pool = native_pool();
  auto field = native_bitpack_field(MYSQL_TYPE_TINY, 1);
  CU column(nullptr, field, 0, 8, pool);
  const std::vector<int64_t> values{-5, 7, 2};

  std::istringstream input(native_packed_record(MYSQL_TYPE_TINY, 1, values));
  ASSERT_EQ(column.deserialize(nullptr, input, values.size()), SHANNON_SUCCESS);
  EXPECT_EQ(column.pack(values.size()), 4);  // span -5..7 = 12 -> 4 bits
  ASSERT_TRUE(column.is_bitpacked());

  for (size_t row = 0; row < values.size(); ++row) {
    std::array<uchar, 1> slot{};
    ASSERT_TRUE(column.decode_packed_slot(row, slot.data()));
    int8_t decoded = 0;
    std::memcpy(&decoded, slot.data(), sizeof(decoded));
    EXPECT_EQ(decoded, values[row]);
  }
}

TEST(BitPackedCuStorage, PackRefusesIneligibleColumns) {
  auto pool = native_pool();

  // Not tagged BITPACK.
  {
    auto field = native_field();
    CU column(nullptr, field, 0, 4, pool);
    std::istringstream input(native_record());
    ASSERT_EQ(column.deserialize(nullptr, input, 2), SHANNON_SUCCESS);
    EXPECT_EQ(column.pack(2), 0);
    EXPECT_FALSE(column.is_bitpacked());
  }

  // No rows written.
  {
    auto field = native_bitpack_field(MYSQL_TYPE_LONG, 4);
    CU column(nullptr, field, 0, 4, pool);
    EXPECT_EQ(column.pack(0), 0);
    EXPECT_FALSE(column.is_bitpacked());
  }

  // A domain wider than the codec supports: the full int64 span stays canonical.
  {
    auto field = native_bitpack_field(MYSQL_TYPE_LONGLONG, 8);
    CU column(nullptr, field, 0, 4, pool);
    const std::vector<int64_t> values{std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()};
    std::istringstream input(native_packed_record(MYSQL_TYPE_LONGLONG, 8, values));
    ASSERT_EQ(column.deserialize(nullptr, input, values.size()), SHANNON_SUCCESS);
    EXPECT_EQ(column.pack(values.size()), 0);
    EXPECT_FALSE(column.is_bitpacked());
  }
}

TEST(BitPackedCuStorage, PackedSnapshotRoundTripsAndStaysSmallerThanSlots) {
  auto pool = native_pool();
  auto field = native_bitpack_field(MYSQL_TYPE_LONG, 4);
  CU column(nullptr, field, 0, 256, pool);
  std::vector<int64_t> values;
  for (int64_t i = 0; i < 200; ++i) values.push_back((i % 200) - 100);

  std::istringstream input(native_packed_record(MYSQL_TYPE_LONG, 4, values));
  ASSERT_EQ(column.deserialize(nullptr, input, values.size()), SHANNON_SUCCESS);
  // -100..99 spans 199 -> 8 bits per code instead of a 4-byte slot.
  ASSERT_EQ(column.pack(values.size()), 8);

  std::ostringstream out;
  ASSERT_EQ(column.serialize(out, values.size(), true), SHANNON_SUCCESS);
  // Header (88) + payload + packed section (9) + CRC against the slot image the
  // same snapshot would need without the encoding.
  EXPECT_LT(out.str().size(), 88 + values.size() * 4);

  CU restored(nullptr, field, 0, 256, pool);
  std::istringstream round(out.str());
  ASSERT_EQ(restored.deserialize(nullptr, round, values.size()), SHANNON_SUCCESS);
  ASSERT_TRUE(restored.is_bitpacked());
  EXPECT_EQ(restored.bitpack_width(), 8);
  for (size_t row = 0; row < values.size(); ++row) {
    std::array<uchar, 4> slot{};
    ASSERT_TRUE(restored.decode_packed_slot(row, slot.data()));
    int32_t decoded = 0;
    std::memcpy(&decoded, slot.data(), sizeof(decoded));
    EXPECT_EQ(decoded, values[row]);
  }

  // A snapshot restored into a column that did not opt in is corruption, not a
  // silent decode of the stream as slots.
  auto plain_field = native_field();
  CU plain(nullptr, plain_field, 0, 256, pool);
  std::istringstream rejected(out.str());
  EXPECT_NE(plain.deserialize(nullptr, rejected, values.size()), SHANNON_SUCCESS);
}

TEST(BitPackedCuStorage, MatchValueRangeSetsOnlyMatchingRows) {
  auto pool = native_pool();
  auto field = native_bitpack_field(MYSQL_TYPE_LONG, 4);
  CU column(nullptr, field, 0, 16, pool);
  const std::vector<int64_t> values{0, 5, 5, 9, 100, -7, 3};

  std::istringstream input(native_packed_record(MYSQL_TYPE_LONG, 4, values));
  ASSERT_EQ(column.deserialize(nullptr, input, values.size()), SHANNON_SUCCESS);
  ASSERT_NE(0, column.pack(values.size()));

  const auto matches = [&](int64_t low, int64_t high) {
    bit_array_t bits(values.size());
    EXPECT_TRUE(column.match_value_range(0, values.size(), low, high, bits));
    std::vector<bool> out;
    for (size_t i = 0; i < values.size(); ++i) out.push_back(Utils::Util::bit_array_get(&bits, i));
    return out;
  };
  EXPECT_EQ(matches(5, 5), (std::vector<bool>{false, true, true, false, false, false, false}));
  EXPECT_EQ(matches(3, 100), (std::vector<bool>{false, true, true, true, true, false, true}));
  EXPECT_EQ(matches(-7, -7), (std::vector<bool>{false, false, false, false, false, true, false}));
  // An impossible range (low > high) matches nothing; the caller's mask stays clear.
  EXPECT_EQ(matches(6, 4), std::vector<bool>(values.size(), false));

  // The domain the predicate path clamps its bounds against: -7..100 -> 7 bits.
  EXPECT_EQ(column.bitpack_bias(), -7);
  EXPECT_EQ(column.bitpack_width(), 7);
  EXPECT_EQ(column.bitpack_max_value(), int64_t{-7} + ((int64_t{1} << 7) - 1));
}

TEST(BitPackedCuStorage, DecodePackedRangeFillsAScanBatch) {
  auto pool = native_pool();
  auto field = native_bitpack_field(MYSQL_TYPE_LONGLONG, 8);
  CU column(nullptr, field, 0, 16, pool);
  const std::vector<int64_t> values{0, 5, 5, 9, 100, -7, 3};

  std::istringstream input(native_packed_record(MYSQL_TYPE_LONGLONG, 8, values));
  ASSERT_EQ(column.deserialize(nullptr, input, values.size()), SHANNON_SUCCESS);
  ASSERT_NE(0, column.pack(values.size()));

  // One tight loop decodes the batch into a stride-8 array of canonical slots.
  std::vector<uchar> batch(values.size() * sizeof(int64_t), 0xAA);
  ASSERT_TRUE(column.decode_packed_range(0, values.size(), batch.data(), sizeof(int64_t)));
  for (size_t i = 0; i < values.size(); ++i) {
    int64_t decoded = 0;
    std::memcpy(&decoded, batch.data() + i * sizeof(int64_t), sizeof(decoded));
    EXPECT_EQ(decoded, values[i]) << "row " << i;
  }

  // A range that leaves the packed stream is refused, as is a stride narrower
  // than the slot.
  EXPECT_FALSE(column.decode_packed_range(values.size() - 1, 2, batch.data(), sizeof(int64_t)));
  EXPECT_FALSE(column.decode_packed_range(0, values.size(), batch.data(), column.get_normalized_length() - 1));
}

TEST(BitPackedCuStorage, ReducePackedRowsAccumulatesCodes) {
  auto pool = native_pool();
  auto field = native_bitpack_field(MYSQL_TYPE_LONGLONG, 8);
  CU column(nullptr, field, 0, 16, pool);
  const std::vector<int64_t> values{0, 5, 5, 9, 100, -7, 3};

  std::istringstream input(native_packed_record(MYSQL_TYPE_LONGLONG, 8, values));
  ASSERT_EQ(column.deserialize(nullptr, input, values.size()), SHANNON_SUCCESS);
  ASSERT_NE(0, column.pack(values.size()));

  PackedReduction acc;
  for (size_t i = 0; i < values.size(); ++i) {
    ASSERT_TRUE(column.reduce_packed_row(i, acc)) << "row " << i;
    ++acc.rows;
  }
  EXPECT_EQ(acc.rows, values.size());
  EXPECT_EQ(acc.non_null, values.size());
  EXPECT_EQ(acc.sum, static_cast<__int128>(115));  // 0+5+5+9+100-7+3
  EXPECT_EQ(acc.min_value, -7);
  EXPECT_EQ(acc.max_value, 100);
  EXPECT_TRUE(acc.any);

  // A row outside the packed stream is refused, so a caller never silently
  // reduces a shorter range than it asked for.
  EXPECT_FALSE(column.reduce_packed_row(values.size(), acc));

  // Segments fold like values, whatever bias produced them.
  PackedReduction other;
  other.add_value(-7);
  other.rows = 1;
  acc.merge(other);
  EXPECT_EQ(acc.rows, values.size() + 1);
  EXPECT_EQ(acc.non_null, values.size() + 1);
  EXPECT_EQ(acc.sum, static_cast<__int128>(108));
  EXPECT_EQ(acc.min_value, -7);
  EXPECT_EQ(acc.max_value, 100);
}

TEST(BitPackedCuStorage, ReductionSchemaTraitsAcrossWidthsAndNullDensities) {
  auto pool = native_pool();
  const std::pair<enum_field_types, size_t> types[] = {
      {MYSQL_TYPE_TINY, 1}, {MYSQL_TYPE_SHORT, 2}, {MYSQL_TYPE_INT24, 3},
      {MYSQL_TYPE_LONG, 4}, {MYSQL_TYPE_LONGLONG, 8}};
  const std::vector<int64_t> values{-7, 0, 5, 9, 100, -3, 2};
  for (auto [type, width] : types) {
    for (unsigned null_every : {0u, 1u, 2u, 7u}) {
      SCOPED_TRACE(::testing::Message() << "type=" << type << " null_every=" << null_every);
      Imcu owner;
      owner.get_null_masks().push_back(std::make_unique<bit_array_t>(16));
      auto field = native_bitpack_field(type, width);
      field.nullable = null_every != 0;
      PackedReduction expected;
      for (size_t i = 0; i < values.size(); ++i) {
        if (null_every != 0 && i % null_every == 0)
          Utils::Util::bit_array_set(owner.get_null_masks()[0].get(), i);
        else
          expected.add_value(values[i]);
      }
      CU column(&owner, field, 0, 16, pool);
      std::istringstream input(native_packed_record(type, width, values));
      ASSERT_EQ(column.deserialize(nullptr, input, values.size()), SHANNON_SUCCESS);
      if (!expected.any) {
        // An all-NULL CU stays canonical; no packed reduction may be attempted.
        EXPECT_EQ(column.pack(values.size()), 0);
        continue;
      }
      ASSERT_NE(column.pack(values.size()), 0);
      PackedReduction actual;
      for (size_t i = 0; i < values.size(); ++i) ASSERT_TRUE(column.reduce_packed_row(i, actual));
      EXPECT_EQ(actual.non_null, expected.non_null);
      EXPECT_EQ(actual.sum, expected.sum);
      EXPECT_EQ(actual.min_value, expected.min_value);
      EXPECT_EQ(actual.max_value, expected.max_value);
      // A nullable trailing append has a bitmap bit but no packed code.
      if (field.nullable) {
        Utils::Util::bit_array_set(owner.get_null_masks()[0].get(), values.size());
        ASSERT_TRUE(column.reduce_packed_row(values.size(), actual));
        EXPECT_EQ(actual.sum, expected.sum);
        EXPECT_EQ(actual.non_null, expected.non_null);
      } else {
        EXPECT_FALSE(column.reduce_packed_row(values.size(), actual));
      }
    }
  }
}

TEST(NativeCuPersistence, RebindingPreservesPhysicalEnumAndSetTypes) {
  auto pool = native_pool();
  TYPELIB values{};
  Field_enum enum_field(8, true, "e", 1, &values, &my_charset_latin1);
  Field_set set_field(8, false, "s", 1, &values, &my_charset_latin1);
  auto meta = native_field();
  meta.type = enum_field.type();  // Both Field types report MYSQL_TYPE_STRING.
  meta.source_fld = &enum_field;
  meta.nullable = true;
  CU column(nullptr, meta, 0, 16, pool);
  EXPECT_EQ(column.type(), MYSQL_TYPE_STRING);
  EXPECT_EQ(column.real_type(), MYSQL_TYPE_ENUM);
  // Recovery reconnects a live Field rather than serializing runtime traits.
  column.reconstruct_field_metadata(&set_field, &my_charset_latin1);
  EXPECT_EQ(column.real_type(), MYSQL_TYPE_SET);
  column.reconstruct_field_metadata(&enum_field, &my_charset_latin1);
  EXPECT_EQ(column.real_type(), MYSQL_TYPE_ENUM);
}

TEST(BitPackedPredicateRange, IntegerRangeCoversEachComparison) {
  const int64_t lo = -1000, hi = 1000;
  const auto compare = [&](PredicateOperator op, int64_t bound) {
    Simple_Predicate pred(0, op, PredicateValue(bound));
    return pred.integer_range(lo, hi);
  };
  const auto is = [](const std::optional<std::pair<int64_t, int64_t>> &range, int64_t low, int64_t high) {
    return range.has_value() && range->first == low && range->second == high;
  };

  EXPECT_TRUE(is(compare(PredicateOperator::EQUAL, 5), 5, 5));
  EXPECT_TRUE(is(compare(PredicateOperator::LESS_THAN, 5), -1000, 4));
  EXPECT_TRUE(is(compare(PredicateOperator::LESS_EQUAL, 5), -1000, 5));
  EXPECT_TRUE(is(compare(PredicateOperator::GREATER_THAN, 5), 6, 1000));
  EXPECT_TRUE(is(compare(PredicateOperator::GREATER_EQUAL, 5), 5, 1000));
  // A bound outside the domain clamps; strictly-greater than a large bound is empty.
  EXPECT_TRUE(is(compare(PredicateOperator::LESS_THAN, 5000), -1000, 1000));
  auto empty = compare(PredicateOperator::GREATER_THAN, 5000);
  ASSERT_TRUE(empty.has_value());
  EXPECT_GT(empty->first, empty->second);

  Simple_Predicate between(0, PredicateValue(int64_t{-7}), PredicateValue(int64_t{9}));
  EXPECT_TRUE(is(between.integer_range(lo, hi), -7, 9));
  Simple_Predicate inverted(0, PredicateValue(int64_t{9}), PredicateValue(int64_t{-7}));
  auto inverted_range = inverted.integer_range(lo, hi);
  ASSERT_TRUE(inverted_range.has_value());
  EXPECT_GT(inverted_range->first, inverted_range->second);

  // Not one range, or not an integer bound: the caller must evaluate per row.
  EXPECT_FALSE(Simple_Predicate(0, PredicateOperator::NOT_EQUAL, PredicateValue(int64_t{5})).integer_range(lo, hi));
  EXPECT_FALSE(Simple_Predicate(0, PredicateOperator::LIKE, PredicateValue(std::string("%x%"))).integer_range(lo, hi));
  EXPECT_FALSE(Simple_Predicate(0, PredicateOperator::GREATER_THAN, PredicateValue(1.5)).integer_range(lo, hi));
}

// Test FieldMetadata structure
TEST(FieldMetadataTest, BasicProperties) {
  FieldMetadata field_meta;
  field_meta.field_id = 0;
  field_meta.type = MYSQL_TYPE_LONG;
  field_meta.pack_length = 8;
  field_meta.normalized_length = 8;
  field_meta.is_secondary_field = true;
  field_meta.encoding = Compress::ENCODING_TYPE::NONE;
  field_meta.compression_level = Compress::COMPRESS_LEVEL::DEFAULT;

  EXPECT_EQ(field_meta.field_id, 0u);
  EXPECT_EQ(field_meta.type, MYSQL_TYPE_LONG);
  EXPECT_EQ(field_meta.pack_length, 8u);
  EXPECT_EQ(field_meta.normalized_length, 8u);
  EXPECT_TRUE(field_meta.is_secondary_field);
  EXPECT_EQ(field_meta.encoding, Compress::ENCODING_TYPE::NONE);
  EXPECT_EQ(field_meta.compression_level, Compress::COMPRESS_LEVEL::DEFAULT);
}

// Test TableMetadata structure
TEST(TableMetadataTest, BasicProperties) {
  TableMetadata table_meta;
  table_meta.table_id = 1;
  table_meta.db_name = "test_db";
  table_meta.table_name = "test_table";
  table_meta.num_columns = 1;
  table_meta.rows_per_imcu = 100;

  EXPECT_EQ(table_meta.table_id, 1u);
  EXPECT_EQ(table_meta.db_name, "test_db");
  EXPECT_EQ(table_meta.table_name, "test_table");
  EXPECT_EQ(table_meta.num_columns, 1u);
  EXPECT_EQ(table_meta.rows_per_imcu, 100u);
}

// Test MemoryPool allocation for CU usage
TEST(MemoryPoolTest, AllocateForCU) {
  // MemoryPool refuses to reserve a sub-pool smaller than
  // MIN_SUBPOOL_RESERVE_SIZE (16MB, memory_pool.cpp): below it the sub-pool is
  // left empty and every allocation returns nullptr. Size the pool so both
  // halves clear that floor -- 0.5 is the largest ratio validate_config()
  // accepts. The backing store is a lazily-committed aligned_alloc, so the
  // nominal size costs nothing the test does not touch.
  Utils::MemoryPool::Config config(64 * 1024 * 1024);  // 64MB -> 32MB per sub-pool
  config.small_pool_ratio = 0.5;
  auto mem_pool = std::make_shared<Utils::MemoryPool>(config);

  // Test allocation similar to what CU does
  size_t capacity = 100;
  size_t normalized_length = 8;
  size_t total_capacity = capacity * normalized_length;
  if (total_capacity < 64 * 1024) total_capacity = 64 * 1024;

  void *ptr = mem_pool->allocate_auto(total_capacity);
  ASSERT_NE(ptr, nullptr);

  // Test deallocation
  auto result = mem_pool->deallocate(ptr, total_capacity);
  EXPECT_EQ(result, Utils::MemoryPool::Result::OK);
}

TEST(MemoryPoolTest, CapacityAndQuotaFailuresReturnNull) {
  Utils::MemoryPool::Config config(64 * 1024 * 1024);
  config.small_pool_ratio = 0.5;
  config.auto_defragmentation = false;
  auto pool = std::make_shared<Utils::MemoryPool>(config);
  EXPECT_EQ(pool->allocate(40 * 1024 * 1024), nullptr);
  pool->set_tenant_quota("limited", 1024);
  EXPECT_EQ(pool->allocate(2048, Utils::MemoryPool::SubPoolType::LARGE_BLOCK, "limited"), nullptr);
  EXPECT_EQ(pool->get_tenant_usage("limited"), 0U);
  EXPECT_EQ(pool->create_sub_pool(40 * 1024 * 1024), nullptr);
  void *ptr = pool->allocate(512, Utils::MemoryPool::SubPoolType::LARGE_BLOCK, "limited");
  ASSERT_NE(ptr, nullptr);
  EXPECT_EQ(pool->get_tenant_usage("limited"), 512U);
  EXPECT_EQ(pool->deallocate(ptr, 512), Utils::MemoryPool::Result::OK);
  EXPECT_EQ(pool->get_tenant_usage("limited"), 0U);
}

TEST(PredicateValueTest, NumericCoercionReportsFailureWithoutExceptions) {
  for (const char *input : {"", "abc", "1e9999", "1e-9999"}) {
    double value = 123.0;
    EXPECT_FALSE(PredicateValue(input).try_as_numeric(value)) << input;
    EXPECT_EQ(value, 123.0);
    EXPECT_EQ(PredicateValue::stod_or_zero(input), 0.0);
  }
  for (const char *input : {"  -12.5tail", "-12.5", "-0xc.8p0"}) {
    double value = 0.0;
    EXPECT_TRUE(PredicateValue(input).try_as_numeric(value)) << input;
    EXPECT_EQ(value, -12.5);
    EXPECT_EQ(PredicateValue::stod_or_zero(input), -12.5);
  }
  double value = 0.0;
  EXPECT_TRUE(PredicateValue("inf").try_as_numeric(value));
  EXPECT_TRUE(std::isinf(value));
  EXPECT_TRUE(PredicateValue("nan").try_as_numeric(value));
  EXPECT_TRUE(std::isnan(value));
}

// A manifest whose leading magic is wrong must be rejected as CORRUPTION,
// not parsed. load_manifest() first reads and validates MANIFEST_MAGIC and
// MANIFEST_FORMAT_VER before trusting the rest of the file, so a truncated or
// bit-flipped file must not produce a bogus RecoveryManifest.
TEST(TablePersistenceManagerTest, LoadManifestBadMagicIsRejected) {
  namespace fs = std::filesystem;
  const fs::path base = "/tmp/shannon_cu_corrupt_manifest";
  std::error_code ec;
  fs::remove_all(base, ec);

  TablePersistenceManager mgr(base.string(), "db", "tbl");

  // Write a manifest file with the WRONG magic, at the path load_manifest()
  // derives for generation 1.
  const fs::path manifest = base / "db" / "tbl" / "checkpoints" / "checkpoint-1.manifest";
  fs::create_directories(manifest.parent_path(), ec);
  {
    std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
    const uint32_t bad_magic = 0xDEADBEEFu;
    out.write(reinterpret_cast<const char *>(&bad_magic), sizeof(bad_magic));
  }

  auto res = mgr.load_manifest(1);
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(res.error, ErrorCode::CORRUPTION);
}

// A generation with no manifest at all is NOT_FOUND, not CORRUPTION.
TEST(TablePersistenceManagerTest, LoadManifestMissingIsNotFound) {
  namespace fs = std::filesystem;
  const fs::path base = "/tmp/shannon_cu_missing_manifest";
  std::error_code ec;
  fs::remove_all(base, ec);

  TablePersistenceManager mgr(base.string(), "db", "tbl");
  auto res = mgr.load_manifest(1);
  EXPECT_FALSE(res.ok());
  EXPECT_EQ(res.error, ErrorCode::NOT_FOUND);
}

// Generation selection / fallback relies on enumerating exactly the
// "checkpoint-N.manifest" files in ascending order and ignoring non-manifest
// files (e.g. the WAL). recover() walks this list in descending order, so a
// corrupt or missing newest manifest falls back to an older, loadable one.
TEST(TablePersistenceManagerTest, ListManifestGenerationsAscendingAndFiltered) {
  namespace fs = std::filesystem;
  const fs::path base = "/tmp/shannon_cu_gens";
  std::error_code ec;
  fs::remove_all(base, ec);

  TablePersistenceManager mgr(base.string(), "db", "tbl");

  const fs::path ckpt = base / "db" / "tbl" / "checkpoints";
  fs::create_directories(ckpt, ec);
  // Contiguous generation numbers are not required; out-of-order creation still
  // lists ascending. A non-manifest file must be ignored.
  for (uint64_t g : {7ull, 1ull, 3ull}) {
    std::ofstream out(ckpt / ("checkpoint-" + std::to_string(g) + ".manifest"), std::ios::binary | std::ios::trunc);
    out.put('X');
  }
  {
    std::ofstream out(ckpt / "cu_wal.log", std::ios::binary | std::ios::trunc);
    out.put('Y');
  }

  auto gens = mgr.list_manifest_generations();
  const std::vector<uint64_t> expected = {1, 3, 7};
  EXPECT_EQ(gens, expected);
}

// recover() walks manifest generations in DESCENDING order and falls back to an
// older generation when the newest is corrupt. This asserts the two premises of
// that fallback: the newer generation's manifest is rejected (CORRUPTION) and
// the older generation's manifest still loads.
TEST(TablePersistenceManagerTest, FallsBackToOlderGenerationWhenNewestCorrupt) {
  namespace fs = std::filesystem;
  const fs::path base = "/tmp/shannon_cu_fallback";
  std::error_code ec;
  fs::remove_all(base, ec);

  TablePersistenceManager mgr(base.string(), "db", "tbl");

  // Write an independent version-2 manifest fixture for the binlog format.
  const fs::path ckpt = base / "db" / "tbl" / "checkpoints";
  fs::create_directories(ckpt, ec);

  // No CHECKPOINTED IMCUs: the older manifest needs no snapshot files.
  std::string bytes;
  auto append = [&bytes](const auto &value) { bytes.append(reinterpret_cast<const char *>(&value), sizeof(value)); };
  auto append_string = [&bytes, &append](const std::string &value) {
    append(static_cast<uint32_t>(value.size()));
    bytes.append(value);
  };
  append(MANIFEST_MAGIC);
  append(uint16_t{2});       // independent legacy fixture, without v3 SCN fields
  append(uint64_t{0});       // table identity
  append(uint64_t{1});       // generation
  append(uint64_t{0x1234});  // schema fingerprint
  append_string("binlog.000001");
  append(uint64_t{4});
  append_string("source-prefix-digest");
  append(uint32_t{0});  // IMCU count
  append(Utils::crc32c_compute(bytes.data(), bytes.size(), 0));
  {
    std::ofstream out(mgr.manifest_path(1), std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.write(bytes.data(), bytes.size()).good());
  }

  // Write a CORRUPT manifest for generation 2 with a wrong magic.
  const fs::path m2 = ckpt / "checkpoint-2.manifest";
  {
    std::ofstream out(m2, std::ios::binary | std::ios::trunc);
    const uint32_t bad_magic = 0xDEADBEEFu;
    out.write(reinterpret_cast<const char *>(&bad_magic), sizeof(bad_magic));
  }

  auto newer = mgr.load_manifest(2);
  EXPECT_FALSE(newer.ok());
  EXPECT_EQ(newer.error, ErrorCode::CORRUPTION);

  auto older = mgr.load_manifest(1);
  EXPECT_TRUE(older.ok());
  EXPECT_EQ(older.value.generation, 1u);
  EXPECT_EQ(older.value.binlog.file, "binlog.000001");
  EXPECT_EQ(older.value.binlog.offset, 4u);
}

}  // namespace Imcs
}  // namespace ShannonBase

TEST(RapidCrcTest, BaselineDispatchMatchesSoftwareAndChunkedSeed) {
  constexpr char value[] = "123456789";
  const auto expected = 0xe3069283U;
  EXPECT_EQ(expected, ShannonBase::Utils::crc32c_compute(value, 9, 0));
  const auto prefix = ShannonBase::Utils::crc32c_compute(value, 4, 0);
  EXPECT_EQ(expected, ShannonBase::Utils::crc32c_compute(value + 4, 5, prefix));
  uint32_t software = ~0U;
  for (size_t i = 0; i < 9; ++i)
    software = (software >> 8) ^ ShannonBase::Utils::crc32c_table()[(software ^ value[i]) & 0xff];
  EXPECT_EQ(expected, ~software);
}
