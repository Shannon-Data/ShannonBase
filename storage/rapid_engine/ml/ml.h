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

#ifndef __SHANNONBASE_RAPID_ML_H__
#define __SHANNONBASE_RAPID_ML_H__

#include <onnxruntime_cxx_api.h>
#include <string>
//#include "LightGBM/c_api.h"  //lightgbm

class THD;
class JOIN;
class Query_block;
namespace ShannonBase {
namespace ML {
class Query_arbitrator {
 public:
  static float TO_RAPID_THRESHOLD;

  enum class WHERE2GO { TO_PRIMARY, TO_SECONDARY };

  // Feature extraction result
  struct QueryFeatures {
    // ===== Core features from WL#15829 =====
    double mysql_total_ts_nrows;  // f_mysql_total_ts_nrows: Total table scan rows
    double mysql_cost;            // f_MySQLCost: MySQL optimizer cost
    int count_all_base_tables;    // f_count_all_base_tables: Number of base tables
    int count_ref_index_ts;       // f_count_ref_index_ts: Number of index ref accesses
    double base_table_sum_nrows;  // f_BaseTableSumNrows: Sum of all base table rows
    bool are_all_ts_index_ref;    // f_are_all_ts_index_ref: All tables use index?

    // ===== Additional OLAP/OLTP detection features =====
    int table_count;            // Total table count in query
    bool has_having;            // Has HAVING clause
    bool has_group_by;          // Has GROUP BY
    bool has_rollup;            // Has ROLLUP
    bool has_order_by;          // Has ORDER BY
    bool has_limit;             // Has LIMIT
    bool has_join;              // Has JOIN operations
    bool has_subquery;          // Has subqueries
    bool has_aggregation;       // Has aggregation functions (SUM, AVG, COUNT, etc.)
    int select_list_size;       // Number of items in SELECT list
    int where_condition_count;  // Number of WHERE conditions
    double estimated_rows;      // Estimated result rows

    // Constructor with defaults
    QueryFeatures()
        : mysql_total_ts_nrows(0.0),
          mysql_cost(0.0),
          count_all_base_tables(0),
          count_ref_index_ts(0),
          base_table_sum_nrows(0.0),
          are_all_ts_index_ref(false),
          table_count(0),
          has_having(false),
          has_group_by(false),
          has_rollup(false),
          has_order_by(false),
          has_limit(false),
          has_join(false),
          has_subquery(false),
          has_aggregation(false),
          select_list_size(0),
          where_condition_count(0),
          estimated_rows(0.0) {}
  };

  Query_arbitrator() = default;
  ~Query_arbitrator();

  // Disable copy and move
  Query_arbitrator(const Query_arbitrator &) = delete;
  Query_arbitrator &operator=(const Query_arbitrator &) = delete;
  Query_arbitrator(Query_arbitrator &&) = delete;
  Query_arbitrator &operator=(Query_arbitrator &&) = delete;

  void set_model_path(const std::string &model_path) { m_model_path = model_path; }

  // Load the trained ONNX model
  bool load_model(const std::string &model_path);

  // Main prediction interface - works at pre-prepare stage
  WHERE2GO predict(Query_block *qb);

  // Get last prediction features (for debugging/logging)
  const QueryFeatures &last_features() const { return m_last_features; }

  // Get model info (for debugging)
  bool is_model_loaded() const { return m_model_loaded; }
  const std::string &model_path() const { return m_model_path; }

 private:
  // Extract features from Query_block (works at pre-prepare stage)
  QueryFeatures extract_features(Query_block *qb);

  // Perform ONNX Runtime prediction with extracted features
  WHERE2GO predict_with_features(const QueryFeatures &features);

  // Logging helper - format like WL#15829
  void log_decision(const QueryFeatures &features, WHERE2GO decision);

  // Convert features to vector (ensures consistent ordering)
  std::vector<float> features_to_vector(const QueryFeatures &features) const;

  std::string m_model_path;
  std::unique_ptr<Ort::Env> m_env;
  std::unique_ptr<Ort::Session> m_session;
  std::unique_ptr<Ort::SessionOptions> m_session_options;
  bool m_model_loaded{false};
  QueryFeatures m_last_features;

  // ONNX model metadata - store as strings to avoid dangling pointers
  std::vector<std::string> m_input_node_names_storage;
  std::vector<std::string> m_output_node_names_storage;
  std::vector<const char *> m_input_node_names;
  std::vector<const char *> m_output_node_names;
  std::vector<int64_t> m_input_node_dims;
};
}  // namespace ML
}  // namespace ShannonBase
#endif  //__SHANNONBASE_RAPID_ML_H__