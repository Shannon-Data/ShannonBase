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
#include <chrono>
#include <cmath>
#include <vector>
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

#if defined(__SSE__) || defined(__SSE2__)
#define SHANNON_SSE_VECT_SUPPORTED
#include <emmintrin.h>
#elif defined(__AVX__) || defined(__AVX2__)
#include <immintrin.h>
#define SHANNON_AVX_VECT_SUPPORTED
#endif

#if defined(SHANNON_SSE_VECT_SUPPORTED) || defined(SHANNON_AVX_VECT_SUPPORTED)
#define SHANNON_VECTORIZE_SUPPORT
#endif
#define SHANNON_VECTOR_WIDTH 8

extern char *mysql_llm_home_ptr;
namespace ShannonBase {
using row_id_t = size_t;
/** Handler name for rapid */
constexpr auto SHANNON_SUCCESS = 0;

static constexpr char handler_name[] = "Rapid";
static constexpr char rapid_hton_name[] = "Rapid";
static constexpr char rapidpart_hton_name[] = "RapidPart";

// the version of shannonbase.
constexpr uint SHANNONBASE_VERSION = 0x1;
constexpr uint SHANNON_RPD_VERSION = 0x0001;

// unit of shannonbase.
constexpr uint64 SHANNON_KB = 1024;
constexpr uint64 SHANNON_MB = SHANNON_KB * 1024;
constexpr uint64 SHANNON_GB = SHANNON_MB * 1024;

// some sizes used by imcs.
constexpr size_t SHANNON_ROWS_IN_CHUNK = 819200;
constexpr uint64 SHANNON_DEFAULT_MEMRORY_SIZE = 8 * SHANNON_GB;
constexpr uint64 SHANNON_MAX_MEMRORY_SIZE = SHANNON_DEFAULT_MEMRORY_SIZE;
constexpr uint64 SHANNON_POPULATION_HRESHOLD_SIZE = 64 * SHANNON_MB;
constexpr uint64 SHANNON_MAX_POPULATION_BUFFER_SIZE = 128 * SHANNON_MB;
constexpr double SHANNON_TO_MUCH_POP_THRESHOLD_RATIO = 0.85;
constexpr uint64 SHANNON_POP_BUFF_THRESHOLD_COUNT = 10000;
constexpr uint64 SHANNON_PARALLEL_LOAD_THRESHOLD = 1000000;

#define ALIGN_WORD(WORD, TYPE_SIZE) ((WORD + TYPE_SIZE - 1) & ~(TYPE_SIZE - 1))

constexpr uint MAX_N_FIELD_PARALLEL = 128;
constexpr uint DEFAULT_N_FIELD_PARALLEL = 16;
constexpr uint SHANNON_BATCH_NUM = 8;

constexpr char SHANNON_DB_ROW_ID[] = "DB_ROW_ID";
constexpr size_t SHANNON_DB_ROW_ID_LEN = 9;
constexpr size_t SHANNON_DATA_DB_ROW_ID_LEN = 6;

constexpr char SHANNON_DB_TRX_ID[] = "DB_TRX_ID";
constexpr size_t SHANNON_DB_TRX_ID_LEN = 9;
constexpr size_t SHANNON_DATA_DB_TRX_ID_LEN = 6;

constexpr char SHANNON_DB_ROLL_PTR[] = "DB_ROLL_PTR";
constexpr size_t SHANNON_DB_ROLL_PTR_LEN = 11;

constexpr char SHANNON_NULL_PLACEHOLDER[] = "NULL";
constexpr char SHANNON_BLANK_PLACEHOLDER[] = "BLNK";

constexpr char SHANNON_PRIMARY_KEY_NAME[] = "PRIMARY";
constexpr size_t SHANNON_PRIMARY_KEY_LEN = 7;

enum class SYS_FIELD_TYPE_ID { SYS_DB_TRX_ID = 1, SYS_DB_ROW_ID = 2, DB_ROLL_PTR = 3, REGULAR = 0 };

// The lowest value, here, which means it's a invalid value. to describe its
// validity.
constexpr double SHANNON_MIN_DOUBLE = std::numeric_limits<double>::min();
constexpr double SHANNON_MAX_DOUBLE = std::numeric_limits<double>::max();
constexpr int SHANNON_MIN_INT = std::numeric_limits<int>::min();
constexpr int SHANNON_MAX_INT = std::numeric_limits<int>::max();
constexpr row_id_t INVALID_ROW_ID = std::numeric_limits<size_t>::max();

constexpr auto SHANNON_MAX_STMP = std::chrono::time_point<std::chrono::high_resolution_clock>::max();
constexpr auto SHANNON_MAX_TRX_ID = std::numeric_limits<uint64_t>::max();
constexpr auto SHANNON_GC_RATIO_THRESHOLD = 0.8;

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

inline bool is_valid(double a) { return are_equal(a, SHANNON_MIN_DOUBLE); }

inline bool is_valid(int a) { return are_equal(a, SHANNON_MIN_INT); }
// This is use for Rapid cluster in future. in next, we will build up a AP clust
// for ShannonBase.
enum class RPD_NODE_ROLE {
  // meta node and primary role, name node.
  NODE_PRIMARY_NODE = 0,
  // secondary node: data node
  NODE_SECONDARY_NODE
};

enum class OPER_TYPE : uint8 { OPER_INSERT, OPER_UPDATE, OPER_DELETE };
constexpr int PREFETCH_AHEAD = 2;

#if !defined(__cplusplus) && (!defined(__STDC__) || (__STDC_VERSION__ < 201112L))
/*! \brief Thread local specifier no-op in C using standards before C11. */
#define SHANNON_THREAD_LOCAL
#elif !defined(__cplusplus)
/*! \brief Thread local specifier. */
#define SHANNON_THREAD_LOCAL _Thread_local
#elif defined(_MSC_VER)
/*! \brief Thread local specifier. */
#define SHANNON_THREAD_LOCAL __declspec(thread)
#else
/*! \brief Thread local specifier. */
#define SHANNON_THREAD_LOCAL thread_local
#endif

// key_part_len, part name of key. such as composite index <keypart1, keypart2, ..., keypartn>
// key_part_len, the field length of that key part. and the field name of that key part.
using key_meta_t = std::pair<uint, std::vector<std::string>>;

}  // namespace ShannonBase
#endif  //__SHANNONBASE_CONST_H__
