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
#ifndef __SHANNONBASE_CHUNK_H__
#define __SHANNONBASE_CHUNK_H__

#include <atomic>
#include <shared_mutex>
#include <tuple>
#include <unordered_map>

#include "field_types.h"  //for MYSQL_TYPE_XXX
#include "sql/field.h"    //Field
#include "sql/sql_class.h"
#include "storage/rapid_engine/include/rapid_arch_inf.h"  //cache line sz
#include "storage/rapid_engine/include/rapid_const.h"
#include "storage/rapid_engine/include/rapid_object.h"  // SHANNON_ALIGNAS
#include "storage/rapid_engine/trx/readview.h"
#include "storage/rapid_engine/trx/transaction.h"  //Transaction

namespace ShannonBase {
class Rapid_context;
class Rapid_load_context;
class Rapid_scan_context;
namespace Imcs {
class Cu;
/**
 * The chunk is an memory pool area, where the data writes here.
 * The format of rows is following: (ref to:
 * https://github.com/Shannon-Data/ShannonBase/issues/8)
 * Every chunk has some validity bitmap in its header. they are used to describ
 * whether data is null or valid in this position. and all text data are
 * encoding with dictionarycompression algorithm. */
template <typename T>
struct SHANNON_ALIGNAS chunk_deleter_helper {
  void operator()(T *ptr) {
    if (ptr) my_free(ptr);
  }
};

class Chunk : public MemoryObject {
 public:
  /**TODO: A Snapshot Metadata Unit (SMU) contains metadata and transactional
   * information for an associated IMCU.*/
  using Chunk_header = struct SHANNON_ALIGNAS Chunk_header_t {
   public:
    // a copy of source field info, only use its meta info. do NOT use it
    // directly.
    Field *m_source_fld{nullptr};

    // data type in mysql.
    enum_field_types m_type{MYSQL_TYPE_NULL};

    // original pack length
    size_t m_pack_length{0};

    // normalized pack length.
    size_t m_normalized_pack_length{0};

    // can be null or not.
    bool m_nullable{false};

    // null bitmap of all data in this.
    std::unique_ptr<ShannonBase::bit_array_t> m_null_mask{nullptr};

    // validity bitmap of all data in this.
    std::unique_ptr<ShannonBase::bit_array_t> m_del_mask{nullptr};

    // the snapshot meta unit pointer, which contains all trx info.
    std::unique_ptr<ShannonBase::ReadView::Snapshot_meta_unit> m_smu;

    // the min trx id and max trxid of this chunk.
    Transaction::ID m_trx_min;
    Transaction::ID m_trx_max;

    // statistics data of this chunk.
    std::atomic<double> m_max{SHANNON_MIN_DOUBLE};
    std::atomic<double> m_min{SHANNON_MAX_DOUBLE};
    std::atomic<double> m_median{0};
    std::atomic<double> m_middle{0};
    std::atomic<double> m_avg{0};
    std::atomic<double> m_sum{0};

    // physical row count. If you want to get logical rows, you should consider
    // MVCC to decide that whether this phyical row is visiable or not to this
    // transaction.
    std::atomic<row_id_t> m_prows{0};

    // the length of key.
    size_t m_key_len{0};

    // the last GC timestamp
    std::chrono::time_point<std::chrono::steady_clock> m_last_gc_tm;

    // which cu it belongs to.
    Cu *m_owner;
  };

  Chunk(Cu *owner, const Field *field);
  Chunk(Cu *owner, const Field *field, std::string &keyname);
  virtual ~Chunk();

  Chunk(Chunk &&) = delete;
  Chunk &operator=(Chunk &&) = delete;

  inline Chunk_header *header() {
    std::shared_lock<std::shared_mutex> lock(m_header_mutex);
    return m_header.get();
  }

  inline uchar *base() const {
    std::lock_guard<std::mutex> lock(m_chunk_memory_mutex);
    return m_chunk_memory->get();
  }
  inline uchar *end() const {
    std::lock_guard<std::mutex> lock(m_chunk_memory_mutex);
    return m_chunk_memory->get() + static_cast<ptrdiff_t>(m_capacity);
  }

  // the data write to where in this chunk.
  inline uchar *where() { return m_data.load(std::memory_order_acquire); }

  // returns the address of rowid.
  inline uchar *tell(row_id_t rowid) {
    std::lock_guard<std::mutex> lock(m_chunk_memory_mutex);
    if (rowid > SHANNON_ROWS_IN_CHUNK) return end();
    return m_chunk_memory->get() + static_cast<ptrdiff_t>(rowid * m_header->m_normalized_pack_length);
  }

  /** start to read the data from the last pos to data.
   * [in] data, where the data read to.
   * [in] len, read data size.
   * return the read start point.
   */
  uchar *read(const Rapid_load_context *context, uchar *data, size_t len);

  /**start to write the data to chunk.
   * [in] data, the data to write in.
   * [in] rowid, the pos to write.
   * [in] len, the data len.
   * return the address where the data write to.*/
  uchar *write(const Rapid_load_context *context, row_id_t rowid, uchar *data, size_t len);

