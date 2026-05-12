#include <gtest/gtest.h>
#include <memory>

#include "storage/rapid_engine/imcs/col0stats.h"
#include "storage/rapid_engine/imcs/table0meta.h"
#include "storage/rapid_engine/utils/memory_pool.h"

namespace ShannonBase {
namespace Imcs {

// Test TableMetadata initialization
TEST(TableMetadataTest, Initialization) {
  TableMetadata table_meta;
  table_meta.table_id = 1;
  table_meta.db_name = "test_db";
  table_meta.table_name = "test_table";
  table_meta.num_columns = 2;
  table_meta.rows_per_imcu = 1000;

  EXPECT_EQ(table_meta.table_id, 1u);
  EXPECT_EQ(table_meta.db_name, "test_db");
  EXPECT_EQ(table_meta.table_name, "test_table");
  EXPECT_EQ(table_meta.num_columns, 2u);
  EXPECT_EQ(table_meta.rows_per_imcu, 1000u);
}

// Test TableMetadata with multiple fields
TEST(TableMetadataTest, MultipleFields) {
  TableMetadata table_meta;
  table_meta.table_id = 1;
  table_meta.num_columns = 3;

  // Add multiple fields
  FieldMetadata field1;
  field1.field_id = 0;
  field1.type = MYSQL_TYPE_LONG;
  field1.is_secondary_field = true;
  table_meta.fields.push_back(std::move(field1));

  FieldMetadata field2;
  field2.field_id = 1;
  field2.type = MYSQL_TYPE_VARCHAR;
  field2.is_secondary_field = true;
  table_meta.fields.push_back(std::move(field2));

  FieldMetadata field3;
  field3.field_id = 2;
  field3.type = MYSQL_TYPE_DOUBLE;
  field3.is_secondary_field = false;  // Primary field
  table_meta.fields.push_back(std::move(field3));

  EXPECT_EQ(table_meta.fields.size(), 3u);
  EXPECT_EQ(table_meta.fields[0].type, MYSQL_TYPE_LONG);
  EXPECT_EQ(table_meta.fields[1].type, MYSQL_TYPE_VARCHAR);
  EXPECT_EQ(table_meta.fields[2].type, MYSQL_TYPE_DOUBLE);
  EXPECT_FALSE(table_meta.fields[2].is_secondary_field);
}

// Test MemoryPool configuration for table usage
TEST(MemoryPoolTest, TableConfiguration) {
  Utils::MemoryPool::Config config(2 * 1024 * 1024);  // 2MB
  config.tenant_name = "test_table";
  config.allow_expansion = false;

  auto mem_pool = std::make_shared<Utils::MemoryPool>(config);

  // Test allocation
  void* ptr = mem_pool->allocate_auto(4096);
  ASSERT_NE(ptr, nullptr);

  // Test deallocation
  auto result = mem_pool->deallocate(ptr, 4096);
  EXPECT_EQ(result, Utils::MemoryPool::Result::OK);
}

// Test table statistics
TEST(TableMetadataTest, TableStatistics) {
  TableMetadata table_meta;

  // Simulate table operations
  table_meta.total_rows = 1000;
  table_meta.deleted_rows = 50;
  table_meta.total_imcus = 10;

  EXPECT_EQ(table_meta.total_rows.load(), 1000u);
  EXPECT_EQ(table_meta.deleted_rows.load(), 50u);
  EXPECT_EQ(table_meta.total_imcus.load(), 10u);

  // Test derived statistics
  EXPECT_EQ(table_meta.total_rows.load() - table_meta.deleted_rows.load(), 950u);
}

}  // namespace Imcs
}  // namespace ShannonBase