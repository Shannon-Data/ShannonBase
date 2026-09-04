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

   Vectorized window function execution for the Rapid engine.
*/
#include "storage/rapid_engine/executor/iterators/window_iterator.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/field.h"
#include "sql/item_sum.h"
#include "sql/parse_tree_nodes.h"  // PT_frame, PT_border
#include "sql/sql_class.h"
#include "sql/sql_executor.h"   // copy_funcs
#include "sql/sql_optimizer.h"  // JOIN, Switch_ref_item_slice

#include "sql/table.h"
#include "sql/temp_table_param.h"
#include "sql/window.h"
#include "sql/window_lex.h"

#include "storage/rapid_engine/monitor/rapid_monitor.h"
#include "storage/rapid_engine/utils/SIMD.h"

namespace ShannonBase {
namespace Executor {

namespace {
void SwitchSlice(JOIN *join, int slice_num) {
  if (slice_num != -1 && !join->ref_items[slice_num].is_null()) {
    join->set_ref_item_slice(slice_num);
  }
}

/// Bit array layout shared with Utils::SIMD: bit set means NULL.
inline void SetNullBit(std::vector<uint8_t> *bits, size_t index) {
  const size_t byte = index >> 3;
  if (bits->size() <= byte) bits->resize(byte + 1, 0);
  (*bits)[byte] |= static_cast<uint8_t>(1u << (index & 7u));
}

inline bool GetNullBit(const std::vector<uint8_t> &bits, size_t index) {
  const size_t byte = index >> 3;
  if (bits.size() <= byte) return false;
  return (bits[byte] >> (index & 7u)) & 1u;
}

inline void EnsureNullBits(std::vector<uint8_t> *bits, size_t rows) {
  const size_t bytes = (rows + 7) / 8;
  if (bits->size() < bytes) bits->resize(bytes, 0);
}

/// Drop the first `count` bits of a bit array, shifting the rest down.
void EraseNullBitPrefix(std::vector<uint8_t> *bits, size_t count, size_t rows) {
  if (count == 0) return;
  const size_t remaining = rows - count;
  std::vector<uint8_t> shifted((remaining + 7) / 8, 0);
  for (size_t i = 0; i < remaining; ++i) {
    const size_t src = i + count;
    if ((*bits)[src >> 3] & (1u << (src & 7u))) shifted[i >> 3] |= static_cast<uint8_t>(1u << (i & 7u));
  }
  bits->swap(shifted);
}

/// Integers wider than 32 bits can overflow an int64 accumulator, so only the
/// narrow ones may take the SIMD reduction; the rest use the overflow-checked
/// scalar accumulator.
bool IsNarrowInteger(enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      return true;
    default:
      return false;
  }
}

/**
 * Argument types this operator reads through val_int()/val_real()/val_decimal()
 * without losing anything.
 *
 * Item_result is too coarse to decide this on its own: a DATETIME(6) argument
 * reports INT_RESULT, but its val_int() drops the fractional seconds that
 * MySQL's own accumulation keeps. The same goes for YEAR, BIT and ENUM, whose
 * value and storage representations differ. Restrict to the types where the
 * value read back is the value stored.
 */
bool IsNumericArgumentType(enum_field_types type, Item_result result_type) {
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      return result_type == INT_RESULT;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      return result_type == REAL_RESULT;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      return result_type == DECIMAL_RESULT;
    default:
      return false;
  }
}
}  // namespace

// ArgColumn
void VectorizedWindowIterator::ArgColumn::Clear() {
  ints.clear();
  reals.clear();
  decimals.clear();
  null_bits.clear();
  rows = 0;
  null_count = 0;
}

void VectorizedWindowIterator::ArgColumn::Reserve(size_t n) {
  switch (kind) {
    case ValueKind::kInt:
    case ValueKind::kUint:
      ints.reserve(n);
      break;
    case ValueKind::kReal:
      reals.reserve(n);
      break;
    case ValueKind::kDecimal:
      decimals.reserve(n);
      break;
    default:
      break;
  }
}

void VectorizedWindowIterator::ArgColumn::AppendNull() {
  SetNullBit(&null_bits, rows);
  ++null_count;
}

// IntSumAccumulator
void VectorizedWindowIterator::IntSumAccumulator::Reset() {
  acc = 0;
  carry_used = false;
  my_decimal_set_zero(&carry);
}

void VectorizedWindowIterator::IntSumAccumulator::FoldIntoCarry() {
  my_decimal part;
  int2my_decimal(E_DEC_FATAL_ERROR, acc, false, &part);
  if (!carry_used) {
    carry = part;
    carry_used = true;
  } else {
    my_decimal sum;
    my_decimal_add(E_DEC_FATAL_ERROR, &sum, &carry, &part);
    carry = sum;
  }
  acc = 0;
}

void VectorizedWindowIterator::IntSumAccumulator::Add(int64_t value) {
  int64_t next;
  if (__builtin_add_overflow(acc, value, &next)) {
    FoldIntoCarry();
    acc = value;
  } else {
    acc = next;
  }
}

