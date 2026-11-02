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
// An int128 holds 38 decimal digits exactly. A DECIMAL column wider than that
// still often stores values that fit, because the packed format keeps leading
// zero groups, so fields are accepted and DecodePackedDecimal128() refuses the
// individual batches that do not fit. A wide constant or computed result has no
// such slack: its precision describes the value itself, so it is refused here.
constexpr unsigned kMaxExactPrecision = 38;

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

// NOTE: unlike the MySQL convention used elsewhere in the executor
// (true == error), these helpers return true on SUCCESS.
bool VectorizedExpression::Compile(Item *expression) {
  m_nodes.clear();
  m_numbers.clear();
  m_nulls.clear();
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
  if (!node.integer && item->type() != Item::NULL_ITEM && item->type() != Item::FIELD_ITEM &&
      item->decimal_precision() > kMaxExactPrecision)
    return false;

  if (item->type() == Item::FIELD_ITEM) {
    node.op = Op::FIELD;
    node.field = down_cast<Item_field *>(item)->field;
    if (!node.field || node.field->is_unsigned()) return false;

    switch (node.field->type()) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
        break;
      case MYSQL_TYPE_NEWDECIMAL:
        // Any declared precision up to DECIMAL_MAX_PRECISION is accepted: the
        // column may hold values that fit int128 exactly, and the ones that do
        // not make Evaluate() decline for that batch instead of failing a
        // compile that would otherwise be usable.
        break;
      default:
        return false;
    }
  } else if (item->type() == Item::INT_ITEM) {
    node.op = Op::CONSTANT;
    node.constant = {item->val_int(), false};
  } else if (item->type() == Item::DECIMAL_ITEM) {
    node.op = Op::CONSTANT;
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
    node.op = Op::CONSTANT;
    node.constant = Value{};  // explicitly NULL, not an accident of defaults
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

  if (!Grow(m_nodes, m_nodes.size() + 1)) return false;

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
  if (!Reserve(m_numbers, size) || !Reserve(m_nulls, size)) return false;

  // Every slot read below is written first in this call, so existing slots need
  // no re-initialisation: only grow.
  if (m_numbers.size() < size) m_numbers.resize(size);
  if (m_nulls.size() < size) m_nulls.resize(size);
  for (size_t instruction = 0; instruction < m_nodes.size(); ++instruction) {
    const Node &node = m_nodes[instruction];
    int128_t *out = m_numbers.data() + instruction * rows;
    uint8_t *out_null = m_nulls.data() + instruction * rows;
    if (node.op == Op::CONSTANT) {
      // Operands read a constant with stride 0, so only the root (whose column
      // Store()/Matches()/Result() read) needs all rows filled.
      const size_t fill = instruction + 1 == m_nodes.size() ? rows : 1;
      std::fill_n(out, fill, node.constant.number);
      std::fill_n(out_null, fill, static_cast<uint8_t>(node.constant.is_null));
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

        if (input->nullable_fast(physical)) {
          out[row] = 0;
          out_null[row] = 1;
          continue;
        }

        out_null[row] = 0;
        const uchar *data = input->data_fast(physical);
        if (node.field->type() == MYSQL_TYPE_TINY) {
          out[row] = static_cast<int8_t>(data[0]);
        } else if (node.field->type() == MYSQL_TYPE_SHORT) {
          int16_t value;
          std::memcpy(&value, data, sizeof(value));
          out[row] = value;
        } else if (node.field->type() == MYSQL_TYPE_INT24) {
          const int32_t raw = data[0] | (data[1] << 8) | (data[2] << 16);
          out[row] = (raw ^ 0x800000) - 0x800000;  // sign-extend 24 bits
        } else if (node.field->type() == MYSQL_TYPE_LONG) {
          int32_t value;
          std::memcpy(&value, data, sizeof(value));
          out[row] = value;
        } else if (node.field->type() == MYSQL_TYPE_LONGLONG) {
          int64_t value;
          std::memcpy(&value, data, sizeof(value));
          out[row] = value;
        } else if (!Utils::DecodePackedDecimal128(data, input->width(),
                                                  down_cast<Field_new_decimal *>(node.field)->precision, node.scale,
                                                  &out[row])) {
          return false;
        }
      }
      continue;
    }

    const int128_t *left = m_numbers.data() + node.left * rows, *right = m_numbers.data() + node.right * rows;
    const uint8_t *left_null = m_nulls.data() + node.left * rows, *right_null = m_nulls.data() + node.right * rows;
    const unsigned left_scale = m_nodes[node.left].scale, right_scale = m_nodes[node.right].scale;
    const size_t left_stride = m_nodes[node.left].op == Op::CONSTANT ? 0 : 1;
    const size_t right_stride = m_nodes[node.right].op == Op::CONSTANT ? 0 : 1;
    int128_t left_factor = 1, right_factor = 1;
    if (node.op != Op::MULTIPLY && node.op != Op::NEGATE) {
      const unsigned scale = std::max(left_scale, right_scale);
      if (!Rescale(1, scale - left_scale, &left_factor) || !Rescale(1, scale - right_scale, &right_factor))
        return false;
    }

    // One tight loop per operator: the opcode is loop-invariant, and dispatching
    // it per row keeps the compiler from specializing the body.
    // Integer-only nodes need no scale alignment and fit in int64, so they take
    // a 64-bit overflow check instead of 128-bit arithmetic.
    const bool narrow = node.integer && m_nodes[node.left].integer && m_nodes[node.right].integer && left_factor == 1 &&
                        right_factor == 1;
    const auto run = [&](auto &&apply) -> bool {
      for (size_t row = 0; row < rows; ++row) {
        const size_t li = row * left_stride, ri = row * right_stride;
        if (left_null[li] || right_null[ri]) {
          out[row] = 0;
          out_null[row] = 1;
          continue;
        }
        int128_t a = left[li], b = right[ri], result = 0;
        if ((left_factor != 1 && SHANNON_MUL_OVERFLOW(a, left_factor, &a)) ||
            (right_factor != 1 && SHANNON_MUL_OVERFLOW(b, right_factor, &b)))
          return false;
        if (!apply(a, b, &result)) return false;
        if (node.integer &&
            (result < std::numeric_limits<int64_t>::min() || result > std::numeric_limits<int64_t>::max()))
          return false;
        out[row] = result;
        out_null[row] = 0;
      }
      return true;
    };
    bool ok = false;
    switch (node.op) {
      case Op::ADD:
        ok = narrow ? run([](int128_t a, int128_t b, int128_t *r) {
          int64_t v;
          if (SHANNON_ADD_OVERFLOW(static_cast<int64_t>(a), static_cast<int64_t>(b), &v)) return false;
          *r = v;
          return true;
        })
                    : run([](int128_t a, int128_t b, int128_t *r) { return !SHANNON_ADD_OVERFLOW(a, b, r); });
        break;
      case Op::SUBTRACT:
        ok = narrow ? run([](int128_t a, int128_t b, int128_t *r) {
          int64_t v;
          if (SHANNON_SUB_OVERFLOW(static_cast<int64_t>(a), static_cast<int64_t>(b), &v)) return false;
          *r = v;
          return true;
        })
                    : run([](int128_t a, int128_t b, int128_t *r) { return !SHANNON_SUB_OVERFLOW(a, b, r); });
        break;
      case Op::MULTIPLY:
        ok = narrow ? run([](int128_t a, int128_t b, int128_t *r) {
          int64_t v;
          if (SHANNON_MUL_OVERFLOW(static_cast<int64_t>(a), static_cast<int64_t>(b), &v)) return false;
          *r = v;
          return true;
        })
                    : run([](int128_t a, int128_t b, int128_t *r) { return !SHANNON_MUL_OVERFLOW(a, b, r); });
        break;
      case Op::NEGATE:
        ok = run([](int128_t a, int128_t, int128_t *r) { return !SHANNON_SUB_OVERFLOW(int128_t{0}, a, r); });
        break;
      case Op::EQ:
        ok = run([](int128_t a, int128_t b, int128_t *r) {
          *r = a == b;
          return true;
        });
        break;
      case Op::NE:
        ok = run([](int128_t a, int128_t b, int128_t *r) {
          *r = a != b;
          return true;
        });
        break;
      case Op::LT:
        ok = run([](int128_t a, int128_t b, int128_t *r) {
          *r = a < b;
          return true;
        });
        break;
      case Op::LE:
        ok = run([](int128_t a, int128_t b, int128_t *r) {
          *r = a <= b;
          return true;
        });
        break;
      case Op::GT:
        ok = run([](int128_t a, int128_t b, int128_t *r) {
          *r = a > b;
          return true;
        });
        break;
      case Op::GE:
        ok = run([](int128_t a, int128_t b, int128_t *r) {
          *r = a >= b;
          return true;
        });
        break;
      default:
        return false;
    }
    if (!ok) return false;
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
  const int128_t *values = m_numbers.data() + (m_nodes.size() - 1) * rows;
  const uint8_t *nulls = m_nulls.data() + (m_nodes.size() - 1) * rows;
  uchar packed[DECIMAL_MAX_FIELD_SIZE];
  for (size_t row = 0; row < rows; ++row) {
    const bool is_null = nulls[row] != 0;
    if (!is_null && !EncodeDecimal(values[row], field->precision, field->decimals(), packed)) return false;
    if (!target->add(is_null ? nullptr : packed, is_null ? 0 : target->width(), is_null)) return false;
  }
  return true;
}

bool VectorizedExpression::Matches(size_t row) const {
  if (m_nodes.empty() || row >= m_rows) return false;
  const size_t index = (m_nodes.size() - 1) * m_rows + row;
  return m_nulls[index] == 0 && m_numbers[index] != 0;
}

bool VectorizedExpression::CopyProgram(const VectorizedExpression &source) {
  if (!Reserve(m_nodes, source.m_nodes.size())) return false;
  m_nodes.assign(source.m_nodes.begin(), source.m_nodes.end());
  return !m_nodes.empty();
}

bool VectorizedExpression::Result(size_t row, int128_t *value, bool *is_null, unsigned *scale) const {
  if (m_nodes.empty() || row >= m_rows) return false;
  const size_t index = (m_nodes.size() - 1) * m_rows + row;
  *value = m_numbers[index];
  *is_null = m_nulls[index] != 0;
  *scale = m_nodes.back().scale;
  return true;
}
}  // namespace Executor
}  // namespace ShannonBase
