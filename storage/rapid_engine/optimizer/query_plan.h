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
#ifndef __SHANNONBASE_QUERY_PLAN_H__
#define __SHANNONBASE_QUERY_PLAN_H__

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/my_base.h"  // ha_rows
#include "storage/rapid_engine/include/rapid_types.h"

class THD;
class Query_expression;
class JOIN;
class AccessPath;
class Item_func;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
class Predicate;
}  // namespace Imcs

namespace Optimizer {
class PlanNode : public MemoryObject {
 public:
  // Type of the plan node.
  enum class Type : uint8_t {
    SCAN,
    HASH_JOIN,
    NESTED_LOOP_JOIN,
    LOCAL_AGGREGATE,
    GLOBAL_AGGREGATE,
    FILTER,
    PROJECTION,
    TOP_N,
    LIMIT,
    ZERO_ROWS
  };

  virtual ~PlanNode() = default;
  // Get the type of the plan node.
  virtual Type type() const = 0;
  // Generate a string representation of the plan node with indentation.
  virtual std::string ToString(int indent = 0) const = 0;

  // child nodes.
  std::vector<std::unique_ptr<PlanNode>> children;
  // estimated cost.
  double cost{0.0};
  // estimated output rows.
  ha_rows estimated_rows{0};
  // can be vectorized or not.
  bool vectorized{true};
};

// Alias for a unique pointer to a PlanNode.
using Plan = std::unique_ptr<PlanNode>;

// Various plan node types.
// ScanTable represents a table scan operation.
class ScanTable : public PlanNode {
 public:
  TABLE *source_table{nullptr};
  Imcs::RpdTable *rpd_table;

  // Indicates whether storage index pruning is used.
  bool use_storage_index{false};
  // Optional predicate for pruning.
  std::unique_ptr<Imcs::Predicate> prune_predicate;

  // list of column indices to read. Empty means read all columns
  std::vector<uint32_t> projected_columns;

  Type type() const override { return Type::SCAN; }
  enum class ScanType : uint8_t {
    FULL_TABLE_SCAN,
    INDEX_SCAN,
    COVERING_INDEX_SCAN,
    EQ_REF_SCAN
  } scan_type{ScanType::FULL_TABLE_SCAN};

  std::string ToString(int indent) const override;

  // Helper: check if a column should be read
  inline bool should_read_column(uint32_t col_idx) const {
    return projected_columns.empty() ||
           std::find(projected_columns.begin(), projected_columns.end(), col_idx) != projected_columns.end();
  }
};

// Filter represents a filtering operation.
class Filter : public PlanNode {
 public:
  Item *condition{nullptr};
  bool materialize_subqueries{false};
  std::unique_ptr<ShannonBase::Imcs::Predicate> predict{nullptr};

  Type type() const override { return Type::FILTER; }
  std::string ToString(int indent) const override;
};

// Hash join represents a hash join operation.
class HashJoin : public PlanNode {
 public:
  std::vector<Item *> join_conditions;
  bool allow_spill{false};
  Type type() const override { return Type::HASH_JOIN; }
  std::string ToString(int indent) const override;
};

// Nested loop join represents a nested loop join operation.
class NestLoopJoin : public PlanNode {
 public:
  std::vector<Item *> join_conditions;
  bool allow_spill{false};
  Type type() const override { return Type::NESTED_LOOP_JOIN; }
  std::string ToString(int indent) const override;
};

// LocalAgg represents a local aggregation operation.
class LocalAgg : public PlanNode {
 public:
  std::vector<Item *> group_by;
  std::vector<Item *> order_by;
  std::vector<Item_func *> aggregates;
  Type type() const override { return Type::LOCAL_AGGREGATE; }
  std::string ToString(int indent) const override;
};

// GlobalAgg represnets a global aggregation operation.
class GlobalAgg : public PlanNode {
 public:
  Type type() const override { return Type::GLOBAL_AGGREGATE; }
  std::string ToString(int indent) const override;
};

// TopN represents a top-N operation.
class TopN : public PlanNode {
 public:
  ORDER *order{nullptr};
  ha_rows limit{0};
  Type type() const override { return Type::TOP_N; }
  std::string ToString(int indent) const override;
};

// ZeroRows represents an operation that produces zero rows.
class ZeroRows : public PlanNode {
 public:
  Type type() const override { return Type::ZERO_ROWS; }
  std::string ToString(int indent) const override;
};

// Limit represents a limit operation.
class Limit : public PlanNode {
 public:
  ha_rows limit{0};
  ha_rows offset{0};
  bool count_all_rows;
  bool reject_multiple_rows;

  Type type() const override { return Type::LIMIT; }
  std::string ToString(int indent) const override;
};

// The overall query plan.
class QueryPlan : public MemoryObject {
 public:
  QueryPlan() = default;
  ~QueryPlan() = default;

  std::string Explain() const;
  std::string ToString() const { return root ? root->ToString() : "EMPTY PLAN"; }

  AccessPath *BuildAccessPath(THD *thd) const;

  Plan root;
  double total_cost{0.0};
  std::string plan_id;
};

void WalkPlan(PlanNode *node, std::function<void(PlanNode *)> callback);

}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_OPTIMIZER_H__