void VectorizedWindowIterator::IntSumAccumulator::AddUnsigned(uint64_t value) {
  if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    // Rare enough that folding through a decimal is the cheapest correct
    // option; the int64 fast path stays branch-predictable for everything else.
    FoldIntoCarry();
    my_decimal part;
    int2my_decimal(E_DEC_FATAL_ERROR, static_cast<longlong>(value), true, &part);
    if (!carry_used) {
      carry = part;
      carry_used = true;
    } else {
      my_decimal sum;
      my_decimal_add(E_DEC_FATAL_ERROR, &sum, &carry, &part);
      carry = sum;
    }
    return;
  }
  Add(static_cast<int64_t>(value));
}

void VectorizedWindowIterator::IntSumAccumulator::Value(my_decimal *out) const {
  my_decimal part;
  int2my_decimal(E_DEC_FATAL_ERROR, acc, false, &part);
  if (!carry_used) {
    *out = part;
    return;
  }
  my_decimal_add(E_DEC_FATAL_ERROR, out, &carry, &part);
}

// WindowFn
void VectorizedWindowIterator::WindowFn::ResetPartition() {
  count = 0;
  int_sum.Reset();
  my_decimal_set_zero(&dec_sum);
  real_sum = 0.0;
  has_extreme = false;
  ext_int = 0;
  ext_real = 0.0;
  my_decimal_set_zero(&ext_dec);
}

void VectorizedWindowIterator::WindowFn::ClearResults() {
  res_null.clear();
  res_int.clear();
  res_real.clear();
  res_dec.clear();
}

void VectorizedWindowIterator::WindowFn::EraseResultPrefix(size_t n) {
  if (n == 0) return;
  auto trim = [n](auto &vec) {
    if (vec.empty()) return;
    const size_t drop = std::min(n, vec.size());
    vec.erase(vec.begin(), vec.begin() + drop);
  };
  trim(res_null);
  trim(res_int);
  trim(res_real);
  trim(res_dec);
}

// Construction / eligibility
VectorizedWindowIterator::VectorizedWindowIterator(THD *thd, unique_ptr_destroy_only<RowIterator> source,
                                                   Temp_table_param *temp_table_param, JOIN *join, int output_slice,
                                                   size_t buffer_memory_limit)
    : RowIterator(thd),
      m_source(std::move(source)),
      m_temp_table_param(temp_table_param),
      m_window(temp_table_param->m_window),
      m_join(join),
      m_output_slice(output_slice),
      m_buffer_memory_limit(buffer_memory_limit) {
  assert(m_window != nullptr && m_window->needs_buffering());
}

VectorizedWindowIterator::~VectorizedWindowIterator() {
  if (m_spill_file != nullptr) std::fclose(m_spill_file);
}

bool VectorizedWindowIterator::CanVectorize(const Temp_table_param *param) {
  // Lets a test run the very same Rapid plan with this operator out of the way,
  // so the two window implementations can be compared against each other rather
  // than only against InnoDB.
  DBUG_EXECUTE_IF("rapid_disable_vectorized_window", return false;);

  if (param == nullptr || param->items_to_copy == nullptr) return false;

  const Window *w = param->m_window;
  if (w == nullptr) return false;

  // A window that does not buffer is already streamed by MySQL's WindowIterator
  // at essentially no cost; there is nothing to win by taking it over.
  if (!w->needs_buffering()) return false;

  const PT_frame *frame = w->frame();
  if (frame == nullptr || frame->m_from == nullptr || frame->m_to == nullptr) return false;
  if (frame->m_exclusion != nullptr) return false;
  if (frame->m_query_expression != WFU_ROWS && frame->m_query_expression != WFU_RANGE) return false;

  // Frames that start anywhere but at the partition head need rows to leave the
  // frame again, which is exactly what the single-pass accumulators here cannot
  // express.
  if (frame->m_from->m_border_type != WBT_UNBOUNDED_PRECEDING) return false;
  if (frame->m_to->m_border_type != WBT_CURRENT_ROW && frame->m_to->m_border_type != WBT_UNBOUNDED_FOLLOWING) {
    return false;
  }

  const TABLE *out_table = nullptr;
  bool saw_window_function = false;

  for (const Func_ptr &func : *param->items_to_copy) {
    if (!func.should_copy(CFT_WF)) continue;
    saw_window_function = true;

    const Item_sum *item = down_cast<Item_sum *>(func.func());
    if (item->has_with_distinct()) return false;
    if (item->needs_partition_cardinality()) return false;
    if (item->uses_only_one_row()) return false;

    switch (item->sum_func()) {
      case Item_sum::ROW_NUMBER_FUNC:
      case Item_sum::RANK_FUNC:
      case Item_sum::DENSE_RANK_FUNC:
        break;
      case Item_sum::COUNT_FUNC:
        if (item->argument_count() != 1) return false;
        break;
      case Item_sum::SUM_FUNC:
      case Item_sum::AVG_FUNC: {
        if (item->argument_count() != 1) return false;
        if (item->result_type() != DECIMAL_RESULT && item->result_type() != REAL_RESULT) return false;
        const Item *arg = item->get_arg(0);
        if (!IsNumericArgumentType(arg->data_type(), arg->result_type())) return false;
        break;
      }
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC: {
        if (item->argument_count() != 1) return false;
        const Item *arg = item->get_arg(0);
        if (!IsNumericArgumentType(arg->data_type(), arg->result_type())) return false;
        // The result is stored straight into the result field, so the
        // aggregate's own type must be the argument's.
        if (item->result_type() != arg->result_type()) return false;
        break;
      }
      default:
        return false;
    }

    const Field *field = func.result_field();
    if (field == nullptr || field->table == nullptr) return false;
    if (out_table == nullptr) {
      out_table = field->table;
    } else if (out_table != field->table) {
      return false;
    }
  }

  if (!saw_window_function || out_table == nullptr) return false;

  // Buffered rows are kept as raw output-record images. That is only safe when
  // every value lives inside record[0]; a BLOB/TEXT/JSON/GEOMETRY column stores
  // a pointer there instead, so those layouts stay on the native iterator.
  if (out_table->s == nullptr || out_table->s->blob_fields != 0) return false;
  if (out_table->record[0] == nullptr) return false;

  // All non-window expressions of the step must be writable into that same
  // record, otherwise restoring a buffered row would not restore the whole row.
  for (const Func_ptr &func : *param->items_to_copy) {
    if (func.should_copy(CFT_WF)) continue;
    const Field *field = func.result_field();
    if (field == nullptr || field->table != out_table) return false;
  }

  return true;
}

