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
#include "storage/innobase/include/mach0data.ic"

#include "storage/rapid_engine/imcs/imcu.h"
#include "storage/rapid_engine/utils/utils.h"  //bit_array_xxx

namespace ShannonBase {
namespace Imcs {
bool Simple_Predicate::evaluate(const uchar *&input_value, bool low_order) const {
  // Handle NULL
  if (!input_value) {
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
  PredicateValue col_value = extract_value(input_value, low_order);
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

void Simple_Predicate::evaluate_batch(const std::vector<const uchar *> &input_values, bit_array_t &result,
                                      size_t batch_num) const {
  size_t i = 0;
  auto values_sz = input_values.size();
  // Process with loop unrolling
  for (; i + batch_num <= values_sz; i += batch_num) {
    for (size_t j = 0; j < batch_num; j++) {
      auto input_value = input_values[i + j];
      bool match = evaluate(input_value);
      (match) ? Utils::Util::bit_array_set(&result, i + j) : Utils::Util::bit_array_reset(&result, i + j);
    }
  }

  // Process remainder
  for (; i < values_sz; i++) {
    auto input_value = input_values[i];
    bool match = evaluate(input_value);
    (match) ? Utils::Util::bit_array_set(&result, i) : Utils::Util::bit_array_reset(&result, i);
  }
}

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
      if (pred_value < min_val || pred_value > max_val) return 0.0;
      // Use distinct count if available
      return (distinct_count > 0) ? 1.0 / distinct_count : 0.1;  // Default
    } break;
    case PredicateOperator::NOT_EQUAL: {
      pred_value = value.as_double();

      // Value out of range - all rows match
      if (pred_value < min_val || pred_value > max_val) return 1.0;
      return (distinct_count > 0) ? (distinct_count - 1.0) / distinct_count : 0.9;
    } break;
    case PredicateOperator::LESS_THAN: {
      pred_value = value.as_double();

      if (pred_value <= min_val) return 0.0;
      if (pred_value >= max_val) return 1.0;

      return (range > 0) ? (pred_value - min_val) / range : 0.5;
    } break;
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
    } break;
    case PredicateOperator::GREATER_THAN: {
      pred_value = value.as_double();

      if (pred_value >= max_val) return 0.0;
      if (pred_value <= min_val) return 1.0;
      return (range > 0) ? (max_val - pred_value) / range : 0.5;
    } break;
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
    } break;
    case PredicateOperator::BETWEEN: {
      double low = value.as_double();
      double high = value2.as_double();

      // No overlap
      if (high < min_val || low > max_val) return 0.0;
      // Full coverage
      if (low <= min_val && high >= max_val) return 1.0;

      // Partial overlap
      if (range > 0) {
        double overlap_min = std::max(low, min_val);
        double overlap_max = std::min(high, max_val);
        double overlap_range = overlap_max - overlap_min;
        return overlap_range / range;
      }
      return 0.5;
    } break;
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
    } break;
    case PredicateOperator::IN: {
      if (value_list.empty()) return 0.0;

      return (distinct_count > 0)
                 ? std::min(1.0, static_cast<double>(value_list.size()) /
                                     distinct_count)        // Estimate: min(1.0, num_values / distinct_count)
                 : std::min(1.0, 0.1 * value_list.size());  // Heuristic: 10% per value, capped at 1.0;
    } break;
    case PredicateOperator::NOT_IN: {
      if (value_list.empty()) return 1.0;

      if (distinct_count > 0) {
        double in_sel = std::min(1.0, static_cast<double>(value_list.size()) / distinct_count);
        return 1.0 - in_sel;
      }
      return std::max(0.0, 1.0 - 0.1 * value_list.size());
    } break;
    case PredicateOperator::IS_NULL: {
      if (!has_null) return 0.0;
      return (estimated_total_rows > 0) ? static_cast<double>(null_count) / estimated_total_rows : 0.05;
    }
    case PredicateOperator::IS_NOT_NULL: {
      if (!has_null) return 1.0;
      return (estimated_total_rows > 0) ? non_null_rows / estimated_total_rows : 0.95;
    } break;
    case PredicateOperator::LIKE:
    case PredicateOperator::NOT_LIKE: {
      // Pattern analysis could be done here
      // For now, use rough estimate
      double like_sel = 0.2;  // 20% for LIKE
      return (op == PredicateOperator::LIKE) ? like_sel : (1.0 - like_sel);
    } break;
    case PredicateOperator::REGEXP:
    case PredicateOperator::NOT_REGEXP: {
      double regexp_sel = 0.15;  // 15% for REGEXP
      return (op == PredicateOperator::REGEXP) ? regexp_sel : (1.0 - regexp_sel);
    } break;
    default:
      return 0.5;
  }
}

