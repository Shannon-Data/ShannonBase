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

   The fundmental code for imcs.

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/
#ifndef __SHANNONBASE_UTILS_H__
#define __SHANNONBASE_UTILS_H__

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "include/field_types.h"
#include "include/my_inttypes.h"
#include "sql-common/my_decimal.h"
#include "sql/field.h"  //Field
#include "sql/partition_element.h"
#include "sql/partition_info.h"
#include "sql/sql_base.h"
#include "sql/sql_class.h"                                        // THD
#include "sql/tztime.h"                                           //timzone
#include "storage/rapid_engine/compress/dictionary/dictionary.h"  //Dictionary
#include "storage/rapid_engine/include/rapid_arch_inf.h"          // SHANNON_OVERFLOW_BUILTINS_SUPPORTED
#include "storage/rapid_engine/include/rapid_types.h"             // bit_array_t

class THD;
class TABLE;
class Field;
class key_range;
class CHARSET_INFO;
namespace ShannonBase {
class Rapid_load_context;
namespace Compress {
class Dictionary;
}
namespace Utils {
// Overflow-checked arithmetic (SHANNON_ADD/SUB/MUL_OVERFLOW); true means
// overflow, and the wrapped result is written through the out parameter. The
// platform mapping -- native compiler builtins, or the portable fallback --
// lives in rapid_arch_inf.h so every component shares one definition.

// Configuration and shutdown share the wait mutex, so notifications cannot be
// lost between checking the predicate and entering the condition-variable wait.
class MaintenanceWait {
 public:
  using Duration = std::chrono::steady_clock::duration;
  explicit MaintenanceWait(Duration interval) : m_interval(interval) {}

  void set_interval(Duration interval) {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_interval = interval;
      ++m_generation;
    }
    m_cv.notify_all();
  }

  Duration interval() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_interval;
  }

  void start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopped = false;
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopped = true;
    }
    m_cv.notify_all();
  }

  // Return true only when the current interval expires. Changing the interval
  // restarts the deadline rather than performing maintenance immediately.
  bool wait() {
    return wait([] {});
  }

  // The callback observes wait entry while configuration remains locked.
  template <typename BeforeWait>
  bool wait(BeforeWait before_wait) {
    std::unique_lock<std::mutex> lock(m_mutex);
    before_wait();
    while (!m_stopped) {
      const auto generation = m_generation;
      if (!m_cv.wait_for(lock, m_interval, [&] { return m_stopped || generation != m_generation; })) return true;
    }
    return false;
  }

 private:
  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  Duration m_interval;
  unsigned long long m_generation{0};
  bool m_stopped{false};
};

/**
 * Build the Rapid routing key of one physical partition.
 *
 * Rapid stores a partitioned table as one sub-table per physical partition,
 * registered under "<part_name>#<part_id>".
 */
inline std::string rapid_partition_key(partition_info *part_info, uint32 part_id) {
  if (part_info == nullptr || part_id == NOT_A_PARTITION_ID || part_id >= part_info->get_tot_partitions())
    return std::string();

  const partition_element *elem = part_info->partitions[part_id];
  if (elem == nullptr || elem->partition_name == nullptr) return std::string();

  std::string key(elem->partition_name);
  key.append("#").append(std::to_string(part_id));
  return key;
}

class Util {
 public:
  /**
   * Create a spill file for an executor operator that has outgrown its memory
   * budget, and return it as a stdio stream.
   *
   * std::tmpfile() puts the file in P_tmpdir (/tmp), which ignores --tmpdir,
   * bypasses the server's temp-file accounting, and lands multi-gigabyte hash
   * and aggregate spills on whatever filesystem /tmp happens to be -- often a
   * tmpfs, where the "spill" consumes the RAM it was meant to relieve. This
   * routes through mysql_tmpfile() instead, so spills obey mysql_tmpdir and
   * are registered with mysys like every other server temp file. The file is
   * already unlinked, so closing the stream deletes it.
   *
   * Returns nullptr on failure; close with close_spill_file().
   */
  static FILE *create_spill_file(const char *prefix);