// Setup
size_t VectorizedWindowIterator::AddArgColumn(Item *item, ValueKind kind, bool null_only) {
  for (size_t i = 0; i < m_arg_columns.size(); ++i) {
    if (m_arg_columns[i].item == item && m_arg_columns[i].kind == kind && m_arg_columns[i].null_only == null_only) {
      return i;
    }
  }
  ArgColumn col;
  col.item = item;
  col.kind = kind;
  col.null_only = null_only;
  m_arg_columns.push_back(std::move(col));
  return m_arg_columns.size() - 1;
}

bool VectorizedWindowIterator::SetupPlan() {
  m_functions.clear();
  m_arg_columns.clear();
  m_out_table = nullptr;

  for (const Func_ptr &func : *m_temp_table_param->items_to_copy) {
    if (!func.should_copy(CFT_WF)) continue;

    WindowFn fn;
    fn.item = down_cast<Item_sum *>(func.func());
    fn.result_field = func.result_field();
    if (m_out_table == nullptr) m_out_table = fn.result_field->table;

    Item *arg = fn.item->argument_count() > 0 ? fn.item->get_arg(0) : nullptr;

    switch (fn.item->sum_func()) {
      case Item_sum::ROW_NUMBER_FUNC:
        fn.kind = WfKind::kRowNumber;
        fn.framing = false;
        break;
      case Item_sum::RANK_FUNC:
        fn.kind = WfKind::kRank;
        fn.framing = false;
        break;
      case Item_sum::DENSE_RANK_FUNC:
        fn.kind = WfKind::kDenseRank;
        fn.framing = false;
        break;
      case Item_sum::COUNT_FUNC:
        fn.kind = WfKind::kCount;
        fn.framing = true;
        fn.result_kind = ValueKind::kInt;
        fn.arg_col = AddArgColumn(arg, ValueKind::kNone, /*null_only=*/true);
        break;
      case Item_sum::SUM_FUNC:
      case Item_sum::AVG_FUNC: {
        fn.kind = fn.item->sum_func() == Item_sum::SUM_FUNC ? WfKind::kSum : WfKind::kAvg;
        fn.framing = true;
        if (fn.item->result_type() == REAL_RESULT) {
          fn.result_kind = ValueKind::kReal;
          fn.arg_col = AddArgColumn(arg, ValueKind::kReal, false);
        } else {
          fn.result_kind = ValueKind::kDecimal;
          if (arg->result_type() == INT_RESULT) {
            fn.arg_col = AddArgColumn(arg, arg->unsigned_flag ? ValueKind::kUint : ValueKind::kInt, false);
            m_arg_columns[fn.arg_col].narrow_int = IsNarrowInteger(arg->data_type());
          } else {
            fn.arg_col = AddArgColumn(arg, ValueKind::kDecimal, false);
          }
        }
        if (fn.kind == WfKind::kAvg) {
          fn.prec_increment = down_cast<Item_sum_avg *>(fn.item)->prec_increment;
        }
        break;
      }
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC: {
        fn.kind = fn.item->sum_func() == Item_sum::MIN_FUNC ? WfKind::kMin : WfKind::kMax;
        fn.framing = true;
        switch (fn.item->result_type()) {
          case INT_RESULT:
            fn.result_kind = fn.item->unsigned_flag ? ValueKind::kUint : ValueKind::kInt;
            break;
          case REAL_RESULT:
            fn.result_kind = ValueKind::kReal;
            break;
          default:
            fn.result_kind = ValueKind::kDecimal;
            break;
        }
        fn.arg_col = AddArgColumn(arg, fn.result_kind, false);
        break;
      }
      default:
        // CanVectorize() rejected everything else.
        assert(false);
        return true;
    }

    fn.result_unsigned = fn.item->unsigned_flag;
    m_functions.push_back(std::move(fn));
  }

  if (m_functions.empty() || m_out_table == nullptr) return true;
  m_record_length = m_out_table->s->reclength;
  if (m_record_length == 0) return true;

  const PT_frame *frame = m_window->frame();
  if (frame->m_to->m_border_type == WBT_UNBOUNDED_FOLLOWING) {
    m_granularity = Granularity::kPerPartition;
  } else if (frame->m_query_expression == WFU_ROWS) {
    m_granularity = Granularity::kPerRow;
  } else {
    m_granularity = Granularity::kPerPeerGroup;
  }
  return false;
}

