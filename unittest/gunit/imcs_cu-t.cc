#include <gtest/gtest.h>
#include <memory>

#include "storage/rapid_engine/imcs/col0stats.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/utils/memory_pool.h"

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
  Utils::MemoryPool::Config config(1024 * 1024);  // 1MB
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

}  // namespace Imcs
}  // namespace ShannonBase</content>