  /** Close a stream from create_spill_file(). Safe on nullptr. */
  static void close_spill_file(FILE *stream);

  /**
   * Build-side memory ceiling for the vectorized hash join, in bytes: the
   * unreserved part of rapid_memory_size_max, bounded by the host's
   * MemAvailable and floored at join_buffer_size.
   * The planner and the iterator must ask the same question, so both call this.
   */
  static size_t hash_join_memory_budget(const THD *thd);

  /** Same ceiling for the vectorized hash aggregate, floored at floor_bytes. */
  static size_t hash_aggregate_memory_budget(const THD *thd, size_t floor_bytes);

  /** Same ceiling for the vectorized sort, floored at sort_buffer_size. */
  static size_t sort_memory_budget(const THD *thd);

  // open a table via schema name and table name.
  static TABLE *open_table_by_name(THD *thd, std::string schema_name, std::string table_name, thr_lock_type mode);

  // close a opened table.
  static int close_table(THD *thd, TABLE *table);

  // to check whether this type is supported or not.
  static bool is_support_type(enum_field_types type);

  enum class STAGE { BEGIN = 0, END };
  static bool update_rpd_meta_info(const ShannonBase::Rapid_load_context *context, const TABLE *table,
                                   Util::STAGE stage = Util::STAGE::BEGIN);

  template <typename T>
  static T get_field_numeric(Field *field, const uchar *data_ptr, const Compress::Dictionary *dict,
                             bool db_low_byte_first = false);

  /**
    get_field_numeric() for a caller that already knows the field's type and
    signedness. Both are virtual calls, and the compiled ART key codec resolves
    them once per index rather than once per lookup.
  */
  template <typename T>
  static T get_field_numeric_typed(Field *field, enum_field_types field_type, bool is_unsigned, const uchar *data_ptr,
                                   const Compress::Dictionary *dict, bool db_low_byte_first = false);

  static int get_range_value(enum_field_types, const Compress::Dictionary *, const key_range *, const key_range *,
                             double &, double &);
  static int mem2string(uchar *buff, uint length, std::string &result);

  // convert a string padding format(with padding). if the length is less then
  // pack length, then padding the string with `space`(0x20) with dest charset.
  static uchar *pack_str(uchar *from, size_t length, const CHARSET_INFO *from_cs, uchar *to, size_t to_length,
                         const CHARSET_INFO *to_cs);

  static inline std::string get_key_name(Field *field) {
    std::ostringstream ostr;
    ostr << field->table->s->db.str << "." << *field->table_name << "." << field->field_name;
    return ostr.str();
  }

  static inline std::string get_key_name(const char *db_name, const char *table_name, const char *field_name) {
    std::ostringstream ostr;
    ostr << db_name << "." << table_name << "." << field_name;
    return ostr.str();
  }

  // Per-bit access. Bound is `rows`, not size*8: the padding bits in the last
  // byte must stay clear, or count_ones()/is_all_true() over-report.
  static inline bool bit_array_get(bit_array_t *ba, size_t n) {
    return ba && ba->data && n < ba->rows && ((ba->data[n >> 3] >> (n & 7)) & 1);
  }

  static inline bool bit_array_get_fast(const uint8_t *data, size_t n) { return (data[n >> 3] >> (n & 7)) & 1; }

  // set Nth is 1.
  static inline void bit_array_set(bit_array_t *ba, size_t n) {
    if (!ba || !ba->data || n >= ba->rows) return;
    ba->data[n >> 3] |= static_cast<uint8_t>(1u << (n & 7));
  }

  // reset Nth is 0.
  static inline void bit_array_reset(bit_array_t *ba, size_t n) {
    if (!ba || !ba->data || n >= ba->rows) return;
    ba->data[n >> 3] &= static_cast<uint8_t>(~(1u << (n & 7)));
  }

