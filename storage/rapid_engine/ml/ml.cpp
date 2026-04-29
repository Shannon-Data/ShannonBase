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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/

#include "ml.h"

#include "sql/item.h"
#include "sql/log.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"

namespace ShannonBase {
namespace ML {
std::atomic<Query_arbitrator *> Query_arbitrator::s_instance{nullptr};

bool Query_arbitrator::initialize(const std::string &model_path) {
  // Guard: only the first call takes effect (called from plugin init thread).
  if (s_instance.load(std::memory_order_acquire) != nullptr) return true;

  auto *qa = new (std::nothrow) Query_arbitrator();
  if (!qa) {
    sql_print_error("Query_arbitrator::initialize: allocation failed");
    return false;
  }

  if (!qa->load_model(model_path)) {
    delete qa;
    sql_print_error("Query_arbitrator::initialize: load_model failed for %s", model_path.c_str());
    return false;
  }

  s_instance.store(qa, std::memory_order_release);
  sql_print_information("Query_arbitrator: singleton initialized from %s", model_path.c_str());
  return true;
}
Query_arbitrator *Query_arbitrator::instance() { return s_instance.load(std::memory_order_acquire); }

bool Query_arbitrator::load_model(const std::string &model_path) {
  if (model_path.empty()) {
    sql_print_warning("Query_arbitrator: Model path is empty");
    return false;
  }

  m_model_path = model_path;

  // Initialize ONNX Runtime environment
  m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_ERROR, "QueryArbitrator");

  // Create session options
  m_session_options = std::make_unique<Ort::SessionOptions>();
  m_session_options->SetIntraOpNumThreads(1);
  m_session_options->SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

  // Load the ONNX model
#ifdef _WIN32
  std::wstring wmodel_path(model_path.begin(), model_path.end());
  m_session = std::make_unique<Ort::Session>(*m_env, wmodel_path.c_str(), *m_session_options);
#else
  m_session = std::make_unique<Ort::Session>(*m_env, model_path.c_str(), *m_session_options);
#endif

  // Get input/output metadata
  Ort::AllocatorWithDefaultOptions allocator;

  // Input metadata
  size_t num_input_nodes = m_session->GetInputCount();
  if (num_input_nodes != 1) {
    sql_print_error("Query_arbitrator: Expected 1 input node, got %zu", num_input_nodes);
    return false;
  }

  // Get input name - Store string copy to avoid dangling pointer
  auto input_name_ptr = m_session->GetInputNameAllocated(0, allocator);
  m_input_node_names_storage.push_back(std::string(input_name_ptr.get()));
  m_input_node_names.push_back(m_input_node_names_storage.back().c_str());

  // Get input dimensions
  Ort::TypeInfo type_info = m_session->GetInputTypeInfo(0);
  auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
  m_input_node_dims = tensor_info.GetShape();

  if (m_input_node_dims.size() != 2) {
    sql_print_error("Query_arbitrator: Invalid input shape dimensions");
    return false;
  }

  // Validate expected feature count (18 features)
  int64_t expected_features = 18;
  int64_t model_features = m_input_node_dims[1];
  if (model_features != -1 && model_features != expected_features) {
    sql_print_warning("Query_arbitrator: Model expects %lld features, but we provide %lld", model_features,
                      expected_features);
  }

  // Output metadata
  size_t num_output_nodes = m_session->GetOutputCount();
  if (num_output_nodes < 1) {
    sql_print_error("Query_arbitrator: No output nodes found");
    return false;
  }

  // Get output name
  auto output_name_ptr = m_session->GetOutputNameAllocated(0, allocator);
  m_output_node_names_storage.push_back(std::string(output_name_ptr.get()));
  m_output_node_names.push_back(m_output_node_names_storage.back().c_str());

  m_model_loaded = true;
  sql_print_information("Query_arbitrator: Successfully loaded model from %s", model_path.c_str());
  sql_print_information("Query_arbitrator: Input node: %s, shape: [%lld, %lld]", m_input_node_names[0],
                        m_input_node_dims[0], m_input_node_dims[1]);
  sql_print_information("Query_arbitrator: Output node: %s", m_output_node_names[0]);

