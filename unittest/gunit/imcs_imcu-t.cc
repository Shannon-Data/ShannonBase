#include <gtest/gtest.h>
#include <memory>

#include "storage/rapid_engine/imcs/col0stats.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/utils/memory_pool.h"

namespace ShannonBase {
namespace Imcs {

// Test TableMetadata with fields
TEST(TableMetadataTest, WithFields) {
  TableMetadata table_meta;
  table_meta.table_id = 1;
  table_meta.db_name = "test_db";
  table_meta.table_name = "test_table";
  table_meta.num_columns = 1;
  table_meta.rows_per_imcu = 100;

  // Add a field
  FieldMetadata field_meta;
  field_meta.field_id = 0;
  field_meta.type = MYSQL_TYPE_LONG;
  field_meta.pack_length = 8;
  field_meta.normalized_length = 8;
  field_meta.is_secondary_field = true;
  field_meta.encoding = Compress::ENCODING_TYPE::NONE;
  field_meta.compression_level = Compress::COMPRESS_LEVEL::DEFAULT;
  table_meta.fields.push_back(std::move(field_meta));

  EXPECT_EQ(table_meta.fields.size(), 1u);
  EXPECT_EQ(table_meta.fields[0].field_id, 0u);
  EXPECT_EQ(table_meta.fields[0].type, MYSQL_TYPE_LONG);
  EXPECT_TRUE(table_meta.fields[0].is_secondary_field);
}

// Test MemoryPool sub-pool creation (similar to what Imcu does)
TEST(MemoryPoolTest, SubPoolCreation) {
  Utils::MemoryPool::Config config(1024 * 1024);  // 1MB
  auto mem_pool = std::make_shared<Utils::MemoryPool>(config);

  // Test basic allocation that Imcu would use
  size_t allocation_size = 1024;
  void* ptr = mem_pool->allocate_auto(allocation_size);
  ASSERT_NE(ptr, nullptr);

  // Fill with test data
  memset(ptr, 0xAA, allocation_size);

  // Verify data
  for (size_t i = 0; i < allocation_size; ++i) {
    EXPECT_EQ(static_cast<unsigned char*>(ptr)[i], 0xAA);
  }

  // Test deallocation
  auto result = mem_pool->deallocate(ptr, allocation_size);
  EXPECT_EQ(result, Utils::MemoryPool::Result::OK);
}

// Test statistics tracking
TEST(TableMetadataTest, Statistics) {
  TableMetadata table_meta;
  table_meta.total_imcus = 5;
  table_meta.total_rows = 500;
  table_meta.deleted_rows = 10;

  EXPECT_EQ(table_meta.total_imcus.load(), 5u);
  EXPECT_EQ(table_meta.total_rows.load(), 500u);
  EXPECT_EQ(table_meta.deleted_rows.load(), 10u);
}

}  // namespace Imcs
}  // namespace ShannonBase