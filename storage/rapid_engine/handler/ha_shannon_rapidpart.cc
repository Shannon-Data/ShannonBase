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
#include <sstream>
#include "include/mysqld_error.h"
#include "my_dbug.h"
#include "sql/key.h"  //key_copy
#include "storage/innobase/handler/ha_innodb.h"
#include "storage/innobase/include/dict0dd.h"  //dd_is_partitioned

#include "storage/rapid_engine/autopilot/loader.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/imcs/table0view.h"
#include "storage/rapid_engine/include/rapid_column_info.h"
#include "storage/rapid_engine/include/rapid_config.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
extern int shannon_rpd_async_column_threshold;
ha_rapidpart::ha_rapidpart(handlerton *hton, TABLE_SHARE *table)
    : ha_rapid(hton, table), Partition_helper(this), m_thd(ha_thd()), m_share(nullptr) {}

int ha_rapidpart::open(const char *name, int mode, unsigned int test_if_locked, const dd::Table *table_def) {
  int error = ha_rapid::open(name, mode, test_if_locked, table_def);
  if (error) return error;

  if (open_partitioning(nullptr)) {
    ha_rapid::close();
    return HA_ERR_INITIALIZATION;
  }
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::close() {
  close_partitioning();
  return ha_rapid::close();
}

int ha_rapidpart::rnd_pos(uchar *, uchar *) {
  // A partition-local Rapid rowid is not globally unique and the current ref
  // format does not encode part_id. Delegating to the base cursor could read a
  // row from the wrong partition. Fail until a {part_id,rowid} ref is defined.
  return HA_ERR_WRONG_COMMAND;
}

int ha_rapidpart::rnd_init(bool scan) {
  m_current_part_empty = false;

  if (m_cursor->init()) {
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
  auto part_name = m_cursor->source()->part_info->partitions[part_id]->partition_name;
  part_key.append(part_name).append("#").append(std::to_string(part_id));

  const auto &rpd_table = m_cursor->table_source();
  auto partition_ptr = down_cast<ShannonBase::Imcs::PartTable *>(rpd_table)->get_partition(part_key);
  if (partition_ptr == nullptr) {
    m_current_part_empty = true;
    return ShannonBase::SHANNON_SUCCESS;
  }
  auto n_rows = partition_ptr->meta().active_rows();
  m_current_part_empty = (n_rows) ? false : true;

  if (!m_current_part_empty) m_cursor->active_table(partition_ptr);

  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::rnd_next_in_part(uint part_id, uchar *buf) {
  int error{HA_ERR_END_OF_FILE};
  if (m_current_part_empty) return error;

  if (inited == handler::RND) {
    auto reader_pool = ShannonBase::Imcs::Imcs::pool();
    if (table_share->fields <= static_cast<uint>(ShannonBase::shannon_rpd_engine_cfg.async_column_threshold) ||
        reader_pool == nullptr) {
      error = m_cursor->next(buf);
    } else {
      std::future<int> fut = boost::asio::co_spawn(*reader_pool, m_cursor->next_async(buf), boost::asio::use_future);
      error = fut.get();
    }
    // Normalise HA_ERR_KEY_NOT_FOUND → HA_ERR_END_OF_FILE for both paths.
    if (error == HA_ERR_KEY_NOT_FOUND) {
      error = HA_ERR_END_OF_FILE;
    }
  }

  // increase the row count.
  if (error == ShannonBase::SHANNON_SUCCESS) ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return error;
}

int ha_rapidpart::rnd_end_in_part(uint, bool) { return ShannonBase::SHANNON_SUCCESS; }

int ha_rapidpart::rnd_end() {
  if (m_cursor->end()) return HA_ERR_GENERIC;

  m_start_of_scan = false;
  inited = handler::NONE;
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::index_init(uint keynr, bool sorted) {
  m_part_scan_state.assign(m_tot_parts, PartIndexScanState{});
  m_cursor_part_id = NO_CURRENT_PART_ID;

  if (int error = ph_index_init_setup(keynr, sorted); error) return error;

  if (sorted) {
    // Needed for ordered cross-partition merges (handle_ordered_index_scan()):
    // several partitions' rows are primed and compared concurrently.
    if (int error = init_record_priority_queue(); error) {
      destroy_record_priority_queue();
      return error;
    }
  }

  if (m_cursor->init()) {
    if (sorted) destroy_record_priority_queue();
    return HA_ERR_GENERIC;
  }

  m_cursor->set_active_index(static_cast<int8_t>(keynr));
  active_index = keynr;
  inited = handler::INDEX;
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::index_end() {
  m_part_scan_state.clear();
  m_cursor_part_id = NO_CURRENT_PART_ID;
  if (m_ordered) destroy_record_priority_queue();
  return ha_rapid::index_end();
}

int ha_rapidpart::switch_to_partition(uint part_id) {
  if (part_id == m_cursor_part_id) return ShannonBase::SHANNON_SUCCESS;

  std::string part_key;
  auto part_name = m_cursor->source()->part_info->partitions[part_id]->partition_name;
  part_key.append(part_name).append("#").append(std::to_string(part_id));

  const auto &rpd_table = m_cursor->table_source();
  auto partition_ptr = down_cast<ShannonBase::Imcs::PartTable *>(rpd_table)->get_partition(part_key);
  if (partition_ptr == nullptr) return HA_ERR_END_OF_FILE;  // nothing ever loaded for this partition.

  m_cursor->active_table(partition_ptr);
  m_cursor_part_id = part_id;
  return ShannonBase::SHANNON_SUCCESS;
}

bool ha_rapidpart::try_resume_partition(uint part_id, uchar *buf, int *error) {
  auto &state = m_part_scan_state[part_id];
  if (!state.valid) return false;

  *error = m_cursor->index_read(buf, state.key.data(), static_cast<uint>(state.key.size()), state.find_flag);
  if (*error == ShannonBase::SHANNON_SUCCESS)
    save_scan_position(part_id, buf, state.find_flag == HA_READ_BEFORE_KEY);
  else
    state.valid = false;
  return true;
}

void ha_rapidpart::save_scan_position(uint part_id, const uchar *buf, bool reverse) {
  auto &state = m_part_scan_state[part_id];
  const KEY &key_info = table->key_info[active_index];
  state.key.resize(key_info.key_length);
  key_copy(state.key.data(), buf, &key_info, key_info.key_length);
  state.find_flag = reverse ? HA_READ_BEFORE_KEY : HA_READ_AFTER_KEY;
  state.valid = true;
}

void ha_rapidpart::save_miss_position(uint part_id, const uchar *key, uint key_len, bool reverse) {
  auto &state = m_part_scan_state[part_id];
  if (key == nullptr || key_len == 0) {
    state.valid = false;
    return;
  }
  state.key.assign(key, key + key_len);
  state.find_flag = reverse ? HA_READ_BEFORE_KEY : HA_READ_AFTER_KEY;
  state.valid = true;
}

int ha_rapidpart::index_first_in_part(uint part_id, uchar *buf) {
  int error = switch_to_partition(part_id);
  if (error) return error;

  error = m_cursor->index_read(buf, nullptr, 0, HA_READ_KEY_OR_NEXT);
  if (error == ShannonBase::SHANNON_SUCCESS) save_scan_position(part_id, buf, false);
  return error;
}

int ha_rapidpart::index_last_in_part(uint part_id, uchar *buf) {
  int error = switch_to_partition(part_id);
  if (error) return error;

  error = m_cursor->index_read(buf, nullptr, 0, HA_READ_BEFORE_KEY);
  /* MySQL does not seem to allow this to return HA_ERR_KEY_NOT_FOUND (mirrors ha_rapid::index_last). */
  if (error == HA_ERR_KEY_NOT_FOUND) error = HA_ERR_END_OF_FILE;
  if (error == ShannonBase::SHANNON_SUCCESS) save_scan_position(part_id, buf, true);
  return error;
}

int ha_rapidpart::index_prev_in_part(uint part_id, uchar *buf) {
  const bool same_partition = (part_id == m_cursor_part_id);
  int error = switch_to_partition(part_id);
  if (error) return error;

  if (!same_partition && try_resume_partition(part_id, buf, &error)) return error;

  error = m_cursor->index_prev(buf);
  if (error == ShannonBase::SHANNON_SUCCESS) save_scan_position(part_id, buf, true);
  return error;
}

int ha_rapidpart::index_next_in_part(uint part_id, uchar *buf) {
  const bool same_partition = (part_id == m_cursor_part_id);
  int error = switch_to_partition(part_id);
  if (error) return error;

  if (!same_partition && try_resume_partition(part_id, buf, &error)) return error;

  error = m_cursor->index_next(buf);
  if (error == ShannonBase::SHANNON_SUCCESS) save_scan_position(part_id, buf, false);
  return error;
}

int ha_rapidpart::index_next_same_in_part(uint part_id, uchar *buf, const uchar *, uint) {
  return index_next_in_part(part_id, buf);
}

int ha_rapidpart::index_read_map_in_part(uint part_id, uchar *buf, const uchar *key, key_part_map keypart_map,
                                         ha_rkey_function find_flag) {
  int error = switch_to_partition(part_id);
  if (error) return error;

  const uint key_len = calculate_key_len(table, active_index, keypart_map);
  const bool reverse = (find_flag == HA_READ_KEY_OR_PREV || find_flag == HA_READ_BEFORE_KEY ||
                        find_flag == HA_READ_PREFIX_LAST || find_flag == HA_READ_PREFIX_LAST_OR_PREV);

  error = m_cursor->index_read(buf, key, key_len, find_flag);
  if (error == ShannonBase::SHANNON_SUCCESS)
    save_scan_position(part_id, buf, reverse);
  else if (error == HA_ERR_KEY_NOT_FOUND)
    save_miss_position(part_id, key, key_len, reverse);
  return error;
}

int ha_rapidpart::index_read_last_map_in_part(uint part_id, uchar *buf, const uchar *key, key_part_map keypart_map) {
  return index_read_map_in_part(part_id, buf, key, keypart_map, HA_READ_PREFIX_LAST);
}

int ha_rapidpart::read_range_first_in_part(uint part_id, uchar *buf, const key_range *start_key,
                                           const key_range *end_key, bool eq_range_arg) {
  uchar *record = buf ? buf : table->record[0];

  eq_range = eq_range_arg;
  set_end_range(end_key, handler::RANGE_SCAN_ASC);
  range_key_part = table->key_info[active_index].key_part;

  int error = start_key
                  ? index_read_map_in_part(part_id, record, start_key->key, start_key->keypart_map, start_key->flag)
                  : index_first_in_part(part_id, record);
  if (error) return (error == HA_ERR_KEY_NOT_FOUND) ? HA_ERR_END_OF_FILE : error;

  if (compare_key(end_range) > 0) {
    unlock_row();
    error = HA_ERR_END_OF_FILE;
  }
  return error;
}

int ha_rapidpart::read_range_next_in_part(uint part_id, uchar *buf) {
  uchar *record = buf ? buf : table->record[0];
  int error;

  if (eq_range) {
    /* We trust that index_next_same always gives a row in range. */
    error = index_next_same_in_part(part_id, record, end_range->key, end_range->length);
  } else {
    error = index_next_in_part(part_id, record);
    if (error) return error;

    if (compare_key(end_range) > 0) {
      unlock_row();
      error = HA_ERR_END_OF_FILE;
    }
  }
  return error;
}

int ha_rapidpart::index_read_idx_map_in_part(uint part_id, uchar *buf, uint index, const uchar *key,
                                             key_part_map keypart_map, ha_rkey_function find_flag) {
  // index_read_idx targets a specific index without a preceding index_init(),
  // possibly different from the one currently active; bracket it exactly like
  // the default handler::index_read_idx_map does for the non-partitioned case.
  // Goes through our own index_init()/index_end() overrides (not ha_rapid's
  // directly) so per-partition scan state is reset consistently too.
  const bool needs_reinit = (index != active_index);
  if (needs_reinit && index_init(index, false)) return HA_ERR_GENERIC;

  int error = index_read_map_in_part(part_id, buf, key, keypart_map, find_flag);

  if (needs_reinit) {
    int end_error = index_end();
    if (!error) error = end_error;
  }
  return error;
}

int ha_rapidpart::write_row_in_new_part(uint) {
  // Unsupported paths must fail loudly; success would expose an uninitialised row buffer.
  return HA_ERR_WRONG_COMMAND;
}

int ha_rapidpart::load_table(const TABLE &table, bool *skip_metadata_update) {
  ut_a(table.file != nullptr);
  ut_ad(table.s != nullptr);

  // Check if specific partitions are being loaded (e.g. SECONDARY_LOAD PARTITION (p1)).
  Table_ref *table_list = m_thd->lex->query_block->get_table_list();
  bool is_partition_load = (table_list != nullptr && table_list->partition_names != nullptr);

  if (!is_partition_load && shannon_loaded_tables->get(table.s->db.str, table.s->table_name.str) != nullptr) {
    std::ostringstream oss;
    oss << table.s->db.str << "." << table.s->table_name.str << " already loaded";
    my_error(ER_SECONDARY_ENGINE, MYF(0), oss.str().c_str());
    return HA_ERR_GENERIC;
  }

  for (auto idx = 0u; idx < table.s->fields; idx++) {
    auto fld = *(table.field + idx);
    if (!bitmap_is_set(table.read_set, idx) || fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;

    if (!ShannonBase::Utils::Util::is_support_type(fld->type())) {
      std::ostringstream oss;
      oss << table.s->table_name.str << fld->field_name << " type not allowed";
      my_error(ER_SECONDARY_ENGINE, MYF(0), oss.str().c_str());
      return HA_ERR_GENERIC;
    }
  }

  m_thd->set_sent_row_count(0);
  // start to read data from innodb and load to rapid.
  ShannonBase::Rapid_load_context context;
  context.m_table = const_cast<TABLE *>(&table);
  context.m_table_id = table.file->get_table_id();
  context.m_thd = m_thd;
  context.m_extra_info.m_keynr = active_index;
  context.m_schema_name = table.s->db.str;
  context.m_table_name = table.s->table_name.str;
  context.m_sch_tb_name = context.m_schema_name + "." + context.m_table_name;

  context.m_trx = Transaction::get_or_create_trx(m_thd);
  context.m_trx->begin_stmt();
  context.m_extra_info.m_trxid = context.m_trx->get_id();
  context.m_extra_info.m_scn = TransactionCoordinator::instance().allocate_scn();  // see the commont on RpdTable load.

  // use specific partion. such as partition(p1, p2, p10, ..., pn).
  std::vector<logical_part_loaded_t> part_tb_infos;
  if (table_list->partition_names && table.file->get_partition_handler()) {
    partition_info *part_info = table_list->table->part_info;
    List_iterator_fast<String> it(*table_list->partition_names);
    String *str{nullptr};
    while ((str = it++)) {
      uint part_id;
      if (part_info->get_part_elem(str->c_ptr(), &part_id) && part_id != NOT_A_PARTITION_ID) {
        context.m_extra_info.m_partition_infos.emplace(std::make_pair(str->c_ptr(), part_id));
      }
      part_tb_infos.emplace_back(logical_part_loaded_t{.id = part_id,
                                                       .name = std::string(str->c_ptr()),
                                                       .load_scn = context.m_extra_info.m_scn,
                                                       .load_type = load_type_t::USER});
    }
  } else {  // using all part.
    for (auto index = 0u; index < table.part_info->get_tot_partitions(); index++) {
      auto part_name = table.part_info->partitions[index]->partition_name;
      context.m_extra_info.m_partition_infos.emplace(std::make_pair(part_name, index));
      part_tb_infos.emplace_back(logical_part_loaded_t{
          .id = index, .name = part_name, .load_scn = context.m_extra_info.m_scn, .load_type = load_type_t::USER});
    }
  }

  Utils::Util::update_rpd_meta_info(&context, &table, Utils::Util::STAGE::BEGIN);
  if (Imcs::Imcs::instance()->load_parttable(&context, const_cast<TABLE *>(&table))) {
    my_error(ER_SECONDARY_ENGINE, MYF(0), table.s->db.str, table.s->table_name.str);
    context.m_trx->rollback_stmt();
    return HA_ERR_GENERIC;
  }
  Utils::Util::update_rpd_meta_info(&context, &table, Utils::Util::STAGE::END);
  context.m_trx->commit();

  // For partition-level loads on an already-loaded table, the share and
  // shannon_loaded_tables entry already exist — don't replace them.
  if (is_partition_load && shannon_loaded_tables->get(table.s->db.str, table.s->table_name.str) != nullptr) {
    return ShannonBase::SHANNON_SUCCESS;
  }

  m_share = std::make_shared<RapidPartShare>(table);
  m_share->m_source_table = &table;
  m_share->is_partitioned = true;
  m_share->file = this;
  m_share->m_tableid = context.m_table_id;

  shannon_loaded_tables->add(table.s->db.str, table.s->table_name.str, m_share);
  if (shannon_loaded_tables->get(table.s->db.str, table.s->table_name.str) == nullptr) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), table.s->db.str, table.s->table_name.str);
    return HA_ERR_KEY_NOT_FOUND;
  }
  // start population thread if table loaded successfully.
  ShannonBase::Populate::Populator::start();
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::unload_table(const char *db_name, const char *table_name, bool error_if_not_loaded) {
  auto share = shannon_loaded_tables->get(db_name, table_name);
  if (error_if_not_loaded && !share) {
    std::ostringstream oss;
    oss << db_name << "." << table_name << " table is not loaded into rapid yet";
    my_error(ER_SECONDARY_ENGINE, MYF(0), oss.str().c_str());
    return HA_ERR_GENERIC;
  }

  auto table_id = share ? share->m_tableid : 0;

  // Check if this is a partition-level unload (e.g. SECONDARY_UNLOAD PARTITION (p0, p2)).
  // In that case we must remove only the named partitions from the PartTable
  // and leave the shannon_loaded_tables entry intact so that subsequent
  // partition-level operations still find the table.
  Table_ref *table_list = m_thd->lex->query_block->get_table_list();
  if (table_list != nullptr && table_list->partition_names != nullptr) {
    auto *part_table = down_cast<Imcs::PartTable *>(Imcs::Imcs::instance()->get_rpd_parttable(table_id));
    if (part_table != nullptr) {
      partition_info *part_info = table_list->table->part_info;
      List_iterator_fast<String> it(*table_list->partition_names);
      String *str{nullptr};
      while ((str = it++)) {
        uint part_id;
        if (part_info->get_part_elem(str->c_ptr(), &part_id) && part_id != NOT_A_PARTITION_ID) {
          std::string part_key;
          part_key.append(str->c_ptr()).append("#").append(std::to_string(part_id));
          part_table->remove_partition(part_key);
        }
      }
    }
    // Do NOT erase from shannon_loaded_tables — other partitions may still be loaded.
    return ShannonBase::SHANNON_SUCCESS;
  }

  // Full table unload — existing logic.
  ShannonBase::Populate::Populator::unload(table_id);

  ShannonBase::Rapid_load_context context;
  context.m_table = share ? (share->m_source_table ? const_cast<TABLE *>(share->m_source_table) : nullptr) : nullptr;
  context.m_thd = m_thd;
  context.m_extra_info.m_keynr = active_index;
  context.m_schema_name = db_name;
  context.m_table_name = table_name;

  Imcs::Imcs::instance()->unload_table(&context, table_id, false, true);

  // ease the meta info.
  {
    std::lock_guard<std::mutex> lock(ShannonBase::shannon_rpd_columns_mutex);
    for (ShannonBase::rpd_columns_container::iterator it = ShannonBase::shannon_rpd_columns_info.begin();
         it != ShannonBase::shannon_rpd_columns_info.end();) {
      if (!strcmp(db_name, it->schema_name) && !strcmp(table_name, it->table_name))
        it = ShannonBase::shannon_rpd_columns_info.erase(it);
      else
        ++it;
    }
  }
  // if all cus has been unloaded, then we can remove the meta info. Considering the following
  // scenario: alter table xxx secondary_load partion(p0, p1, xxx, pN), then unload a part of
  // partitions, not all alter table xxx secondary_unload partition(p0, p10). Under this stage,
  // we think that the table is still in loading status.
  shannon_loaded_tables->erase(db_name, table_name);

  if (ShannonBase::shannon_self_load_mgr_inst)
    ShannonBase::shannon_self_load_mgr_inst->remove_table(db_name, table_name);

  if (!shannon_loaded_tables->size()) ShannonBase::Populate::Populator::shutdown();

  return ShannonBase::SHANNON_SUCCESS;
}
}  // namespace ShannonBase
