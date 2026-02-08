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
struct Row_Result;
class Predicate;

class RapidCursor;
struct RecieverBase {
  inline void on_batch_begin() {}
  inline void on_batch_end() {}
  inline bool accept_more() const { return true; }
};

struct RowBufferRecv : RecieverBase {
  RowBufferRecv(RapidCursor *c, std::vector<std::unique_ptr<RowBuffer>> &buf) : cursor(c), cache(buf) {}
  RapidCursor *cursor{nullptr};
  std::vector<std::unique_ptr<RowBuffer>> &cache;

  void on_batch_begin() { cache.clear(); }
  void on_row(row_id_t rowid, const std::vector<const uchar *> &row_data);
};

struct ColumnChunkRecv : RecieverBase {
  ColumnChunkRecv(RapidCursor *c, const std::vector<uint32_t> &proj, std::vector<Executor::ColumnChunk> &ch,
                  size_t &cnt)
      : cursor(c), projection_cols(proj), chunks(ch), read_cnt(cnt) {}
  RapidCursor *cursor{nullptr};
  const std::vector<uint32_t> &projection_cols;
  std::vector<Executor::ColumnChunk> &chunks;
  size_t &read_cnt;

  void on_batch_begin() { read_cnt = 0; }
  void on_row(row_id_t, const std::vector<const uchar *> &row_data);
};

class RapidCursor : public MemoryObject {
 public:
  struct SHANNON_ALIGNAS CursorState {
    size_t curr_imcu_idx{0};
    size_t curr_imcu_offset{0};
    std::atomic<size_t> curr_row_idx{0};
    size_t batch_start{0};
    size_t batch_end{0};
    std::atomic<bool> exhausted{false};
    void reset() {
      curr_imcu_idx = 0;
      curr_imcu_offset = 0;
      curr_row_idx.store(0, std::memory_order_release);
      batch_start = 0;
      batch_end = 0;
      exhausted.store(false, std::memory_order_release);
    }
    void seek(row_id_t row_id, size_t rows_per_imcu) {
      curr_row_idx.store(row_id, std::memory_order_release);
      curr_imcu_idx = row_id / rows_per_imcu;
      curr_imcu_offset = row_id % rows_per_imcu;
      batch_start = 0;
      batch_end = 0;
      exhausted.store(false, std::memory_order_release);
    }
  };

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
    m_scan_state.reset();
    m_scan_state.curr_row_idx.store(0, std::memory_order_release);
  }

  inline TABLE *source() const { return m_data_source; }

  int open();
  int close();
  // Begin transaction, build ColumnChunk buffers, reset all scan state.
  int init();
  // Commit transaction, release scan state.
  int end();

  // to the next rows.
  int next(uchar *buf);

  boost::asio::awaitable<int> next_async(uchar *buf);

  // read the data in data in batch mode.
  int next(size_t batch_size, std::vector<ShannonBase::Executor::ColumnChunk> &data, size_t &read_cnt);

  // Random-access / position
  int rnd_pos(uchar *buff, uchar *pos);
  row_id_t position(const unsigned char *record);

  int rnd_pos(uchar *buff, uchar *pos);

  row_id_t position(const unsigned char *record);

  // get the data pos.
  row_id_t find(uchar *buf);

  // Index scan
  int index_init(uint keynr, bool sorted);
  int index_end();
  int index_read(uchar *buf, const uchar *key, uint key_len, ha_rkey_function find_flag);
  int index_next(uchar *buf);
  int index_prev(uchar *buf);

  inline void set_end_range(key_range *end_range) { m_end_range = end_range; }

  inline void set_scan_predicates(std::unique_ptr<Predicate> pred) {
    m_scan_predicates.clear();
    if (pred) m_scan_predicates.push_back(std::move(pred));
  }

  inline void set_projection_columns(const std::vector<uint32_t> &cols) {
    m_projection_columns = cols;
    m_proj_cols_dirty = true;
  }

  // SQL:  SELECT … LIMIT <limit> OFFSET <offset>
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

  // Returns the cached projection column list (read_set ∪ m_projection_columns).
  // Rebuilt lazily when m_proj_cols_dirty is true.
  std::vector<uint32_t> projection_columns() const;

  std::vector<uint32_t> projection_columns() const;

  // Helper method to encode key parts for ART storage
  void encode_key_parts(uchar *encoded_key, const uchar *original_key, uint key_len, KEY *key_info);

  // Populate one MySQL row from the current position in m_col_chunks.
  // row_idx is the row offset within the current batch.
  int populate_row_from_chunks(size_t row_idx);

  // (Re)initialise m_col_chunks based on the current table read_set.
  // Must be called after init() has set up m_data_source and m_rpd_table.
  void init_col_chunks();

  template <typename Reciever>
  size_t scan_batch_core(size_t batch_size, const std::vector<uint32_t> &projection_cols, Reciever &sink);

  int position(row_id_t start_row_id);

  template <typename Reciever>
  size_t scan_batch_internal(size_t batch_size, const std::vector<uint32_t> &projection_cols, Reciever &sink);

 private:
  std::atomic<bool> m_inited{false};
  TABLE *m_data_source{nullptr};
  RpdTable *m_rpd_table{nullptr};      ///< active partition (or full table)
  RpdTable *m_src_rpd_table{nullptr};  ///< root/parent table

  // rapid table.
  RpdTable *m_rpd_table{nullptr}, *m_src_rpd_table{nullptr} /**if it partition rpd, pointer to parent rpd. */;

  CursorState m_scan_state;

  // Batch mode flag
  std::atomic<bool> m_using_batch{true};

  // context
  std::unique_ptr<Rapid_scan_context> m_scan_context{nullptr};

  CursorState m_scan_state;

  std::vector<ShannonBase::Executor::ColumnChunk> m_col_chunks;  ///< one per field
  std::vector<row_id_t> m_batch_row_ids;                         ///< parallel row-id array

  // Read by position(const uchar*) for rnd_pos() support.
  row_id_t m_last_returned_rowid{INVALID_ROW_ID};

  ha_rows m_scan_limit{HA_POS_ERROR};
  ha_rows m_scan_offset{0};
  ha_rows m_rows_skipped{0};   ///< qualifying rows discarded so far (OFFSET)
  ha_rows m_rows_returned{0};  ///< qualifying rows returned so far  (LIMIT)

  std::vector<uint32_t> m_projection_columns;       ///< explicit override (may be empty)
  mutable std::vector<uint32_t> m_proj_cols_cache;  ///< computed: read_set ∪ m_projection_columns
  mutable bool m_proj_cols_dirty{true};             ///< true → rebuild on next access

  std::unique_ptr<Index::Iterator, IteratorDeleter> m_index_iter;
  int8_t m_active_index{MAX_KEY};
  std::unique_ptr<uchar[]> m_key{nullptr};
  key_range *m_end_range{nullptr};
  std::unique_ptr<uchar[]> m_end_key{nullptr};

  mutable std::mutex m_predicate_mutex;
  std::vector<std::unique_ptr<Predicate>> m_scan_predicates;
  bool m_use_storage_index{false};

  std::atomic<uint64_t> m_total_rows_scanned{0};
  std::atomic<uint64_t> m_batch_fetch_count{0};
};
}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_TABLE_VIEW_H__