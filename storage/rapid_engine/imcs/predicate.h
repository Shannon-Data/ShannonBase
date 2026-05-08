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
#include "my_inttypes.h"

#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_types.h"

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

enum class PredicateValueType { NULL_VALUE, INT64, DOUBLE, DECIMAL, STRING, BLOB, DATETIME };

class Predicate;
class Simple_Predicate;
class Compound_Predicate;
class StorageIndex;

class PredicateValue {
 public:
  PredicateValueType type{PredicateValueType::NULL_VALUE};

  union {
    int64 int_value;
    double double_value;
    void *ptr_value{nullptr};
  };

  std::string string_value;  // Used for strings and BLOBs

  PredicateValue() : type(PredicateValueType::NULL_VALUE), ptr_value(nullptr) {}
  explicit PredicateValue(int64 val) : type(PredicateValueType::INT64), int_value(val) {}
  explicit PredicateValue(double val) : type(PredicateValueType::DOUBLE), double_value(val) {}
  explicit PredicateValue(const std::string &val) : type(PredicateValueType::STRING), string_value(val) {}
  explicit PredicateValue(const std::string &val, PredicateValueType value_type)
      : type(value_type), string_value(val) {}
  explicit PredicateValue(const char *val) : type(PredicateValueType::STRING), string_value(val) {}

  static PredicateValue null_value() {
    PredicateValue val;
    val.type = PredicateValueType::NULL_VALUE;
    return val;
  }

  inline bool is_null() const { return type == PredicateValueType::NULL_VALUE; }

  inline int64 as_int() const {
    switch (type) {
      case PredicateValueType::INT64:
        return int_value;
      case PredicateValueType::DOUBLE:
        return static_cast<int64>(double_value);
      case PredicateValueType::DECIMAL:
      case PredicateValueType::STRING:
        return static_cast<int64>(std::stod(string_value));
      default:
        return 0;
    }
  }

  inline double as_double() const {
    switch (type) {
      case PredicateValueType::INT64:
        return static_cast<double>(int_value);
      case PredicateValueType::DOUBLE:
        return double_value;
      case PredicateValueType::DECIMAL:
      case PredicateValueType::STRING:
        return std::stod(string_value);
      default:
        return 0.0;
    }
  }

  inline std::string as_string() const {
    switch (type) {
      case PredicateValueType::INT64:
        return std::to_string(int_value);
      case PredicateValueType::DOUBLE:
        return std::to_string(double_value);
      case PredicateValueType::DECIMAL:
      case PredicateValueType::STRING:
        return string_value;
      default:
        return "";
    }
  }

  inline bool try_as_numeric(double &out) const {
    switch (type) {
      case PredicateValueType::INT64:
        out = static_cast<double>(int_value);
        return true;
      case PredicateValueType::DOUBLE:
        out = double_value;
        return true;
      case PredicateValueType::DECIMAL:
      case PredicateValueType::STRING:
        try {
          out = std::stod(string_value);
          return true;
        } catch (const std::invalid_argument &) {
          return false;  // non-numeric string; caller will use string comparison
        } catch (const std::out_of_range &) {
          return false;
        }
      default:
        return false;
    }
  }

  inline bool operator==(const PredicateValue &other) const {
    if (type == PredicateValueType::NULL_VALUE || other.type == PredicateValueType::NULL_VALUE) return false;

    if (type == other.type) {
      switch (type) {
        case PredicateValueType::NULL_VALUE:
          return true;
        case PredicateValueType::INT64:
          return int_value == other.int_value;
        case PredicateValueType::DOUBLE:
          return std::abs(double_value - other.double_value) < 1e-9;
        case PredicateValueType::DECIMAL: {
          double lhs = 0.0;
          double rhs = 0.0;
          if (try_as_numeric(lhs) && other.try_as_numeric(rhs)) {
            return std::abs(lhs - rhs) < 1e-9;
          }
          return string_value == other.string_value;
        }
        case PredicateValueType::STRING:
          return string_value == other.string_value;
        default:
          return false;
      }
    }

    double lhs = 0.0;
    double rhs = 0.0;
    if (try_as_numeric(lhs) && other.try_as_numeric(rhs)) {
      return std::abs(lhs - rhs) < 1e-9;
    }
    return false;
  }

  inline bool operator<(const PredicateValue &other) const {
    if (type == PredicateValueType::NULL_VALUE || other.type == PredicateValueType::NULL_VALUE) return false;

    if (type == other.type) {
      switch (type) {
        case PredicateValueType::INT64:
          return int_value < other.int_value;
        case PredicateValueType::DOUBLE:
          return double_value < other.double_value;
        case PredicateValueType::DECIMAL: {
          double lhs = 0.0;
          double rhs = 0.0;
          if (try_as_numeric(lhs) && other.try_as_numeric(rhs)) {
            return lhs < rhs;
          }
          return string_value < other.string_value;
        }
        case PredicateValueType::STRING:
          return string_value < other.string_value;
        default:
          return false;
      }
    }

    double lhs = 0.0;
    double rhs = 0.0;
    if (try_as_numeric(lhs) && other.try_as_numeric(rhs)) {
      return lhs < rhs;
    }
    return false;
  }

  inline bool operator<=(const PredicateValue &other) const { return *this < other || *this == other; }
  inline bool operator>(const PredicateValue &other) const { return !(*this <= other); }
  inline bool operator>=(const PredicateValue &other) const { return !(*this < other); }
  inline bool operator!=(const PredicateValue &other) const { return !(*this == other); }
};

class Predicate {
 public:
  struct SHANNON_ALIGNAS ColumnBatch {
    std::vector<const uchar *> column_ptrs;  // Starting addr for each column
    std::vector<size_t> column_strides;      // Stride (in bytes) for each column
    std::vector<bool> column_nulls;          // Whether each column contains NULLs
    size_t num_rows;                         // Batch size
  };

