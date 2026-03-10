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
#include "storage/rapid_engine/cost/cost.h"

#include <unistd.h>
#include <mutex>
#include <unordered_map>

#include "sql/item.h"                                 //Item
#include "sql/item_cmpfunc.h"                         //Item_cmpfunc
#include "sql/item_func.h"                            //Item_func
#include "sql/join_optimizer/access_path.h"           //AccessPath
#include "sql/join_optimizer/make_join_hypergraph.h"  //HyperGraph
#include "sql/sql_optimizer.h"                        //JOIN
#include "sql/table.h"                                //TABLE

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"
#include "storage/rapid_engine/imcs/col0stats.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/predicate.h"
#include "storage/rapid_engine/imcs/table.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/optimizer/utils.h"

namespace ShannonBase {
namespace Optimizer {
std::unordered_map<CostEstimator::Type, CostEstimator *> CostModelServer::instances_;
std::mutex CostModelServer::instance_mutex_;
/**
 *   MySQL Cost Model Reference:
 * - Row evaluation cost: 0.1 (CPU cost per row)
 * - Key comparison cost: 0.05 (CPU cost per key comparison)
 * - Memory block read cost: 0.25 (reading a block from memory)
 * - Disk block read cost: 1.0 (reading a block from disk)
 * - Disk seek cost: 2.0 (seek operation cost)
 */
CostEstimator *CostModelServer::Instance(CostEstimator::Type type) {
  std::lock_guard<std::mutex> lock(instance_mutex_);
  auto it = instances_.find(type);
  if (it != instances_.end()) return it->second;

  if (type == CostEstimator::Type::RPD_ENG) {
    long num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus <= 0) num_cpus = 1;

    double cpu_factor{MySQLCostConstants::ROW_EVALUATE_COST}, mem_factor{MySQLCostConstants::MEMORY_BLOCK_READ},
        io_factor{MySQLCostConstants::IO_BLOCK_READ};
    double vectorization_speedup{1.0};  // Will adjust based on SIMD
    double columnar_efficiency{0.7};    // 30% faster than row-based (cache locality)
    double compression_benefit{0.5};    // 50% less I/O due to compression
#ifdef SHANNON_X86_PLATFORM
    bool has_avx512 = __builtin_cpu_supports("avx512f");
    bool has_avx2 = __builtin_cpu_supports("avx2");
    bool has_sse4_2 = __builtin_cpu_supports("sse4.2");

    if (has_avx512) {
      // AVX-512: process 16 floats or 8 doubles per instruction
      vectorization_speedup = 0.25;  // 4x speedup (75% cost reduction)
    } else if (has_avx2) {
      // AVX2: process 8 floats or 4 doubles per instruction
      vectorization_speedup = 0.4;  // 2.5x speedup (60% cost reduction)
    } else if (has_sse4_2) {
      // SSE4.2: process 4 floats or 2 doubles per instruction
      vectorization_speedup = 0.6;  // 1.67x speedup (40% cost reduction)
    } else {
      // No SIMD: minimal vectorization benefit
      vectorization_speedup = 0.9;  // 1.11x speedup (10% cost reduction)
    }
#elif defined(SHANNON_ARM_PLATFORM)
#ifdef SHANNON_ARM_VECT_SUPPORTED
    // ARM NEON: process 4 floats or 2 doubles per instruction
    vectorization_speedup = 0.5;  // 2x speedup (50% cost reduction)
#else
    // No SIMD support
    vectorization_speedup = 0.9;  // Minimal benefit
#endif
#else
    // Unknown platform: conservative estimate
    vectorization_speedup = 0.8;
#endif
    // Adjust for CPU count (parallelism benefit)
    double parallelism_factor = 1.0;
    if (num_cpus >= 64) {
      parallelism_factor = 0.5;  // High parallelism: 50% of sequential cost
    } else if (num_cpus >= 32) {
      parallelism_factor = 0.6;  // Good parallelism: 60% of sequential cost
    } else if (num_cpus >= 16) {
      parallelism_factor = 0.7;  // Moderate parallelism: 70% of sequential cost
    } else if (num_cpus >= 8) {
      parallelism_factor = 0.8;  // Some parallelism: 80% of sequential cost
    } else {
      parallelism_factor = 0.9;  // Limited parallelism: 90% of sequential cost
    }
    // CPU factor: MySQL baseline * columnar * vectorization * parallelism
    cpu_factor =
        MySQLCostConstants::ROW_EVALUATE_COST * columnar_efficiency * vectorization_speedup * parallelism_factor;

    // Memory factor: MySQL baseline * columnar efficiency
    // (Columnar access has better cache locality)
    mem_factor = MySQLCostConstants::MEMORY_BLOCK_READ * columnar_efficiency;

    // I/O factor: MySQL baseline * compression benefit
    // (Compression significantly reduces I/O)
    // Note: Rapid is in-memory, so I/O is actually memory access
    io_factor = MySQLCostConstants::MEMORY_BLOCK_READ * compression_benefit;

    cpu_factor = std::max(cpu_factor, MIN_CPU_FACTOR);
    mem_factor = std::max(mem_factor, MIN_MEM_FACTOR);
    io_factor = std::max(io_factor, MIN_IO_FACTOR);
    auto *estimator = new RpdCostEstimator(cpu_factor, mem_factor, io_factor);
    instances_[type] = estimator;
    return estimator;
  }

