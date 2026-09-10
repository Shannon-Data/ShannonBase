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

extern char *mysql_llm_home_ptr;
namespace ShannonBase {
using row_id_t = size_t;
using table_id_t = uint64_t;
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
static constexpr char SHANNON_DATA_AREAR_NAME[] = "DATA_AREAR";
static constexpr char SHANNON_META_AREAN_NAME[] = "META_DATA_AREAR";

constexpr size_t SHANNON_ROWS_IN_CHUNK = 81920;

// rapid_memory_size_max is a byte count, following the MySQL convention that
// innodb_buffer_pool_size and friends set: getopt already understands the K/M/G
// suffixes, so an operator writes "4G", not 4294967296 and not a megabyte count
// that reads like one. The config field is named for its unit -- it used to be
// memory_pool_size_mb while holding bytes, which is the only part of this that
// ever misled anyone.
constexpr uint64 SHANNON_DEFAULT_MEMRORY_SIZE = 2 * SHANNON_GB;
// A real ceiling, not an alias of the default: rapid_memory_size_max was
// declared with def == min == max, which pinned the pool at 2GB and made the
// variable unsettable from my.cnf (getopt clamps to [min,max]) as well as
// unraisable at runtime. Sizing the pool for the working set is the operator's
// call, and nothing here second-guesses it against physical memory.
// initialize_pools() halves its request until it fits and logs what it settled
// for, so an absurdly small pool degrades loudly rather than corrupting
// anything.
constexpr uint64 SHANNON_MIN_MEMRORY_SIZE = 1;
constexpr uint64 SHANNON_MAX_MEMRORY_SIZE = 64 * SHANNON_GB;

// A table's sub-pool is carved out of the pool above in one piece and never
// expands, so its size has to be right at create time: at a flat 128MB a
// table stopped loading near a million rows (81920 rows/IMCU * 8B * 16
// columns ~= 10.5MB per IMCU). It is now derived from the InnoDB table's own
// data volume (see estimate_table_pool_size) and bounded only by the pool
// itself -- there is deliberately no per-table ceiling to configure, because
// a table's size is not something an operator can know in advance and the
// pool-wide rapid_memory_size_max is already the budget that matters.
// The two constants below are a floor for that estimate and the fixed size
// used for placeholder/parent tables, not policy limits.
constexpr uint64 SHANNON_TABLE_MEMRORY_SIZE = 128 * SHANNON_MB;
constexpr uint64 SHANNON_SMALL_TABLE_MEMRORY_SIZE = 64 * SHANNON_MB;
constexpr uint64 SHANNON_MIN_TABLE_MEMRORY_SIZE = 16 * SHANNON_MB;

constexpr uint64 SHANNON_POPULATION_HRESHOLD_SIZE = 64 * SHANNON_MB;
constexpr uint64 SHANNON_MAX_POPULATION_BUFFER_SIZE = 256 * SHANNON_MB;
constexpr double SHANNON_TO_MUCH_POP_THRESHOLD_RATIO = 0.85;
constexpr uint64 SHANNON_POP_BUFF_THRESHOLD_COUNT = 10000;

constexpr uint64 SHANNON_PARALLEL_LOAD_THRESHOLD = 10000;
constexpr uint64 SHANNON_PARALLEL_PARTTB_THRESHOLD = 32;

constexpr uint64 SHANNON_DEFAULT_SELF_LOAD_INTERVAL = 86400;
constexpr uint64 SHANNON_DEFAULT_SELF_LOAD_FILL_PERCENTAGE = 70;

constexpr uint64 SHANNON_DEFAULT_MAX_PURGER_TIMEOUT = 5000;
constexpr uint SHANNON_MIN_PURGER_TIMEOUT = 256;
// rapid_purge_batch_size related
constexpr uint SHANNON_DEFAULT_PURGE_BATCH_SIZE = 64;
constexpr uint SHANNON_MIN_PURGE_BATCH_SIZE = 1;
constexpr uint SHANNON_MAX_PURGE_BATCH_SIZE = 65536;
// rapid_min_versions_for_purge related
constexpr uint SHANNON_DEFAULT_MIN_VERSIONS_FOR_PURGE = 10;
// rapid_purge_efficiency_threshold
constexpr double SHANNON_DEFAULT_PURGE_EFFICIENCY_THRESHOLD = 0.1;

constexpr double SHANNON_HIGH_DELETE_RATIO = 0.3;
constexpr double SHANNON_MEDIUM_DELETE_RATIO = 0.2;
constexpr size_t SHANNON_LARGE_DELETE_COUNT = 10000;

constexpr size_t SHANNON_DEFAULT_GC_INTERVAL_SCN = 1000000;
constexpr size_t SHANNON_DEFAULT_GC_INTERVAL_TIME = 30;

const uint SHANNON_MAX_COLUMNS = 256;

constexpr uint MAX_N_FIELD_PARALLEL = 128;
constexpr uint DEFAULT_N_FIELD_PARALLEL = 16;
constexpr uint SHANNON_BATCH_NUM = 1024;

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

enum class OPER_TYPE : uint8 { OPER_NONE = 0, OPER_INSERT, OPER_UPDATE, OPER_DELETE };

// key_part_len, part name of key. such as composite index <keypart1, keypart2, ..., keypartn>
// key_part_len, the field length of that key part. and the field name of that key part.
using key_meta_t = std::pair<uint, std::vector<std::string>>;

// optimization factors.
// read factor.
constexpr double SHANNON_HD_READ_FACTOR = 0.8f;
constexpr double SHANNON_RAM_READ_FACTOR = 0.2f;

// cpu factor.
constexpr double SHANNON_CPU_FACTOR = 0.05f;

/**
 * Thin internal result/error model for the IMCS core.
 *
 * The storage core must NOT leak MySQL handler error codes (HA_ERR_*) across
 * its API boundary.  Those codes are translated to HA_ERR_* only in the
 * handler layer.  Keeping a single, small error vocabulary here lets callers
 * reason about failures without mixing bool / int / HA_ERR_* / row_id_t.
 */
enum class ErrorCode : uint8 {
  OK = 0,
  NOT_FOUND,
  OUT_OF_RANGE,
  NO_SPACE,
  IO_ERROR,
  CORRUPTION,
  CONFLICT,
  UNSUPPORTED,
  INTERNAL
};

template <typename T>
struct Result {
  ErrorCode error{ErrorCode::OK};
  T value{};

  Result() = default;
  explicit Result(ErrorCode e) : error(e) {}
  Result(ErrorCode e, T v) : error(e), value(std::move(v)) {}

  bool ok() const { return error == ErrorCode::OK; }
  explicit operator bool() const { return ok(); }
};

}  // namespace ShannonBase
#endif  //__SHANNONBASE_CONST_H__