  Predicate(PredicateOperator oper, bool compound = false) : op(oper), compound_pred(compound) {}
  virtual ~Predicate() = default;

  virtual bool evaluate(const uchar *&input_value) const = 0;

  virtual void evaluate_batch(const std::vector<const uchar *> &input_values, bit_array_t &result,
                              size_t batch_num = 8) const = 0;

  virtual std::vector<uint32> get_columns() const = 0;

  virtual std::unique_ptr<Predicate> clone() const = 0;

  virtual std::string to_string() const = 0;

  virtual bool is_compound() const { return compound_pred; }
  virtual double estimate_selectivity(const StorageIndex *storage_index = nullptr) const = 0;

  PredicateOperator op;  // AND, OR, NOT
  bool compound_pred{false};
};

/**
 * Simple predicate: single column comparison
 * Examples: age > 25, name = 'Alice'
 */
class Simple_Predicate : public Predicate {
 public:
  Simple_Predicate(uint32 col_id, PredicateOperator op_type, const PredicateValue &val,
                   enum_field_types type = MYSQL_TYPE_NULL)
      : Predicate(op_type, false), column_id(col_id), value(val), column_type(type) {}

  // BETWEEN constructor
  Simple_Predicate(uint32 col_id, const PredicateValue &min_val, const PredicateValue &max_val,
                   enum_field_types type = MYSQL_TYPE_NULL)
      : Predicate(PredicateOperator::BETWEEN, false),
        column_id(col_id),
        value(min_val),
        value2(max_val),
        column_type(type) {}

  // IN constructor
  Simple_Predicate(uint32 col_id, const std::vector<PredicateValue> &values, bool is_not_in = false,
                   enum_field_types type = MYSQL_TYPE_NULL)
      : Predicate(is_not_in ? PredicateOperator::NOT_IN : PredicateOperator::IN, false),
        value_list(values),
        column_type(type) {}

  // Evaluation implementation
  bool evaluate(const uchar *&input_value) const override;
  void evaluate_batch(const std::vector<const uchar *> &input_values, bit_array_t &result,
                      size_t batch_num = 8) const override;
  void evaluate_vecotrized(const std::vector<const uchar *> &col_data, size_t num_rows, bit_array_t &result);

  // Helper methods
  std::vector<uint32> get_columns() const override { return {column_id}; }
  std::unique_ptr<Predicate> clone() const override { return std::make_unique<Simple_Predicate>(*this); }
  std::string to_string() const override;
  double estimate_selectivity(const StorageIndex *storage_index = nullptr) const override;

 public:
  uint32 column_id;                               // Column index
  PredicateValue value;                           // Comparison value
  PredicateValue value2;                          // Second value (for BETWEEN)
  std::vector<PredicateValue> value_list;         // Value list (for IN)
  Field *field_meta{nullptr};                     // using the field meta.
  bool low_order{false};                          // low order.
  enum_field_types column_type{MYSQL_TYPE_NULL};  // Column type
 private:
  PredicateValue extract_value(const uchar *data, bool low_order = false) const;
  bool evaluate_like(const std::string &str, const std::string &pattern) const;
  bool evaluate_regexp(const std::string &str, const std::string &pattern) const;

  void evaluate_int32_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows, bit_array_t &result);
  void evaluate_int64_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows, bit_array_t &result);
  void evaluate_double_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows, bit_array_t &result);
  void evaluate_decimal_vectorized(const std::vector<const uchar *> &col_data, size_t num_rows, bit_array_t &result);
};

/**
 * Compound predicate: logical combinations
 * Examples: (age > 25 AND city = 'Beijing') OR (salary > 10000)
 */
class Compound_Predicate : public Predicate {
 public:
  Compound_Predicate(PredicateOperator op_type) : Predicate(op_type, true) {}

  /**
   * Add child predicate
   */
  inline void add_child(std::unique_ptr<Predicate> child) { children.push_back(std::move(child)); }

  // Evaluation implementation
  bool evaluate(const uchar *&input_value) const override;
  void evaluate_batch(const std::vector<const uchar *> &input_values, bit_array_t &result,
                      size_t batch_num = 8) const override;
  // Helper methods
  std::vector<uint32> get_columns() const override;
  std::unique_ptr<Predicate> clone() const override;
  std::string to_string() const override;
  double estimate_selectivity(const StorageIndex *storage_index = nullptr) const override;

  std::vector<std::unique_ptr<Predicate>> children;
};

/**
 * Predicate Builder (Builder pattern)
 */
class Predicate_Builder {
 public:
  static std::unique_ptr<Simple_Predicate> create_simple(uint32 col_id, PredicateOperator op,
                                                         const PredicateValue &value,
                                                         enum_field_types type = MYSQL_TYPE_NULL);

  static std::unique_ptr<Simple_Predicate> create_between(uint32 col_id, const PredicateValue &min_val,
                                                          const PredicateValue &max_val,
                                                          enum_field_types type = MYSQL_TYPE_NULL);

  static std::unique_ptr<Simple_Predicate> create_in(uint32 col_id, const std::vector<PredicateValue> &values,
                                                     bool is_not_in = false, enum_field_types type = MYSQL_TYPE_NULL);

  static std::unique_ptr<Compound_Predicate> create_and(std::vector<std::unique_ptr<Predicate>> predicates);

  static std::unique_ptr<Compound_Predicate> create_or(std::vector<std::unique_ptr<Predicate>> predicates);

  static std::unique_ptr<Compound_Predicate> create_not(std::unique_ptr<Predicate> predicate);
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_IMCS_PREDICATE_H__