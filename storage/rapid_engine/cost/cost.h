/**
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs optimizer.
*/
#ifndef __SHANNONBASE_COST_H__
#define __SHANNONBASE_COST_H__
#include "storage/rapid_engine/include/rapid_types.h"

#include "include/my_base.h"
#include "sql/handler.h"  //Cost_estimator

#include "sql/opt_costconstants.h"
#include "sql/opt_costmodel.h"
#include "storage/rapid_engine/optimizer/query_plan.h"

class Item;
class Item_cond;
class Item_func;
class JOIN;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
class Predicate;
}  // namespace Imcs
namespace Optimizer {
/**
 * MySQL Cost Model Constants (for reference)
 *
 * These are the default values from MySQL's server_cost and engine_cost tables.
 * Query them with: SELECT * FROM mysql.server_cost;
 */
struct MySQLCostConstants {
  // Server costs (CPU operations)
  static constexpr double ROW_EVALUATE_COST = 0.1;        // Cost to evaluate WHERE on a row
  static constexpr double KEY_COMPARE_COST = 0.05;        // Cost to compare two keys
  static constexpr double MEMORY_TEMPTABLE_CREATE = 1.0;  // Cost to create temp table in memory
  static constexpr double MEMORY_TEMPTABLE_ROW = 0.1;     // Cost per row in memory temp table
  static constexpr double DISK_TEMPTABLE_CREATE = 20.0;   // Cost to create temp table on disk
  static constexpr double DISK_TEMPTABLE_ROW = 0.5;       // Cost per row in disk temp table

  // Engine costs (I/O operations)
  static constexpr double MEMORY_BLOCK_READ = 0.25;  // Cost to read block from buffer pool
  static constexpr double IO_BLOCK_READ = 1.0;       // Cost to read block from disk (SSD)
  static constexpr double DISK_SEEK_COST = 2.0;      // Cost of disk seek operation
};

/**
 * Cost Estimator Base Class
 */
class CostEstimator : public MemoryObject {
 public:
  virtual ~CostEstimator() = default;
  enum class Type : uint8_t { NONE = 0, RPD_ENG };

  // to calc the cost of `query_plan`.
  virtual double cost(const Plan &query_plan) = 0;
  virtual double cost(const JOIN *join) = 0;

  // estimate join cost between two relations.
  virtual double estimate_join_cost(ha_rows left_card, ha_rows right_card) = 0;
  // estimate scan cost given rows and number of imcus.
  virtual double estimate_scan_cost(ha_rows rows, size_t num_imcus) = 0;

  virtual inline double cpu_factor() const { return m_cpu_factor; }
  virtual inline double memory_factor() const { return m_memory_factor; }
  virtual inline double io_factor() const { return m_io_factor; }

 protected:
  Type m_cost_estimator_type{Type::NONE};
  double m_cpu_factor{0.0};
  double m_memory_factor{0.0};
  double m_io_factor{0.0};
};

/**
 * Rapid Engine Cost Estimator
 *
 * Cost factors are calibrated relative to MySQL's cost model:
 * - cpu_factor: Relative to MySQL's ROW_EVALUATE_COST (0.1)
 * - memory_factor: Relative to MySQL's MEMORY_BLOCK_READ (0.25)
 * - io_factor: Relative to MySQL's IO_BLOCK_READ (1.0)
 */
class RpdCostEstimator : public CostEstimator {
 public:
  static constexpr double VECTOR_CPU_FACTOR = 0.001;  // vectorized CPU factor
  static constexpr double HASH_BUILD_FACTOR = 0.02;   // Hash building cost factor
  static constexpr double HASH_PROBE_FACTOR = 0.01;   // Hash probing cost factor

  // Rapid efficiency multipliers
  static constexpr double COLUMNAR_EFFICIENCY = 0.7;    // Cache locality benefit
  static constexpr double COMPRESSION_BENEFIT = 0.5;    // I/O reduction from compression
  static constexpr double STORAGE_INDEX_BENEFIT = 0.2;  // Skip factor with storage indexes

  RpdCostEstimator(double cpu_factor, double mem_factor, double io_factor) {
    m_cost_estimator_type = CostEstimator::Type::RPD_ENG;
    m_cpu_factor = cpu_factor;
    m_memory_factor = mem_factor;
    m_io_factor = io_factor;  // IMCS in memory, IO factor is very low
  }

  /**
   * calc the cost of `query_plan`, the total query tree.
   */
  double cost(const Plan &query_plan) override;
  /**
   * calc the cost of `join`.
   */
  double cost(const JOIN *join) override;
  /**
   * estimate join cost between two relations. part of a query plan.
   */
  virtual double estimate_join_cost(ha_rows left_card, ha_rows right_card) override;
  /**
   * estimate scan cost given rows and number of imcus. part of a query plan.
   */
  virtual double estimate_scan_cost(ha_rows rows, size_t num_imcus) override;

 private:
  Imcs::RpdTable *get_rapid_table(TABLE *table);

  double calculate_scan_cost_detailed(TABLE *table, ha_rows total_rows, double filter_selectivity,
                                      bool can_use_storage_index, JOIN_TAB *tab);

  double calculate_hash_join_cost_detailed(double probe_rows, double build_rows, JOIN_TAB *tab);

  double estimate_predicate_selectivity(TABLE *table, Item *condition, bool *can_use_storage_index);

  double estimate_selectivity_fallback(Item *condition);