  static inline bool is_varstring(enum_field_types type) {
    /**if this is a string type, it will be use local dictionary encoding, therefore,
     * using stringid as field value. */
    return ((type == MYSQL_TYPE_VARCHAR || type == MYSQL_TYPE_VAR_STRING) ? true : false);
  }

  // Deliberately narrower than sql/field_common_properties.h's
  // is_string_type(), which also answers true for the BLOB family, ENUM, SET
  // and JSON. Rapid means "a char/varchar column, dictionary-encoded" here,
  // so the two are not interchangeable.
  static inline bool is_string(enum_field_types type) {
    return (type == MYSQL_TYPE_VARCHAR || type == MYSQL_TYPE_VAR_STRING || type == MYSQL_TYPE_STRING) ? true : false;
  }

  // Mirrors sql/field_common_properties.h's is_blob(): the four BLOB sizes plus
  // GEOMETRY and VECTOR. JSON is stored as a BLOB but reports its own type, so
  // callers that care about payload size must name it separately.
  static inline bool is_blob(enum_field_types type) {
    switch (type) {
      case MYSQL_TYPE_TINY_BLOB:
      case MYSQL_TYPE_MEDIUM_BLOB:
      case MYSQL_TYPE_BLOB:
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_GEOMETRY:
      case MYSQL_TYPE_VECTOR:
        return true;
      default:
        return false;
    }
  }

  // Payload lives off-page: only a pointer in the row image, a VarlenDataPool
  // reference in the CU. is_blob() plus JSON, which reports its own type.
  static inline bool is_off_page(enum_field_types type) { return is_blob(type) || type == MYSQL_TYPE_JSON; }

  /** A typed array reports its element type, so it is correctly not one. */
  static inline bool is_off_page(const Field *field) { return field != nullptr && is_off_page(field->type()); }

  // ENUM and SET keep their numeric code in the record image, so the image is
  // directly comparable and pack_length() is the whole key.
  static inline bool is_enum_or_set(enum_field_types type) { return type == MYSQL_TYPE_ENUM || type == MYSQL_TYPE_SET; }

  // The temporal family. These share one property that nothing else in the
  // engine relies on: the packed image is fixed width and ordered like the
  // value, so the raw bytes can serve as an equality or range key without a
  // make_sort_key() or a text round trip.
  static inline bool is_datetime(enum_field_types type) {
    switch (type) {
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_DATETIME2:
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_TIMESTAMP2:
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIME2:
        return true;
      default:
        return false;
    }
  }

  // Integer family, including YEAR, which stores a plain integer and encodes
  // into a group key exactly like one. Every member fits an int64 without loss.
  static inline bool is_integer(enum_field_types type) {
    switch (type) {
      case MYSQL_TYPE_TINY:
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_INT24:
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_YEAR:
        return true;
      default:
        return false;
    }
  }

  // The binary floats alone. Kept separate from is_double() because a DECIMAL
  // must never take the float path: its key comes from the packed bytes, and
  // reading it through val_real() would lose precision and produce bytes that
  // differ from the batch encoder's.
  static inline bool is_binary_float(enum_field_types type) {
    return type == MYSQL_TYPE_FLOAT || type == MYSQL_TYPE_DOUBLE;
  }

  // The non-integer numerics: the binary floats and DECIMAL. Named after the
  // float family because that is what the encoder branches on; DECIMAL shares
  // the predicate because every caller treats "not an exact integer" the same
  // way (a value read through val_real()/val_decimal() rather than val_int()).
  static inline bool is_double(enum_field_types type) { return is_binary_float(type) || type == MYSQL_TYPE_NEWDECIMAL; }

  // Mirrors sql/field_common_properties.h's is_numeric_type(): the integer
  // family plus the non-integer numerics.
  static inline bool is_numeric(enum_field_types type) { return is_integer(type) || is_double(type); }

