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
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>

#include "storage/rapid_engine/imcs/col0stats.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/recovery/table_persistence.h"
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