  // TODO: the other types Estimator...
  return nullptr;
}

double SelectivityEstimator::estimate_selectivity(const TABLE *table, const Item *condition) {
  bool dummy{false};
  return SelectivityEstimator::estimate_predicate_selectivity_internal(table, condition, &dummy);
}

double SelectivityEstimator::estimate_selectivity(const JoinHypergraph &graph, AccessPath *path) {
  if (!path) return 1.0;

  switch (path->type) {
    case AccessPath::FILTER:
      return estimate_filter_selectivity(graph, path);
    case AccessPath::AGGREGATE:
      return estimate_aggregate_selectivity(graph, path);
    case AccessPath::HASH_JOIN:
    case AccessPath::NESTED_LOOP_JOIN:
      return estimate_join_selectivity(graph, path);
    default:
      return 1.0;
  }

  return 1.0;
}
double SelectivityEstimator::estimate_join_selectivity(const Item_field *left_field, const Item_field *right_field) {
  if (!left_field || !left_field->field || !left_field->field->table || !right_field || !right_field->field ||
      !right_field->field->table)
    return SelectivityEstimator::kDefaultJoinSelectivity;

  Imcs::RpdTable *left_rpd = Utils::rpd_lookup_func()(left_field->field->table);
  Imcs::RpdTable *right_rpd = Utils::rpd_lookup_func()(right_field->field->table);

  if (!left_rpd || !right_rpd) return SelectivityEstimator::kDefaultJoinSelectivity;

  //  using NDV
  uint32_t left_col_idx = left_field->field->field_index();
  uint32_t right_col_idx = right_field->field->field_index();

  size_t left_ndv = 0;
  size_t right_ndv = 0;
  if (auto *left_stats = left_rpd->get_column_stats(left_col_idx))
    left_ndv = left_stats->get_basic_stats().distinct_count;

  if (auto *right_stats = right_rpd->get_column_stats(right_col_idx)) {
    right_ndv = right_stats->get_basic_stats().distinct_count;
  }

  if (left_ndv == 0 || right_ndv == 0) return SelectivityEstimator::kDefaultJoinSelectivity;

  // calc: 1 / max(NDV_L, NDV_R)
  double sel = 1.0 / static_cast<double>(std::max(left_ndv, right_ndv));
  return std::max(SelectivityEstimator::kMinJoinSelectivity, std::min(SelectivityEstimator::kMaxJoinSelectivity, sel));
}

double SelectivityEstimator::estimate_join_selectivity(const std::vector<Item *> &join_conditions) {
  if (join_conditions.empty()) return SelectivityEstimator::kDefaultJoinSelectivity;

  double combined{1.0};
  int eq_count{0};

  for (const Item *cond : join_conditions) {
    if (!cond) continue;

    double sel = estimate_join_item_selectivity(cond);
    if (sel < SelectivityEstimator::kDefaultJoinSelectivity + 1e-9 &&
        sel > SelectivityEstimator::kDefaultJoinSelectivity - 1e-9) {
      if (!cond || cond->type() != Item::FUNC_ITEM) continue;
      const auto *f = static_cast<const Item_func *>(cond);
      if (f->functype() != Item_func::EQ_FUNC) continue;
      if (f->argument_count() != 2) continue;
    }

    combined *= sel;
    ++eq_count;
  }
  if (eq_count == 0) return SelectivityEstimator::kDefaultJoinSelectivity;

  return std::max(SelectivityEstimator::kMinJoinSelectivity,
                  std::min(SelectivityEstimator::kMaxJoinSelectivity, combined));
}

double SelectivityEstimator::estimate_join_item_selectivity(const Item *condition) {
  if (!condition || condition->type() != Item::FUNC_ITEM) return SelectivityEstimator::kDefaultJoinSelectivity;

  const auto *func = static_cast<const Item_func *>(condition);

  // only deal with `EQ_FUNC ( t1.a = t2.b )`
  if (func->functype() != Item_func::EQ_FUNC || func->argument_count() != 2)
    return SelectivityEstimator::kDefaultJoinSelectivity;

  const Item *lhs = func->arguments()[0];
  const Item *rhs = func->arguments()[1];

  if (lhs->type() != Item::FIELD_ITEM || rhs->type() != Item::FIELD_ITEM)
    return SelectivityEstimator::kDefaultJoinSelectivity;

  return SelectivityEstimator::estimate_join_selectivity(static_cast<const Item_field *>(lhs),
                                                         static_cast<const Item_field *>(rhs));
}

double SelectivityEstimator::estimate_filter_selectivity(const JoinHypergraph &graph, AccessPath *path) {
  if (path->type != AccessPath::FILTER) return 1.0;

  auto &f = path->filter();
  if (!f.condition) return 1.0;

  table_map covered_tables = Utils::get_tablescovered(f.child);
  std::vector<TABLE *> tables = get_tables_from_tablemap(covered_tables, graph);
  if (tables.empty()) return 1.0;

  double combined_selectivity = 1.0;
  bool has_any_estimate = false;

  auto *est = CostModelServer::Instance(CostEstimator::Type::RPD_ENG);
  if (!est) return 0.5;

  for (TABLE *table : tables) {
    double table_selectivity = SelectivityEstimator::estimate_selectivity(table, f.condition);
    if (table_selectivity < 1.0) {
      combined_selectivity *= table_selectivity;
      has_any_estimate = true;
    }
  }

  if (!has_any_estimate) combined_selectivity = 0.3;
  return std::max(0.0001, std::min(1.0, combined_selectivity));
}

double SelectivityEstimator::estimate_aggregate_selectivity(const JoinHypergraph &graph, AccessPath *path) {
  if (path->type != AccessPath::AGGREGATE) return 1.0;

  auto &agg = path->aggregate();
  if (!agg.child) return 1.0;

  double input_rows = agg.child->num_output_rows();
  if (input_rows <= 1.0) return 1.0;

  table_map covered = ShannonBase::Optimizer::Utils::get_tablescovered(path);
  std::vector<TABLE *> tables = get_tables_from_tablemap(covered, graph);

  if (tables.empty()) return std::sqrt(input_rows) / input_rows;

  TABLE *primary_table = tables[0];
  auto *rpd_table = Utils::rpd_lookup_func()(primary_table);

  if (!rpd_table) return std::sqrt(input_rows) / input_rows;

  const auto &table_meta = rpd_table->meta();
  std::vector<uint64_t> ndv_values;
  for (const auto &field : table_meta.fields) {
    if (field.statistics) {
      const auto &basic = field.statistics->get_basic_stats();
      if (basic.distinct_count > 0) {
        ndv_values.push_back(basic.distinct_count);
      }
    }
  }

  if (ndv_values.empty()) return std::sqrt(input_rows) / input_rows;

  std::sort(ndv_values.begin(), ndv_values.end(), std::greater<uint64_t>());

  uint64_t estimated_ndv = 0;
  if (ndv_values.size() == 1) {
    estimated_ndv = ndv_values[0];
  } else {
    estimated_ndv = estimate_multicolumnNDV(ndv_values, input_rows);
  }

  double output_rows = std::min(static_cast<double>(estimated_ndv), input_rows);
  output_rows = std::max(output_rows, std::sqrt(input_rows));
  return output_rows / input_rows;
}

double SelectivityEstimator::estimate_join_selectivity(const JoinHypergraph &graph, AccessPath *path) {
  // Simplified implementation: returns a conservative estimate
  // Full implementation requires analyzing the column cardinality
  // of the join predicate (e.g., based on NDV statistics)
  return 0.1;
}

std::vector<TABLE *> SelectivityEstimator::get_tables_from_tablemap(table_map tmap, const JoinHypergraph &graph) {
  std::vector<TABLE *> tables;

  for (size_t node_idx = 0; node_idx < graph.nodes.size(); ++node_idx) {
    TABLE *table = graph.nodes[node_idx].table();
    if (!table) continue;

    table_map table_bit = table->pos_in_table_list->map();
    if (tmap & table_bit) {
      tables.push_back(table);
    }
  }

  return tables;
}

uint64_t SelectivityEstimator::estimate_singlecolumnNDV(TABLE *table, Item *group_item, double input_rows) {
  if (!table || !group_item) return 0;

  if (group_item->type() != Item::FIELD_ITEM) return static_cast<uint64_t>(std::sqrt(input_rows));

  auto *field_item = static_cast<Item_field *>(group_item);
  if (field_item->field->table != table) return 0;

  uint32_t col_idx = field_item->field->field_index();
  auto *col_stats = SelectivityEstimator::get_column_statistics(table, col_idx);

  if (!col_stats) return static_cast<uint64_t>(std::sqrt(input_rows));

  const auto &basic = col_stats->get_basic_stats();
  uint64_t ndv = basic.distinct_count;
  return std::min(ndv, static_cast<uint64_t>(input_rows));
}

uint64_t SelectivityEstimator::estimate_multicolumnNDV(const std::vector<uint64_t> &column_ndvs, double input_rows) {
  if (column_ndvs.empty()) return 0;
  if (column_ndvs.size() == 1) return column_ndvs[0];

  uint64_t ndv1 = column_ndvs[0];
  uint64_t ndv2 = column_ndvs[1];

  double geometric_mean = std::sqrt(static_cast<double>(ndv1) * static_cast<double>(ndv2));

  constexpr double correlation_factor = 1.5;
  uint64_t combined_ndv = static_cast<uint64_t>(geometric_mean * correlation_factor);

  if (column_ndvs.size() > 2) {
    for (size_t i = 2; i < column_ndvs.size(); ++i) {
      double growth_factor = 1.0 + 0.5 / i;
      combined_ndv = static_cast<uint64_t>(combined_ndv * growth_factor);
    }
  }

  combined_ndv = std::min(combined_ndv, static_cast<uint64_t>(input_rows));
  combined_ndv = std::max(combined_ndv, ndv1);

  return combined_ndv;
}

Imcs::ColumnStatistics *SelectivityEstimator::get_column_statistics(TABLE *table, uint32_t col_idx) {
  if (!table) return nullptr;

  auto *rpd_table = Utils::rpd_lookup_func()(table);
  if (!rpd_table) return nullptr;

  const auto &table_meta = rpd_table->meta();
  if (col_idx >= table_meta.fields.size()) return nullptr;

  return table_meta.fields[col_idx].statistics.get();
}

double SelectivityEstimator::estimate_predicate_selectivity_internal(const TABLE *table, const Item *condition,
                                                                     bool *can_use_storage_index) {
  if (!condition) return 1.0;
  *can_use_storage_index = false;
  auto get_rapid_table = [&](const TABLE *table) -> Imcs::RpdTable * {
    if (!table || !table->file) return nullptr;
    auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
    if (!share) return nullptr;
    Imcs::RpdTable *rpd_table = (share->is_partitioned)
                                    ? ShannonBase::Imcs::Imcs::instance()->get_rpd_parttable(share->m_tableid)
                                    : ShannonBase::Imcs::Imcs::instance()->get_rpd_table(share->m_tableid);
    return rpd_table;
  };

  // Get Rapid table for statistics
  auto *rpd_table = get_rapid_table(table);
  if (!rpd_table) {
    return SelectivityEstimator::estimate_selectivity_fallback(condition);
  }

  // Parse Item tree to extract predicates
  PredicateAnalyzer analyzer(table, rpd_table);
  double selectivity = analyzer.analyze(condition, can_use_storage_index);
  return selectivity;
}

double SelectivityEstimator::estimate_selectivity_fallback(const Item *condition) {
  if (!condition) return 1.0;

  // Simple heuristics based on Item type
  switch (condition->type()) {
    case Item::FUNC_ITEM: {
      Item_func *func = static_cast<Item_func *>(const_cast<Item *>(condition));
      switch (func->functype()) {
        case Item_func::EQ_FUNC:
          return 0.1;  // Equality: 10%
        case Item_func::LT_FUNC:
        case Item_func::LE_FUNC:
        case Item_func::GT_FUNC:
        case Item_func::GE_FUNC:
          return 0.33;  // Range: 33%
        case Item_func::BETWEEN:
          return 0.25;  // Between: 25%
        case Item_func::IN_FUNC:
          return 0.2;  // IN: 20%
        default:
          return 0.5;  // Unknown: 50%
      }
    } break;
    case Item::COND_ITEM: {
      Item_cond *cond = static_cast<Item_cond *>(const_cast<Item *>(condition));
      if (cond->functype() == Item_func::COND_AND_FUNC) {
        // AND: multiply selectivities
        double sel = 1.0;
        List_iterator<Item> li(*cond->argument_list());
        Item *arg;
        while ((arg = li++)) {
          sel *= SelectivityEstimator::estimate_selectivity_fallback(arg);
        }
        return sel;
      } else if (cond->functype() == Item_func::COND_OR_FUNC) {
        // OR: 1 - product of (1 - selectivity)
        double prob_none = 1.0;
        List_iterator<Item> li(*cond->argument_list());
        Item *arg;
        while ((arg = li++)) {
          double s = SelectivityEstimator::estimate_selectivity_fallback(arg);
          prob_none *= (1.0 - s);
        }
        return 1.0 - prob_none;
      }
      return 0.5;
    } break;
    default:
      return 0.5;  // Unknown: 50%
  }
}
/**
 * for fast estimation under without tree structure (only metadata) situations.
 */
double RpdCostEstimator::estimate_join_cost(ha_rows left_card, ha_rows right_card) {
  // Assume the right table is the Build Table and the left table is the Probe Table
  // In vectorized hash join, Build cost is typically slightly higher than single Probe cost
  double build_rows = std::min(static_cast<double>(left_card), static_cast<double>(right_card));
  double probe_rows = std::max(static_cast<double>(left_card), static_cast<double>(right_card));

  double build_cost = build_rows * m_memory_factor * HASH_BUILD_FACTOR;
  double probe_cost = probe_rows * m_cpu_factor * HASH_PROBE_FACTOR;

  double output_rows = (build_rows * probe_rows) * 0.1;
  double output_cost = output_rows * m_cpu_factor * 0.001;
  return build_cost + probe_cost + output_cost;
}

/**
 * for fast estimation under without tree structure (only metadata) situations.
 */
double RpdCostEstimator::estimate_scan_cost(ha_rows rows, size_t num_imcus) {
  // Consider IO overhead from IMCU count and CPU overhead from row processing
  return (num_imcus * m_io_factor) + (rows * m_cpu_factor * 0.001);
}

double RpdCostEstimator::estimate_scan_cost(THD *thd, Imcs::RpdTable *rpd_table, AccessPath *path) {
  ha_rows total_rows = rpd_table->meta().total_rows.load(std::memory_order_relaxed);
  size_t total_imcus = rpd_table->meta().total_imcus.load(std::memory_order_relaxed);

  double imcu_skip_ratio = 0.0;
  double row_selectivity = 1.0;

  Item *filter_cond = nullptr;
  if (path->type == AccessPath::FILTER) {
    filter_cond = path->filter().condition;
  }

  if (filter_cond) {
    ShannonBase::Imcs::ImcuPruningAnalyzer analyzer(rpd_table);
    imcu_skip_ratio = analyzer.estimate_skip_ratio(filter_cond);
    row_selectivity = analyzer.estimate_row_selectivity(filter_cond);
  }

  size_t effective_imcus = static_cast<size_t>(total_imcus * (1.0 - imcu_skip_ratio));
  ha_rows effective_rows = static_cast<ha_rows>(total_rows * row_selectivity);

  double imcu_read_cost = effective_imcus * RapidCostConstants::kImcuReadCost;
  size_t projected_cols = rpd_table->meta().fields.size();  // TODO: estimate_projected_columns(path, table);
  double decomp_cost = effective_rows * projected_cols * RapidCostConstants::kVectorDecompCostPerCell;

  double filter_cost = filter_cond ? effective_rows * RpdCostEstimator::VECTOR_CPU_FACTOR : 0.0;

  double null_check_cost = effective_rows * RapidCostConstants::kNullBitmapCheckCost;

  double total_cost = imcu_read_cost + decomp_cost + filter_cost + null_check_cost;

  return total_cost;
}
/**
 * Calculate the cost of executing a JOIN using the Rapid engine
 *
 * @param join: MySQL JOIN structure containing join order and predicates
 * @return: Estimated total cost of executing the join
 */
double RpdCostEstimator::cost(const JOIN *join) {
  if (!join || join->tables == 0) return 0.0;

  double total_cost{0.0}, cumulative_cardinality{1.0};
  // Traverse the join order determined by MySQL optimizer
  for (uint i = 0; i < join->tables; ++i) {
    POSITION *pos = &join->positions[i];
    JOIN_TAB *tab = pos->table;
    TABLE *table = tab->table();
    if (!table || !table->file) continue;

    // Get table statistics
    ha_rows base_rows = table->file->stats.records;
    if (base_rows == 0) base_rows = 1000;  // Default estimate for empty statistics

    // Get WHERE clause predicates pushed down to this table
    Item *table_condition = tab->condition();
    double selectivity{1.0};
    bool can_use_storage_index{false};
    if (table_condition) {
      selectivity =
          SelectivityEstimator::estimate_predicate_selectivity_internal(table, table_condition, &can_use_storage_index);
      selectivity = std::max(0.001, std::min(1.0, selectivity));
    }
    double scan_cost = calculate_scan_cost_detailed(table, base_rows, selectivity, can_use_storage_index, tab);
    double join_cost = 0.0;
    if (i > 0) {
      // Determine join type and algorithm
      double probe_rows = cumulative_cardinality;   // Left side (probe)
      double build_rows = base_rows * selectivity;  // Right side (build)

      // Check if this is a hash join, nested loop, etc.
      // For Rapid engine, we prefer hash joins
      join_cost = calculate_hash_join_cost_detailed(probe_rows, build_rows, tab);
    }

    total_cost += scan_cost + join_cost;

    // Update cumulative cardinality for next iteration
    cumulative_cardinality =
        (i == 0) ? (base_rows * selectivity)
                 : (std::max(1.0, cumulative_cardinality * base_rows * selectivity * 0.1)) /**10% selectivity*/;
  }
  return total_cost;
}

/**
 * detailed scan cost calculation.
  consider:
    - IMCU-level I/O cost with storage index pruning
    - Column projection cost based on read_set
    - Filter evaluation cost (vectorized)
    - Dictionary decoding cost for string columns
 */
double RpdCostEstimator::calculate_scan_cost_detailed(TABLE *table, ha_rows total_rows, double filter_selectivity,
                                                      bool can_use_storage_index, JOIN_TAB *tab) {
  double cost{0.0};
  // Get Rapid table metadata
  auto rpd_table = get_rapid_table(table);  // Helper function to get RpdTable
  if (!rpd_table) {
    // Fallback to basic calculation if Rapid metadata unavailable
    return calculate_vectorized_scan_cost(table, filter_selectivity, can_use_storage_index);
  }

  const auto &metadata = rpd_table->meta();
  size_t num_imcus = metadata.total_imcus.load();
  double imcu_cost = 0.0;
  if (can_use_storage_index) {
    // Storage index (min/max pruning) can skip IMCUs
    // Assume we can skip (1 - filter_selectivity) of IMCUs
    double imcus_to_scan = num_imcus * filter_selectivity;
    imcu_cost = imcus_to_scan * m_io_factor * 0.5;  // Reduced I/O per IMCU
  } else {
    // Must scan all IMCUs
    imcu_cost = num_imcus * m_io_factor;
  }

  cost += imcu_cost;

  double column_cost{0.0};
  size_t columns_to_read{0}, total_column_bytes{0};
  for (uint col_idx = 0; col_idx < table->s->fields; ++col_idx) {
    if (bitmap_is_set(table->read_set, col_idx)) {
      columns_to_read++;
      size_t col_bytes = table->field[col_idx]->pack_length() * total_rows;
      double compression_ratio{0.3};  // Assume 30% of original size
      col_bytes = static_cast<size_t>(col_bytes * compression_ratio);
      total_column_bytes += col_bytes;
    }
  }

  // Column scan cost = bytes to read * I/O factor + decompression CPU cost
  column_cost = (total_column_bytes / (1024.0 * 1024.0)) * m_io_factor +         // MB to read
                (total_column_bytes / (1024.0 * 1024.0)) * m_cpu_factor * 0.01;  // Decompression
  cost += column_cost;

  if (filter_selectivity < 1.0) {
    // CPU cost to evaluate predicates (vectorized)
    double filter_cost = total_rows * m_cpu_factor * VECTOR_CPU_FACTOR * columns_to_read;
    cost += filter_cost;
  }

  // Check if any string columns need dictionary decoding
  for (uint col_idx = 0; col_idx < table->s->fields; ++col_idx) {
    if (bitmap_is_set(table->read_set, col_idx)) {
      Field *field = table->field[col_idx];
      if (field->real_type() == MYSQL_TYPE_VARCHAR || field->real_type() == MYSQL_TYPE_STRING) {
        // Add dictionary lookup cost
        double decode_cost = total_rows * filter_selectivity * m_cpu_factor * 0.002;
        cost += decode_cost;
      }
    }
  }
  return cost;
}

double RpdCostEstimator::calculate_vectorized_scan_cost(TABLE *table, double selectivity, bool pruned) {
  if (!table || !table->file) return 0.0;

  // Get table statistics
  ha_rows total_rows = table->file->stats.records;
  if (total_rows == 0) total_rows = 1000;  // Default estimate for empty/missing statistics

  selectivity = std::max(0.0001, std::min(1.0, selectivity));
  size_t num_imcus{0}, rows_per_imcu{SHANNON_ROWS_IN_CHUNK};
  auto rpd_table = get_rapid_table(table);
  if (rpd_table) {
    const auto &metadata = rpd_table->meta();
    num_imcus = metadata.total_imcus.load(std::memory_order_relaxed);
    rows_per_imcu = metadata.rows_per_imcu;
  }
  if (num_imcus == 0) {
    num_imcus = (total_rows + rows_per_imcu - 1) / rows_per_imcu;
    if (num_imcus == 0) num_imcus = 1;
  }

  double total_cost{0.0};
  total_cost += calculate_imcu_io_cost(num_imcus, selectivity, pruned);
  total_cost += calculate_column_scan_cost(table, rpd_table, total_rows, selectivity, pruned);
  total_cost += calculate_decompression_cost(table, rpd_table, total_rows, selectivity, pruned);
  total_cost += calculate_dictionary_decoding_cost(table, rpd_table, total_rows, selectivity);
  total_cost += calculate_null_bitmap_cost(table, total_rows, selectivity);

  if (selectivity < 1.0) total_cost += calculate_filter_evaluation_cost(table, total_rows, selectivity);

  total_cost += calculate_memory_bandwidth_cost(table, total_rows, selectivity);

  // Vectorized execution is more efficient than row-based
  // Apply a discount factor based on SIMD capabilities
  total_cost *= get_vectorization_efficiency_factor();
  return total_cost;
}

/**
 * Calculate IMCU-level I/O cost
 *
 * Cost to read IMCU metadata and determine which IMCUs to scan
 */
double RpdCostEstimator::calculate_imcu_io_cost(size_t num_imcus, double selectivity, bool pruned) {
  double cost{0.0};
  if (pruned) {
    // With storage index pruning (min/max filtering):
    // - Read all IMCU metadata (cheap): num_imcus * small_overhead
    // - Only scan IMCUs that might contain matching rows

    double metadata_read_cost = num_imcus * m_io_factor * 0.001;  // Metadata is small

    // Assume storage index can skip (1 - selectivity) of IMCUs
    // Add some overhead for false positives (10%)
    double imcus_to_scan = num_imcus * selectivity * 1.1;
    double imcu_scan_cost = imcus_to_scan * m_io_factor * 0.1;
    cost = metadata_read_cost + imcu_scan_cost;
  } else {
    // Without pruning: must scan all IMCUs， Each IMCU read has I/O overhead
    cost = num_imcus * m_io_factor * 0.2;
  }
  return cost;
}

/**
 * Calculate column scanning cost
 *
 * Cost to read the actual column data from selected columns
 */
double RpdCostEstimator::calculate_column_scan_cost(TABLE *table, Imcs::RpdTable *rpd_table, ha_rows total_rows,
                                                    double selectivity, bool pruned) {
  double cost{0.0};
  // Count columns to read and estimate data volume
  size_t num_columns_to_read{0}, total_uncompressed_bytes{0};
  for (uint col_idx = 0; col_idx < table->s->fields; ++col_idx) {
    if (!bitmap_is_set(table->read_set, col_idx)) continue;  // Column not needed

    num_columns_to_read++;
    Field *field = table->field[col_idx];
    // Get column metadata if available
    double avg_row_size = field->pack_length();
    if (rpd_table && col_idx < rpd_table->meta().fields.size()) {
      const auto &field_meta = rpd_table->meta().fields[col_idx];
      if (field_meta.statistics) {  // Use actual statistics if available
        // For variable-length fields, use average length
        if (field->type() == MYSQL_TYPE_VARCHAR || field->type() == MYSQL_TYPE_STRING ||
            field->type() == MYSQL_TYPE_BLOB) {
          auto string_stats = field_meta.statistics->get_string_stats();
          if (string_stats) avg_row_size = string_stats->avg_length;
        }
      }
    }

    // Calculate uncompressed data volume for this column
    size_t column_bytes = static_cast<size_t>(avg_row_size * total_rows);
    total_uncompressed_bytes += column_bytes;
  }

  // Apply compression ratio
  // Typical columnar compression: 20-40% of original size
  double compression_ratio{0.3};  // Assume 30% after compression
  size_t compressed_bytes = static_cast<size_t>(total_uncompressed_bytes * compression_ratio);
  double compressed_mb = compressed_bytes / (1024.0 * 1024.0);

  // I/O cost: reading compressed column data
  // With storage index pruning, we read fewer rows
  double effective_mb = compressed_mb;
  if (pruned) effective_mb *= selectivity;

  cost = effective_mb * m_io_factor;
  // Add cost for seeking between columns (if not sequential)
  // Columnar layout may require multiple seeks
  if (num_columns_to_read > 1) {
    double seek_cost = num_columns_to_read * m_io_factor * 0.01;
    cost += seek_cost;
  }
  return cost;
}

/**
 * Calculate decompression cost
 *
 * CPU cost to decompress columnar data
 */
double RpdCostEstimator::calculate_decompression_cost(TABLE *table, Imcs::RpdTable *rpd_table, ha_rows total_rows,
                                                      double selectivity, bool pruned) {
  double cost{0.0};
  for (uint col_idx = 0; col_idx < table->s->fields; ++col_idx) {
    if (!bitmap_is_set(table->read_set, col_idx)) continue;

    // Determine compression algorithm if metadata available
    Compress::COMPRESS_LEVEL comp_level = Compress::COMPRESS_LEVEL::DEFAULT;
    if (rpd_table && col_idx < rpd_table->meta().fields.size()) {
      comp_level = rpd_table->meta().fields[col_idx].compression_level;
    }

    // Calculate decompression CPU cost based on compression level
    double decompress_cpu_factor = 0.01;  // Default
    switch (comp_level) {
      case Compress::COMPRESS_LEVEL::NONE:
        decompress_cpu_factor = 0.0;  // No decompression needed
        break;
      case Compress::COMPRESS_LEVEL::LZ4:
        decompress_cpu_factor = 0.005;  // LZ4-like: very fast
        break;
      case Compress::COMPRESS_LEVEL::DEFAULT:
        decompress_cpu_factor = 0.01;  // Balanced
        break;
      case Compress::COMPRESS_LEVEL::ZSTD:
        decompress_cpu_factor = 0.02;  // ZSTD/Snappy: more CPU
        break;
      default:
        decompress_cpu_factor = 0.01;
        break;
    }
    // Rows to decompress
    double rows_to_decompress = total_rows;
    if (pruned) {
      rows_to_decompress *= selectivity;
    }
    // Decompression cost = rows * CPU factor
    cost += rows_to_decompress * m_cpu_factor * decompress_cpu_factor;
  }
  return cost;
}

/**
 * Calculate dictionary decoding cost
 *
 * Cost to decode dictionary-encoded string columns
 */
double RpdCostEstimator::calculate_dictionary_decoding_cost(TABLE *table, Imcs::RpdTable *rpd_table, ha_rows total_rows,
                                                            double selectivity) {
  double cost{0.0};
  for (uint col_idx = 0; col_idx < table->s->fields; ++col_idx) {
    if (!bitmap_is_set(table->read_set, col_idx)) continue;

    Field *field = table->field[col_idx];
    // Check if column is dictionary-encoded
    bool is_dictionary_encoded = false;
    if (rpd_table && col_idx < rpd_table->meta().fields.size()) {
      const auto &field_meta = rpd_table->meta().fields[col_idx];
      // Dictionary encoding is typically used for strings (except ENUM)
      if (field_meta.dictionary && field->real_type() != MYSQL_TYPE_ENUM) {
        is_dictionary_encoded = true;
      }
    } else {
      // Heuristic: assume VARCHAR/STRING columns are dictionary-encoded
      if (field->real_type() == MYSQL_TYPE_VARCHAR || field->real_type() == MYSQL_TYPE_STRING) {
        is_dictionary_encoded = true;
      }
    }
    if (!is_dictionary_encoded) continue;

    // Dictionary decoding cost:
    // 1. Read dictionary ID (4 bytes)
    // 2. Lookup in dictionary (hash table or array)
    // 3. Copy string to output buffer

    double rows_to_decode = total_rows * selectivity;
    // Dictionary lookup is typically O(1) but has overhead
    // Cost depends on average string length
    double avg_string_length{20.0};  // Default estimate
    if (rpd_table && col_idx < rpd_table->meta().fields.size()) {
      const auto &field_meta = rpd_table->meta().fields[col_idx];
      if (field_meta.statistics) {
        auto string_stats = field_meta.statistics->get_string_stats();
        if (string_stats) {
          avg_string_length = string_stats->avg_length;
        }
      }
    }

    // Decoding cost = (lookup + memcpy) * rows
    double lookup_cost_per_row = m_cpu_factor * 0.001;  // Hash lookup
    double memcpy_cost_per_row = m_cpu_factor * 0.0001 * avg_string_length;
    cost += rows_to_decode * (lookup_cost_per_row + memcpy_cost_per_row);
  }
  return cost;
}

/**
 * Calculate NULL bitmap scanning cost
 *
 * Cost to check NULL bitmaps for each column
 */
double RpdCostEstimator::calculate_null_bitmap_cost(TABLE *table, ha_rows total_rows, double selectivity) {
  double cost{0.0};
  size_t nullable_columns{0};
  for (uint col_idx = 0; col_idx < table->s->fields; ++col_idx) {
    if (!bitmap_is_set(table->read_set, col_idx)) continue;

    Field *field = table->field[col_idx];
    if (field->is_nullable()) {
      nullable_columns++;
    }
  }

  if (nullable_columns == 0) {
    return 0.0;
  }

  // NULL bitmap checking is very fast (bitwise operations)
  // Cost = rows * columns * bit_check_cost
  double rows_to_check = total_rows * selectivity;
  // Vectorized NULL checking is extremely efficient
  cost = rows_to_check * nullable_columns * m_cpu_factor * 0.0001;
  return cost;
}

/**
 * Calculate filter evaluation cost
 *
 * Cost to evaluate predicates in vectorized fashion
 */
double RpdCostEstimator::calculate_filter_evaluation_cost(TABLE *table, ha_rows total_rows, double selectivity) {
  // Filter evaluation cost depends on:
  // 1. Number of columns involved in predicates
  // 2. Complexity of predicates
  // 3. Vectorization efficiency

  // Count columns in read_set as proxy for predicate complexity
  size_t columns_in_predicates = 0;
  for (uint col_idx = 0; col_idx < table->s->fields; ++col_idx) {
    if (bitmap_is_set(table->read_set, col_idx)) {
      columns_in_predicates++;
    }
  }

  // Vectorized predicate evaluation is very efficient
  // Process in batches (e.g., 1024 rows at a time)
  double cost = total_rows * columns_in_predicates * m_cpu_factor * VECTOR_CPU_FACTOR;
  return cost;
}

/**
 * Calculate memory bandwidth cost
 *
 * Cost for memory access patterns in columnar scans
 */
double RpdCostEstimator::calculate_memory_bandwidth_cost(TABLE *table, ha_rows total_rows, double selectivity) {
  // Columnar access has good cache locality
  // Memory bandwidth cost is lower than row-based access

  size_t total_bytes_accessed{0};
  for (uint col_idx = 0; col_idx < table->s->fields; ++col_idx) {
    if (!bitmap_is_set(table->read_set, col_idx)) continue;

    Field *field = table->field[col_idx];
    size_t bytes_per_row = field->pack_length();
    total_bytes_accessed += bytes_per_row * total_rows;
  }

  // Apply selectivity
  total_bytes_accessed = static_cast<size_t>(total_bytes_accessed * selectivity);
  // Memory bandwidth cost depends on access patterns
  // Columnar: sequential access = good cache performance
  double mb_accessed = total_bytes_accessed / (1024.0 * 1024.0);
  // Lower memory factor for sequential columnar access
  double memory_cost = mb_accessed * m_memory_factor * 0.1;
  return memory_cost;
}

/**
 * Get vectorization efficiency factor
 *
 * Returns a discount factor based on SIMD capabilities
 */
double RpdCostEstimator::get_vectorization_efficiency_factor() {
  double factor{1.0};
#ifdef SHANNON_X86_PLATFORM
  bool has_avx512 = __builtin_cpu_supports("avx512f");
  bool has_avx2 = __builtin_cpu_supports("avx2");

  if (has_avx512) {
    factor = 0.6;  // 40% speedup from AVX-512
  } else if (has_avx2) {
    factor = 0.75;  // 25% speedup from AVX2
  } else {
    factor = 0.9;  // 10% speedup from SSE
  }
#elif defined(SHANNON_ARM_PLATFORM)
#ifdef SHANNON_ARM_VECT_SUPPORTED
  factor = 0.75;                  // NEON vectorization
#else
  factor = 0.95;                  // Limited vectorization
#endif
#else
  factor = 0.95;  // Generic platform
#endif
  return factor;
}

/**
 * Detailed hash join cost calculation
 */
double RpdCostEstimator::calculate_hash_join_cost_detailed(double probe_rows, double build_rows, JOIN_TAB *tab) {
  // A. Hash table build cost
  // Cost to build hash table from smaller relation
  // Includes: hashing + memory allocation + insertion
  double build_cost = build_rows * m_memory_factor * HASH_BUILD_FACTOR;

  // Hash table memory overhead
  double hash_table_memory = build_rows * 16;  // ~16 bytes per entry
  double memory_overhead_cost = (hash_table_memory / (1024.0 * 1024.0)) * m_memory_factor * 0.01;

  build_cost += memory_overhead_cost;

  // B. Hash probe cost
  // Cost to probe hash table with larger relation
  // Includes: hashing + lookups + output materialization
  double probe_cost = probe_rows * m_cpu_factor * HASH_PROBE_FACTOR;

  // C. Output materialization cost
  // Estimated join output (assuming 10% join selectivity)
  double output_rows = probe_rows * build_rows * 0.1;
  double output_cost = output_rows * m_cpu_factor * 0.001;

  // D. Join condition evaluation cost
  // If there are non-equijoin conditions, add evaluation cost
  Item *join_cond = tab->join_cond();
  if (join_cond) {
    // Additional CPU cost for complex join predicates
    double join_pred_cost = output_rows * m_cpu_factor * 0.005;
    probe_cost += join_pred_cost;
  }
  return build_cost + probe_cost + output_cost;
}

/**
 * Helper function to get RpdTable from TABLE
 */
Imcs::RpdTable *RpdCostEstimator::get_rapid_table(TABLE *table) {
  if (!table || !table->file) return nullptr;
  auto share = ShannonBase::shannon_loaded_tables->get(table->s->db.str, table->s->table_name.str);
  if (!share) return nullptr;
  Imcs::RpdTable *rpd_table = (share->is_partitioned)
                                  ? ShannonBase::Imcs::Imcs::instance()->get_rpd_parttable(share->m_tableid)
                                  : ShannonBase::Imcs::Imcs::instance()->get_rpd_table(share->m_tableid);
  return rpd_table;
}

double PredicateAnalyzer::analyze(const Item *condition, bool *can_use_si) {
  if (!condition) return 1.0;
  bool can_prune = false;
  double selectivity = analyze_recursive(const_cast<Item *>(condition), &can_prune);
  if (can_use_si) {
    *can_use_si = can_prune;
  }
  return selectivity;
}

double PredicateAnalyzer::analyze_recursive(Item *item, bool *can_prune) {
  if (!item) return 1.0;
  switch (item->type()) {
    case Item::FUNC_ITEM:
      return analyze_function(static_cast<Item_func *>(item), can_prune);
    case Item::COND_ITEM:
      return analyze_condition(static_cast<Item_cond *>(item), can_prune);
    default:
      return 0.5;
  }
}

double PredicateAnalyzer::analyze_function(Item_func *func, bool *can_prune) {
  // Check if this is a simple predicate on a single column
  if (func->argument_count() != 2) return 0.5;

  Item *left = func->arguments()[0];
  Item *right = func->arguments()[1];
  // Check if one side is a field and the other is a constant
  Item_field *field_item = nullptr;
  Item *value_item = nullptr;
  if (left->type() == Item::FIELD_ITEM && right->const_item()) {
    field_item = static_cast<Item_field *>(left);
    value_item = right;
  } else if (right->type() == Item::FIELD_ITEM && left->const_item()) {
    field_item = static_cast<Item_field *>(right);
    value_item = left;
  }

  if (!field_item) return 0.5;

  // Get field index
  uint field_idx = field_item->field->field_index();
  // Get column statistics
  const auto &metadata = m_rpd_table->meta();
  if (field_idx >= metadata.fields.size()) return 0.5;
  auto &field_meta = metadata.fields[field_idx];
  auto stats = field_meta.statistics.get();
  if (!stats) {
    // No statistics available
    *can_prune = false;
    return estimate_without_stats(func);
  }

  // Extract value
  double value = extract_numeric_value(value_item);
  // Estimate selectivity based on operator and statistics
  double selectivity = 0.5;
  switch (func->functype()) {
    case Item_func::EQ_FUNC:
      selectivity = stats->estimate_equality_selectivity(value);
      *can_prune = true;  // Can use min/max pruning
      break;
    case Item_func::LT_FUNC:
      selectivity = stats->estimate_range_selectivity(stats->get_basic_stats().min_value, value);
      *can_prune = true;
      break;
    case Item_func::LE_FUNC:
      selectivity = stats->estimate_range_selectivity(stats->get_basic_stats().min_value, value);
      *can_prune = true;
      break;
    case Item_func::GT_FUNC:
      selectivity = stats->estimate_range_selectivity(value, stats->get_basic_stats().max_value);
      *can_prune = true;
      break;
    case Item_func::GE_FUNC:
      selectivity = stats->estimate_range_selectivity(value, stats->get_basic_stats().max_value);
      *can_prune = true;
      break;
    case Item_func::BETWEEN:
      if (func->argument_count() == 3) {
        double lower = extract_numeric_value(func->arguments()[1]);
        double upper = extract_numeric_value(func->arguments()[2]);
        selectivity = stats->estimate_range_selectivity(lower, upper);
        *can_prune = true;
      }
      break;
    default:
      *can_prune = false;
      selectivity = 0.5;
  }
  return selectivity;
}

double PredicateAnalyzer::analyze_condition(Item_cond *cond, bool *can_prune) {
  List_iterator<Item> li(*cond->argument_list());
  if (cond->functype() == Item_func::COND_AND_FUNC) {
    // AND: multiply selectivities
    double sel = 1.0;
    bool all_can_prune = true;
    Item *arg;
    while ((arg = li++)) {
      bool child_can_prune = false;
      double child_sel = analyze_recursive(arg, &child_can_prune);
      sel *= child_sel;
      all_can_prune &= child_can_prune;
    }

    *can_prune = all_can_prune;
    return sel;
  } else if (cond->functype() == Item_func::COND_OR_FUNC) {
    // OR: 1 - product of (1 - selectivity)
    double prob_none = 1.0;
    Item *arg;
    while ((arg = li++)) {
      bool child_can_prune = false;
      double child_sel = analyze_recursive(arg, &child_can_prune);
      prob_none *= (1.0 - child_sel);
    }
    *can_prune = false;  // OR predicates typically can't use storage index
    return 1.0 - prob_none;
  }
  return 0.5;
}

double PredicateAnalyzer::estimate_without_stats(Item_func *func) {
  switch (func->functype()) {
    case Item_func::EQ_FUNC:
      return 0.1;
    case Item_func::LT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GT_FUNC:
    case Item_func::GE_FUNC:
      return 0.33;
    case Item_func::BETWEEN:
      return 0.25;
    default:
      return 0.5;
  }
}

double PredicateAnalyzer::extract_numeric_value(Item *item) {
  if (!item || !item->const_item()) return 0.0;
  switch (item->result_type()) {
    case INT_RESULT:
      return static_cast<double>(item->val_int());
    case REAL_RESULT:
      return item->val_real();
    case DECIMAL_RESULT:
      return item->val_real();
    default:
      return 0.0;
  }
}

/**
 * calc the cost of `query_plan`, the total query tree.
 */
double RpdCostEstimator::cost(const Plan &plan) {
  if (!plan) return 0.0;

  double total_cost{0.0};
  for (const auto &child : plan->children) {
    total_cost += cost(child);
  }

  double node_self_cost{0.0};
  switch (plan->type()) {
    case PlanNode::Type::SCAN: {
      auto *scan = static_cast<const ScanTable *>(plan.get());
      size_t num_imcus = scan->rpd_table ? scan->rpd_table->meta().total_imcus.load() : 0;
      // Base cost = (IMCU count * IO factor) + (estimated rows * CPU factor)
      node_self_cost = (num_imcus * m_io_factor) + (scan->estimated_rows * m_cpu_factor * 0.001);

      // If Storage Index pruning is enabled (set in prune.cpp)
      // Assume only a small portion of IMCUs need to be scanned
      if (scan->use_storage_index) {
        node_self_cost *= 0.2;  // Assume SI filtering is effective, retaining only 20% of the cost
      }

      if (scan->limit || scan->order) {
        // more a bit of  CPU cost and heap management cost
        node_self_cost += scan->estimated_rows * m_cpu_factor * 0.005;
      }
    } break;
    case PlanNode::Type::HASH_JOIN: {
      // Costs of left and right subtrees are already included in total_cost
      // Calculate the matching cost of Hash Join itself here
      double probe_rows = plan->children[0]->estimated_rows;
      double build_rows = plan->children[1]->estimated_rows;

      // Hash build cost (usually on the right) + hash probe cost (left)
      double build_cost = build_rows * m_memory_factor * 0.05;
      double probe_cost = probe_rows * m_cpu_factor * 0.01;
      node_self_cost = build_cost + probe_cost;
    } break;
    case PlanNode::Type::FILTER: {
      // CPU consumption of vectorized Filter is very low
      node_self_cost = plan->children[0]->estimated_rows * m_cpu_factor * 0.005;
    } break;
    case PlanNode::Type::LOCAL_AGGREGATE:
    case PlanNode::Type::GLOBAL_AGGREGATE: {
      // Aggregation overhead: depends on input row count
      node_self_cost = plan->children[0]->estimated_rows * m_cpu_factor * 0.02;
    } break;
    case PlanNode::Type::TOP_N: {
      if (plan->children.empty() || !plan->children[0]) {
        node_self_cost = 0.0;
        break;
      }

      auto *top_n = static_cast<const TopN *>(plan.get());
      double input_rows = plan->children[0]->estimated_rows;
      double limit_rows = static_cast<double>(top_n->limit);
      if (input_rows <= 0) {
        node_self_cost = 0.0;
        break;
      }

      if (limit_rows > 0 && limit_rows < input_rows) {
        // case A: heap sort (Top-K) - O(N log K)
        node_self_cost = input_rows * std::log2(limit_rows + 1) * m_cpu_factor * 0.01;
      } else {
        // case B: full sort - O(N log N)
        node_self_cost = input_rows * std::log2(input_rows + 1) * m_cpu_factor * 0.02;
      }
    } break;
    case PlanNode::Type::LIMIT: {
      // Mainly sorting or truncation of result sets
      node_self_cost = m_cpu_factor * 0.001;
    } break;
    case PlanNode::Type::MYSQL_NATIVE: {
      auto *mysql = static_cast<const MySQLNative *>(plan.get());
      // fallback to MySQL，then using MySQL original cost
      node_self_cost = mysql->original_path->cost();
    } break;
    default:
      node_self_cost = m_cpu_factor;
      break;
  }
  return total_cost + node_self_cost;
}
}  // namespace Optimizer
}  // namespace ShannonBase