  /**
   * Store raw binary data to a blob-like field using the base-class
   * Field_blob::store(), bypassing type-specific overrides such as
   * Field_json::store() (which would parse the input as JSON text).
   *
   * All varlen data (BLOB, TEXT, GEOMETRY, JSON, VECTOR) coming from
   * InnoDB is already in the correct internal binary format, so the
   * base-class store is the right choice for all of them.
   */
  static inline void store_blob_data(Field *fld, const char *data, size_t len) {
    down_cast<Field_blob *>(fld)->Field_blob::store(data, len, fld->charset());
  }

  static void write_trace_reason(THD *thd, const char *text, const char *reason);

  static std::vector<std::string> split(const std::string &str, char delimiter);

  static uint normalized_length(const Field *field);

  static inline std::string currenttime_to_string() {
    auto now = std::chrono::system_clock::now();
    auto now_time_t = std::chrono::system_clock::to_time_t(now);
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm = *std::localtime(&now_time_t);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "." << std::setfill('0') << std::setw(3) << now_ms.count();
    return oss.str();
  }
  static bool wait_for_server_bootup(int timeout_seconds = 300);
  static bool wait_for_server_bootup(int timeout_seconds, std::function<bool()> should_stop);
};

/** Pairs open_table_by_name() with close_table() on every scope exit.
 * The THD must outlive this guard. This releases the external handler lock
 * and completes the statement transaction as close_table() normally does;
 * TABLE storage remains owned by the THD until close_thread_tables().
 */
class TableGuard final {
 public:
  TableGuard(THD *thd, const std::string &schema, const std::string &table, thr_lock_type mode)
      : m_thd(thd), m_table(Util::open_table_by_name(thd, schema, table, mode)) {}

  // Adopt a table opened by open_table_by_name(), not an arbitrary borrowed TABLE.
  TableGuard(THD *thd, TABLE *table) noexcept : m_thd(thd), m_table(table) {}
  ~TableGuard() noexcept { close(); }

  TableGuard(const TableGuard &) = delete;
  TableGuard &operator=(const TableGuard &) = delete;
  TableGuard(TableGuard &&) = delete;
  TableGuard &operator=(TableGuard &&) = delete;

  TABLE *get() const noexcept { return m_table; }
  // Transfer the close obligation to another guard.
  TABLE *release() noexcept { return std::exchange(m_table, nullptr); }
  void close() noexcept {
    if (auto *table = release()) Util::close_table(m_thd, table);
  }

 private:
  THD *m_thd;
  TABLE *m_table;
};

class Table_closer {
 public:
  explicit Table_closer(THD *thd) : m_thd(thd) {}
  ~Table_closer() {
    if (!m_thd) return;
    if (m_thd && m_thd->is_system_thread()) close_thread_tables(m_thd);
  }

  Table_closer(const Table_closer &) = delete;
  Table_closer &operator=(const Table_closer &) = delete;

 private:
  THD *m_thd{nullptr};
};

class ColumnMapGuard {
 public:
  enum class TYPE : uint8_t { READ = 0, WRITE, ALL };
  ColumnMapGuard(TABLE *t, TYPE type = TYPE::READ);
  ~ColumnMapGuard();

  ColumnMapGuard(const ColumnMapGuard &) = delete;
  ColumnMapGuard &operator=(const ColumnMapGuard &) = delete;

 private:
  TYPE bit_type{TYPE::READ};
  TABLE *table{nullptr};
  my_bitmap_map *old_rmap{nullptr};
  my_bitmap_map *old_wmap{nullptr};
};
}  // namespace Utils
}  // namespace ShannonBase

// Templates live in the .tpp, which opens ShannonBase::Utils itself.
#include "storage/rapid_engine/utils/utils.tpp"

#endif  //__SHANNONBASE_UTILS_H__
