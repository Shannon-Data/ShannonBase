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

   Copyright (c) 2026, Shannon Data AI and/or its affiliates.
*/
#ifndef SHANNON_CHANGE_RECORD_H
#define SHANNON_CHANGE_RECORD_H
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_propagation_config.h"
#include "storage/rapid_engine/include/rapid_table_info.h"
#include "storage/rapid_engine/utils/row_image_buffer.h"

namespace ShannonBase {
struct BinlogPosition {
  std::string file;
  uint64_t offset{0};
  std::string prefix_digest;
  bool valid() const { return !file.empty() && offset >= 4; }
};
// it's an iterterface struct to store all the info of changed data. such as where it comes from
// data and its length, etc.
typedef struct SHANNON_ALIGNAS change_record_buff_t {
  using off_page_data_t =
      std::map<size_t, std::pair<size_t, std::shared_ptr<uchar[]>>>;  //<field_id, <length, off_page_data>>
  Source m_source;                                                    // data source
  CopyInfoMode m_copy_info_mode{CopyInfoMode::NOTIFICATION};
  size_t m_size;
  size_t m_accounted_bytes{0};
  enum class OperType : uint8 { UNSET = 0, INSERT, DELETE, UPDATE } m_oper{OperType::UNSET};  // oper type
  table_id_t m_table_id{0};

  // COPY_INFO transaction metadata. Row notifications are queued immediately
  // with the real primary InnoDB creator id and commit_scn == 0, which means
  // ACTIVE in Rapid MVCC. Primary COMMIT/ROLLBACK publishes only the outcome;
  // SourceTxnTracker later finalizes the version to a Rapid commit
  // SCN or aborts it. SQL visibility always uses the primary InnoDB ReadView.
  uint64_t m_source_trx_id{0};
  uint64_t m_notification_sequence{0};  // volatile notification identity
  uint64_t m_commit_scn{0};             // 0 => ACTIVE notification version

  // Physical partition routing. A partitioned Rapid table stores its rows in
  // one sub-table per partition (PartTable::build_partitions), so m_table_id
  // alone only reaches the parent. The capture side owns the TABLE and its
  // partition_info, so it resolves the partition there and carries the key
  // ("<part_name>#<part_id>") in the record; the apply worker has no TABLE of
  // its own and cannot recompute it. Both are empty for a non-partitioned
  // table. m_old_part_key is the partition the pre-image lives in
  // (DELETE/UPDATE), m_part_key the one the post-image belongs to
  // (INSERT/UPDATE); they differ when an UPDATE moves a row across partitions.
  std::string m_part_key;
  std::string m_old_part_key;
#ifndef NDEBUG
  std::string m_schema_name, m_table_name;
#endif
  std::shared_ptr<uchar[]> m_buff0{nullptr};  // rep: record[0]
  off_page_data_t m_offpage_data0;
  std::shared_ptr<uchar[]> m_buff1{nullptr};  // rep: record[1]
  off_page_data_t m_offpage_data1;            // using to store offpage data.

  change_record_buff_t(Source sc, size_t s) : m_source(sc), m_size(s) {
    if (!s) return;
    if (s > std::numeric_limits<size_t>::max() / 2) throw std::length_error("row image size overflow");
    m_buff0 = RowImageBufferPool::acquire(2 * s);
    // Both images share one allocation and lifetime; copies remain detached
    // from TABLE even when the producer thread has already exited.
    m_buff1 = std::shared_ptr<uchar[]>(m_buff0, m_buff0.get() + s);
  }

  change_record_buff_t() : m_source(Source::UN_KNOWN), m_size(0), m_oper(OperType::UNSET) {}

  change_record_buff_t(const change_record_buff_t &other) = default;
  change_record_buff_t &operator=(const change_record_buff_t &other) = default;
  change_record_buff_t(change_record_buff_t &&other) noexcept = default;
  change_record_buff_t &operator=(change_record_buff_t &&other) noexcept = default;
  ~change_record_buff_t() = default;

  size_t payload_bytes() const noexcept {
    if (m_accounted_bytes) return m_accounted_bytes;
    size_t bytes = 2 * m_size;
    for (const auto *image : {&m_offpage_data0, &m_offpage_data1})
      for (const auto &entry : *image) {
        if (entry.second.first > std::numeric_limits<size_t>::max() - bytes) return std::numeric_limits<size_t>::max();
        bytes += entry.second.first;
      }
    return bytes;
  }

  inline bool is_committed_binlog() const noexcept {
    return m_source == Source::COPY_INFO && m_copy_info_mode == CopyInfoMode::COMMITTED_BINLOG;
  }

  inline uchar *get_buff0() const { return m_buff0.get(); }
  inline uchar *get_buff1() const { return m_buff1.get(); }

  inline bool has_buff0() const { return m_buff0 != nullptr; }
  inline bool has_buff1() const { return m_buff1 != nullptr; }
  inline long use_count_buff0() const { return m_buff0.use_count(); }
  inline long use_count_buff1() const { return m_buff1.use_count(); }
} change_record_buff;

/**
 * Result of offering one buffered change record to the module that owns its
 * record format (log_redolog.cc / log_dml_notification.cc).
 *
 * The propagation worker uses this to decide whether to advance the apply
 * watermark, retry the same record in place, or quarantine the table. Keeping
 * that classification here, rather than re-deriving it from Source inside the
 * worker, is what lets the worker stay free of per-source knowledge.
 */
struct SHANNON_ALIGNAS ChangeApplyResult {
  enum class Status : uint8_t {
    APPLIED = 0, /** record fully consumed; the apply watermark may advance */
    RETRYABLE,   /** transient failure; retry the same record in place */
    PERMANENT    /** unrecoverable; the table must be quarantined */
  };

  Status status{Status::RETRYABLE};
  size_t parsed_bytes{0};
  // Why the table goes stale if this result ends in quarantine. A retryable
  // failure that exhausts the retry budget also reuses this reason.
  stale_reason_t stale_reason{stale_reason_t::UNIDENTIFIED_ERROR};
};

}  // namespace ShannonBase
#endif