  return true;
}

// Helper function to estimate table cardinality at pre-prepare stage
static double estimate_table_cardinality(TABLE *table) {
  if (!table || !table->file) return 0.0;

  // Get statistics from storage engine
  ha_rows records = table->file->stats.records;

  // If stats are stale or unavailable, try to get fresh stats
  if (records == 0) {
    table->file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
    records = table->file->stats.records;
  }

  return static_cast<double>(records);
}

static void walk_item_for_subqueries(Item *item, double &total_scan_rows, double &total_cost);

static void accumulate_subquery_cost(Query_block *qb, double &total_scan_rows, double &total_cost, int depth = 0) {
  if (!qb || depth > 8) return;

  for (Table_ref *tl = qb->get_table_list(); tl != nullptr; tl = tl->next_local) {
    if (tl->is_view_or_derived() || !tl->table) continue;
    double card = estimate_table_cardinality(tl->table);
    if (qb->where_cond() && tl->table->s->keys > 0) {
      total_scan_rows += card * 0.1;
      total_cost += card * 0.1 * 1.1;
    } else {
      total_scan_rows += card;
      total_cost += card * 1.1;
    }
  }

  for (Item *item : qb->fields) walk_item_for_subqueries(item, total_scan_rows, total_cost);

  // dealing with subqueries in HAVING and WHERE clauses.
  walk_item_for_subqueries(qb->having_cond(), total_scan_rows, total_cost);
  walk_item_for_subqueries(qb->where_cond(), total_scan_rows, total_cost);
}

static void walk_item_for_subqueries(Item *item, double &total_scan_rows, double &total_cost) {
  if (!item) return;
  if (auto *sub = dynamic_cast<Item_subselect *>(item)) {
    if (Query_expression *unit = sub->query_expr()) {
      for (Query_block *inner = unit->first_query_block(); inner != nullptr; inner = inner->next_query_block())
        accumulate_subquery_cost(inner, total_scan_rows, total_cost, 1);
    }
    return;
  }

  if (item->type() == Item::COND_ITEM) {
    auto *cond = static_cast<Item_cond *>(item);
    List_iterator<Item> it(*cond->argument_list());
    while (Item *child = it++) walk_item_for_subqueries(child, total_scan_rows, total_cost);
  }
}

Query_arbitrator::QueryFeatures Query_arbitrator::extract_features(Query_block *qb) {
  QueryFeatures features;
  if (!qb) return features;

  Table_ref *tables_list = qb->get_table_list();

  int table_count = 0;
  int base_table_count = 0;
  double base_table_sum_nrows = 0.0;

  for (Table_ref *tl = tables_list; tl != nullptr; tl = tl->next_local) {
    ++table_count;
    if (!tl->is_view_or_derived()) {
      ++base_table_count;
      if (tl->table) base_table_sum_nrows += estimate_table_cardinality(tl->table);
    }
  }

  features.table_count = table_count;
  features.count_all_base_tables = base_table_count;
  features.base_table_sum_nrows = base_table_sum_nrows;

  features.mysql_total_ts_nrows = 0.0;
  features.count_ref_index_ts = 0;
  features.are_all_ts_index_ref = true;

  for (Table_ref *tl = tables_list; tl != nullptr; tl = tl->next_local) {
    if (tl->is_view_or_derived() || !tl->table) continue;
    TABLE *table = tl->table;
    if (qb->where_cond() && table->s->keys > 0) {
      ++features.count_ref_index_ts;
    } else {
      features.mysql_total_ts_nrows += estimate_table_cardinality(table);
      features.are_all_ts_index_ref = false;
    }
  }

  features.mysql_cost = base_table_sum_nrows * 1.1 * (table_count > 1 ? table_count : 1);
  features.estimated_rows = base_table_sum_nrows;

  if (qb->where_cond()) features.estimated_rows *= 0.1;
  if (qb->group_list.elements > 0) features.estimated_rows *= 0.01;
  if (qb->has_limit() && qb->select_limit) {
    ha_rows lv = qb->select_limit->val_uint();
    if (static_cast<double>(lv) < features.estimated_rows) features.estimated_rows = static_cast<double>(lv);
  }

  features.has_having = (qb->having_cond() != nullptr);
  features.has_group_by = (qb->group_list.elements > 0);
  features.has_rollup = (qb->olap == ROLLUP_TYPE);
  features.has_order_by = (qb->order_list.elements > 0);
  features.has_limit = qb->has_limit();
  features.has_join = (table_count > 1);
  features.has_subquery =
      (qb->has_sj_candidates() || qb->materialized_derived_table_count > 0 || qb->n_scalar_subqueries > 0);

  features.has_aggregation = features.has_group_by;
  if (!features.has_aggregation) {
    for (Item *item : qb->fields) {
      if (item->type() == Item::SUM_FUNC_ITEM) {
        features.has_aggregation = true;
        break;
      }
    }
  }

  features.select_list_size = static_cast<int>(qb->fields.size());
  features.where_condition_count = 0;
  if (Item *cond = qb->where_cond()) {
    if (cond->type() == Item::COND_ITEM)
      features.where_condition_count = static_cast<int>(static_cast<Item_cond *>(cond)->argument_list()->size());
    else
      features.where_condition_count = 1;
  }

  if (features.has_subquery) {
    double sub_scan = 0.0, sub_cost = 0.0;
    walk_item_for_subqueries(qb->having_cond(), sub_scan, sub_cost);
    walk_item_for_subqueries(qb->where_cond(), sub_scan, sub_cost);
    // SELECT list scalar subqueries（such as: SELECT (SELECT ...) ...）
    for (Item *item : qb->fields) walk_item_for_subqueries(item, sub_scan, sub_cost);

    features.mysql_total_ts_nrows += sub_scan;
    features.mysql_cost += sub_cost;
  }

  return features;
}

std::vector<float> Query_arbitrator::features_to_vector(const QueryFeatures &features) const {
  // CRITICAL: Feature order must match training data exactly
  return {
      static_cast<float>(features.mysql_total_ts_nrows),   // 0
      static_cast<float>(features.mysql_cost),             // 1
      static_cast<float>(features.count_all_base_tables),  // 2
      static_cast<float>(features.count_ref_index_ts),     // 3
      static_cast<float>(features.base_table_sum_nrows),   // 4
      features.are_all_ts_index_ref ? 1.0f : 0.0f,         // 5
      static_cast<float>(features.table_count),            // 6
      features.has_having ? 1.0f : 0.0f,                   // 7
      features.has_group_by ? 1.0f : 0.0f,                 // 8
      features.has_rollup ? 1.0f : 0.0f,                   // 9
      features.has_order_by ? 1.0f : 0.0f,                 // 10
      features.has_limit ? 1.0f : 0.0f,                    // 11
      features.has_join ? 1.0f : 0.0f,                     // 12
      features.has_subquery ? 1.0f : 0.0f,                 // 13
      features.has_aggregation ? 1.0f : 0.0f,              // 14
      static_cast<float>(features.select_list_size),       // 15
      static_cast<float>(features.where_condition_count),  // 16
      static_cast<float>(features.estimated_rows)          // 17
  };
}

Query_arbitrator::WHERE2GO Query_arbitrator::predict_with_features(const QueryFeatures &features) {
  if (!m_model_loaded || !m_session) {
    sql_print_warning("Query_arbitrator: Model not loaded, defaulting to PRIMARY");
    return WHERE2GO::TO_PRIMARY;
  }

  std::vector<float> feature_values = features_to_vector(features);
  if (m_input_node_dims[1] != -1 && static_cast<int64_t>(feature_values.size()) != m_input_node_dims[1]) {
    sql_print_error("Query_arbitrator: Feature count mismatch. Expected %lld, got %zu", m_input_node_dims[1],
                    feature_values.size());
    return WHERE2GO::TO_PRIMARY;
  }

  std::vector<int64_t> input_shape = {1, static_cast<int64_t>(feature_values.size())};
  Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, feature_values.data(), feature_values.size(),
                                                            input_shape.data(), input_shape.size());

