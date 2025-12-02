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
#include "storage/rapid_engine/include/rapid_object.h"

class THD;
class Query_expression;
class JOIN;
class AccessPath;
class Item_func;
namespace ShannonBase {
namespace Imcs {
class RpdTable;
}
namespace Optimizer {
class PlanNode : public MemoryObject {
 public:
  enum class Type : uint8_t {
    SCAN,
    HASH_JOIN,
    LOCAL_AGGREGATE,
    GLOBAL_AGGREGATE,
    FILTER,
    PROJECTION,
    TOP_N,
    LIMIT,
    ZERO_ROWS
  };

  virtual ~PlanNode() = default;
  virtual Type type() const = 0;
  virtual std::string ToString(int indent = 0) const = 0;

  std::vector<std::unique_ptr<PlanNode>> children;
  double cost{0.0};
  ha_rows estimated_rows{0};
  bool vectorized{true};
};

using PlanPtr = std::unique_ptr<PlanNode>;
class ScanTable : public PlanNode {
 public:
  Imcs::RpdTable *rpd_table;
  bool use_storage_index{false};
  Type type() const override { return Type::SCAN; }
  std::string ToString(int indent) const override;
};

class HashJoin : public PlanNode {
 public:
  std::vector<Item *> join_conditions;
  bool allow_spill{false};
  Type type() const override { return Type::HASH_JOIN; }
  std::string ToString(int indent) const override;
};

class LocalAgg : public PlanNode {
 public:
  std::vector<Item *> group_by;
  std::vector<Item_func *> aggregates;
  Type type() const override { return Type::LOCAL_AGGREGATE; }
  std::string ToString(int indent) const override;
};

class GlobalAgg : public PlanNode {
 public:
  Type type() const override { return Type::GLOBAL_AGGREGATE; }
  std::string ToString(int indent) const override;
};

class RapidTopN : public PlanNode {
 public:
  ORDER *order{nullptr};
  ha_rows limit{0};
  Type type() const override { return Type::TOP_N; }
  std::string ToString(int indent) const override;
};

class ZeroRows : public PlanNode {
 public:
  Type type() const override { return Type::ZERO_ROWS; }
  std::string ToString(int indent) const override;
};

class QueryPlan : public MemoryObject {
 public:
  QueryPlan() = default;
  ~QueryPlan() = default;

  std::string Explain() const;
  std::string ToString() const { return root ? root->ToString() : "EMPTY PLAN"; }

  AccessPath *BuildAccessPath(THD *thd) const;

  PlanPtr root;
  double total_cost{0.0};
  std::string plan_id;
};

void WalkPlan(PlanNode *node, std::function<void(PlanNode *)> callback);

}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_OPTIMIZER_H__