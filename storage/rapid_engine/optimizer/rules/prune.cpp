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
#include "storage/rapid_engine/optimizer/rules/prune.h"

#include "sql/item.h"  //Item
#include "sql/item_func.h"
#include "sql/sql_lex.h"  //query_expression
#include "sql/sql_list.h"

#include "storage/rapid_engine/imcs/table.h"  //RpdTable
namespace ShannonBase {
namespace Optimizer {
void ColumnCollector::visit(Item *item) {
  if (!item) return;

  // 1. Handle Field Items (The Leaf Nodes we look for)
  if (item->type() == Item::FIELD_ITEM) {
    auto *f = static_cast<Item_field *>(item);
    // Ensure field and table are valid
    if (f->field && f->field->table) {
      // Construct unique key: "db.table"
      std::string key = std::string(f->field->table->s->db.str) + "." + std::string(f->field->table->s->table_name.str);

      // Insert the field index (0-based)
      used_columns[key].insert(f->field->field_index());
    }
    return;
  }

  // 2. Handle Function Items (e.g., ABS(col), col1 + col2)
  // Item_sum (Aggregates) also inherits from Item_func
  if (item->type() == Item::FUNC_ITEM || item->type() == Item::SUM_FUNC_ITEM) {
    auto *func = static_cast<Item_func *>(item);
    for (uint i = 0; i < func->argument_count(); i++) {
      visit(func->arguments()[i]);
    }
    return;
  }

  // 3. Handle Condition Items (e.g., AND, OR, XOR)
  if (item->type() == Item::COND_ITEM) {
    auto *cond = static_cast<Item_cond *>(item);
    List_iterator<Item> li(*cond->argument_list());
    Item *it;
    while ((it = li++)) {
      visit(it);
    }
    return;
  }

  // 4. Handle Reference Items (e.g., references in HAVING or ORDER BY)
  // These point to other items which might contain fields.
  if (item->type() == Item::REF_ITEM) {
    auto *ref = static_cast<Item_ref *>(item);
    auto item_ptr = ref->ref_item();
    if (item_ptr) visit(item_ptr);
    return;
  }

  // Note: SUBSELECT_ITEM etc. are complex.
  // Usually for projection pruning we care about the current level's table references.
}

StorageIndexPrune::StorageIndexPrune(std::shared_ptr<Query_expression> &expression) : m_query_expr(expression) {}
StorageIndexPrune::~StorageIndexPrune() {}
void StorageIndexPrune::apply(Plan &root) {}

ProjectionPruning::ProjectionPruning(std::shared_ptr<Query_expression> &expression) : m_query_expr(expression) {}
ProjectionPruning::~ProjectionPruning() {}
void ProjectionPruning::apply(Plan &root) {
  if (!root) return;

  ColumnCollector collector;

  // ---------------------------------------------------------
  // Step 1: Collect columns from Query Block (Select List, Where, Having)
  // ---------------------------------------------------------
  if (m_query_expr) {
    Query_block *qb = m_query_expr->first_query_block();
    if (qb) {
      // 1.1 Select List
      mem_root_deque<Item *> *fields = qb->get_fields_list();
      if (fields) {
        for (auto it = fields->begin(); it != fields->end(); ++it) {
          Item *item = *it;
          collector.visit(item);
        }
      }

      // 1.2 Where Condition
      if (qb->where_cond()) {
        collector.visit(qb->where_cond());
      }

      // 1.3 Having Condition
      if (qb->having_cond()) {
        collector.visit(qb->having_cond());
      }
    }
  }

  // ---------------------------------------------------------
  // Step 2: Collect columns from Plan Nodes (Join Conditions, Group By, Sort)
  // ---------------------------------------------------------
  WalkPlan(root.get(), [&](PlanNode *node) {
    switch (node->type()) {
      case PlanNode::Type::HASH_JOIN: {
        auto *hj = static_cast<HashJoin *>(node);
        for (auto *item : hj->join_conditions) {
          collector.visit(item);
        }
        break;
      }
      case PlanNode::Type::LOCAL_AGGREGATE: {
        auto *agg = static_cast<LocalAgg *>(node);
        for (auto *item : agg->group_by) {
          collector.visit(item);
        }
        for (auto *item : agg->aggregates) {
          // aggregates are Item_func/Item_sum, visited recursively
          collector.visit(item);
        }
        break;
      }
      case PlanNode::Type::TOP_N: {
        auto *topn = static_cast<TopN *>(node);
        if (topn->order) {
          for (ORDER *ord = topn->order; ord; ord = ord->next) {
            if (ord->item && *ord->item) {
              collector.visit(*ord->item);
            }
          }
        }
        break;
      }
      case PlanNode::Type::FILTER: {
        // If Filter node has specific expressions not in WHERE
        // auto *filter = static_cast<Filter *>(node);
        // collector.visit(filter->condition);
        break;
      }
      default:
        break;
    }
  });

  // ---------------------------------------------------------
  // Step 3: Push collected columns down to ScanTable nodes
  // ---------------------------------------------------------
  WalkPlan(root.get(), [&](PlanNode *node) {
    if (node->type() == PlanNode::Type::SCAN) {
      auto *scan = static_cast<ScanTable *>(node);

      // Ensure RpdTable is valid
      if (scan->rpd_table) {
        std::string key =
            std::string(scan->rpd_table->meta().db_name) + "." + std::string(scan->rpd_table->meta().table_name);

        // Check if we found any used columns for this table
        if (collector.used_columns.count(key)) {
          const auto &cols [[maybe_unused]] = collector.used_columns[key];

          /**
           * IMPROTANT:
           * Ensure your ScanTable class (in query_plan.h) has a member to store this.
           * Example: std::vector<uint32_t> projected_columns;
           */

          // scan->projected_columns.assign(cols.begin(), cols.end());

          // Debugging output (Optional)
          // printf("Pruning Table %s: Used %lu columns\n", key.c_str(), cols.size());
        } else {
          // Edge case: Count(*) with no specific columns.
          // Strategy: Usually engines read the primary key or the smallest column if list is empty,
          // or handle explicit count(*) optimization.
        }
      }
    }
  });
}

}  // namespace Optimizer
}  // namespace ShannonBase