  auto output_tensors = m_session->Run(Ort::RunOptions{nullptr}, m_input_node_names.data(), &input_tensor, 1,
                                       m_output_node_names.data(), 1);

  float *output_data = output_tensors[0].GetTensorMutableData<float>();
  auto type_info = output_tensors[0].GetTensorTypeAndShapeInfo();
  auto shape = type_info.GetShape();

  float prediction_score = 0.0f;

  if (shape.size() >= 2 && shape[1] == 2) {
    // Two-class output: [prob_class_0, prob_class_1]
    prediction_score = output_data[1];
  } else if (shape.size() >= 1) {
    // Single output value
    prediction_score = output_data[0];
  } else {
    sql_print_warning("Query_arbitrator: Unexpected output shape");
    return WHERE2GO::TO_PRIMARY;
  }

  // Apply threshold
  int olap_score = (int)features.has_group_by + (int)features.has_having + (int)features.has_aggregation +
                   (int)features.has_order_by + (int)features.has_subquery;
  float effective_threshold = TO_RAPID_THRESHOLD;
  if (olap_score >= Query_arbitrator::OLAP_FEATURE_THRESHOLD)
    effective_threshold *= Query_arbitrator::OLAP_FACTOR;  // Reduce threshold for complex OLAP queries

  WHERE2GO decision = prediction_score > effective_threshold ? WHERE2GO::TO_SECONDARY : WHERE2GO::TO_PRIMARY;

#ifndef NDEBUG
  sql_print_information("Query_arbitrator: Prediction score=%.4f, threshold=%.2f, decision=%s", prediction_score,
                        TO_RAPID_THRESHOLD, decision == WHERE2GO::TO_SECONDARY ? "TO_SECONDARY" : "TO_PRIMARY");
#endif
  return decision;
}