bool VectorizedWindowIterator::Init() {
  if (m_source->Init()) return true;

  m_window->reset_round();
  if (SetupPlan()) return true;

  m_input_slice = m_join->get_ref_item_slice();

  // Buffered partitions spill past the memory budget. Reaching that with real
  // data needs a partition of tens of megabytes, so let tests shrink the budget
  // to a few records instead.
  DBUG_EXECUTE_IF("rapid_window_tiny_buffer", m_buffer_memory_limit = 4 * m_record_length;);

  m_rows = 0;
  m_accum_rows = 0;
  m_emit_end = 0;
  m_emit_row = 0;
  m_emit_group = 0;
  m_emit_part_rowno = 1;
  m_emit_rank = 1;
  m_emit_dense_rank = 0;
  m_partition_closed = false;
  m_eof = false;
  m_done = false;
  m_carry_valid = false;
  m_carry_record.assign(m_record_length, 0);
  m_new_peer.clear();
  for (ArgColumn &col : m_arg_columns) col.Clear();
  for (WindowFn &fn : m_functions) {
    fn.ResetPartition();
    fn.ClearResults();
  }
  ResetRecordBuffer();
  m_stats = VectorizedOperatorStats{};
  return false;
}

// Record image buffer
void VectorizedWindowIterator::ResetRecordBuffer() {
  m_records.clear();
  m_mem_rows = 0;
  m_spill_rows = 0;
  m_spill_read = 0;
  m_spill_reading = false;
  if (m_spill_file != nullptr) {
    std::clearerr(m_spill_file);
    (void)std::fseek(m_spill_file, 0, SEEK_SET);
  }
}

bool VectorizedWindowIterator::SpillRecord(const uchar *record) {
  if (m_spill_file == nullptr) {
    m_spill_file = std::tmpfile();
    if (m_spill_file == nullptr) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return true;
    }
  }
  if (m_spill_reading) {
    std::clearerr(m_spill_file);
    if (std::fseek(m_spill_file, static_cast<long>(m_spill_rows * m_record_length), SEEK_SET) != 0) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return true;
    }
    m_spill_reading = false;
  }
  if (std::fwrite(record, 1, m_record_length, m_spill_file) != m_record_length) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }
  ++m_spill_rows;
  m_stats.spill_rows++;
  m_stats.spill_bytes += m_record_length;
  return false;
}

bool VectorizedWindowIterator::AppendRecord() {
  const uchar *record = m_out_table->record[0];
  const bool over_budget = (m_mem_rows + 1) * m_record_length > m_buffer_memory_limit;
  if (m_spill_rows > 0 || (over_budget && m_mem_rows > 0)) return SpillRecord(record);

  m_records.insert(m_records.end(), record, record + m_record_length);
  ++m_mem_rows;
  m_stats.bytes_copied += m_record_length;
  return false;
}

bool VectorizedWindowIterator::LoadRecord(size_t row) {
  if (row < m_mem_rows) {
    std::memcpy(m_out_table->record[0], m_records.data() + row * m_record_length, m_record_length);
    return false;
  }
  const size_t file_row = row - m_mem_rows;
  assert(file_row == m_spill_read);
  if (!m_spill_reading) {
    std::clearerr(m_spill_file);
    if (std::fseek(m_spill_file, 0, SEEK_SET) != 0) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return true;
    }
    m_spill_reading = true;
  }
  if (std::fread(m_out_table->record[0], 1, m_record_length, m_spill_file) != m_record_length) {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return true;
  }
  ++m_spill_read;
  return false;
}

