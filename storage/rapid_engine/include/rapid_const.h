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
#ifndef __SHANNONBASE_CONST_H__
#define __SHANNONBASE_CONST_H__
#include <cmath>

#include "my_inttypes.h"
#include "rapid_arch_inf.h"

#if defined(__arm__) || defined(__aarch64__)
#define SHANNON_ARM_PLATFORM
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64)
#define SHANNON_X86_PLATFORM
#elif defined(__APPLE__) && defined(__arm64__)
#define SHANNON_APPLE_PLATFORM
#endif

#define SHANNON_ALIGNAS alignas(CACHE_LINE_SIZE)

// THE FOLLOWING FOR PREFETCH CPU INSTRUCTION.
#define SHANNON_PREFETCH_FOR_READ 0
#define SHANNON_PREFETCH_FOR_WRITE 1

#define SHANNON_PREFETCH__NONE_LOCALITY 0
#define SHANNON_PREFETCH_L3_LOCALITY 1
#define SHANNON_PREFETCH_L2_LOCALITY 2
#define SAHNNON_PREFETCH_L1_LOCALITY 3

namespace ShannonBase {
using row_id_t = size_t;
/** Handler name for rapid */
static constexpr char handler_name[] = "Rapid";
static constexpr char rapid_hton_name[] = "Rapid";

// the version of shannonbase.
constexpr uint SHANNONBASE_VERSION = 0x1;
constexpr uint SHANNON_RPD_VERSION = 0x0001;

// unit of shannonbase.
constexpr uint64 SHANNON_KB = 1024;
constexpr uint64 SHANNON_MB = SHANNON_KB * 1024;
constexpr uint64 SHANNON_GB = SHANNON_MB * 1024;

// some sizes used by imcs.
constexpr size_t SHANNON_ROWS_IN_CHUNK = 122880;
constexpr uint64 SHANNON_DEFAULT_MEMRORY_SIZE = 8 * SHANNON_GB;
constexpr uint64 SHANNON_MAX_MEMRORY_SIZE = SHANNON_DEFAULT_MEMRORY_SIZE;
constexpr uint64 SHANNON_DEFAULT_POPULATION_BUFFER_SIZE = 64 * SHANNON_MB;
constexpr uint64 SHANNON_MAX_POPULATION_BUFFER_SIZE = 64 * SHANNON_MB;

#define ALIGN_WORD(WORD, TYPE_SIZE) ((WORD + TYPE_SIZE - 1) & ~(TYPE_SIZE - 1))

constexpr uint SHANNON_BATCH_NUM = 8;

constexpr char SHANNON_NULL_PLACEHOLDER[] = "NULL_SHANNON_PLACEHOLDER";
constexpr char SHANNON_BLANK_PLACEHOLDER[] = "BLANK_CONTENT";

constexpr char SHANNON_DB_ROW_ID[] = "DB_ROW_ID";
constexpr size_t SHANNON_DB_ROW_ID_LEN = 9;

constexpr char SHANNON_DB_TRX_ID[] = "DB_TRX_ID";
constexpr size_t SHANNON_DB_TRX_ID_LEN = 9;
constexpr size_t SHANNON_DATA_DB_TRX_ID_LEN = 6;

constexpr char SHANNON_DB_ROLL_PTR[] = "DB_ROLL_PTR";
constexpr size_t SHANNON_DB_ROLL_PTR_LEN = 11;

// The lowest value, here, which means it's a invalid value. to describe its
// validity.
constexpr double SHANNON_LOWEST_DOUBLE = std::numeric_limits<double>::lowest();
constexpr double SHANNON_LOWEST_INT = std::numeric_limits<int>::lowest();

constexpr double SHANNON_EPSILON = 1e-10;
inline bool are_equal(double a, double b, double epsilon = SHANNON_EPSILON) { return (std::fabs(a - b) < epsilon); }

inline bool is_less_than(double a, double b, double epsilon = SHANNON_EPSILON) { return ((b - a) > epsilon); }
inline bool is_less_than_or_eq(double a, double b, double epsilon = SHANNON_EPSILON) {
  return (((b - a) > epsilon) || are_equal(a, b));
}

inline bool is_greater_than(double a, double b, double epsilon = SHANNON_EPSILON) { return ((a - b) > epsilon); }

inline bool is_greater_than_or_eq(double a, double b, double epsilon = SHANNON_EPSILON) {
  return (((a - b) > epsilon) || are_equal(a, b));
}

inline bool is_valid(double a) { return are_equal(a, SHANNON_LOWEST_DOUBLE); }

inline bool is_valid(int a) { return are_equal(a, SHANNON_LOWEST_INT); }
// This is use for Rapid cluster in future. in next, we will build up a AP clust
// for ShannonBase.
enum class RPD_NODE_ROLE {
  // meta node and primary role, name node.
  NODE_PRIMARY_NODE = 0,
  // secondary node: data node
  NODE_SECONDARY_NODE
};

enum class OPER_TYPE : uint8 { OPER_INSERT, OPER_UPDATE, OPER_DELETE };

}  // namespace ShannonBase
#endif  //__SHANNONBASE_CONST_H__
