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
#include "sql/join_optimizer/access_path.h"  //AccessPath
#include "storage/rapid_engine/imcs/table.h"

namespace ShannonBase {
namespace Optimizer {
std::unordered_map<CostEstimator::Type, CostEstimator *> CostModelServer::instances_;
std::mutex CostModelServer::instance_mutex_;

static constexpr double DEFAULT_CPU_FACTOR = 0.01;
static constexpr double DEFAULT_MEM_FACTOR = 0.25;
static constexpr double DEFAULT_IO_FACTOR = 1.0;

CostEstimator *CostModelServer::Instance(CostEstimator::Type type) {
  std::lock_guard<std::mutex> lock(instance_mutex_);
  auto it = instances_.find(type);
  if (it != instances_.end()) return it->second;

  if (type == CostEstimator::Type::RPD_ENG) {
    long num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cpus <= 0) num_cpus = 1;

    double cpu_factor = DEFAULT_CPU_FACTOR;
    double mem_factor = DEFAULT_MEM_FACTOR;
    double io_factor = DEFAULT_IO_FACTOR;
#ifdef SHANNON_X86_PLATFORM
    bool has_avx512 = __builtin_cpu_supports("avx512f");
    bool has_avx2 = __builtin_cpu_supports("avx2");

    if (has_avx512) {
      cpu_factor *= 0.5;
    } else if (has_avx2) {
      cpu_factor *= 0.8;
    } else {
      cpu_factor *= 2.0;
    }
#elif defined(SHANNON_ARM_PLATFORM)
#ifdef SHANNON_ARM_VECT_SUPPORTED
    cpu_factor *= 0.8;  // NEON support
#else
    cpu_factor *= 2.0;  // No vector support
#endif
#else
    cpu_factor *= 2.0;
#endif
    if (num_cpus >= 32) cpu_factor *= 0.7;
    auto *estimator = new RpdCostEstimator(cpu_factor, mem_factor, io_factor);
    instances_[type] = estimator;
    return estimator;
  }

  // TODO: the other types Estimator...
  return nullptr;
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

double RpdCostEstimator::cost(const Plan &plan) {
  if (!plan) return 0.0;

  double total_cost = 0.0;
  // 1. Recursively calculate child node costs (bottom-up)
  for (const auto &child : plan->children) {
    total_cost += cost(child);
  }

  // 2. Calculate current node's operator overhead based on its type
  double node_self_cost = 0.0;
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