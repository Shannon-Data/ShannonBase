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

#include "sql/log.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"

namespace ShannonBase {
namespace ML {
float Query_arbitrator::TO_RAPID_THRESHOLD = 0.5;
Query_arbitrator::~Query_arbitrator() {
  m_session.reset();
  m_session_options.reset();
  m_env.reset();
}

bool Query_arbitrator::load_model(const std::string &model_path) {
  if (model_path.empty()) {
    sql_print_warning("Query_arbitrator: Model path is empty");
    return false;
  }

  m_model_path = model_path;

  try {
    // Initialize ONNX Runtime environment
    m_env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "QueryArbitrator");

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
  } catch (const Ort::Exception &e) {
    sql_print_error("Query_arbitrator: ONNX Runtime error: %s", e.what());
    m_session.reset();
    m_session_options.reset();
    m_env.reset();
    return false;
  } catch (const std::exception &e) {
    sql_print_error("Query_arbitrator: Error: %s", e.what());
    return false;
  }
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

Query_arbitrator::QueryFeatures Query_arbitrator::extract_features(Query_block *qb) {
  QueryFeatures features;
  if (!qb) return features;

  // Get table list
  Table_ref *tables_list = qb->get_table_list();
  // ========== Core Features==========
  // Count tables and base tables
  int table_count = 0;
  int base_table_count = 0;
  double base_table_sum_nrows = 0.0;

  for (Table_ref *tl = tables_list; tl != nullptr; tl = tl->next_global) {
    table_count++;

    if (!tl->is_view_or_derived()) {
      base_table_count++;

      // Get table cardinality
      if (tl->table) {
        double card = estimate_table_cardinality(tl->table);
        base_table_sum_nrows += card;
      }
    }
  }

  features.table_count = table_count;
  features.count_all_base_tables = base_table_count;
  features.base_table_sum_nrows = base_table_sum_nrows;

  // ESTIMATE mysql_total_ts_nrows: Assume all tables without indexes do full scan
  // This is a heuristic - actual decision happens during optimization
  features.mysql_total_ts_nrows = 0.0;
  features.count_ref_index_ts = 0;
  features.are_all_ts_index_ref = true;

  for (Table_ref *tl = tables_list; tl != nullptr; tl = tl->next_global) {
    if (tl->is_view_or_derived() || !tl->table) continue;

    TABLE *table = tl->table;
    // Check if table has indexes that could be used
    // Look for conditions on this table in WHERE clause
    if (qb->where_cond() && table->s->keys > 0) {
      // Simple heuristic: if table has indexes, assume they might be used
      // More sophisticated analysis would check if WHERE conditions match index columns
      features.count_ref_index_ts++;
    } else if (table->s->keys == 0) {
      // No indexes at all - likely full table scan
      features.mysql_total_ts_nrows += estimate_table_cardinality(table);
      features.are_all_ts_index_ref = false;
    } else {
      // Has indexes but no WHERE clause - might still do full scan
      // Conservative estimate: count as potential full scan
      features.mysql_total_ts_nrows += estimate_table_cardinality(table);
      features.are_all_ts_index_ref = false;
    }
  }

  // ESTIMATE mysql_cost: Use a simple heuristic before optimization
  // Real cost is computed by optimizer, here we use table size as proxy
  features.mysql_cost = base_table_sum_nrows * 1.1;  // Simple cost estimation

  // If there's a join, multiply by number of tables (nested loop estimation)
  if (table_count > 1) {
    features.mysql_cost *= table_count;
  }

  // ESTIMATE estimated_rows: Use heuristics based on query structure
  features.estimated_rows = base_table_sum_nrows;

  // Adjust for WHERE clause (assume 10% selectivity if present)
  if (qb->where_cond()) {
    features.estimated_rows *= 0.1;
  }

  // Adjust for aggregation (reduces result set significantly)
  if (qb->group_list.elements > 0) {
    features.estimated_rows *= 0.01;  // GROUP BY typically reduces rows significantly
  }

  // Adjust for LIMIT
  if (qb->has_limit() && qb->select_limit) {
    ha_rows limit_val = qb->select_limit->val_uint();
    if (limit_val < features.estimated_rows) {
      features.estimated_rows = static_cast<double>(limit_val);
    }
  }

  // ========== Query Shape Features (these are accurate at pre-prepare) ==========
  features.has_having = (qb->having_cond() != nullptr);
  features.has_group_by = (qb->group_list.elements > 0);
  features.has_rollup = (qb->olap == ROLLUP_TYPE);
  features.has_order_by = (qb->order_list.elements > 0);
  features.has_limit = qb->has_limit();
  features.has_join = (table_count > 1);
  features.has_subquery =
      (qb->has_sj_candidates() || qb->materialized_derived_table_count > 0 || qb->n_scalar_subqueries > 0);

  // Aggregation function detection
  features.has_aggregation = false;
  for (Item *item : qb->fields) {
    if (item->type() == Item::SUM_FUNC_ITEM) {
      features.has_aggregation = true;
      break;
    }
  }
  if (features.has_group_by) features.has_aggregation = true;

  features.select_list_size = qb->fields.size();

  // WHERE conditions count
  features.where_condition_count = 0;
  if (qb->where_cond()) {
    Item *cond = qb->where_cond();
    if (cond->type() == Item::COND_ITEM) {
      Item_cond *cond_item = static_cast<Item_cond *>(cond);
      features.where_condition_count = cond_item->argument_list()->size();
    } else {
      features.where_condition_count = 1;
    }
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

  try {
    // Prepare feature vector
    std::vector<float> feature_values = features_to_vector(features);

    // Validate feature count
    if (m_input_node_dims[1] != -1 && static_cast<int64_t>(feature_values.size()) != m_input_node_dims[1]) {
      sql_print_error("Query_arbitrator: Feature count mismatch. Expected %lld, got %zu", m_input_node_dims[1],
                      feature_values.size());
      return WHERE2GO::TO_PRIMARY;
    }

    // Create input tensor
    std::vector<int64_t> input_shape = {1, static_cast<int64_t>(feature_values.size())};
    Ort::MemoryInfo memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(memory_info, feature_values.data(), feature_values.size(),
                                                              input_shape.data(), input_shape.size());

    // Run inference
    auto output_tensors = m_session->Run(Ort::RunOptions{nullptr}, m_input_node_names.data(), &input_tensor, 1,
                                         m_output_node_names.data(), 1);

    // Get output
    float *output_data = output_tensors[0].GetTensorMutableData<float>();

    // Check output shape
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
    WHERE2GO decision = prediction_score > TO_RAPID_THRESHOLD ? WHERE2GO::TO_SECONDARY : WHERE2GO::TO_PRIMARY;

#ifndef NDEBUG
    sql_print_information("Query_arbitrator: Prediction score=%.4f, threshold=%.2f, decision=%s", prediction_score,
                          TO_RAPID_THRESHOLD, decision == WHERE2GO::TO_SECONDARY ? "TO_SECONDARY" : "TO_PRIMARY");
#endif
    return decision;

  } catch (const Ort::Exception &e) {
    sql_print_error("Query_arbitrator: ONNX Runtime prediction failed: %s", e.what());
    return WHERE2GO::TO_PRIMARY;
  } catch (const std::exception &e) {
    sql_print_error("Query_arbitrator: Prediction error: %s", e.what());
    return WHERE2GO::TO_PRIMARY;
  }
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