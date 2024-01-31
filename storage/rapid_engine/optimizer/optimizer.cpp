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

   Copyright (c) 2023 - 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs optimizer.
*/
#include "storage/rapid_engine/optimizer/optimizer.h"

#include "storage/innobase/include/ut0dbg.h" //ut_a
#include "sql/sql_optimizer.h"  //JOIN
#include "sql/sql_lex.h"        //Query_expression

#include "storage/rapid_engine/cost/cost.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/optimizer/rules/const_fold_rule.h"

namespace ShannonBase {
namespace Optimizer {

Timer::Timer() {
  m_begin = std::chrono::steady_clock::now();
}
std::chrono::nanoseconds Timer::lap() {
  const auto now = std::chrono::steady_clock::now();
  const auto lap_duration = std::chrono::nanoseconds{now - m_begin};
  m_begin = now;
  return lap_duration;
}
std::string Timer::lap_formatted() {
  auto stream = std::stringstream{};
  return stream.str();
}  

//ctor
Optimizer::Optimizer(std::shared_ptr<Query_expression>& expr,
                     const std::shared_ptr<CostEstimator>& cost_estimator) : 
                     m_query_expr(expr),m_cost_estimator(cost_estimator) {
  this->add_rules();
}

void Optimizer::add_rule(std::unique_ptr<Rule>& rule) {
  rule->m_cost_estimator = this->m_cost_estimator;
  m_rules.emplace_back(std::move(rule));
}
void  Optimizer::add_rules() {
  m_rules.push_back(std::make_unique<Const_fold>(m_query_expr));
}

//start to do optimization.
std::unique_ptr<JOIN> Optimizer::optimize(ShannonBase::OptimizeContext* context,
   std::shared_ptr<Query_expression>& root) const {
  //start to optimize
    
  //copy a query expression and do transfer freely.
  std::shared_ptr<Query_expression> query_expr;
  //query_expr.reset(root->clone());

  for (const auto& rule :m_rules) { //apply the rules to query block.
    auto rule_timer = Timer{};
    
  }

  std::unique_ptr<JOIN> m_optimized_join;
  return std::move(m_optimized_join);
}

} //ns:optimizer
} //ns:shannonbase