// Ingestion
bool VectorizedWindowIterator::EvaluateArguments() {
  for (ArgColumn &col : m_arg_columns) {
    bool is_null;
    if (col.null_only) {
      if (col.item->update_null_value()) return true;
      is_null = col.item->null_value;
    } else {
      switch (col.kind) {
        case ValueKind::kInt:
        case ValueKind::kUint: {
          const longlong value = col.item->val_int();
          is_null = col.item->null_value;
          col.ints.push_back(static_cast<int64_t>(value));
          break;
        }
        case ValueKind::kReal: {
          const double value = col.item->val_real();
          is_null = col.item->null_value;
          col.reals.push_back(value);
          break;
        }
        case ValueKind::kDecimal: {
          my_decimal buffer;
          const my_decimal *value = col.item->val_decimal(&buffer);
          is_null = col.item->null_value;
          col.decimals.push_back(is_null || value == nullptr ? my_decimal() : *value);
          break;
        }
        default:
          assert(false);
          return true;
      }
    }
    if (thd()->is_error()) return true;
    EnsureNullBits(&col.null_bits, col.rows + 1);
    if (is_null) col.AppendNull();
    ++col.rows;
  }
  return false;
}

void VectorizedWindowIterator::EraseArgPrefix(size_t count) {
  if (count == 0) return;
  for (ArgColumn &col : m_arg_columns) {
    assert(count <= col.rows);
    size_t removed_nulls = 0;
    for (size_t i = 0; i < count; ++i) {
      if (GetNullBit(col.null_bits, i)) ++removed_nulls;
    }
    EraseNullBitPrefix(&col.null_bits, count, col.rows);
    col.null_count -= removed_nulls;
    if (!col.ints.empty()) col.ints.erase(col.ints.begin(), col.ints.begin() + count);
    if (!col.reals.empty()) col.reals.erase(col.reals.begin(), col.reals.begin() + count);
    if (!col.decimals.empty()) col.decimals.erase(col.decimals.begin(), col.decimals.begin() + count);
    col.rows -= count;
  }
}

int VectorizedWindowIterator::IngestBatch() {
  const size_t target = m_rows + kBatchRows;
  while (m_rows < target) {
    int err;
    {
      Switch_ref_item_slice slice_switch(m_join, m_input_slice);
      err = m_source->Read();
    }
    if (err == 1) return 1;
    if (err == -1) {
      m_eof = true;
      m_partition_closed = true;
      break;
    }

    /*
      Same phase order as BufferingWindowIterator: non-window expressions are
      evaluated from the input record into the output record first, so that both
      the buffered image and the window function arguments (which read the
      output record's fields) are consistent.
    */
    {
      Switch_ref_item_slice slice_switch(m_join, m_input_slice);
      if (copy_funcs(m_temp_table_param, thd(), CFT_HAS_NO_WF)) return 1;
    }

    m_window->check_partition_boundary();
    const bool new_partition = (m_window->partition_rowno() == 1);
    // Called for every row, boundary or not: the cached ORDER BY values must
    // advance in lockstep with the input for the next comparison to be right.
    const bool new_peer = m_window->in_new_order_by_peer_set() || new_partition;

    if (EvaluateArguments()) return 1;
    m_stats.rows_in++;

    if (new_partition && m_rows > 0) {
      std::memcpy(m_carry_record.data(), m_out_table->record[0], m_record_length);
      m_carry_valid = true;
      m_partition_closed = true;
      break;
    }

    if (AppendRecord()) return 1;
    m_new_peer.push_back(new_peer ? 1 : 0);
    ++m_rows;
  }
  m_stats.batches++;
  return 0;
}