  /**
   * Calculate the cost of a vectorized scan operation on a columnar table
   *
   * @param table: MySQL TABLE structure
   * @param selectivity: Expected filter selectivity (0.0 - 1.0)
   * @param pruned: Whether storage index pruning can be applied
   * @return: Estimated total scan cost
   */
  double CalculateVectorizedScanCost(TABLE *table, double selectivity, bool pruned);

  /**
   * Calculate IMCU-level I/O cost
   *
   * @param num_imcus: Number of IMCUs in the table
   * @param selectivity: Filter selectivity
   * @param pruned: Whether storage index pruning is enabled
   * @return: IMCU I/O cost
   */
  double calculate_imcu_io_cost(size_t num_imcus, double selectivity, bool pruned);

  /**
   * Calculate column scanning cost
   *
   * @param table: MySQL TABLE structure
   * @param rpd_table: Rapid table metadata (may be nullptr)
   * @param total_rows: Total number of rows
   * @param selectivity: Filter selectivity
   * @param pruned: Whether storage index pruning is enabled
   * @return: Column scan cost
   */
  double calculate_column_scan_cost(TABLE *table, Imcs::RpdTable *rpd_table, ha_rows total_rows, double selectivity,
                                    bool pruned);

  /**
   * Calculate decompression cost
   *
   * @param table: MySQL TABLE structure
   * @param rpd_table: Rapid table metadata (may be nullptr)
   * @param total_rows: Total number of rows
   * @param selectivity: Filter selectivity
   * @param pruned: Whether storage index pruning is enabled
   * @return: Decompression CPU cost
   */
  double calculate_decompression_cost(TABLE *table, Imcs::RpdTable *rpd_table, ha_rows total_rows, double selectivity,
                                      bool pruned);

  /**
   * Calculate dictionary decoding cost
   *
   * @param table: MySQL TABLE structure
   * @param rpd_table: Rapid table metadata (may be nullptr)
   * @param total_rows: Total number of rows
   * @param selectivity: Filter selectivity
   * @return: Dictionary decoding cost
   */
  double calculate_dictionary_decoding_cost(TABLE *table, Imcs::RpdTable *rpd_table, ha_rows total_rows,
                                            double selectivity);

  /**
   * Calculate NULL bitmap scanning cost
   *
   * @param table: MySQL TABLE structure
   * @param total_rows: Total number of rows
   * @param selectivity: Filter selectivity
   * @return: NULL bitmap check cost
   */
  double calculate_null_bitmap_cost(TABLE *table, ha_rows total_rows, double selectivity);

  /**
   * Calculate filter evaluation cost
   *
   * @param table: MySQL TABLE structure
   * @param total_rows: Total number of rows
   * @param selectivity: Filter selectivity
   * @return: Filter evaluation cost
   */
  double calculate_filter_evaluation_cost(TABLE *table, ha_rows total_rows, double selectivity);

  /**
   * Calculate memory bandwidth cost
   *
   * @param table: MySQL TABLE structure
   * @param total_rows: Total number of rows
   * @param selectivity: Filter selectivity
   * @return: Memory bandwidth cost
   */
  double calculate_memory_bandwidth_cost(TABLE *table, ha_rows total_rows, double selectivity);

  /**
   * Get vectorization efficiency factor
   *
   * Returns a discount factor based on SIMD capabilities
   * @return: Efficiency factor (< 1.0 means speedup)
   */
  double get_vectorization_efficiency_factor();
};

/**
 * Helper class for predicate analysis
 */
class PredicateAnalyzer {
 public:
  PredicateAnalyzer(TABLE *table, Imcs::RpdTable *rpd_table) : m_table(table), m_rpd_table(rpd_table) {}
  virtual ~PredicateAnalyzer() = default;
  /**
   * Analyze the predicate condition to estimate selectivity and whether
   * storage index can be used for pruning.
   *
   * @param condition The predicate condition to analyze.
   * @param can_use_si Output parameter indicating if storage index can be used.
   * @return Estimated selectivity of the predicate (between 0.0 and 1.0).
   */
  double analyze(Item *condition, bool *can_use_si);

 private:
  double analyze_recursive(Item *item, bool *can_prune);
  double analyze_function(Item_func *func, bool *can_prune);
  double analyze_condition(Item_cond *cond, bool *can_prune);
  double estimate_without_stats(Item_func *func);
  double extract_numeric_value(Item *item);

  TABLE *m_table;
  Imcs::RpdTable *m_rpd_table;
};

/**
 * Cost Model Server Singleton
 */
class CostModelServer : public Cost_model_server {
 public:
  CostModelServer() = default;

  /* get a costEstimator instance.
   * @param type: Type of cost estimator
   * @return: Cost estimator instance (cached singleton)
   */
  static CostEstimator *Instance(CostEstimator::Type type);

  static void Cleanup() {
    std::lock_guard<std::mutex> lock(instance_mutex_);
    for (auto &pair : instances_) {
      delete pair.second;
    }
    instances_.clear();
  }

 private:
  static constexpr double MIN_CPU_FACTOR = 0.01;  // At least 1/10th of MySQL's cost
  static constexpr double MIN_MEM_FACTOR = 0.05;  // At least 1/5th of MySQL's cost
  static constexpr double MIN_IO_FACTOR = 0.05;   // At least 1/20th of MySQL's cost

  static std::unordered_map<CostEstimator::Type, CostEstimator *> instances_;
  static std::mutex instance_mutex_;
};
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_COST_H__