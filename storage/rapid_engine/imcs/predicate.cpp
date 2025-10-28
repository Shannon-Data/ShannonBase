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
#include "storage/rapid_engine/imcs/predicate.h"

#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/utils/utils.h"  //bit_array_xxx

namespace ShannonBase {
namespace Imcs {
PredicateValue::PredicateValue() : type(PredicateValueType::NULL_VALUE), ptr_value(nullptr) {}

PredicateValue::PredicateValue(int64_t val) : type(PredicateValueType::INT), int_value(val) {}

PredicateValue::PredicateValue(double val) : type(PredicateValueType::DOUBLE), double_value(val) {}

PredicateValue::PredicateValue(const std::string &val) : type(PredicateValueType::STRING), string_value(val) {}

PredicateValue::PredicateValue(const char *val) : type(PredicateValueType::STRING), string_value(val) {}

PredicateValue PredicateValue::null_value() {
  PredicateValue val;
  val.type = PredicateValueType::NULL_VALUE;
  return val;
}

bool PredicateValue::is_null() const { return type == PredicateValueType::NULL_VALUE; }

int64_t PredicateValue::as_int() const {
  switch (type) {
    case PredicateValueType::INT:
      return int_value;
    case PredicateValueType::DOUBLE:
      return static_cast<int64_t>(double_value);
    case PredicateValueType::STRING:
      return std::stoll(string_value);
    default:
      return 0;
  }
}

double PredicateValue::as_double() const {
  switch (type) {
    case PredicateValueType::INT:
      return static_cast<double>(int_value);
    case PredicateValueType::DOUBLE:
      return double_value;
    case PredicateValueType::STRING:
      return std::stod(string_value);
    default:
      return 0.0;
  }
}

std::string PredicateValue::as_string() const {
  switch (type) {
    case PredicateValueType::INT:
      return std::to_string(int_value);
    case PredicateValueType::DOUBLE:
      return std::to_string(double_value);
    case PredicateValueType::STRING:
      return string_value;
    default:
      return "";
  }
}

bool PredicateValue::operator==(const PredicateValue &other) const {
  if (type != other.type) return false;

  switch (type) {
    case PredicateValueType::NULL_VALUE:
      return true;
    case PredicateValueType::INT:
      return int_value == other.int_value;
    case PredicateValueType::DOUBLE:
      return std::abs(double_value - other.double_value) < 1e-9;
    case PredicateValueType::STRING:
      return string_value == other.string_value;
    default:
      return false;
  }
}

bool PredicateValue::operator<(const PredicateValue &other) const {
  if (type != other.type) return false;

  switch (type) {
    case PredicateValueType::INT:
      return int_value < other.int_value;
    case PredicateValueType::DOUBLE:
      return double_value < other.double_value;
    case PredicateValueType::STRING:
      return string_value < other.string_value;
    default:
      return false;
  }
}

bool PredicateValue::operator<=(const PredicateValue &other) const { return *this < other || *this == other; }

bool PredicateValue::operator>(const PredicateValue &other) const { return !(*this <= other); }

bool PredicateValue::operator>=(const PredicateValue &other) const { return !(*this < other); }

bool PredicateValue::operator!=(const PredicateValue &other) const { return !(*this == other); }

void Predicate::evaluate_batch(const unsigned char **row_data, size_t num_rows, size_t num_columns,
                               bit_array_t &result) const {
  // Default implementation: row-by-row evaluation
  for (size_t i = 0; i < num_rows; i++) {
    bool match = evaluate(&row_data[i * num_columns], num_columns);
    if (match) {
      Utils::Util::bit_array_set(&result, i);
    } else {
      Utils::Util::bit_array_reset(&result, i);
    }
  }
}

Simple_Predicate::Simple_Predicate(uint32_t col_id, PredicateOperator op_type, const PredicateValue &val,
                                   enum_field_types type)
    : Predicate(op_type), column_id(col_id), value(val), column_type(type) {}

Simple_Predicate::Simple_Predicate(uint32_t col_id, const PredicateValue &min_val, const PredicateValue &max_val,
                                   enum_field_types type)
    : Predicate(PredicateOperator::BETWEEN), column_id(col_id), value(min_val), value2(max_val), column_type(type) {}

Simple_Predicate::Simple_Predicate(uint32_t col_id, const std::vector<PredicateValue> &values, bool is_not_in,
                                   enum_field_types type)
    : Predicate(is_not_in ? PredicateOperator::NOT_IN : PredicateOperator::IN), value_list(values), column_type(type) {}

bool Simple_Predicate::evaluate(const unsigned char **row_data, size_t num_columns) const {
  if (column_id >= num_columns) return false;

  const unsigned char *col_data = row_data[column_id];

  // Handle NULL
  if (!col_data) {
    switch (op) {
      case PredicateOperator::IS_NULL:
        return true;
      case PredicateOperator::IS_NOT_NULL:
        return false;
      default:
        return false;  // NULL compared with any value is false
    }
  }

  // Extract column value
  PredicateValue col_value = extract_value(col_data);

  // Evaluate based on operator
  switch (op) {
    case PredicateOperator::EQUAL:
      return col_value == value;

    case PredicateOperator::NOT_EQUAL:
      return col_value != value;

    case PredicateOperator::LESS_THAN:
      return col_value < value;

    case PredicateOperator::LESS_EQUAL:
      return col_value <= value;

    case PredicateOperator::GREATER_THAN:
      return col_value > value;

    case PredicateOperator::GREATER_EQUAL:
      return col_value >= value;

    case PredicateOperator::BETWEEN:
      return col_value >= value && col_value <= value2;

    case PredicateOperator::NOT_BETWEEN:
      return col_value < value || col_value > value2;

    case PredicateOperator::IN:
      return std::find(value_list.begin(), value_list.end(), col_value) != value_list.end();

    case PredicateOperator::NOT_IN:
      return std::find(value_list.begin(), value_list.end(), col_value) == value_list.end();

    case PredicateOperator::IS_NULL:
      return false;  // NULL already handled

    case PredicateOperator::IS_NOT_NULL:
      return true;

    case PredicateOperator::LIKE:
      return evaluate_like(col_value.as_string(), value.as_string());

    case PredicateOperator::NOT_LIKE:
      return !evaluate_like(col_value.as_string(), value.as_string());

    case PredicateOperator::REGEXP:
      return evaluate_regexp(col_value.as_string(), value.as_string());

    case PredicateOperator::NOT_REGEXP:
      return !evaluate_regexp(col_value.as_string(), value.as_string());

    default:
      return false;
  }
}

void Simple_Predicate::evaluate_batch(const unsigned char **row_data, size_t num_rows, size_t num_columns,
                                      bit_array_t &result) const {
  if (column_id >= num_columns) return;

  // SIMD optimization for common operators (simplified version)
  switch (op) {
    case PredicateOperator::EQUAL:
    case PredicateOperator::NOT_EQUAL:
    case PredicateOperator::LESS_THAN:
    case PredicateOperator::GREATER_THAN:
      evaluate_comparison_batch(row_data, num_rows, num_columns, result);
      break;

    case PredicateOperator::BETWEEN:
      evaluate_between_batch(row_data, num_rows, num_columns, result);
      break;

    default:
      // Fall back to base implementation
      Predicate::evaluate_batch(row_data, num_rows, num_columns, result);
      break;
  }
}

bool Simple_Predicate::can_use_storage_index() const {
  // Only comparison and range operators can use Storage Index
  switch (op) {
    case PredicateOperator::EQUAL:
    case PredicateOperator::LESS_THAN:
    case PredicateOperator::LESS_EQUAL:
    case PredicateOperator::GREATER_THAN:
    case PredicateOperator::GREATER_EQUAL:
    case PredicateOperator::BETWEEN:
    case PredicateOperator::IS_NULL:
      return true;
    default:
      return false;
  }
}

bool Simple_Predicate::apply_storage_index(const StorageIndex *storage_index) const {
  // Note: This is a simplified implementation
  // In real implementation, you would use actual StorageIndex statistics
  if (!can_use_storage_index() || !storage_index) {
    return false;
  }

  // For demonstration, assume StorageIndex has these methods
  // const auto& stats = storage_index->get_column_stats(column_id);

  // Simplified logic - in real implementation, use actual statistics
  switch (op) {
    case PredicateOperator::EQUAL: {
      // If value is outside IMCU range, skip entire IMCU
      return false;  // Simplified return
    }

    case PredicateOperator::LESS_THAN: {
      // If IMCU min >= value, entire IMCU doesn't satisfy
      return false;  // Simplified return
    }

      // ... other cases

    default:
      return false;
  }
}

std::vector<uint32_t> Simple_Predicate::get_columns() const { return {column_id}; }

std::unique_ptr<Predicate> Simple_Predicate::clone() const { return std::make_unique<Simple_Predicate>(*this); }

std::string Simple_Predicate::to_string() const {
  std::ostringstream oss;
  oss << "col_" << column_id << " ";

  switch (op) {
    case PredicateOperator::EQUAL:
      oss << "= ";
      break;
    case PredicateOperator::NOT_EQUAL:
      oss << "!= ";
      break;
    case PredicateOperator::LESS_THAN:
      oss << "< ";
      break;
    case PredicateOperator::LESS_EQUAL:
      oss << "<= ";
      break;
    case PredicateOperator::GREATER_THAN:
      oss << "> ";
      break;
    case PredicateOperator::GREATER_EQUAL:
      oss << ">= ";
      break;
    case PredicateOperator::BETWEEN:
      oss << "BETWEEN " << value.as_string() << " AND " << value2.as_string();
      return oss.str();
    case PredicateOperator::IN:
      oss << "IN (...)";
      return oss.str();
    case PredicateOperator::IS_NULL:
      oss << "IS NULL";
      return oss.str();
    case PredicateOperator::IS_NOT_NULL:
      oss << "IS NOT NULL";
      return oss.str();
    case PredicateOperator::LIKE:
      oss << "LIKE ";
      break;
    default:
      oss << "? ";
      break;
  }

  oss << value.as_string();
  return oss.str();
}

double Simple_Predicate::estimate_selectivity(const StorageIndex *storage_index) const {
  if (!storage_index) {
    // No statistics, use empirical defaults
    switch (op) {
      case PredicateOperator::EQUAL:
        return 0.1;  // 10%
      case PredicateOperator::NOT_EQUAL:
        return 0.9;  // 90%
      case PredicateOperator::LESS_THAN:
      case PredicateOperator::LESS_EQUAL:
      case PredicateOperator::GREATER_THAN:
      case PredicateOperator::GREATER_EQUAL:
        return 0.33;  // 33%
      case PredicateOperator::BETWEEN:
        return 0.25;  // 25%
      case PredicateOperator::IN:
        return std::min(0.5, 0.1 * value_list.size());
      case PredicateOperator::NOT_IN:
        return std::max(0.5, 1.0 - 0.1 * value_list.size());
      case PredicateOperator::IS_NULL:
        return 0.05;  // 5%
      case PredicateOperator::IS_NOT_NULL:
        return 0.95;  // 95%
      case PredicateOperator::LIKE:
      case PredicateOperator::REGEXP:
        return 0.2;  // 20%
      default:
        return 0.5;  // 50%
    }
  }

  // Use storage index statistics
  const double min_val = storage_index->get_min_value(column_id);
  const double max_val = storage_index->get_max_value(column_id);
  const size_t null_count = storage_index->get_null_count(column_id);
  const bool has_null = storage_index->get_has_null(column_id);

  // Get column stats for distinct count
  const auto *stats = storage_index->get_column_stats_snapshot(column_id);
  const size_t distinct_count = stats ? stats->distinct_count.load(std::memory_order_acquire) : 0;

  // Estimate total rows (this should ideally be passed in or stored)
  const size_t estimated_total_rows = std::max(distinct_count * 10, size_t(1000));
  const double non_null_rows =
      estimated_total_rows > null_count ? estimated_total_rows - null_count : estimated_total_rows;

  double pred_value = 0.0;
  double range = max_val - min_val;

  switch (op) {
    case PredicateOperator::EQUAL: {
      pred_value = value.as_double();

      // Value out of range
      if (pred_value < min_val || pred_value > max_val) {
        return 0.0;
      }

      // Use distinct count if available
      if (distinct_count > 0) {
        return 1.0 / distinct_count;
      }

      return 0.1;  // Default
    }

    case PredicateOperator::NOT_EQUAL: {
      pred_value = value.as_double();

      // Value out of range - all rows match
      if (pred_value < min_val || pred_value > max_val) {
        return 1.0;
      }

      if (distinct_count > 0) {
        return (distinct_count - 1.0) / distinct_count;
      }

      return 0.9;
    }

    case PredicateOperator::LESS_THAN: {
      pred_value = value.as_double();

      if (pred_value <= min_val) return 0.0;
      if (pred_value >= max_val) return 1.0;

      if (range > 0) {
        return (pred_value - min_val) / range;
      }

      return 0.5;
    }

    case PredicateOperator::LESS_EQUAL: {
      pred_value = value.as_double();

      if (pred_value < min_val) return 0.0;
      if (pred_value >= max_val) return 1.0;

      if (range > 0) {
        double sel = (pred_value - min_val) / range;
        // Add small adjustment for equality
        if (distinct_count > 0) {
          sel += 0.5 / distinct_count;
        }
        return std::min(1.0, sel);
      }

      return 1.0;  // Single value
    }

    case PredicateOperator::GREATER_THAN: {
      pred_value = value.as_double();

      if (pred_value >= max_val) return 0.0;
      if (pred_value <= min_val) return 1.0;

      if (range > 0) {
        return (max_val - pred_value) / range;
      }

      return 0.5;
    }

    case PredicateOperator::GREATER_EQUAL: {
      pred_value = value.as_double();

      if (pred_value > max_val) return 0.0;
      if (pred_value <= min_val) return 1.0;

      if (range > 0) {
        double sel = (max_val - pred_value) / range;
        if (distinct_count > 0) {
          sel += 0.5 / distinct_count;
        }
        return std::min(1.0, sel);
      }

      return 1.0;
    }

    case PredicateOperator::BETWEEN: {
      double low = value.as_double();
      double high = value2.as_double();

      // No overlap
      if (high < min_val || low > max_val) {
        return 0.0;
      }

      // Full coverage
      if (low <= min_val && high >= max_val) {
        return 1.0;
      }

      // Partial overlap
      if (range > 0) {
        double overlap_min = std::max(low, min_val);
        double overlap_max = std::min(high, max_val);
        double overlap_range = overlap_max - overlap_min;
        return overlap_range / range;
      }

      return 0.5;
    }

    case PredicateOperator::NOT_BETWEEN: {
      // Complement of BETWEEN
      double between_sel = 0.0;

      double low = value.as_double();
      double high = value2.as_double();

      if (high < min_val || low > max_val) {
        between_sel = 0.0;
      } else if (low <= min_val && high >= max_val) {
        between_sel = 1.0;
      } else if (range > 0) {
        double overlap_min = std::max(low, min_val);
        double overlap_max = std::min(high, max_val);
        double overlap_range = overlap_max - overlap_min;
        between_sel = overlap_range / range;
      } else {
        between_sel = 0.5;
      }

      return 1.0 - between_sel;
    }

    case PredicateOperator::IN: {
      if (value_list.empty()) return 0.0;

      if (distinct_count > 0) {
        // Estimate: min(1.0, num_values / distinct_count)
        return std::min(1.0, static_cast<double>(value_list.size()) / distinct_count);
      }

      // Heuristic: 10% per value, capped at 1.0
      return std::min(1.0, 0.1 * value_list.size());
    }

    case PredicateOperator::NOT_IN: {
      if (value_list.empty()) return 1.0;

      if (distinct_count > 0) {
        double in_sel = std::min(1.0, static_cast<double>(value_list.size()) / distinct_count);
        return 1.0 - in_sel;
      }

      return std::max(0.0, 1.0 - 0.1 * value_list.size());
    }

    case PredicateOperator::IS_NULL: {
      if (!has_null) return 0.0;

      if (estimated_total_rows > 0) {
        return static_cast<double>(null_count) / estimated_total_rows;
      }

      return 0.05;
    }

    case PredicateOperator::IS_NOT_NULL: {
      if (!has_null) return 1.0;

      if (estimated_total_rows > 0) {
        return non_null_rows / estimated_total_rows;
      }

      return 0.95;
    }

    case PredicateOperator::LIKE:
    case PredicateOperator::NOT_LIKE: {
      // Pattern analysis could be done here
      // For now, use rough estimate
      double like_sel = 0.2;  // 20% for LIKE
      return (op == PredicateOperator::LIKE) ? like_sel : (1.0 - like_sel);
    }

    case PredicateOperator::REGEXP:
    case PredicateOperator::NOT_REGEXP: {
      double regexp_sel = 0.15;  // 15% for REGEXP
      return (op == PredicateOperator::REGEXP) ? regexp_sel : (1.0 - regexp_sel);
    }

    default:
      return 0.5;
  }
}

PredicateValue Simple_Predicate::extract_value(const unsigned char *data) const {
  // Note: This is a simplified implementation
  // In real implementation, you would properly handle each MySQL type

  switch (column_type) {
    case MYSQL_TYPE_TINY:
      return PredicateValue(static_cast<int64_t>(*reinterpret_cast<const int8_t *>(data)));

    case MYSQL_TYPE_SHORT:
      return PredicateValue(static_cast<int64_t>(*reinterpret_cast<const int16_t *>(data)));

    case MYSQL_TYPE_LONG:
      return PredicateValue(static_cast<int64_t>(*reinterpret_cast<const int32_t *>(data)));

    case MYSQL_TYPE_LONGLONG:
      return PredicateValue(*reinterpret_cast<const int64_t *>(data));

    case MYSQL_TYPE_FLOAT:
      return PredicateValue(static_cast<double>(*reinterpret_cast<const float *>(data)));

    case MYSQL_TYPE_DOUBLE:
      return PredicateValue(*reinterpret_cast<const double *>(data));

    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING: {
      // Assume null-terminated string
      return PredicateValue(reinterpret_cast<const char *>(data));
    }

    default:
      return PredicateValue::null_value();
  }
}

bool Simple_Predicate::evaluate_like(const std::string &str, const std::string &pattern) const {
  // Simplified implementation: convert LIKE pattern to regex
  std::string regex_pattern = pattern;

  // Escape special characters
  std::string escaped;
  for (char c : regex_pattern) {
    switch (c) {
      case '%':
        escaped += ".*";
        break;
      case '_':
        escaped += ".";
        break;
      case '.':
      case '*':
      case '+':
      case '?':
      case '|':
      case '(':
      case ')':
      case '[':
      case ']':
      case '{':
      case '}':
      case '^':
      case '$':
      case '\\':
        escaped += '\\';
        escaped += c;
        break;
      default:
        escaped += c;
    }
  }

  try {
    std::regex re(escaped, std::regex::icase);
    return std::regex_match(str, re);
  } catch (const std::regex_error &) {
    return false;
  }
}

bool Simple_Predicate::evaluate_regexp(const std::string &str, const std::string &pattern) const {
  try {
    std::regex re(pattern);
    return std::regex_search(str, re);
  } catch (const std::regex_error &) {
    return false;
  }
}

void Simple_Predicate::evaluate_comparison_batch(const unsigned char **row_data, size_t num_rows, size_t num_columns,
                                                 bit_array_t &result) const {
  // Here SIMD instructions could be used for optimization
  // Simplified version: loop unrolling

  constexpr size_t UNROLL = 4;
  size_t i = 0;

  // Process with loop unrolling
  for (; i + UNROLL <= num_rows; i += UNROLL) {
    for (size_t j = 0; j < UNROLL; j++) {
      bool match = evaluate(&row_data[(i + j) * num_columns], num_columns);
      if (match) {
        Utils::Util::bit_array_set(&result, i + j);
      } else {
        Utils::Util::bit_array_reset(&result, i + j);
      }
    }
  }

  // Process remainder
  for (; i < num_rows; i++) {
    bool match = evaluate(&row_data[i * num_columns], num_columns);
    if (match) {
      Utils::Util::bit_array_set(&result, i);
    } else {
      Utils::Util::bit_array_reset(&result, i);
    }
  }
}

void Simple_Predicate::evaluate_between_batch(const unsigned char **row_data, size_t num_rows, size_t num_columns,
                                              bit_array_t &result) const {
  // Simplified implementation
  for (size_t i = 0; i < num_rows; i++) {
    bool match = evaluate(&row_data[i * num_columns], num_columns);
    if (match) {
      Utils::Util::bit_array_set(&result, i);
    } else {
      Utils::Util::bit_array_reset(&result, i);
    }
  }
}

Compound_Predicate::Compound_Predicate(PredicateOperator op_type) : Predicate(op_type) {}

void Compound_Predicate::add_child(std::unique_ptr<Predicate> child) { children.push_back(std::move(child)); }

bool Compound_Predicate::evaluate(const unsigned char **row_data, size_t num_columns) const {
  switch (op) {
    case PredicateOperator::AND: {
      for (const auto &child : children) {
        if (!child->evaluate(row_data, num_columns)) {
          return false;  // Short-circuit evaluation
        }
      }
      return true;
    }

    case PredicateOperator::OR: {
      for (const auto &child : children) {
        if (child->evaluate(row_data, num_columns)) {
          return true;  // Short-circuit evaluation
        }
      }
      return false;
    }

    case PredicateOperator::NOT: {
      if (children.empty()) return false;
      return !children[0]->evaluate(row_data, num_columns);
    }

    default:
      return false;
  }
}

void Compound_Predicate::evaluate_batch(const unsigned char **row_data, size_t num_rows, size_t num_columns,
                                        bit_array_t &result) const {
  if (children.empty()) return;

  switch (op) {
    case PredicateOperator::AND: {
      // Initialize to all 1s
      for (size_t i = 0; i < num_rows; i++) {
        Utils::Util::bit_array_set(&result, i);
      }

      // Evaluate each child predicate and AND with result
      bit_array_t child_result(num_rows);
      for (const auto &child : children) {
        // Reset child result
        child->evaluate_batch(row_data, num_rows, num_columns, child_result);

        // result = result AND child_result
        for (size_t i = 0; i < num_rows; i++) {
          if (!Utils::Util::bit_array_get(&child_result, i)) {
            Utils::Util::bit_array_reset(&result, i);
          }
        }
      }

      break;
    }

    case PredicateOperator::OR: {
      // Initialize to all 0s
      for (size_t i = 0; i < num_rows; i++) {
        Utils::Util::bit_array_reset(&result, i);
      }

      // Evaluate each child predicate and OR with result
      bit_array_t child_result(num_rows);
      for (const auto &child : children) {
        // Reset child result
        child->evaluate_batch(row_data, num_rows, num_columns, child_result);

        // result = result OR child_result
        for (size_t i = 0; i < num_rows; i++) {
          if (Utils::Util::bit_array_get(&child_result, i)) {
            Utils::Util::bit_array_set(&result, i);
          }
        }
      }

      break;
    }

    case PredicateOperator::NOT: {
      if (children.empty()) return;

      // Evaluate child predicate
      children[0]->evaluate_batch(row_data, num_rows, num_columns, result);

      // Invert
      for (size_t i = 0; i < num_rows; i++) {
        if (Utils::Util::bit_array_get(&result, i)) {
          Utils::Util::bit_array_reset(&result, i);
        } else {
          Utils::Util::bit_array_set(&result, i);
        }
      }
      break;
    }

    default:
      break;
  }
}

bool Compound_Predicate::can_use_storage_index() const {
  // Only AND can effectively use Storage Index
  if (op == PredicateOperator::AND) {
    for (const auto &child : children) {
      if (child->can_use_storage_index()) {
        return true;
      }
    }
  }
  return false;
}

bool Compound_Predicate::apply_storage_index(const StorageIndex *storage_index) const {
  if (!storage_index) return false;

  switch (op) {
    case PredicateOperator::AND: {
      // AND: If any child predicate can skip, entire IMCU can be skipped
      for (const auto &child : children) {
        if (child->apply_storage_index(storage_index)) {
          return true;  // Can skip
        }
      }
      return false;
    }

    case PredicateOperator::OR: {
      // OR: Only if all child predicates can skip, then can skip
      for (const auto &child : children) {
        if (!child->apply_storage_index(storage_index)) {
          return false;  // Cannot skip
        }
      }
      return !children.empty();
    }

    case PredicateOperator::NOT: {
      // NOT: Not suitable for Storage Index filtering
      return false;
    }

    default:
      return false;
  }
}

std::vector<uint32_t> Compound_Predicate::get_columns() const {
  std::vector<uint32_t> columns;

  for (const auto &child : children) {
    auto child_cols = child->get_columns();
    columns.insert(columns.end(), child_cols.begin(), child_cols.end());
  }

  // Remove duplicates
  std::sort(columns.begin(), columns.end());
  columns.erase(std::unique(columns.begin(), columns.end()), columns.end());

  return columns;
}

std::unique_ptr<Predicate> Compound_Predicate::clone() const {
  auto cloned = std::make_unique<Compound_Predicate>(op);

  for (const auto &child : children) {
    cloned->add_child(child->clone());
  }

  return cloned;
}

std::string Compound_Predicate::to_string() const {
  if (children.empty()) return "()";

  std::ostringstream oss;

  switch (op) {
    case PredicateOperator::AND:
      oss << "(";
      for (size_t i = 0; i < children.size(); i++) {
        if (i > 0) oss << " AND ";
        oss << children[i]->to_string();
      }
      oss << ")";
      break;

    case PredicateOperator::OR:
      oss << "(";
      for (size_t i = 0; i < children.size(); i++) {
        if (i > 0) oss << " OR ";
        oss << children[i]->to_string();
      }
      oss << ")";
      break;

    case PredicateOperator::NOT:
      oss << "NOT (" << children[0]->to_string() << ")";
      break;

    default:
      oss << "(?)";
      break;
  }

  return oss.str();
}

double Compound_Predicate::estimate_selectivity(const StorageIndex *storage_index) const {
  if (children.empty()) return 1.0;

  switch (op) {
    case PredicateOperator::AND: {
      // AND: Multiply selectivities
      double selectivity = 1.0;
      for (const auto &child : children) {
        selectivity *= child->estimate_selectivity(storage_index);
      }
      return selectivity;
    }

    case PredicateOperator::OR: {
      // OR: 1 - (1-s1) * (1-s2) * ...
      double prob_none = 1.0;
      for (const auto &child : children) {
        double s = child->estimate_selectivity(storage_index);
        prob_none *= (1.0 - s);
      }
      return 1.0 - prob_none;
    }

    case PredicateOperator::NOT: {
      double s = children[0]->estimate_selectivity(storage_index);
      return 1.0 - s;
    }

    default:
      return 0.5;
  }
}

std::unique_ptr<Simple_Predicate> Predicate_Builder::create_simple(uint32_t col_id, PredicateOperator op,
                                                                   const PredicateValue &value, enum_field_types type) {
  return std::make_unique<Simple_Predicate>(col_id, op, value, type);
}

std::unique_ptr<Simple_Predicate> Predicate_Builder::create_between(uint32_t col_id, const PredicateValue &min_val,
                                                                    const PredicateValue &max_val,
                                                                    enum_field_types type) {
  return std::make_unique<Simple_Predicate>(col_id, min_val, max_val, type);
}

std::unique_ptr<Simple_Predicate> Predicate_Builder::create_in(uint32_t col_id,
                                                               const std::vector<PredicateValue> &values,
                                                               bool is_not_in, enum_field_types type) {
  return std::make_unique<Simple_Predicate>(col_id, values, is_not_in, type);
}

std::unique_ptr<Compound_Predicate> Predicate_Builder::create_and(std::vector<std::unique_ptr<Predicate>> predicates) {
  auto compound = std::make_unique<Compound_Predicate>(PredicateOperator::AND);

  for (auto &pred : predicates) {
    compound->add_child(std::move(pred));
  }

  return compound;
}

std::unique_ptr<Compound_Predicate> Predicate_Builder::create_or(std::vector<std::unique_ptr<Predicate>> predicates) {
  auto compound = std::make_unique<Compound_Predicate>(PredicateOperator::OR);

  for (auto &pred : predicates) {
    compound->add_child(std::move(pred));
  }

  return compound;
}

std::unique_ptr<Compound_Predicate> Predicate_Builder::create_not(std::unique_ptr<Predicate> predicate) {
  auto compound = std::make_unique<Compound_Predicate>(PredicateOperator::NOT);
  compound->add_child(std::move(predicate));
  return compound;
}

std::unique_ptr<Predicate> Predicate_Optimizer::optimize(std::unique_ptr<Predicate> predicate) {
  if (!predicate) return nullptr;

  // 1. Constant folding
  predicate = fold_constants(std::move(predicate));

  // 2. Predicate pushdown (extract pushable simple predicates)
  predicate = push_down_predicates(std::move(predicate));

  // 3. Predicate reordering (low selectivity first)
  predicate = reorder_predicates(std::move(predicate));

  return predicate;
}

std::pair<std::vector<Predicate *>, std::vector<Predicate *>> Predicate_Optimizer::separate_index_predicates(
    Predicate *root) {
  std::vector<Predicate *> index_preds;
  std::vector<Predicate *> non_index_preds;

  if (!root) return {index_preds, non_index_preds};

  // Recursive separation
  separate_recursive(root, index_preds, non_index_preds);

  return {index_preds, non_index_preds};
}

std::unique_ptr<Predicate> Predicate_Optimizer::fold_constants(std::unique_ptr<Predicate> pred) {
  // Simplified implementation: detect always-true/always-false predicates

  // Example: age > 10 AND age > 5 => age > 10
  // Here only simple examples

  return pred;
}

std::unique_ptr<Predicate> Predicate_Optimizer::push_down_predicates(std::unique_ptr<Predicate> pred) {
  // Simplified implementation
  return pred;
}

std::unique_ptr<Predicate> Predicate_Optimizer::reorder_predicates(std::unique_ptr<Predicate> pred) {
  auto *compound = dynamic_cast<Compound_Predicate *>(pred.get());

  if (!compound || compound->op != PredicateOperator::AND) {
    return pred;
  }

  // Sort by selectivity
  std::sort(compound->children.begin(), compound->children.end(),
            [](const std::unique_ptr<Predicate> &a, const std::unique_ptr<Predicate> &b) {
              return a->estimate_selectivity() < b->estimate_selectivity();
            });

  return pred;
}

void Predicate_Optimizer::separate_recursive(Predicate *pred, std::vector<Predicate *> &index_preds,
                                             std::vector<Predicate *> &non_index_preds) {
  if (pred->can_use_storage_index()) {
    index_preds.push_back(pred);
  } else {
    non_index_preds.push_back(pred);
  }

  // Recursively process compound predicates
  auto *compound = dynamic_cast<Compound_Predicate *>(pred);
  if (compound) {
    for (auto &child : compound->children) {
      separate_recursive(child.get(), index_preds, non_index_preds);
    }
  }
}

bool Predicate_Executor::execute(const Predicate *predicate, const unsigned char **row_data, size_t num_columns) {
  if (!predicate) return true;
  return predicate->evaluate(row_data, num_columns);
}

void Predicate_Executor::execute_batch(const Predicate *predicate, const unsigned char **row_data, size_t num_rows,
                                       size_t num_columns, bit_array_t &result) {
  if (!predicate) {
    // No predicate, all rows match
    for (size_t i = 0; i < num_rows; i++) {
      Utils::Util::bit_array_set(&result, i);
    }
    return;
  }

  predicate->evaluate_batch(row_data, num_rows, num_columns, result);
}

bool Predicate_Executor::apply_storage_index(const Predicate *predicate, const StorageIndex *storage_index) {
  if (!predicate || !storage_index) return false;

  return predicate->apply_storage_index(storage_index);
}
}  // namespace Imcs
}  // namespace ShannonBase