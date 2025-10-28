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

   Copyright (c) 2023, 2024, 2025 Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
/**
 * Predicate
 *
 * Purpose:
 * 1. Represent WHERE clause filtering conditions
 * 2. Support predicate pushdown to IMCU level
 * 3. Support vectorized evaluation
 * 4. Support Storage Index filtering
 *
 * Supported predicate types:
 * - Comparison predicates: =, !=, <, <=, >, >=
 * - Range predicates: BETWEEN, IN, NOT IN
 * - NULL predicates: IS NULL, IS NOT NULL
 * - String predicates: LIKE, NOT LIKE
 * - Logical predicates: AND, OR, NOT
 */

#ifndef __SHANNONBASE_IMCS_PREDICATE_H__
#define __SHANNONBASE_IMCS_PREDICATE_H__

#include <cmath>
#include <functional>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "include/field_types.h"  // enum_field_types
#include "storage/rapid_engine/include/rapid_object.h"

namespace ShannonBase {
namespace Imcs {
/**
 * Predicate operator enumeration
 */
enum class PredicateOperator {
  // Comparison operators
  EQUAL,          // =
  NOT_EQUAL,      // !=, <>
  LESS_THAN,      //
  LESS_EQUAL,     // <=
  GREATER_THAN,   // >
  GREATER_EQUAL,  // >=

  // Range operators
  BETWEEN,      // BETWEEN min AND max
  NOT_BETWEEN,  // NOT BETWEEN
  IN,           // IN (value1, value2, ...)
  NOT_IN,       // NOT IN

  // NULL operators
  IS_NULL,      // IS NULL
  IS_NOT_NULL,  // IS NOT NULL

  // String operators
  LIKE,        // LIKE 'pattern'
  NOT_LIKE,    // NOT LIKE
  REGEXP,      // REGEXP 'pattern'
  NOT_REGEXP,  // NOT REGEXP

  // Logical operators
  AND,  // AND
  OR,   // OR
  NOT   // NOT
};

/**
 * Predicate value type
 */
enum class PredicateValueType { NULL_VALUE, INT, DOUBLE, STRING, BLOB, DATETIME };

// Forward declarations
class Predicate;
class Simple_Predicate;
class Compound_Predicate;
class StorageIndex;

/**
 * Predicate value wrapper
 */
class PredicateValue {
 public:
  PredicateValueType type;

  union {
    int64_t int_value;
    double double_value;
    void *ptr_value;
  };

  std::string string_value;  // Used for strings and BLOBs

  PredicateValue();
  explicit PredicateValue(int64_t val);
  explicit PredicateValue(double val);
  explicit PredicateValue(const std::string &val);
  explicit PredicateValue(const char *val);

  static PredicateValue null_value();
  bool is_null() const;

  int64_t as_int() const;
  double as_double() const;
  std::string as_string() const;

  bool operator==(const PredicateValue &other) const;
  bool operator<(const PredicateValue &other) const;
  bool operator<=(const PredicateValue &other) const;
  bool operator>(const PredicateValue &other) const;
  bool operator>=(const PredicateValue &other) const;
  bool operator!=(const PredicateValue &other) const;
};

/**
 * Predicate abstract base class
 */
class Predicate {
 public:
  Predicate(PredicateOperator oper) : op(oper) {}
  virtual ~Predicate() = default;

  /**
   * Evaluate (single row)
   * @param row_data: Row data (array of column pointers)
   * @param num_columns: Number of columns
   * @return: Predicate result (true/false)
   */
  virtual bool evaluate(const unsigned char **row_data, size_t num_columns) const = 0;

  /**
   * Vectorized evaluation (batch)
   * @param row_data: Array of row data (row-major)
   * @param num_rows: Number of rows
   * @param num_columns: Number of columns
   * @param result: Output result bitmap
   */
  virtual void evaluate_batch(const unsigned char **row_data, size_t num_rows, size_t num_columns,
                              bit_array_t &result) const;

  /**
   * Check if can be used for Storage Index filtering
   */
  virtual bool can_use_storage_index() const = 0;

  /**
   * Apply Storage Index filtering
   * @param storage_index: Storage Index
   * @return: true indicates entire IMCU can be skipped
   */
  virtual bool apply_storage_index(const StorageIndex *storage_index) const = 0;

  /**
   * Get involved columns
   */
  virtual std::vector<uint32_t> get_columns() const = 0;

  /**
   * Clone predicate
   */
  virtual std::unique_ptr<Predicate> clone() const = 0;

  /**
   * Convert to string (for debugging)
   */
  virtual std::string to_string() const = 0;

  /**
   * Estimate selectivity
   * @param storage_index: Storage Index (optional)
   * @return: Selectivity [0.0, 1.0]
   */
  virtual double estimate_selectivity(const StorageIndex *storage_index = nullptr) const = 0;

  PredicateOperator op;  // AND, OR, NOT
};

// Simple Predicate (Single Column Predicate)
/**
 * Simple predicate: single column comparison
 * Examples: age > 25, name = 'Alice'
 */
class Simple_Predicate : public Predicate {
 public:
  Simple_Predicate(uint32_t col_id, PredicateOperator op_type, const PredicateValue &val,
                   enum_field_types type = MYSQL_TYPE_NULL);

