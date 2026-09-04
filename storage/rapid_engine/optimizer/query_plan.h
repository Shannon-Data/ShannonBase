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

   Copyright (c) 2023, 2026 Shannon Data AI and/or its affiliates.

   The fundmental code for imcs optimizer.
*/
#ifndef __SHANNONBASE_QUERY_PLAN_H__
#define __SHANNONBASE_QUERY_PLAN_H__

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/my_base.h"                     // ha_rows
#include "sql/join_optimizer/overflow_bitset.h"  // OverflowBitset
#include "sql/join_type.h"                       // JoinType
#include "sql/olap.h"                            // olap_type
#include "storage/rapid_engine/include/rapid_types.h"

class THD;
class Query_expression;
class JOIN;
class AccessPath;
class Item_func;
class ORDER;
class Filesort;
class JoinPredicate;
namespace ShannonBase {
enum class AggregateStrategy : uint8_t;

namespace Imcs {
class RpdTable;
class Predicate;
}  // namespace Imcs

namespace Optimizer {
class PlanNode : public MemoryObject {
 public:
  // Type of the plan node.
  enum class Type : uint8_t {
    SCAN = 0,
    HASH_JOIN,
    NESTED_LOOP_JOIN,
    LOCAL_AGGREGATE,
    GLOBAL_AGGREGATE,
    FILTER,
    PROJECTION,
    SORT,
    TOP_N,
    LIMIT,
    ZERO_ROWS,
    UNION,
    INTERSECT,
    EXCEPT,
    MATERIALIZE_CTE,
    MATERIALIZE_DERIVED,
    WINDOW,
    MYSQL_NATIVE
  };

  virtual ~PlanNode() = default;
  // Get the type of the plan node.
  virtual Type type() const = 0;
  // Convert the plan node to an AccessPath for execution.
  virtual AccessPath *ToAccessPath(THD *thd) = 0;
  // Generate a string representation of the plan node with indentation.
  virtual std::string ToString(int indent = 0) const = 0;

  AccessPath *original_path{nullptr};  // to save the original MySQL AccessPath.
  // child nodes.
  std::vector<std::unique_ptr<PlanNode>> children;
  // estimated cost.
  double cost{0.0};
  // estimated output rows.
  ha_rows estimated_rows{0};
  // can be vectorized or not.
  bool vectorized{true};

 protected:
  // Returns true if all provided child AccessPaths are vectorized.
  // Use this to propagate vectorized flag from children.
  static bool AllChildrenVectorized(std::initializer_list<const AccessPath *> paths);
};

// Alias for a unique pointer to a PlanNode.
using Plan = std::unique_ptr<PlanNode>;

// Various plan node types.
// ScanTable represents a table scan operation.
class ScanTable : public PlanNode {
 public:
  ScanTable() = default;
  ~ScanTable() override = default;

  TABLE *source_table{nullptr};
  Imcs::RpdTable *rpd_table{nullptr};

  // Indicates whether storage index pruning is used.
  bool use_storage_index{false};

  // Optional predicate for pruning.
  std::unique_ptr<Imcs::Predicate> prune_predicate{nullptr};

  // list of column indices to read. Empty means read all columns
  // Then during execution, only read these columns from CUs
  std::vector<uint32_t> projected_columns;

  // LIMIT and OFFSET for the scan
  ha_rows limit{HA_POS_ERROR};  // HA_POS_ERROR no limit
  ha_rows offset{0};

  // ORDER BY pushdown（TopN optimization）
  ORDER *order{nullptr};

  // The source AccessPath was an ordered index scan used by MySQL to satisfy
  // an ORDER BY. Rapid still converts it to its vectorized scan under forced
  // secondary execution, but LIMIT must remain above that scan.
  bool has_required_order{false};

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::SCAN; }
  enum class ScanType : uint8_t {
    FULL_TABLE_SCAN = 0,
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
  Filter() = default;
  ~Filter() override = default;

