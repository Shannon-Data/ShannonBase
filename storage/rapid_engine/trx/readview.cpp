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
   Now that, we use innodb trx as rapid's. But, in future, we will impl
   our own trx implementation, because we use innodb trx id in rapid
   for our visibility check. MVCC algorithm is refed the paper titled:
   <Fast Serializable Multi-Version Concurrency Control for Main-Memory Database Systems>
   by Thomas Neumann, Tobias Mühlbauer and Alfons Kemper.
*/
#include "storage/rapid_engine/trx/readview.h"
#include "include/my_inttypes.h"

#include "storage/innobase/include/dict0mem.h"
#include "storage/innobase/include/read0types.h"

#include "storage/rapid_engine/imcs/chunk.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/SIMD.h"

namespace ShannonBase {
namespace ReadView {

smu_item_t::smu_item_t(size_t size) {
  sz = size;
  data = nullptr;

  if (size != UNIV_SQL_NULL) {
    data.reset(new uchar[size]);
  }
  tm_stamp = tm_committed = std::chrono::high_resolution_clock::now();
}

uchar *smu_item_vec_t::reconstruct_data(Rapid_load_context *context, uchar *in_place, size_t &in_place_len,
                                        uint8 &status) {
  std::lock_guard<std::mutex> lock(vec_mutex);
  auto ret = in_place;

  for (auto &it : items) {  // find the last visible item for this trx id, if found reconstruct it.
    if (!context->m_trx->changes_visible(it.trxid, context->m_table_name.c_str()) &&
        it.tm_committed != ShannonBase::SHANNON_MAX_STMP) {  // if can not be seen and not rollback. means it still in
      auto oper_type = it.oper_type;                         // transaction processing then rebuild the old version.
      switch (oper_type) {
        case OPER_TYPE::OPER_INSERT: {
          ut_a(it.sz == in_place_len);
          ret = in_place;
          in_place_len = it.sz;
          status |= static_cast<uint8>(RECONSTRUCTED_STATUS::STAT_DELETED);
        } break;
        case OPER_TYPE::OPER_DELETE: {
          ut_a(it.sz == in_place_len || it.sz == UNIV_SQL_NULL);
          if (it.data.get() == nullptr && it.sz == UNIV_SQL_NULL) {
            ret = nullptr;
            in_place_len = UNIV_SQL_NULL;
            status |= static_cast<uint8>(RECONSTRUCTED_STATUS::STAT_NULL);
          } else {
            std::memcpy(in_place, it.data.get(), it.sz);
            in_place_len = it.sz;
            ret = in_place;
            status = static_cast<uint8>(RECONSTRUCTED_STATUS::STAT_NORMAL);
          }
        } break;
        case OPER_TYPE::OPER_UPDATE: {
          ut_a(it.sz == in_place_len || it.sz == UNIV_SQL_NULL);
          if (it.data.get() == nullptr && it.sz == UNIV_SQL_NULL) {
            in_place_len = UNIV_SQL_NULL;
            ret = nullptr;
            status = static_cast<uint8>(RECONSTRUCTED_STATUS::STAT_NULL);
          } else {
            std::memcpy(in_place, it.data.get(), it.sz);
            in_place_len = it.sz;
            ret = in_place;
            status = static_cast<uint8>(RECONSTRUCTED_STATUS::STAT_NORMAL);
          }
        } break;
        default:
          break;  // unkonwn oper type.
      }
    } else if (context->m_trx->changes_visible(it.trxid, context->m_table_name.c_str()) &&
               it.tm_committed == ShannonBase::SHANNON_MAX_STMP) {  // means it has been rollback.
      status = static_cast<uint8>(RECONSTRUCTED_STATUS::STAT_ROLLBACKED);
      return nullptr;
    }
  }

  return ret;
}

void Snapshot_meta_unit::set_owner(ShannonBase::Imcs::Chunk *owner) { m_owner = owner; }

uchar *Snapshot_meta_unit::build_prev_vers(Rapid_load_context *context, ShannonBase::row_id_t rowid, uchar *in_place,
                                           size_t &in_place_len, uint8 &status) {
  if (m_owner->is_deleted(context, rowid)) status |= static_cast<uint8>(RECONSTRUCTED_STATUS::STAT_DELETED);
  if (m_owner->is_null(context, rowid)) {
    status |= static_cast<uint8>(RECONSTRUCTED_STATUS::STAT_NULL);
    in_place_len = UNIV_SQL_NULL;
  }
  ////if it has not version data, just return itself.
  return (m_version_info.find(rowid) == m_version_info.end())
             ? in_place
             : m_version_info[rowid].reconstruct_data(context, in_place, in_place_len, status);
}

BitmapResult Snapshot_meta_unit::build_prev_vers_batch(Rapid_load_context *context, ShannonBase::row_id_t row_start,
                                                       size_t row_count, const uchar *chunk_base_ptr,
                                                       size_t normalized_len, uchar *reconstruct_buf) {
  BitmapResult result;
  if (row_count == 0) {
    result.visible_count = 0;
    return result;
  }
  size_t bytes = (row_count + 7) / 8;
  result.bitmask.assign(bytes, 0);

  // 1) collect pointers to SMU_items if exists, under shared lock to m_version_info
  std::vector<ReadView::SMU_items *> smu_ptrs;
  smu_ptrs.resize(row_count, nullptr);

  {
    std::shared_lock lk(m_version_mutex);
    for (size_t i = 0; i < row_count; ++i) {
      ShannonBase::row_id_t rid = row_start + static_cast<ShannonBase::row_id_t>(i);
      auto it = m_version_info.find(rid);
      if (it != m_version_info.end()) {
        smu_ptrs[i] = &it->second;
      }
    }
  }

  // 2) iterate rows in this range, compute status and fill bit
  // reconstruct_buf must be at least row_count * normalized_len bytes
  for (size_t i = 0; i < row_count; ++i) {
    ShannonBase::row_id_t rid = row_start + static_cast<ShannonBase::row_id_t>(i);
    uint8 status = 0;
    size_t in_place_len = normalized_len;
    uchar *in_place = reconstruct_buf ? (reconstruct_buf + i * normalized_len)
                                      : const_cast<uchar *>(chunk_base_ptr + i * normalized_len);

    // quick check: owner-level flags
    if (m_owner->is_deleted(context, rid)) status |= static_cast<uint8>(ReadView::RECONSTRUCTED_STATUS::STAT_DELETED);
    if (m_owner->is_null(context, rid)) {
      status |= static_cast<uint8>(ReadView::RECONSTRUCTED_STATUS::STAT_NULL);
      in_place_len = UNIV_SQL_NULL;
    }

    // if there's SMU versions, reconstruct into in_place
    if (smu_ptrs[i] != nullptr) {
      // reconstruct_data will lock SMU_items::vec_mutex internally
      uchar *res_ptr = smu_ptrs[i]->reconstruct_data(context, in_place, in_place_len, status);
      // reconstruct_data may set STAT_ROLLBACKED and return nullptr
      (void)res_ptr;  // we don't need pointer content here; just status
    }

    // determine visibility: visible if not deleted and not rollbacked
    bool visible = !(status & static_cast<uint8>(ReadView::RECONSTRUCTED_STATUS::STAT_DELETED)) &&
                   !(status & static_cast<uint8>(ReadView::RECONSTRUCTED_STATUS::STAT_ROLLBACKED));

    if (visible) {
      size_t byte_idx = i >> 3;
      uint8_t bit_mask = static_cast<uint8_t>(1u << (i & 7));
      result.bitmask[byte_idx] |= bit_mask;
    }
  }

  // 3) compute popcount (visible_count)
  result.visible_count = ShannonBase::Utils::SIMD::popcount_bitmap(result.bitmask);
  return result;
}

int Snapshot_meta_unit::purge(const char *tname, ::ReadView *rv) {
  if (m_version_info.empty()) return SHANNON_SUCCESS;

  int ret{SHANNON_SUCCESS};
  table_name_t name{const_cast<char *>(tname)};
  std::unique_lock lk(m_version_mutex);
  for (auto it = m_version_info.begin(); it != m_version_info.end(); /* no ++ here */) {
    auto &smu_items = it->second;
    {
      auto &items = smu_items.items;

      if (items.empty()) {
        it = m_version_info.erase(it);
        continue;
      }

      // check every smu_items condition.
      items.erase(
          std::remove_if(items.begin(), items.end(),
                         [&](ShannonBase::ReadView::SMU_item &smu_it) {
                           // Only satisfied when:
                           // 1. Committed (tm_committed != SHANNON_MAX_STMP)
                           // 2. Invisible to the current read view (i.e., no active transaction is accessing)
                           return (rv->changes_visible(smu_it.trxid, name) && smu_it.tm_committed == SHANNON_MAX_STMP);
                         }),
          items.end());

      // after that.
      if (items.empty()) {
        it = m_version_info.erase(it);
        continue;
      }
    }
    ++it;
  }

  return ret;
}

}  // namespace ReadView
}  // namespace ShannonBase