  // BETWEEN constructor
  Simple_Predicate(uint32_t col_id, const PredicateValue &min_val, const PredicateValue &max_val,
                   enum_field_types type = MYSQL_TYPE_NULL);

  // IN constructor
  Simple_Predicate(uint32_t col_id, const std::vector<PredicateValue> &values, bool is_not_in = false,
                   enum_field_types type = MYSQL_TYPE_NULL);

  // Evaluation implementation
  bool evaluate(const unsigned char **row_data, size_t num_columns) const override;
  void evaluate_batch(const unsigned char **row_data, size_t num_rows, size_t num_columns,
                      bit_array_t &result) const override;

  // Storage Index support
  bool can_use_storage_index() const override;
  bool apply_storage_index(const StorageIndex *storage_index) const override;

  // Helper methods
  std::vector<uint32_t> get_columns() const override;
  std::unique_ptr<Predicate> clone() const override;
  std::string to_string() const override;
  double estimate_selectivity(const StorageIndex *storage_index = nullptr) const override;

 public:
  uint32_t column_id;                      // Column index
  PredicateValue value;                    // Comparison value
  PredicateValue value2;                   // Second value (for BETWEEN)
  std::vector<PredicateValue> value_list;  // Value list (for IN)
  enum_field_types column_type;            // Column type
 private:
  PredicateValue extract_value(const unsigned char *data) const;
  bool evaluate_like(const std::string &str, const std::string &pattern) const;
  bool evaluate_regexp(const std::string &str, const std::string &pattern) const;
  void evaluate_comparison_batch(const unsigned char **row_data, size_t num_rows, size_t num_columns,
                                 bit_array_t &result) const;
  void evaluate_between_batch(const unsigned char **row_data, size_t num_rows, size_t num_columns,
                              bit_array_t &result) const;
};

// Compound Predicate
/**
 * Compound predicate: logical combinations
 * Examples: (age > 25 AND city = 'Beijing') OR (salary > 10000)
 */
class Compound_Predicate : public Predicate {
 public:
  Compound_Predicate(PredicateOperator op_type);

  /**
   * Add child predicate
   */
  void add_child(std::unique_ptr<Predicate> child);

  // Evaluation implementation
  bool evaluate(const unsigned char **row_data, size_t num_columns) const override;
  void evaluate_batch(const unsigned char **row_data, size_t num_rows, size_t num_columns,
                      bit_array_t &result) const override;

  // Storage Index support
  bool can_use_storage_index() const override;
  bool apply_storage_index(const StorageIndex *storage_index) const override;

  // Helper methods
  std::vector<uint32_t> get_columns() const override;
  std::unique_ptr<Predicate> clone() const override;
  std::string to_string() const override;
  double estimate_selectivity(const StorageIndex *storage_index = nullptr) const override;

  std::vector<std::unique_ptr<Predicate>> children;
};

// Predicate Builder
/**
 * Predicate Builder (Builder pattern)
 */
class Predicate_Builder {
 public:
  static std::unique_ptr<Simple_Predicate> create_simple(uint32_t col_id, PredicateOperator op,
                                                         const PredicateValue &value,
                                                         enum_field_types type = MYSQL_TYPE_NULL);

  static std::unique_ptr<Simple_Predicate> create_between(uint32_t col_id, const PredicateValue &min_val,
                                                          const PredicateValue &max_val,
                                                          enum_field_types type = MYSQL_TYPE_NULL);

  static std::unique_ptr<Simple_Predicate> create_in(uint32_t col_id, const std::vector<PredicateValue> &values,
                                                     bool is_not_in = false, enum_field_types type = MYSQL_TYPE_NULL);

  static std::unique_ptr<Compound_Predicate> create_and(std::vector<std::unique_ptr<Predicate>> predicates);

  static std::unique_ptr<Compound_Predicate> create_or(std::vector<std::unique_ptr<Predicate>> predicates);

  static std::unique_ptr<Compound_Predicate> create_not(std::unique_ptr<Predicate> predicate);
};

// Predicate Optimizer
/**
 * Predicate Optimizer
 * - Predicate pushdown
 * - Constant folding
 * - Predicate reordering
 */
class Predicate_Optimizer {
 public:
  static std::unique_ptr<Predicate> optimize(std::unique_ptr<Predicate> predicate);

  static std::pair<std::vector<Predicate *>, std::vector<Predicate *>> separate_index_predicates(Predicate *root);

 private:
  static std::unique_ptr<Predicate> fold_constants(std::unique_ptr<Predicate> pred);
  static std::unique_ptr<Predicate> push_down_predicates(std::unique_ptr<Predicate> pred);
  static std::unique_ptr<Predicate> reorder_predicates(std::unique_ptr<Predicate> pred);
  static void separate_recursive(Predicate *pred, std::vector<Predicate *> &index_preds,
                                 std::vector<Predicate *> &non_index_preds);
};

// Predicate Executor
/**
 * Predicate Executor
 * - Vectorized execution
 * - Batch processing
 */
class Predicate_Executor {
 public:
  static bool execute(const Predicate *predicate, const unsigned char **row_data, size_t num_columns);

  static void execute_batch(const Predicate *predicate, const unsigned char **row_data, size_t num_rows,
                            size_t num_columns, bit_array_t &result);

  static bool apply_storage_index(const Predicate *predicate, const StorageIndex *storage_index);
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_IMCS_PREDICATE_H__