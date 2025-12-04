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
#ifndef __SHANNONBASE_CONDITION_PUSHDOWN_RULE_H__
#define __SHANNONBASE_CONDITION_PUSHDOWN_RULE_H__

#include <memory>
#include <string>

#include "storage/rapid_engine/optimizer/rules/rule.h"
class Query_expression;
class Query_block;

namespace ShannonBase {
namespace Optimizer {

class PredicatePushDown : public Rule {
 public:
  PredicatePushDown() = default;
  PredicatePushDown(std::shared_ptr<Query_expression> &expression);
  virtual ~PredicatePushDown();
  void apply(PlanPtr &root) override;
  std::string name() override { return std::string("PredicatePushDown"); }

 private:
  std::shared_ptr<Query_expression> m_query_expr;
};

class AggregationPushDown : public Rule {
 public:
  AggregationPushDown() = default;
  AggregationPushDown(std::shared_ptr<Query_expression> &expression);
  virtual ~AggregationPushDown();
  void apply(PlanPtr &root) override;
  std::string name() override { return std::string("AggregationPushDown"); }

 private:
  std::shared_ptr<Query_expression> m_query_expr;
};

class TopNPushDown : public Rule {
 public:
  TopNPushDown() = default;
  TopNPushDown(std::shared_ptr<Query_expression> &expression);
  virtual ~TopNPushDown();
  void apply(PlanPtr &root) override;
  std::string name() override { return std::string("TopNPushDown"); }

 private:
  std::shared_ptr<Query_expression> m_query_expr;
};

}  // namespace Optimizer
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CONDITION_PUSHDOWN_RULE_H__