PredicateValue Simple_Predicate::extract_value(const uchar *data, bool low_order) const {
  assert(field_meta->field_index() == column_id && field_meta->type() == column_type);
  switch (column_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      auto val = Utils::Util::get_field_numeric<int64_t>(field_meta, data, nullptr, low_order);
      return PredicateValue(val);
    } break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_DECIMAL: {
      auto val = Utils::Util::get_field_numeric<double>(field_meta, data, nullptr, low_order);
      return PredicateValue(val);
    } break;
    case MYSQL_TYPE_STRING: {  // padding with ` ` in store formate, so trim the extra space
      uint32 pred_length = value.as_string().length();
      auto length = std::min(pred_length, field_meta->pack_length());
      std::string val(reinterpret_cast<const char *>(data), length);
      return PredicateValue(val);
    } break;
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      std::string val(reinterpret_cast<const char *>(data));
      return PredicateValue(val);
    } break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2: {
      auto val = Utils::Util::get_field_numeric<int64_t>(field_meta, data, nullptr, low_order);
      return PredicateValue(val);
    } break;
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB: {
      // TODO: ref: row0row.cpp: RowBuffer::extract_field_data
      assert(false);
    } break;
    default:
      return PredicateValue::null_value();
  }
  return PredicateValue::null_value();
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

bool Compound_Predicate::evaluate(const uchar *&input_value, bool low_order) const {
  switch (op) {
    case PredicateOperator::AND: {
      for (const auto &child : children) {
        if (!child->evaluate(input_value)) return false;  // Short-circuit evaluation
      }
      return true;
    } break;
    case PredicateOperator::OR: {
      for (const auto &child : children) {
        if (child->evaluate(input_value)) return true;  // Short-circuit evaluation
      }
      return false;
    } break;
    case PredicateOperator::NOT: {
      if (children.empty()) return false;
      return !children[0]->evaluate(input_value);
    } break;
    default:
      return false;
  }
}

void Compound_Predicate::evaluate_batch(const std::vector<const uchar *> &input_values, bit_array_t &result,
                                        size_t batch_num) const {
  if (children.empty()) return;

  auto input_sz = input_values.size();
  switch (op) {
    case PredicateOperator::AND: {
      // Initialize to all 1s
      for (size_t i = 0; i < input_sz; i++) {
        Utils::Util::bit_array_set(&result, i);
      }
      // Evaluate each child predicate and AND with result
      bit_array_t child_result(input_sz);
      for (const auto &child : children) {
        // Reset child result
        child->evaluate_batch(input_values, child_result, batch_num);
        // result = result AND child_result
        for (size_t i = 0; i < input_sz; i++) {
          if (!Utils::Util::bit_array_get(&child_result, i)) {
            Utils::Util::bit_array_reset(&result, i);
          }
        }
      }
    } break;
    case PredicateOperator::OR: {
      // Initialize to all 0s
      for (size_t i = 0; i < input_sz; i++) {
        Utils::Util::bit_array_reset(&result, i);
      }
      // Evaluate each child predicate and OR with result
      bit_array_t child_result(input_sz);
      for (const auto &child : children) {
        // Reset child result
        child->evaluate_batch(input_values, child_result, batch_num);
        // result = result OR child_result
        for (size_t i = 0; i < input_sz; i++) {
          if (Utils::Util::bit_array_get(&child_result, i)) {
            Utils::Util::bit_array_set(&result, i);
          }
        }
      }
    } break;
    case PredicateOperator::NOT: {
      if (children.empty()) return;
      // Evaluate child predicate
      children[0]->evaluate_batch(input_values, result, batch_num);
      // Invert
      for (size_t i = 0; i < input_sz; i++) {
        Utils::Util::bit_array_get(&result, i) ? Utils::Util::bit_array_reset(&result, i)
                                               : Utils::Util::bit_array_set(&result, i);
      }
    } break;
    default:
      break;
  }
}

std::vector<uint32> Compound_Predicate::get_columns() const {
  std::vector<uint32> columns;
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
    } break;
    case PredicateOperator::OR: {
      // OR: 1 - (1-s1) * (1-s2) * ...
      double prob_none = 1.0;
      for (const auto &child : children) {
        double s = child->estimate_selectivity(storage_index);
        prob_none *= (1.0 - s);
      }
      return 1.0 - prob_none;
    } break;
    case PredicateOperator::NOT: {
      double s = children[0]->estimate_selectivity(storage_index);
      return 1.0 - s;
    } break;
    default:
      return 0.5;
  }
}

std::unique_ptr<Simple_Predicate> Predicate_Builder::create_simple(uint32 col_id, PredicateOperator op,
                                                                   const PredicateValue &value, enum_field_types type) {
  return std::make_unique<Simple_Predicate>(col_id, op, value, type);
}

std::unique_ptr<Simple_Predicate> Predicate_Builder::create_between(uint32 col_id, const PredicateValue &min_val,
                                                                    const PredicateValue &max_val,
                                                                    enum_field_types type) {
  return std::make_unique<Simple_Predicate>(col_id, min_val, max_val, type);
}

std::unique_ptr<Simple_Predicate> Predicate_Builder::create_in(uint32 col_id, const std::vector<PredicateValue> &values,
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
}  // namespace Imcs
}  // namespace ShannonBase