// Kernels
void VectorizedWindowIterator::AccumulateSegment(WindowFn *fn, size_t from, size_t to) {
  if (from >= to) return;
  const ArgColumn &col = m_arg_columns[fn->arg_col];
  const size_t n = to - from;
  // A column with no NULL at all lets the SIMD kernels run without a mask, and
  // a masked sub-range would need bit-aligned slicing anyway.
  const bool no_nulls = (col.null_count == 0);

  switch (fn->kind) {
    case WfKind::kCount: {
      if (no_nulls) {
        fn->count += static_cast<int64_t>(n);
      } else {
        for (size_t i = from; i < to; ++i) {
          if (!GetNullBit(col.null_bits, i)) ++fn->count;
        }
      }
      m_stats.simd_rows += n;
      return;
    }
    case WfKind::kSum:
    case WfKind::kAvg: {
      if (fn->result_kind == ValueKind::kReal) {
        // Deliberately sequential: MySQL sums doubles in row order, and a
        // reassociated SIMD sum would round differently.
        double sum = 0.0;
        if (no_nulls) {
          for (size_t i = from; i < to; ++i) sum += col.reals[i];
          fn->count += static_cast<int64_t>(n);
        } else {
          for (size_t i = from; i < to; ++i) {
            if (GetNullBit(col.null_bits, i)) continue;
            sum += col.reals[i];
            ++fn->count;
          }
        }
        fn->real_sum += sum;
        m_stats.scalar_fallback_rows += n;
        return;
      }
      if (col.kind == ValueKind::kDecimal) {
        for (size_t i = from; i < to; ++i) {
          if (GetNullBit(col.null_bits, i)) continue;
          my_decimal sum;
          my_decimal_add(E_DEC_FATAL_ERROR, &sum, &fn->dec_sum, &col.decimals[i]);
          fn->dec_sum = sum;
          ++fn->count;
        }
        m_stats.scalar_fallback_rows += n;
        return;
      }
      // Integer argument, decimal result: accumulate exactly in int64.
      if (no_nulls && col.narrow_int) {
        // Values are at most 32 bits wide, so an int64 SIMD reduction of at
        // most 2^32 of them cannot overflow.
        fn->int_sum.Add(Utils::SIMD::sum<int64_t>(col.ints.data() + from, nullptr, n));
        fn->count += static_cast<int64_t>(n);
        m_stats.simd_rows += n;
        return;
      }
      const bool is_unsigned = (col.kind == ValueKind::kUint);
      for (size_t i = from; i < to; ++i) {
        if (!no_nulls && GetNullBit(col.null_bits, i)) continue;
        if (is_unsigned) {
          fn->int_sum.AddUnsigned(static_cast<uint64_t>(col.ints[i]));
        } else {
          fn->int_sum.Add(col.ints[i]);
        }
        ++fn->count;
      }
      m_stats.scalar_fallback_rows += n;
      return;
    }
    case WfKind::kMin:
    case WfKind::kMax: {
      const bool want_min = (fn->kind == WfKind::kMin);
      switch (fn->result_kind) {
        case ValueKind::kInt: {
          if (no_nulls) {
            const int64_t value = want_min ? Utils::SIMD::min<int64_t>(col.ints.data() + from, nullptr, n)
                                           : Utils::SIMD::max<int64_t>(col.ints.data() + from, nullptr, n);
            if (!fn->has_extreme || (want_min ? value < fn->ext_int : value > fn->ext_int)) fn->ext_int = value;
            fn->has_extreme = true;
            fn->count += static_cast<int64_t>(n);
            m_stats.simd_rows += n;
            return;
          }
          for (size_t i = from; i < to; ++i) {
            if (GetNullBit(col.null_bits, i)) continue;
            const int64_t value = col.ints[i];
            if (!fn->has_extreme || (want_min ? value < fn->ext_int : value > fn->ext_int)) fn->ext_int = value;
            fn->has_extreme = true;
            ++fn->count;
          }
          m_stats.scalar_fallback_rows += n;
          return;
        }
        case ValueKind::kUint: {
          for (size_t i = from; i < to; ++i) {
            if (!no_nulls && GetNullBit(col.null_bits, i)) continue;
            const uint64_t value = static_cast<uint64_t>(col.ints[i]);
            const uint64_t current = static_cast<uint64_t>(fn->ext_int);
            if (!fn->has_extreme || (want_min ? value < current : value > current)) {
              fn->ext_int = static_cast<int64_t>(value);
            }
            fn->has_extreme = true;
            ++fn->count;
          }
          m_stats.scalar_fallback_rows += n;
          return;
        }
        case ValueKind::kReal: {
          if (no_nulls) {
            const double value = want_min ? Utils::SIMD::min<double>(col.reals.data() + from, nullptr, n)
                                          : Utils::SIMD::max<double>(col.reals.data() + from, nullptr, n);
            if (!fn->has_extreme || (want_min ? value < fn->ext_real : value > fn->ext_real)) fn->ext_real = value;
            fn->has_extreme = true;
            fn->count += static_cast<int64_t>(n);
            m_stats.simd_rows += n;
            return;
          }
          for (size_t i = from; i < to; ++i) {
            if (GetNullBit(col.null_bits, i)) continue;
            const double value = col.reals[i];
            if (!fn->has_extreme || (want_min ? value < fn->ext_real : value > fn->ext_real)) fn->ext_real = value;
            fn->has_extreme = true;
            ++fn->count;
          }
          m_stats.scalar_fallback_rows += n;
          return;
        }
        default: {
          for (size_t i = from; i < to; ++i) {
            if (!no_nulls && GetNullBit(col.null_bits, i)) continue;
            const my_decimal &value = col.decimals[i];
            const int cmp = fn->has_extreme ? my_decimal_cmp(&value, &fn->ext_dec) : 0;
            if (!fn->has_extreme || (want_min ? cmp < 0 : cmp > 0)) fn->ext_dec = value;
            fn->has_extreme = true;
            ++fn->count;
          }
          m_stats.scalar_fallback_rows += n;
          return;
        }
      }
    }
    default:
      return;
  }
}