  /**start to delete the data to chunk. just mark it down.
   * [in] rowid, where the data to update.
   * [in] new_dat, the data value to update.
   * [in] len, the data len.
   * return the address where the data update start from.*/
  uchar *update(const Rapid_load_context *context, row_id_t rowid, uchar *new_data, size_t len);

  /**start to delete the data to chunk. just mark it down.
   * [in] rowid, where the data to update.
   * [in] new_dat, the data value to update.
   * [in] len, the data len.
   * return the address where the data update start from.*/
  uchar *update_from_log(const Rapid_load_context *context, row_id_t rowid, uchar *new_data, size_t len);

  // delete the data by rowid
  uchar *remove(const Rapid_load_context *context, row_id_t rowid);

  // free all the data and reset the meta info.
  void truncate();

  // to do purge unused undo buffer.
  int purge();

  // to do garbage collection.
  int GC();

  // gets null bit flag.
  int is_null(const Rapid_scan_context *context, row_id_t pos);

  // gets the delete flag.
  int is_deleted(const Rapid_scan_context *context, row_id_t pos);

  // get the normalized pack length
  inline size_t normalized_pack_length() { return m_header->m_normalized_pack_length; }

  // get the real pack legnth, m_source_fld->pack_length
  inline size_t pack_length() { return m_header->m_source_fld->pack_length(); }

  // get the field length
  inline size_t field_length() { return m_header->m_source_fld->data_length(); }

  // get the field length bytes
  inline size_t field_length_bytes() { return m_header->m_source_fld->get_length_bytes(); }

  inline uchar *seek(row_id_t rowid) {
    std::lock_guard<std::mutex> lock(m_chunk_memory_mutex);
    if (rowid > m_capacity)
      return end();
    else
      return m_chunk_memory->get() + static_cast<ptrdiff_t>(rowid * m_header->m_normalized_pack_length);
  }

  // gets the current pos in row_id_t format.
  inline row_id_t current_pos() {
    assert(m_chunk_memory->get() < m_end.load(std::memory_order_acquire));
    return (m_data.load(std::memory_order_acquire) - m_chunk_memory->get()) / m_header->m_normalized_pack_length;
  }

  // gets row count.
  inline row_id_t rows(Rapid_load_context *context);

  inline std::string &foot_print() { return m_chunk_footprint; }

 private:
  class ChunkMemoryManager {
   public:
    explicit ChunkMemoryManager(size_t size);
    ~ChunkMemoryManager();

    ChunkMemoryManager(const ChunkMemoryManager &) = delete;
    ChunkMemoryManager &operator=(const ChunkMemoryManager &) = delete;

    ChunkMemoryManager(ChunkMemoryManager &&other) noexcept : m_base_addr(other.m_base_addr), m_size(other.m_size) {
      other.m_base_addr = nullptr;
      other.m_size = 0;
    }

    ChunkMemoryManager &operator=(ChunkMemoryManager &&other) noexcept;

    inline uchar *get() const noexcept { return m_base_addr; }
    inline size_t size() const noexcept { return m_size; }
    inline bool valid() const noexcept { return m_base_addr != nullptr; }

   private:
    uchar *m_base_addr{nullptr};
    size_t m_size{0};
  };

 private:
  // the key string of this chunk.
  std::string m_chunk_key, m_chunk_footprint;

  std::shared_mutex m_header_mutex;
  std::unique_ptr<Chunk_header> m_header{nullptr};

  /** the base pointer of chunk, and the current pos of data. whether data
   * should be in order or not */
  mutable std::mutex m_chunk_memory_mutex;
  std::unique_ptr<ChunkMemoryManager> m_chunk_memory{nullptr};

  // current pointer, where the data write to. use for writting.
  std::atomic<uchar *> m_data{nullptr};

  // current pointer, where the data read from. use for reading.
  std::atomic<uchar *> m_rdata{nullptr};

  // end pointer of this chunk.
  std::atomic<uchar *> m_end{nullptr};

  size_t m_capacity{0};

  // the checksum, use base64 or crc32, etc.
  uint64 m_check_sum{0};

  // maigic num of chunk.
  const char *m_magic = "SHANNON_CHUNK";

  void init_chunk_key(const Field *field, const std::string *custom_key = nullptr);

  bool validate_initialization() const;

  bool ensure_null_mask_allocated();

  bool ensure_del_mask_allocated();

  void init_header(Cu *owner, const Field *field);

  void init_body(const Field *field);

  void reset_meta_info();

  // void update_meta_info(OPER_TYPE type, const Field *fld);

  // to update the meta info of this chunk, val is input param.
  void update_meta_info(const Rapid_load_context *context, OPER_TYPE type, uchar *data, uchar *old);

  // check the data type is leagal or not.
  void check_data_type(size_t type_size);

  // build up an old version.
  inline int build_version(row_id_t rowid, Transaction::ID trxid, const uchar *data, size_t len, OPER_TYPE oper);
};

}  // namespace Imcs
}  // namespace ShannonBase
#endif  //__SHANNONBASE_CHUNK_H__