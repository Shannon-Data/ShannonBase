/*****************************************************************************

Copyright (c) 2014, 2024, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*****************************************************************************/

/** @file ha_shannon_rapidpart.cc
Code for native partitioning in rapid.

Created jun 6, 2025 */

#include "ha_shannon_rapidpart.h"
#include "include/mysqld_error.h"
#include "my_dbug.h"
#include "storage/innobase/include/dict0dd.h"  //dd_is_partitioned

#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_status.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
extern int rpd_async_column_threshold;
ha_rapidpart::ha_rapidpart(handlerton *hton, TABLE_SHARE *table)
    : ha_rapid(hton, table), Partition_helper(this), m_thd(ha_thd()), m_share(nullptr) {}

int ha_rapidpart::rnd_pos(uchar *record, uchar *pos) { return ShannonBase::SHANNON_SUCCESS; }

int ha_rapidpart::rnd_init(bool scan) {
  m_current_part_empty = false;

  if (m_rpd_table_viewer->init()) {
    m_start_of_scan = false;
    return HA_ERR_GENERIC;
  }

  inited = handler::RND;
  m_start_of_scan = true;
  return (Partition_helper::ph_rnd_init(scan));
}

int ha_rapidpart::rnd_init_in_part(uint part_id, bool scan) {
  // int err = change_active_index(part_id, table_share->primary_key);
  /* Don't use semi-consistent read in random row reads (by position).
  This means we must disable semi_consistent_read if scan is false. */
  std::string part_key;
  auto part_name = m_rpd_table_viewer->source()->part_info->partitions[part_id]->partition_name;
  part_key.append(part_name).append("#").append(std::to_string(part_id));

  const auto &rpd_table = m_rpd_table_viewer->table_source();
  auto partition_ptr = down_cast<ShannonBase::Imcs::PartTable *>(rpd_table)->get_partition(part_key);
  auto n_rows = partition_ptr->meta().total_rows.load(std::memory_order_relaxed);
  m_current_part_empty = (n_rows) ? false : true;

  if (!m_current_part_empty) m_rpd_table_viewer->active_table(partition_ptr);

  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::rnd_next_in_part(uint part_id, uchar *buf) {
  int error{HA_ERR_END_OF_FILE};
  if (m_current_part_empty) return error;

  if (inited == handler::RND && m_start_of_scan) {
    if (table_share->fields <= static_cast<uint>(ShannonBase::rpd_async_column_threshold)) {
      error = m_rpd_table_viewer->next(buf);
    } else {
      auto reader_pool = ShannonBase::Imcs::Imcs::pool();
      std::future<int> fut =
          boost::asio::co_spawn(*reader_pool, m_rpd_table_viewer->next_async(buf), boost::asio::use_future);
      error = fut.get();  // co_await m_data_table->next_async(buf);  // index_first(buf);
      if (error == HA_ERR_KEY_NOT_FOUND) {
        error = HA_ERR_END_OF_FILE;
      }
    }
  }

  // increase the row count.
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return error;
}

int ha_rapidpart::rnd_end_in_part(uint, bool) { return ShannonBase::SHANNON_SUCCESS; }

int ha_rapidpart::rnd_end() {
  if (m_rpd_table_viewer->end()) return HA_ERR_GENERIC;

  m_start_of_scan = false;
  inited = handler::NONE;
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::index_first_in_part(uint, uchar *) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::index_last_in_part(uint, uchar *) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::index_prev_in_part(uint, uchar *) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::index_next_in_part(uint, uchar *) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::index_next_same_in_part(uint, uchar *, const uchar *, uint) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::index_read_map_in_part(uint, uchar *, const uchar *, key_part_map, ha_rkey_function) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::index_read_last_map_in_part(uint, uchar *, const uchar *, key_part_map) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::read_range_first_in_part(uint, uchar *, const key_range *, const key_range *, bool) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::read_range_next_in_part(uint, uchar *) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::index_read_idx_map_in_part(uint, uchar *, uint, const uchar *, key_part_map, ha_rkey_function) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::write_row_in_new_part(uint) {
  assert(false);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::load_table(const TABLE &table, bool *skip_metadata_update) {
  ut_a(table.file != nullptr);
  ut_ad(table.s != nullptr);

  if (shannon_loaded_tables->get(table.s->db.str, table.s->table_name.str) != nullptr) {
    std::string err;
    err.append(table.s->db.str).append(".").append(table.s->table_name.str).append(" already loaded");
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.c_str());
    return HA_ERR_GENERIC;
  }

  for (auto idx = 0u; idx < table.s->fields; idx++) {
    auto fld = *(table.field + idx);
    if (!bitmap_is_set(table.read_set, idx) || fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    if (!ShannonBase::Utils::Util::is_support_type(fld->type())) {
      std::string err;
      err.append(table.s->table_name.str).append(fld->field_name).append(" type not allowed");
      my_error(ER_SECONDARY_ENGINE, MYF(0), err.c_str());
      return HA_ERR_GENERIC;
    }
  }

  m_thd->set_sent_row_count(0);
  // start to read data from innodb and load to rapid.
  ShannonBase::Rapid_load_context context;
  context.m_table = const_cast<TABLE *>(&table);
  context.m_thd = m_thd;
  context.m_extra_info.m_keynr = active_index;
  context.m_schema_name = table.s->db.str;
  context.m_table_name = table.s->table_name.str;

  context.m_trx = Transaction::get_or_create_trx(m_thd);
  context.m_trx->begin_stmt();
  context.m_extra_info.m_trxid = context.m_trx->get_id();
  context.m_extra_info.m_scn = context.m_extra_info.m_trxid;

  // use specific partion. such as partition(p1, p2, p10, ..., pn).
  Table_ref *table_list = m_thd->lex->query_block->get_table_list();
  if (table_list->partition_names && table.file->get_partition_handler()) {
    partition_info *part_info = table_list->table->part_info;
    List_iterator_fast<String> it(*table_list->partition_names);
    String *str{nullptr};
    while ((str = it++)) {
      uint part_id;
      if (part_info->get_part_elem(str->c_ptr(), &part_id) && part_id != NOT_A_PARTITION_ID) {
        context.m_extra_info.m_partition_infos.emplace(std::make_pair(str->c_ptr(), part_id));
      }
    }
  } else {  // using all part.
    for (auto index = 0u; index < table.part_info->get_tot_partitions(); index++) {
      auto part_name = table.part_info->partitions[index]->partition_name;
      context.m_extra_info.m_partition_infos.emplace(std::make_pair(part_name, index));
    }
  }

  if (Imcs::Imcs::instance()->load_parttable(&context, const_cast<TABLE *>(&table))) {
    my_error(ER_SECONDARY_ENGINE, MYF(0), table.s->db.str, table.s->table_name.str);
    context.m_trx->rollback_stmt();
    return HA_ERR_GENERIC;
  }
  context.m_trx->commit();

  m_share = new RapidPartShare(table);
  m_share->file = this;
  m_share->m_tableid = table.s->table_map_id.id();
  shannon_loaded_tables->add(table.s->db.str, table.s->table_name.str, m_share);
  if (shannon_loaded_tables->get(table.s->db.str, table.s->table_name.str) == nullptr) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), table.s->db.str, table.s->table_name.str);
    return HA_ERR_KEY_NOT_FOUND;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::unload_table(const char *db_name, const char *table_name, bool error_if_not_loaded) {
  RapidShare *share = shannon_loaded_tables->get(db_name, table_name);
  if (error_if_not_loaded && !share) {
    std::string err(db_name);
    err.append(".").append(table_name).append(" table is not loaded into rapid yet");
    my_error(ER_SECONDARY_ENGINE, MYF(0), err.c_str());
    return HA_ERR_GENERIC;
  }

  ShannonBase::Rapid_load_context context;
  Table_ref *table_list = m_thd->lex->query_block->get_table_list();
  context.m_table = table_list ? table_list->table : nullptr;
  context.m_thd = m_thd;
  context.m_extra_info.m_keynr = active_index;
  context.m_schema_name = db_name;
  context.m_table_name = table_name;

  auto part_handler = context.m_table ? context.m_table->file->get_partition_handler() : nullptr;
  auto partition_names = table_list ? table_list->partition_names : nullptr;
  if (partition_names && part_handler) {
    partition_info *part_info = table_list->table->part_info;
    List_iterator_fast<String> it(*table_list->partition_names);
    String *str{nullptr};
    while ((str = it++)) {
      uint part_id;
      if (part_info->get_part_elem(str->c_ptr(), &part_id) && part_id != NOT_A_PARTITION_ID) {
        context.m_extra_info.m_partition_infos.emplace(std::make_pair(str->c_ptr(), part_id));
      }
    }
  }

  Imcs::Imcs::instance()->unload_table(&context, db_name, table_name, false);

  // if all cus has been unloaded, then we can remove the meta info. Considering the following
  // scenario: alter table xxx secondary_load partion(p0, p1, xxx, pN), then unload a part of
  // partitions, not all alter table xxx secondary_unload partition(p0, p10). Under this stage,
  // we think that the table is still in loading status.

  shannon_loaded_tables->erase(db_name, table_name);

  return ShannonBase::SHANNON_SUCCESS;
}

}  // namespace ShannonBase