void VectorizedWindowIterator::AppendResult(WindowFn *fn) {
  switch (fn->kind) {
    case WfKind::kCount:
      fn->res_null.push_back(0);
      fn->res_int.push_back(fn->count);
      return;
    case WfKind::kSum: {
      const bool is_null = (fn->count == 0);
      fn->res_null.push_back(is_null ? 1 : 0);
      if (fn->result_kind == ValueKind::kReal) {
        fn->res_real.push_back(fn->real_sum);
      } else if (m_arg_columns[fn->arg_col].kind == ValueKind::kDecimal) {
        fn->res_dec.push_back(fn->dec_sum);
      } else {
        my_decimal value;
        fn->int_sum.Value(&value);
        fn->res_dec.push_back(value);
      }
      return;
    }
    case WfKind::kAvg: {
      const bool is_null = (fn->count == 0);
      fn->res_null.push_back(is_null ? 1 : 0);
      if (fn->result_kind == ValueKind::kReal) {
        fn->res_real.push_back(is_null ? 0.0 : fn->real_sum / static_cast<double>(fn->count));
        return;
      }
      my_decimal sum;
      if (m_arg_columns[fn->arg_col].kind == ValueKind::kDecimal) {
        sum = fn->dec_sum;
      } else {
        fn->int_sum.Value(&sum);
      }
      if (is_null) {
        fn->res_dec.push_back(sum);
        return;
      }
      // Mirrors Item_sum_avg::val_decimal(): divide the running sum by the
      // non-NULL count with the session's division precision increment.
      my_decimal divisor;
      int2my_decimal(E_DEC_FATAL_ERROR, fn->count, false, &divisor);
      my_decimal average;
      my_decimal_div(E_DEC_FATAL_ERROR, &average, &sum, &divisor, fn->prec_increment);
      fn->res_dec.push_back(average);
      return;
    }
    case WfKind::kMin:
    case WfKind::kMax: {
      fn->res_null.push_back(fn->has_extreme ? 0 : 1);
      switch (fn->result_kind) {
        case ValueKind::kInt:
        case ValueKind::kUint:
          fn->res_int.push_back(fn->ext_int);
          break;
        case ValueKind::kReal:
          fn->res_real.push_back(fn->ext_real);
          break;
        default:
          fn->res_dec.push_back(fn->ext_dec);
          break;
      }
      return;
    }
    default:
      return;
  }
}

// Compute
bool VectorizedWindowIterator::Compute() {
  const size_t base = m_accum_rows;
  const size_t pending = m_rows - base;

  auto accumulate_all = [this, base](size_t from, size_t to) {
    for (WindowFn &fn : m_functions) {
      if (!fn.framing) continue;
      AccumulateSegment(&fn, from - base, to - base);
    }
  };
  auto append_all = [this]() {
    for (WindowFn &fn : m_functions) {
      if (!fn.framing) continue;
      AppendResult(&fn);
    }
  };

  switch (m_granularity) {
    case Granularity::kPerRow: {
      // A running value per row: one column-at-a-time prefix scan per function.
      for (WindowFn &fn : m_functions) {
        if (!fn.framing) continue;
        for (size_t j = 0; j < pending; ++j) {
          AccumulateSegment(&fn, j, j + 1);
          AppendResult(&fn);
        }
      }
      m_emit_end = m_rows;
      break;
    }
    case Granularity::kPerPeerGroup: {
      // The group that was still open when the previous pass ran out of rows is
      // closed by the first row of this one.
      if (base > 0 && pending > 0 && m_new_peer[base] && m_emit_end < base) {
        append_all();
        m_emit_end = base;
      }
      size_t seg_start = base;
      for (size_t i = base + 1; i < m_rows; ++i) {
        if (!m_new_peer[i]) continue;
        accumulate_all(seg_start, i);
        append_all();
        m_emit_end = i;
        seg_start = i;
      }
      if (seg_start < m_rows) accumulate_all(seg_start, m_rows);
      if (m_partition_closed && m_emit_end < m_rows) {
        append_all();
        m_emit_end = m_rows;
      }
      break;
    }
    case Granularity::kPerPartition: {
      if (pending > 0) accumulate_all(base, m_rows);
      if (m_partition_closed && m_emit_end < m_rows) {
        append_all();
        m_emit_end = m_rows;
      }
      break;
    }
  }

  m_accum_rows = m_rows;
  EraseArgPrefix(pending);

  // Once record images have spilled, rows can only be served in one sweep: the
  // spill file is read strictly forward and a partial drain would leave it
  // positioned mid-partition.
  if (m_spill_rows > 0 && !m_partition_closed) m_emit_end = 0;
  return false;
}

// Emission
bool VectorizedWindowIterator::StoreResult(const WindowFn &fn, size_t index, int64_t row_number, int64_t rank,
                                           int64_t dense_rank) {
  Field *field = fn.result_field;
  THD *const thd_ptr = thd();
  const enum_check_fields saved = thd_ptr->check_for_truncated_fields;
  thd_ptr->check_for_truncated_fields = CHECK_FIELD_IGNORE;

  switch (fn.kind) {
    case WfKind::kRowNumber:
    case WfKind::kRank:
    case WfKind::kDenseRank: {
      const int64_t value = fn.kind == WfKind::kRowNumber ? row_number : (fn.kind == WfKind::kRank ? rank : dense_rank);
      field->set_notnull();
      (void)field->store(value, /*unsigned_val=*/false);
      break;
    }
    default: {
      if (fn.res_null[index]) {
        if (field->is_nullable()) {
          field->set_null();
        } else {
          field->reset();
        }
        break;
      }
      field->set_notnull();
      switch (fn.result_kind) {
        case ValueKind::kInt:
        case ValueKind::kUint:
          (void)field->store(fn.res_int[index], fn.result_unsigned);
          break;
        case ValueKind::kReal:
          (void)field->store(fn.res_real[index]);
          break;
        default:
          (void)field->store_decimal(&fn.res_dec[index]);
          break;
      }
      break;
    }
  }

  thd_ptr->check_for_truncated_fields = saved;
  return thd_ptr->is_error();
}