  // the source condition for filtering
  Item *condition{nullptr};
  // predicate condition of converted `condition`, which is used for IMCS converted from `condtion`.
  std::unique_ptr<ShannonBase::Imcs::Predicate> predict{nullptr};

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::FILTER; }
  std::string ToString(int indent) const override;
};

// Whether one side (child) of an equi-join is proven to contribute at most one matching row per
// join-key value, e.g. because the join predicate binds a UNIQUE NOT NULL index of that child's
// table in full. A child proven kAtMostOne cannot fan out rows contributed by the other side —
// the precondition rewrites like eager aggregation pushdown or join reordering need before they
// may reorder or push an operation through this join without changing its result cardinality.
// kUnknown is the safe default: a rule must not assume "at most one" unless this says so.
enum class JoinMultiplicity : uint8_t {
  kUnknown = 0,  // Not proven either way; treat this side as possibly fanning out.
  kAtMostOne,    // Proven: at most one row of this side per distinct join-key value.
};

// Hash join represents a hash join operation.
class HashJoin : public PlanNode {
 public:
  HashJoin() = default;
  ~HashJoin() override = default;

  // the source join conditions from mysql `JOIN`.
  std::vector<Item *> join_conditions;
  bool allow_spill{false};

  // A sorted hash join consumes the probe side in input order and emits all
  // matches for one probe row before advancing to the next one.
  bool preserves_probe_order{false};

  // The SQL join type this HashJoin implements, derived from the source RelationalExpression by
  // Optimizer::translate_access_path. Lets rules classify the join (INNER vs OUTER/SEMI/ANTI/
  // FULL_OUTER) without reaching back into original_path.
  JoinType join_type{JoinType::INNER};

  // child_multiplicity[i] describes children[i]: whether it is proven to contribute at most one
  // row per join-key match against the other child (see JoinMultiplicity above). Only proven for
  // a child that is directly a ScanTable leaf bound by a UNIQUE NOT NULL index on the equi-join
  // columns (see Utils::prove_at_most_one); anything else (a further join, aggregate, etc. as a
  // child) is left kUnknown rather than attempting transitive proof.
  std::array<JoinMultiplicity, 2> child_multiplicity{JoinMultiplicity::kUnknown, JoinMultiplicity::kUnknown};

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::HASH_JOIN; }
  std::string ToString(int indent) const override;
};

// Nested loop join represents a nested loop join operation.
class NestLoopJoin : public PlanNode {
 public:
  NestLoopJoin() = default;
  ~NestLoopJoin() override = default;

  // the source join conditions in `Item` format from mysql `JOIN`.
  const JoinPredicate *source_join_predicate{nullptr};
  OverflowBitset equijoin_predicates;
  std::vector<Item *> join_conditions;
  bool pfs_batch_mode{false};
  bool already_expanded_predicates{false};

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::NESTED_LOOP_JOIN; }
  std::string ToString(int indent) const override;
};

// LocalAgg represents a local aggregation operation.
class LocalAgg : public PlanNode {
 public:
  LocalAgg();
  ~LocalAgg() override = default;

  std::vector<Item *> group_by;  // empty means is GlobalAgg.
  std::vector<Item *> order_by;
  std::vector<Item_func *> aggregates;
  olap_type olap{olap_type::UNSPECIFIED_OLAP_TYPE};
  JOIN *join{nullptr};
  AggregateStrategy strategy;
  // If a full-row grouping sort was removed in favor of hash aggregation,
  // order the much smaller set of hash groups before returning them.
  ORDER *hash_output_order{nullptr};
  bool streaming_over_sorted_hash{false};

  bool is_global{false};

  // Non-null only for a legacy-optimizer unsorted GROUP BY: the original
  // TEMPTABLE_AGGREGATE AccessPath. When set, ToAccessPath() emits a
  // (vectorized) TEMPTABLE_AGGREGATE that keeps that plan's temp table and
  // downstream table scan, so the legacy Item/Field bindings stay valid; the
  // Rapid iterator just fills the temp table with SIMD-aggregated group rows.
  const AccessPath *temptable_src{nullptr};

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::LOCAL_AGGREGATE; }
  std::string ToString(int indent) const override;
};

// TopN represents a top-N operation.
class TopN : public PlanNode {
 public:
  TopN() = default;
  ~TopN() override = default;

