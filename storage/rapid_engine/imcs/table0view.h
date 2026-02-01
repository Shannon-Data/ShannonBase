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

   Copyright (c) 2023, 2024, Shannon Data AI and/or its affiliates.

   The fundmental code for imcs.
*/
#ifndef __SHANNONBASE_TABLE_VIEW_H__
#define __SHANNONBASE_TABLE_VIEW_H__

#include <atomic>
#include <functional>
#include <utility>
#include <vector>

#include "storage/rapid_engine/include/rapid_types.h"

#include "storage/rapid_engine/executor/iterators/iterator.h"
#include "storage/rapid_engine/imcs/index/iterator.h"

#include "storage/rapid_engine/imcs/row0row.h"
#include "storage/rapid_engine/utils/concurrent.h"  //asio
#include "storage/rapid_engine/utils/cpu.h"         //SimpleRatioAdjuster
class TABLE;
class key_range;
namespace ShannonBase {
class Rapid_context;
class Rapid_load_context;
class Rapid_scan_context;
namespace Imcs {
class Imcs;
class Cu;
class RapidTable;
class RpdTable;
class RowBuffer;
struct Row_Result {};
class Predicate;
class RapidCursor : public MemoryObject {
 public:
  RapidCursor(TABLE *source_table, RpdTable *rpd);
  virtual ~RapidCursor() = default;

  // gets its active rapid table.
  inline RpdTable *table() const { return m_rpd_table; }

  // the parent table of partitions.
  inline RpdTable *table_source() const { return m_src_rpd_table; }

  // to reset to a new rpd table source. Used in Partition Table case.
  inline void active_table(RpdTable *rpd_table) {
    m_rpd_table = rpd_table;

    // Reserve buffer cache with extra space to reduce reallocation
    m_row_buffer_cache.clear();
    m_row_buffer_cache.reserve(SHANNON_BATCH_NUM + 16);

    // Initialize scan position
    m_curr_row_idx.store(0, std::memory_order_release);

    m_batch_start = 0;
    m_batch_end = 0;
    m_curr_imcu_idx = 0;
    m_curr_imcu_offset = 0;
    m_scan_exhausted.store(false, std::memory_order_release);
  }

  inline TABLE *source() const { return m_data_source; }

  // open a cursor on db_table to read/write.
  int open();

  // close a cursor.
  int close();

  // intitialize this data table object.
  int init();

  int scan_table(const std::vector<std::unique_ptr<Predicate>> &predicates, const std::vector<uint32_t> &projection,
                 RowCallback &callback);

  size_t scan_table(row_id_t start_offset, size_t limit, const std::vector<std::unique_ptr<Predicate>> &predicates,
                    const std::vector<uint32_t> &projection, RowCallback &callback);

  bool read(const uchar *key_value, Row_Result &result);

  bool range_scan(const uchar *start_key, const uchar *end_key, RowCallback &cb);

  // to the next rows.
  int next(uchar *buf);

  int rnd_pos(uchar *buff, uchar *pos);

  row_id_t position(const unsigned char *record);

  boost::asio::awaitable<int> next_async(uchar *buf);

  // read the data in data in batch mode.
  int next_batch(size_t batch_size, std::vector<ShannonBase::Executor::ColumnChunk> &data, size_t &read_cnt);

  // end of scan.
  int end();

  // get the data pos.
  row_id_t find(uchar *buf);

  // for index scan initialization.
  int index_init(uint keynr, bool sorted);

  // for index scan end.
  int index_end();

  // index read. start_key is true, if start_key is not null or start_key is false.
  int index_read(uchar *buf, const uchar *key, uint key_len, ha_rkey_function find_flag);

  // index read next
  int index_next(uchar *buf);

  // index read prev
  int index_prev(uchar *buf);

  inline void set_end_range(key_range *end_range) { m_end_range = end_range; }

  inline void set_scan_predicates(std::unique_ptr<Predicate> pred) {
    m_scan_predicates.clear();
    if (pred) m_scan_predicates.push_back(std::move(pred));
  }

  inline void set_projection_columns(const std::vector<uint32_t> &cols) { m_projection_columns = cols; }

  // limit Maximum rows to return (HA_POS_ERROR = no limit)
  inline void set_scan_limit(ha_rows limit, ha_rows offset) {
    m_scan_limit = limit;
    m_scan_offset = offset;
  }

  inline void enable_storage_index() { m_use_storage_index = true; }

 private:
  struct IteratorDeleter {
    void operator()(Index::Iterator *p) const {
      delete p;  // Now safe because of virtual destructor
    }
  };
  enum class FETCH_STATUS : uint8 { FETCH_ERROR, FETCH_OK, FETCH_CONTINUE, FETCH_NEXT_ROW };

  // Helper method to encode key parts for ART storage
  void encode_key_parts(uchar *encoded_key, const uchar *original_key, uint key_len, KEY *key_info);

  // Helper to fetch next batch of rows
  bool fetch_next_batch(size_t batch_size = SHANNON_BATCH_NUM);

  int position(row_id_t start_row_id);

 private:
  std::atomic<bool> m_inited{false};

  // the data source, an IMCS.
  TABLE *m_data_source{nullptr};

  // rapid table.
  RpdTable *m_rpd_table{nullptr}, *m_src_rpd_table{nullptr} /**if it partition rpd, pointer to parent rpd. */;

  // start from where.
  std::atomic<row_id_t> m_curr_row_idx{0};

  std::atomic<bool> m_using_batch{true};

  size_t m_batch_start{0};  // Current batch boundaries
  size_t m_batch_end{0};

  size_t m_curr_imcu_idx{0};                  // Which IMCU we're currently scanning
  size_t m_curr_imcu_offset{0};               // Offset within current IMCU
  std::atomic<bool> m_scan_exhausted{false};  // All IMCUs scanned

  // context
  std::unique_ptr<Rapid_scan_context> m_scan_context{nullptr};

  // index iterator.
  std::unique_ptr<Index::Iterator, IteratorDeleter> m_index_iter;

  // active index id.
  int8_t m_active_index{MAX_KEY};

  // key buff to store the encoded key data.
  std::unique_ptr<uchar[]> m_key{nullptr};

  // end key buff to store the encoded key data if it end_range is available.
  key_range *m_end_range{nullptr};
  std::unique_ptr<uchar[]> m_end_key{nullptr};

  // Buffer to hold scanned rows
  std::vector<std::unique_ptr<RowBuffer>> m_row_buffer_cache;
  std::mutex m_buffer_mutex;

  static ShannonBase::Utils::SimpleRatioAdjuster m_adaptive_ratio;

  // Performance metrics
  std::atomic<uint64_t> m_total_rows_scanned{0};
  std::atomic<uint64_t> m_batch_fetch_count{0};

  // using to filter the scan with predicates.
  mutable std::mutex m_predicate_mutex;
  std::vector<std::unique_ptr<Predicate>> m_scan_predicates;

  std::vector<uint32_t> m_projection_columns;

  ha_rows m_scan_limit{HA_POS_ERROR};
  ha_rows m_scan_offset{0};
  bool m_use_storage_index{false};
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_TABLE_VIEW_H__