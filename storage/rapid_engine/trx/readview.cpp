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
#include "storage/rapid_engine/imcs/chunk.h"
#include "storage/rapid_engine/include/rapid_context.h"

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
          ret = nullptr;
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
      return nullptr;
    }
  }

  return ret;
}

void Snapshot_meta_unit::set_owner(ShannonBase::Imcs::Chunk *owner) { m_owner = owner; }

uchar *Snapshot_meta_unit::build_prev_vers(Rapid_load_context *context, ShannonBase::row_id_t rowid, uchar *in_place,
                                           size_t &in_plance_len, uint8 &status) {
  if (m_owner->is_deleted(context, rowid)) status |= static_cast<uint8>(RECONSTRUCTED_STATUS::STAT_DELETED);
  if (m_owner->is_null(context, rowid)) status |= static_cast<uint8>(RECONSTRUCTED_STATUS::STAT_NULL);

  ////if it has not version data, just return itself.
  return (m_version_info.find(rowid) == m_version_info.end())
             ? in_place
             : m_version_info[rowid].reconstruct_data(context, in_place, in_plance_len, status);
}

}  // namespace ReadView
}  // namespace ShannonBase