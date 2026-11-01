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

   The fundmental code for imcs.
*/
#include "storage/rapid_engine/executor/iterators/expression_iterator.h"

#include <cstring>
#include <limits>
#include "sql/item_cmpfunc.h"
#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "storage/rapid_engine/utils/decimal_sum.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace Executor {
namespace {
bool Rescale(int128_t value, unsigned scale, int128_t *result) {
  while (scale--) {
    if (SHANNON_MUL_OVERFLOW(value, int128_t{10}, &value)) return false;
  }
  *result = value;
  return true;
}

// Inverse of DecodePackedDecimal128. No Field buffer or decimal text round trip.
bool EncodeDecimal(int128_t value, unsigned precision, unsigned scale, uchar *output) {
  if (!precision || precision > DECIMAL_MAX_PRECISION || scale > precision || scale > DECIMAL_MAX_SCALE) return false;
  constexpr unsigned bytes[] = {0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
  constexpr uint32_t powers[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000};
  const bool negative = value < 0;
  uint128_t magnitude = negative ? static_cast<uint128_t>(-(value + 1)) + 1 : value;
  size_t offset = decimal_bin_size(precision, scale);

  const auto group = [&](unsigned digits) {
    uint32_t part = magnitude % powers[digits];
    magnitude /= powers[digits];
    for (unsigned i = 0; i < bytes[digits]; ++i) {
      output[--offset] = static_cast<uchar>(part) ^ (negative ? 0xff : 0);
      part >>= 8;
    }
  };

  if (scale % 9) group(scale % 9);

  for (unsigned i = 0; i < scale / 9; ++i) group(9);

  const unsigned integers = precision - scale;
  for (unsigned i = 0; i < integers / 9; ++i) group(9);

  if (integers % 9) group(integers % 9);

  output[0] ^= 0x80;
  return magnitude == 0;
}
}  // namespace

bool VectorizedExpression::Compile(Item *expression) {
  m_nodes.clear();
  m_values.clear();
  m_rows = 0;
  size_t root{0};
  if (CompileNode(expression, &root, 0)) return true;

  m_nodes.clear();
  return false;
}

bool VectorizedExpression::CompileNode(Item *expression, size_t *index, unsigned depth) {
  if (expression == nullptr || depth > 16 || m_nodes.size() >= 64) return false;

  Item *item = expression->real_item();
  if (!item || item->unsigned_flag) return false;

  Node node;
  node.integer = item->result_type() == INT_RESULT;

  if (!node.integer && item->result_type() != DECIMAL_RESULT && item->type() != Item::NULL_ITEM) return false;

  node.scale = node.integer ? 0 : item->decimals;
  if (node.scale > DECIMAL_MAX_SCALE) return false;

  if (item->type() == Item::FIELD_ITEM) {
    node.op = Op::FIELD;
    node.field = down_cast<Item_field *>(item)->field;
    if (!node.field || node.field->is_unsigned()) return false;

    switch (node.field->type()) {
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_NEWDECIMAL:
        break;
      default:
        return false;
    }
  } else if (item->type() == Item::INT_ITEM) {
    node.constant = {item->val_int(), false};
  } else if (item->type() == Item::DECIMAL_ITEM) {
    my_decimal buffer;
    const my_decimal *value = item->val_decimal(&buffer);
    uchar packed[DECIMAL_MAX_FIELD_SIZE] = {0};
    const unsigned precision = item->decimal_precision();
    if (!precision || precision > DECIMAL_MAX_PRECISION || node.scale > precision) return false;

    if (!value || my_decimal2binary(E_DEC_FATAL_ERROR, value, packed, precision, node.scale) != E_DEC_OK ||
        !Utils::DecodePackedDecimal128(packed, decimal_bin_size(precision, node.scale), precision, node.scale,
                                       &node.constant.number))
      return false;
    node.constant.is_null = false;
  } else if (item->type() == Item::NULL_ITEM) {
    node.scale = 0;
  } else if (item->type() == Item::FUNC_ITEM) {
    auto *func = down_cast<Item_func *>(item);
    bool comparison = false;
    switch (func->functype()) {
      case Item_func::PLUS_FUNC:
        node.op = Op::ADD;
        break;
      case Item_func::MINUS_FUNC:
        node.op = Op::SUBTRACT;
        break;
      case Item_func::MUL_FUNC:
        node.op = Op::MULTIPLY;
        break;
      case Item_func::NEG_FUNC:
        node.op = Op::NEGATE;
        break;
      case Item_func::EQ_FUNC:
        node.op = Op::EQ;
        comparison = true;
        break;
      case Item_func::NE_FUNC:
        node.op = Op::NE;
        comparison = true;
        break;
      case Item_func::LT_FUNC:
        node.op = Op::LT;
        comparison = true;
        break;
      case Item_func::LE_FUNC:
        node.op = Op::LE;
        comparison = true;
        break;
      case Item_func::GT_FUNC:
        node.op = Op::GT;
        comparison = true;
        break;
      case Item_func::GE_FUNC:
        node.op = Op::GE;
        comparison = true;
        break;
      default:
        return false;
    }

    if (func->argument_count() != (node.op == Op::NEGATE ? 1U : 2U)) return false;

    if (comparison && down_cast<Item_bool_func2 *>(func)->compare_type() != INT_RESULT &&
        down_cast<Item_bool_func2 *>(func)->compare_type() != DECIMAL_RESULT)
      return false;

    if (!CompileNode(func->arguments()[0], &node.left, depth + 1)) return false;

    node.right = node.left;
    if (func->argument_count() == 2 && !CompileNode(func->arguments()[1], &node.right, depth + 1)) return false;

    const unsigned left_scale = m_nodes[node.left].scale, right_scale = m_nodes[node.right].scale;
    const unsigned exact_scale = node.op == Op::MULTIPLY ? left_scale + right_scale : std::max(left_scale, right_scale);
    // Avoid MySQL's scale clamp: it would change the value being accumulated.
    if (!comparison && exact_scale != node.scale) return false;
  } else {
    return false;
  }

  if (!TryGrow(m_nodes, m_nodes.size() + 1)) return false;

  *index = m_nodes.size();
  m_nodes.push_back(node);
  return true;
}

bool VectorizedExpression::Evaluate(const std::pmr::vector<ColumnChunk> &chunks, size_t rows,
                                    const std::pmr::vector<uint32_t> *selection) {
  m_rows = 0;

  if (m_nodes.empty() || rows > SIZE_MAX / m_nodes.size()) return false;

  if (selection && !selection->empty() && selection->size() != rows) return false;

  if (rows == 0) return true;

  const size_t size = rows * m_nodes.size();
  if (!TryReserve(m_values, size)) return false;

  m_values.resize(size);
  for (size_t instruction = 0; instruction < m_nodes.size(); ++instruction) {
    const Node &node = m_nodes[instruction];
    Value *output = m_values.data() + instruction * rows;
    if (node.op == Op::CONSTANT) {
      std::fill_n(output, rows, node.constant);
      continue;
    }

    if (node.op == Op::FIELD) {
      const ColumnChunk *input = nullptr;
      for (const auto &chunk : chunks) {
        if (chunk.valid() && chunk.source_field() == node.field) {
          input = &chunk;
          break;
        }
      }

      if (!input || input->width() != node.field->pack_length()) return false;

      for (size_t row = 0; row < rows; ++row) {
        const size_t physical = selection && !selection->empty() ? (*selection)[row] : row;
        if (physical >= input->size()) return false;

        output[row] = Value{};
        if (input->nullable_fast(physical)) continue;

        output[row].is_null = false;
        const uchar *data = input->data_fast(physical);
        if (node.field->type() == MYSQL_TYPE_LONG) {
          int32_t value;
          std::memcpy(&value, data, sizeof(value));
          output[row].number = value;
        } else if (node.field->type() == MYSQL_TYPE_LONGLONG) {
          int64_t value;
          std::memcpy(&value, data, sizeof(value));
          output[row].number = value;
        } else if (!Utils::DecodePackedDecimal128(data, input->width(),
                                                  down_cast<Field_new_decimal *>(node.field)->precision, node.scale,
                                                  &output[row].number)) {
          return false;
        }
      }
      continue;
    }

    const Value *left = m_values.data() + node.left * rows, *right = m_values.data() + node.right * rows;
    const unsigned left_scale = m_nodes[node.left].scale, right_scale = m_nodes[node.right].scale;
    int128_t left_factor = 1, right_factor = 1;
    if (node.op != Op::MULTIPLY && node.op != Op::NEGATE) {
      const unsigned scale = std::max(left_scale, right_scale);
      if (!Rescale(1, scale - left_scale, &left_factor) || !Rescale(1, scale - right_scale, &right_factor))
        return false;
    }

    for (size_t row = 0; row < rows; ++row) {
      output[row] = Value{};
      if (left[row].is_null || right[row].is_null) continue;
      int128_t a = left[row].number, b = right[row].number, result = 0;
      if ((left_factor != 1 && SHANNON_MUL_OVERFLOW(a, left_factor, &a)) ||
          (right_factor != 1 && SHANNON_MUL_OVERFLOW(b, right_factor, &b)))
        return false;
      switch (node.op) {
        case Op::ADD:
          if (SHANNON_ADD_OVERFLOW(a, b, &result)) return false;
          break;
        case Op::SUBTRACT:
          if (SHANNON_SUB_OVERFLOW(a, b, &result)) return false;
          break;
        case Op::MULTIPLY:
          if (SHANNON_MUL_OVERFLOW(a, b, &result)) return false;
          break;
        case Op::NEGATE:
          if (SHANNON_SUB_OVERFLOW(int128_t{0}, a, &result)) return false;
          break;
        case Op::EQ:
          result = a == b;
          break;
        case Op::NE:
          result = a != b;
          break;
        case Op::LT:
          result = a < b;
          break;
        case Op::LE:
          result = a <= b;
          break;
        case Op::GT:
          result = a > b;
          break;
        case Op::GE:
          result = a >= b;
          break;
        default:
          return false;
      }
      if (node.integer &&
          (result < std::numeric_limits<int64_t>::min() || result > std::numeric_limits<int64_t>::max()))
        return false;
      output[row] = {result, false};
    }
  }
  m_rows = rows;
  return true;
}

bool VectorizedExpression::Store(ColumnChunk *target, size_t rows) const {
  if (m_nodes.empty() || m_rows != rows || !target || !target->source_field() ||
      target->source_field()->type() != MYSQL_TYPE_NEWDECIMAL)
    return false;
  const auto *field = down_cast<Field_new_decimal *>(target->source_field());
  if (field->decimals() != m_nodes.back().scale) return false;
  if (rows == 0) return true;
  const Value *values = m_values.data() + (m_nodes.size() - 1) * rows;
  uchar packed[DECIMAL_MAX_FIELD_SIZE];
  for (size_t row = 0; row < rows; ++row) {
    if (!values[row].is_null && !EncodeDecimal(values[row].number, field->precision, field->decimals(), packed))
      return false;
    if (!target->add(values[row].is_null ? nullptr : packed, values[row].is_null ? 0 : target->width(),
                     values[row].is_null))
      return false;
  }
  return true;
}

bool VectorizedExpression::Matches(size_t row) const {
  if (m_nodes.empty() || row >= m_rows) return false;
  const Value &value = m_values[(m_nodes.size() - 1) * m_rows + row];
  return !value.is_null && value.number != 0;
}
}  // namespace Executor
}  // namespace ShannonBase