int VectorizedWindowIterator::EmitRow() {
  const size_t row = m_emit_row;
  if (m_new_peer[row]) {
    m_emit_rank = m_emit_part_rowno;
    ++m_emit_dense_rank;
    if (row > 0) ++m_emit_group;
  }

  size_t result_index = 0;
  switch (m_granularity) {
    case Granularity::kPerRow:
      result_index = row;
      break;
    case Granularity::kPerPeerGroup:
      result_index = m_emit_group;
      break;
    case Granularity::kPerPartition:
      result_index = 0;
      break;
  }

  if (LoadRecord(row)) return 1;

  for (const WindowFn &fn : m_functions) {
    if (StoreResult(fn, result_index, m_emit_part_rowno, m_emit_rank, m_emit_dense_rank)) return 1;
  }

  if (m_window->is_last() && copy_funcs(m_temp_table_param, thd(), CFT_HAS_WF)) return 1;

  ++m_emit_part_rowno;
  ++m_emit_row;
  m_stats.rows_out++;
  RapidMonitor::rapid_counter_vectorized_window_rows(1);
  return 0;
}

void VectorizedWindowIterator::ErasePrefix(size_t rows) {
  if (rows == 0) return;
  assert(rows <= m_rows);

  size_t results_consumed = 0;
  switch (m_granularity) {
    case Granularity::kPerRow:
      results_consumed = rows;
      break;
    case Granularity::kPerPeerGroup:
      results_consumed = m_emit_group + 1;
      break;
    case Granularity::kPerPartition:
      results_consumed = (rows == m_rows) ? 1 : 0;
      break;
  }
  for (WindowFn &fn : m_functions) {
    if (!fn.framing) continue;
    fn.EraseResultPrefix(results_consumed);
  }

  if (rows == m_rows) {
    ResetRecordBuffer();
    m_new_peer.clear();
  } else {
    // Partial drains only happen before anything has spilled.
    assert(m_spill_rows == 0 && rows <= m_mem_rows);
    m_records.erase(m_records.begin(), m_records.begin() + rows * m_record_length);
    m_mem_rows -= rows;
    m_new_peer.erase(m_new_peer.begin(), m_new_peer.begin() + rows);
  }

  m_rows -= rows;
  m_accum_rows = (m_accum_rows > rows) ? m_accum_rows - rows : 0;
  m_emit_group = 0;
}

void VectorizedWindowIterator::StartNewPartition() {
  for (WindowFn &fn : m_functions) {
    fn.ResetPartition();
    fn.ClearResults();
  }
  ResetRecordBuffer();
  m_new_peer.clear();
  m_rows = 0;
  m_accum_rows = 0;
  m_emit_end = 0;
  m_emit_row = 0;
  m_emit_group = 0;
  m_emit_part_rowno = 1;
  m_emit_rank = 1;
  m_emit_dense_rank = 0;
  m_partition_closed = false;
}

int VectorizedWindowIterator::Read() {
  SwitchSlice(m_join, m_output_slice);

  for (;;) {
    if (m_emit_row < m_emit_end) {
      if (thd()->killed) {
        thd()->send_kill_message();
        return 1;
      }
      return EmitRow();
    }

    if (m_emit_end > 0) {
      ErasePrefix(m_emit_end);
      m_emit_end = 0;
      m_emit_row = 0;
    }

    if (m_done) return -1;

    if (m_partition_closed) {
      assert(m_rows == 0);
      if (!m_carry_valid) {
        m_done = true;
        return -1;
      }
      // The row that ended the previous partition opens the next one; its
      // arguments are already at the head of the argument columns.
      StartNewPartition();
      std::memcpy(m_out_table->record[0], m_carry_record.data(), m_record_length);
      if (AppendRecord()) return 1;
      m_new_peer.push_back(1);
      m_rows = 1;
      m_carry_valid = false;
    }

    if (!m_partition_closed) {
      if (m_eof) {
        // Input was exhausted while promoting a carried row; the partition that
        // row opened is therefore already complete.
        m_partition_closed = true;
      } else if (IngestBatch() == 1) {
        return 1;
      }
    }
    if (Compute()) return 1;

    if (m_emit_end == 0 && m_rows == 0 && m_partition_closed && !m_carry_valid) {
      m_done = true;
      return -1;
    }
  }
}

}  // namespace Executor
}  // namespace ShannonBase