void Query_arbitrator::log_decision(const QueryFeatures &features, WHERE2GO decision) {
  std::ostringstream log;
  log << "Selective offload classifier: ";
  log << "f_MySQLCost=" << features.mysql_cost << ", ";
  log << "f_mysql_total_ts_nrows=" << features.mysql_total_ts_nrows << ", ";
  log << "f_count_all_base_tables=" << features.count_all_base_tables << ", ";
  log << "f_count_ref_index_ts=" << features.count_ref_index_ts << ", ";
  log << "f_BaseTableSumNrows=" << features.base_table_sum_nrows << ", ";
  log << "f_are_all_ts_index_ref=" << (features.are_all_ts_index_ref ? "true" : "false") << ", ";
  log << "table_count=" << features.table_count << ", ";
  log << "has_aggregation=" << (features.has_aggregation ? "true" : "false") << ", ";
  log << "has_group_by=" << (features.has_group_by ? "true" : "false") << ", ";
  log << "has_join=" << (features.has_join ? "true" : "false") << " | ";
  log << "outcome=" << (decision == WHERE2GO::TO_SECONDARY ? "TO_SECONDARY(1)" : "TO_PRIMARY(0)");

  sql_print_information("%s", log.str().c_str());
}

Query_arbitrator::WHERE2GO Query_arbitrator::predict(Query_block *qb) {
  if (!qb) {
    sql_print_warning("Query_arbitrator: NULL Query_block pointer, defaulting to PRIMARY");
    return WHERE2GO::TO_PRIMARY;
  }

  // Extract features from Query_block (works at pre-prepare stage)
  m_last_features = extract_features(qb);

  // Perform ML-based prediction
  WHERE2GO decision = predict_with_features(m_last_features);

#ifndef NDEBUG
  // Log decision
  log_decision(m_last_features, decision);
#endif
  return decision;
}
}  // namespace ML
}  // namespace ShannonBase