  // the source condition item from mysql `order by ... limit`.
  Filesort *filesort{nullptr};
  ORDER *order{nullptr};
  ha_rows limit{HA_POS_ERROR};  // HA_POS_ERROR rep: no limitation};
  ha_rows offset{0};

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::TOP_N; }
  std::string ToString(int indent) const override;
};

class WindowFunc : public PlanNode {
 public:
  WindowFunc() = default;
  ~WindowFunc() override = default;

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::WINDOW; }
  std::string ToString(int indent) const override;
};

// Sort represents a `oder by` operation.
class Sort : public PlanNode {
 public:
  Sort() = default;
  ~Sort() override = default;

  Filesort *filesort{nullptr};
  ORDER *order{nullptr};

  ha_rows limit{HA_POS_ERROR};
  bool remove_duplicates{false};
  bool unwrap_rollup{false};
  bool force_sort_rowids{false};
  table_map tables_to_get_rowid_for;

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::SORT; }
  std::string ToString(int indent) const override;
};

// ZeroRows represents an operation that produces zero row or one row.
class ZeroRows : public PlanNode {
 public:
  ZeroRows() = default;
  ~ZeroRows() override = default;

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::ZERO_ROWS; }
  std::string ToString(int indent) const override;

  // estimated rows: 0 or 1(const table).
  ha_rows rows_returned{0};
};

// Limit represents a limit operation.
class Limit : public PlanNode {
 public:
  Limit() = default;
  ~Limit() override = default;

  ha_rows limit{0};
  ha_rows offset{0};
  bool count_all_rows{false};
  bool reject_multiple_rows{false};
  ha_rows *send_records_override{nullptr};

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::LIMIT; }
  std::string ToString(int indent) const override;
};

class Union : public PlanNode {
 public:
  bool is_distinct{false};
  Type type() const override { return Type::UNION; }
  AccessPath *ToAccessPath(THD *thd) override;
  std::string ToString(int indent) const override;
};

class MaterializeCTE : public PlanNode {
 public:
  MaterializeCTE() = default;
  ~MaterializeCTE() override = default;

  std::string cte_name;
  TABLE *tmp_table{nullptr};
  std::vector<std::unique_ptr<PlanNode>> inner_plans;

  bool is_recursive{false};     // is recurisve CTE
  ha_rows limit{HA_POS_ERROR};  // LIMIT push down

  Type type() const override { return Type::MATERIALIZE_CTE; }

  AccessPath *ToAccessPath(THD *thd) override;
  std::string ToString(int indent) const override;
};

class MaterializeDerived : public PlanNode {
 public:
  MaterializeDerived() = default;
  ~MaterializeDerived() override = default;

  TABLE *tmp_table{nullptr};
  std::vector<std::unique_ptr<PlanNode>> inner_plans;  // UNION

  bool has_union{false};          // is UNION
  bool is_union_distinct{false};  // UNION vs UNION ALL

  Type type() const override { return Type::MATERIALIZE_DERIVED; }

  AccessPath *ToAccessPath(THD *thd) override;
  std::string ToString(int indent) const override;
};

/**
 * @brief The MySQLNative node is used to encapsulate original MySQL AccessPaths that are not currently handled by the
 * Rapid engine.
 */
class MySQLNative : public PlanNode {
 public:
  MySQLNative() = default;
  ~MySQLNative() override = default;

  AccessPath *ToAccessPath(THD *thd) override;

  Type type() const override { return Type::MYSQL_NATIVE; }
  std::string ToString(int indent) const override;
};

// The overall query plan.
class QueryPlan : public MemoryObject {
 public:
  QueryPlan() = default;
  ~QueryPlan() = default;

  std::string Explain() const;
  std::string ToString() const { return root ? root->ToString() : "EMPTY PLAN"; }
  AccessPath *BuildAccessPath(THD *thd) const { return root ? root->ToAccessPath(thd) : nullptr; }

  Plan root;
  double total_cost{0.0};
  std::string plan_id;
};

void WalkPlan(PlanNode *node, std::function<void(PlanNode *)> callback);

}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_OPTIMIZER_H__
