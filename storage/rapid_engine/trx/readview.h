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

   The fundmental code for imcs. for readview.
*/
#ifndef __SHANNONBASE_READVIEW_H__
#define __SHANNONBASE_READVIEW_H__

#include <deque>
#include <mutex>
#include <tuple>
#include <unordered_map>

#include "storage/rapid_engine/trx/transaction.h"

namespace ShannonBase {
class Rapid_load_context;
namespace Imcs {
class Chunk;
}

namespace ReadView {
// a bit to describe one status.
enum class RECONSTRUCTED_STATUS : uint8 { STAT_NORMAL = 0, STAT_NULL = 1, STAT_DELETED = 2, STAT_ROLLBACKED = 4 };

// in chunk, the latest veresion data always is in. the old version of data moves to
// SMU. So if a trx can see the latest version data, it should travers the version
// link to check whether there's some visible data or not. if yes, return the old ver
// data or otherwise, go to check the next item.
using SMU_item = struct SHANNON_ALIGNAS smu_item_t {
  OPER_TYPE oper_type;

  // trxid of old version value.
  Transaction::ID trxid{ShannonBase::SHANNON_MAX_TRX_ID};

  // timestamp of the modification.
  std::chrono::time_point<std::chrono::high_resolution_clock> tm_stamp{ShannonBase::SHANNON_MAX_STMP};

  // commit timestamp, initail value is invalid tm, update to valid tm, when it be committed.
  std::chrono::time_point<std::chrono::high_resolution_clock> tm_committed{ShannonBase::SHANNON_MAX_STMP};

  // the old version of data. all var data types were encoded.
  size_t sz{0};
  std::unique_ptr<uchar[]> data{nullptr};

  smu_item_t(size_t size);
  smu_item_t() = delete;
  // Disable copying
  smu_item_t(const smu_item_t &) = delete;
  smu_item_t &operator=(const smu_item_t &) = delete;

  // Define a move constructor
  smu_item_t(smu_item_t &&other) noexcept {
    oper_type = other.oper_type;
    trxid = other.trxid;

    tm_stamp = other.tm_stamp;
    tm_committed = other.tm_committed;

    if (other.data)
      data = std::move(other.data);
    else
      data = nullptr;

    sz = other.sz;

    other.sz = 0;
    other.data = nullptr;
  }

  // Define a move assignment operator
  smu_item_t &operator=(smu_item_t &&other) noexcept {
    if (this != &other) {
      oper_type = other.oper_type;
      trxid = other.trxid;

      tm_stamp = other.tm_stamp;
      tm_committed = other.tm_committed;

      if (other.data)
        data = std::move(other.data);
      else
        data = nullptr;

      sz = other.sz;

      other.sz = 0;
      other.data = nullptr;
    }
    return *this;
  }
};

using SMU_items = struct smu_item_vec_t {
  smu_item_vec_t() = default;
  smu_item_vec_t(const smu_item_vec_t &) = delete;
  smu_item_vec_t &operator=(const smu_item_vec_t &) = delete;

  smu_item_vec_t(smu_item_vec_t &&other) noexcept {
    std::lock_guard<std::mutex> lock(other.vec_mutex);
    items = std::move(other.items);
  }

  smu_item_vec_t &operator=(smu_item_vec_t &&other) noexcept {
    if (this != &other) {
      std::lock_guard<std::mutex> lock(other.vec_mutex);
      items = std::move(other.items);
    }
    return *this;
  }

  inline void add(SMU_item &item) {
    std::lock_guard<std::mutex> lock(vec_mutex);
    items.push_front(std::move(item));
  }

  /**  re-construts the first met visibility data in this version link.
   * if in_place_len == UNIV_SQL_NULL, means in_place value is null. flag is to indicate flag bit
   * of the data, such as deleted, null, etc.
   **/
  uchar *reconstruct_data(Rapid_load_context *context, uchar *in_place, size_t &in_place_len, uint8 &status);

  std::mutex vec_mutex;
  std::deque<SMU_item> items;
};

class Snapshot_meta_unit {
 public:
  Snapshot_meta_unit() = default;
  virtual ~Snapshot_meta_unit() = default;

  void set_owner(ShannonBase::Imcs::Chunk *owner);
  /** an item of SMU. consist of <trxid, new_data>. pair of row_id_t and sum_item indicates
   * that each row data has a version link. If this row data not been modified, it does not
   * have any old version. [newest<---->oldest]
   *   |__|
   *   |__|<----->rowidN: {[{trxid:value1} | {trxid:value2} | {trxid:value3} | ...| {trxid:valueN}]}
   *   |__|       rowidM: {[{trxid:value1} | {trxid:value2} | {trxid:value3} | ...| {trxid:valueN}]}
   *   |__|<-----/|\
   *   |__|
   */
  // in_place, means the current version. flag indicates the flag of reconstructed data.
  // such as is null or not, is deleted marked or not.
  uchar *build_prev_vers(Rapid_load_context *context, ShannonBase::row_id_t rowid, uchar *in_place,
                         size_t &in_place_len, uint8 &status);

  // gets the rowid's versions.
  inline SMU_items &versions(ShannonBase::row_id_t rowid) {
    std::scoped_lock lk(m_version_mutex);
    if (m_version_info.find(rowid) != m_version_info.end())
      return m_version_info[rowid];
    else  // not found, insert a empty vect in.
      return m_version_info[rowid];
  }

  inline void add_version(ShannonBase::row_id_t rowid, SMU_items &siv) {
    std::scoped_lock lk(m_version_mutex);
    m_version_info.emplace(rowid, std::move(siv));
  }

  inline std::unordered_map<row_id_t, ReadView::SMU_items> &version_info() {
    std::scoped_lock lk(m_version_mutex);
    return m_version_info;
  }

  // purge the unused items.
  int purge(const char *tname, ::ReadView *rv);

 private:
  std::mutex m_version_mutex;
  std::unordered_map<row_id_t, ReadView::SMU_items> m_version_info;
  ShannonBase::Imcs::Chunk *m_owner;
};

}  // namespace ReadView
}  // namespace ShannonBase
#endif  //__SHANNONBASE_READVIEW_H__