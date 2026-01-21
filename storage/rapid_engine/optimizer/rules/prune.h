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
#ifndef __SHANNONBASE_OBJECT_PRUNE_RULE_H__
#define __SHANNONBASE_OBJECT_PRUNE_RULE_H__

#include <memory>
#include <string>

#include "storage/rapid_engine/optimizer/rules/rule.h"
class Query_expression;
class Query_block;
class Item;
namespace ShannonBase {
namespace Optimizer {

class StorageIndexPrune : public Rule {
 public:
  StorageIndexPrune() = default;
  virtual ~StorageIndexPrune() = default;

  std::string name() override { return std::string("StorageIndexPrune"); }
  void apply(Plan &root) override;
};

class ProjectionPruning : public Rule {
 public:
  ProjectionPruning() = default;
  virtual ~ProjectionPruning() = default;

  std::string name() override { return std::string("ProjectionPruning"); }
  void apply(Plan &root) override;

 private:
  /**
   * Collect all referenced columns from the plan tree
   * @param root Plan root node
   * @return Map of "db.table" -> set of column indices
   */
  std::map<std::string, std::set<uint32_t>> collect_referenced_columns(Plan &root);

  /**
   * Extract columns from join conditions
   */
  void collect_from_join_conditions(const std::vector<Item *> &conditions,
                                    std::map<std::string, std::set<uint32_t>> &columns);

  /**
   * Extract columns from aggregation node
   */
  void collect_from_aggregation(const LocalAgg *agg, std::map<std::string, std::set<uint32_t>> &columns);

  /**
   * Extract columns from filter predicates
   */
  void collect_from_filter(const Filter *filter, std::map<std::string, std::set<uint32_t>> &columns);

  /**
   * Visit an Item and collect field references
   */
  void visit_item(Item *item, std::map<std::string, std::set<uint32_t>> &columns);
};
}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_OBJECT_PRUNE_RULE_H__