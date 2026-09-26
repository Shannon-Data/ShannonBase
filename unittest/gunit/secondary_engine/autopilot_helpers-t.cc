/* Copyright (c) 2023, Shannon Data AI and/or its affiliates. */

#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "storage/rapid_engine/autopilot/loader.h"

namespace ShannonBase::Autopilot::detail {

TEST(RapidAutopilotHelpersTest, MemoryThresholdDoesNotOverflow) {
  constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
  EXPECT_EQ(max, memory_threshold_bytes(max, 100));
  EXPECT_EQ((max / 100) * 70 + ((max % 100) * 70) / 100, memory_threshold_bytes(max, 70));
  EXPECT_EQ(0u, memory_threshold_bytes(max, -1));
  EXPECT_EQ(max, memory_threshold_bytes(max, 101));
}

TEST(RapidAutopilotHelpersTest, MemoryBudgetComparisonDoesNotOverflow) {
  constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
  EXPECT_TRUE(fits_memory_budget(90, 10, 100));
  EXPECT_FALSE(fits_memory_budget(90, 11, 100));
  EXPECT_FALSE(fits_memory_budget(101, 0, 100));
  EXPECT_FALSE(fits_memory_budget(max, 1, max));
  EXPECT_TRUE(fits_memory_budget(max - 1, 1, max));
}

TEST(RapidAutopilotHelpersTest, SystemSchemasUseExactCaseInsensitiveNames) {
  EXPECT_TRUE(is_system_schema_name("mysql"));
  EXPECT_TRUE(is_system_schema_name("INFORMATION_SCHEMA"));
  EXPECT_TRUE(is_system_schema_name("Performance_Schema"));
  EXPECT_TRUE(is_system_schema_name("Sys"));
  EXPECT_FALSE(is_system_schema_name("mysql_app"));
  EXPECT_FALSE(is_system_schema_name("sys_data"));
  EXPECT_FALSE(is_system_schema_name(nullptr));
}

}  // namespace ShannonBase::Autopilot::detail
