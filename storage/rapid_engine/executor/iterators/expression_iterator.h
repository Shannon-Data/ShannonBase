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
#ifndef __SHANNONBASE_BATCH_EXPRESSION_H__
#define __SHANNONBASE_BATCH_EXPRESSION_H__

#include <cstdint>
#include <memory_resource>
#include <vector>

#include "storage/rapid_engine/include/rapid_const.h"

class Field;
class Item;
namespace ShannonBase {
namespace Executor {
class ColumnChunk;
// Exact numeric expressions compiled once, executed one instruction over a
// column at a time. Evaluation is speculative: unsupported values/overflow
// return false without changing Fields, Items, or SQL diagnostics. The caller
// can then evaluate the original Item to preserve MySQL's error semantics.
//
// Return convention: these methods return true on SUCCESS, the opposite of the
// MySQL/RowIterator "true == error" rule used by the surrounding operators, so a
// call site reads as "if (!Compile(...)) fall back". Compile() may be called for
// its side effect only; callers then check compiled().
class VectorizedExpression {
 public:
  explicit VectorizedExpression(std::pmr::memory_resource *memory)
      : m_nodes(memory), m_numbers(memory), m_nulls(memory) {}
  bool Compile(Item *expression);
  [[nodiscard]] bool Evaluate(const std::pmr::vector<ColumnChunk> &chunks, size_t rows,
                              const std::pmr::vector<uint32_t> *selection = nullptr);
  [[nodiscard]] bool Store(ColumnChunk *target, size_t rows) const;
  bool Matches(size_t row) const;
  // Copy only immutable instructions on the coordinator. Evaluation buffers
  // are allocated from the destination worker's own resource.
  [[nodiscard]] bool CopyProgram(const VectorizedExpression &source);
  bool Result(size_t row, int128_t *value, bool *is_null, unsigned *scale) const;
  bool compiled() const { return !m_nodes.empty(); }
  // Evaluation bytes per row: one value and one null flag per instruction.
  size_t row_bytes() const { return m_nodes.size() * (sizeof(int128_t) + sizeof(uint8_t)); }

 private:
  enum class Op { FIELD, CONSTANT, ADD, SUBTRACT, MULTIPLY, NEGATE, EQ, NE, LT, LE, GT, GE };
  struct Value {
    int128_t number{0};
    bool is_null{true};
  };

  struct Node {
    Op op{Op::CONSTANT};
    Field *field{nullptr};
    Value constant;
    size_t left{0}, right{0};
    unsigned scale{0};
    bool integer{false};
  };

  bool CompileNode(Item *expression, size_t *index, unsigned depth);

  std::pmr::vector<Node> m_nodes;
  // Structure of arrays: values and null flags are separate columns, so a node
  // costs 17 bytes per row instead of 32 and the null test touches one byte.
  std::pmr::vector<int128_t> m_numbers;
  std::pmr::vector<uint8_t> m_nulls;
  size_t m_rows{0};
};
}  // namespace Executor
}  // namespace ShannonBase
#endif
