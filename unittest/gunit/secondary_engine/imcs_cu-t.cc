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
#include <memory>

#include "storage/rapid_engine/imcs/col0stats.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/recovery/table_persistence.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/utils/memory_pool.h"
#include "storage/rapid_engine/utils/crc.h"

namespace ShannonBase {
namespace Imcs {

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

  void* ptr = mem_pool->allocate_auto(total_capacity);
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
    std::ofstream out(ckpt / ("checkpoint-" + std::to_string(g) + ".manifest"),
                      std::ios::binary | std::ios::trunc);
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
  auto append = [&bytes](const auto &value) {
    bytes.append(reinterpret_cast<const char *>(&value), sizeof(value));
  };
  auto append_string = [&bytes, &append](const std::string &value) {
    append(static_cast<uint32_t>(value.size()));
    bytes.append(value);
  };
  append(MANIFEST_MAGIC);
  append(MANIFEST_FORMAT_VER);
  append(uint64_t{0});  // table identity
  append(uint64_t{1});  // generation
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
}  // namespace ShannonBase</content>
