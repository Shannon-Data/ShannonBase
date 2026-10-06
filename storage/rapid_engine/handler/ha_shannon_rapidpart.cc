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

#include "ha_shannon_rapidpart.h"

#include <cstring>
#include <optional>
#include <string>

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
#include "storage/rapid_engine/populate/log_populate.h"
#include "storage/rapid_engine/utils/utils.h"

namespace ShannonBase {
namespace {
// Reports a secondary-engine failure to the client and returns the handler error code.
inline int fail_secondary(const std::string &msg) {
  my_error(ER_SECONDARY_ENGINE, MYF(0), msg.c_str());
  return HA_ERR_GENERIC;
}

// Key of a partition inside Imcs::PartTable: "<name>#<id>".
inline std::string part_key_of(const char *name, uint part_id) {
  return std::string(name) + "#" + std::to_string(part_id);
}

// The IMCS table of `part_id`, or nullptr if nothing was ever loaded for it.
inline auto find_partition(Imcs::RapidCursor &cursor, uint part_id) {
  const char *name = cursor.source()->part_info->partitions[part_id]->partition_name;
  const auto &rpd_table = cursor.table_source();
  return down_cast<Imcs::PartTable *>(rpd_table)->get_partition(part_key_of(name, part_id));
}

inline constexpr ha_rkey_function scan_flag(bool reverse) { return reverse ? HA_READ_BEFORE_KEY : HA_READ_AFTER_KEY; }

// True for the seek modes that walk the index backwards.
inline bool is_reverse_read(ha_rkey_function f) {
  return f == HA_READ_KEY_OR_PREV || f == HA_READ_BEFORE_KEY || f == HA_READ_PREFIX_LAST ||
         f == HA_READ_PREFIX_LAST_OR_PREV;
}

// The table list of the statement being executed, or nullptr.
inline Table_ref *statement_table_list(THD *thd) {
  if (thd == nullptr || thd->lex == nullptr || thd->lex->query_block == nullptr) return nullptr;
  return thd->lex->query_block->get_table_list();
}

// Calls fn(name, part_id) for every valid partition named in PARTITION (p0, p1, ...).
template <typename Fn>
void for_each_named_partition(Table_ref *table_list, partition_info *part_info, Fn &&fn) {
  List_iterator_fast<String> it(*table_list->partition_names);
  while (String *str = it++) {
    uint part_id;
    if (part_info->get_part_elem(str->c_ptr(), &part_id) && part_id != NOT_A_PARTITION_ID) fn(str->c_ptr(), part_id);
  }
}

// Describes the first readable column with a type Rapid cannot hold, or std::nullopt if there is none.
std::optional<std::string> unsupported_column(const TABLE &table) {
  for (uint i = 0; i < table.s->fields; ++i) {
    const Field *fld = table.field[i];
    if (!bitmap_is_set(table.read_set, i) || fld->is_flag_set(NOT_SECONDARY_FLAG)) continue;
    if (!Utils::Util::is_support_type(fld->type()))
      return std::string(table.s->table_name.str) + "." + fld->field_name + " type not allowed";
  }
  return std::nullopt;
}

// Fills ctx.m_extra_info.m_partition_infos with the partitions this statement loads: the ones named in
// SECONDARY_LOAD PARTITION (...), otherwise all of them.
void collect_load_partitions(const TABLE &table, Table_ref *table_list, bool is_partition_load,
                             Rapid_load_context &ctx) {
  auto &infos = ctx.m_extra_info.m_partition_infos;

  if (is_partition_load && table.file->get_partition_handler() && table_list->table != nullptr &&
      table_list->table->part_info != nullptr) {
    for_each_named_partition(table_list, table_list->table->part_info,
                             [&infos](const char *name, uint id) { infos.emplace(std::make_pair(name, id)); });
    return;
  }

  for (uint i = 0; i < table.part_info->get_tot_partitions(); ++i)
    infos.emplace(std::make_pair(table.part_info->partitions[i]->partition_name, i));
}

// Waits for the table's already-published change watermark. The unload command holds the table MDL exclusively,
// so no new DML can race in after this barrier; without the drain an in-flight worker could apply to a detached
// partition while a reload creates a fresh object for the same partition key.
int drain_table_changes(THD *thd, table_id_t table_id) {
  using Populate::Populator;
  using Populate::TablePropagationWaitResult;

  auto barrier = Populator::request_table_barrier(table_id);
  while (barrier.needs_wait()) {
    if (thd != nullptr && thd->killed) return HA_ERR_GENERIC;
    if (!Populator::active())
      return fail_secondary("cannot unload Rapid partition while change propagation is stopped");

    const auto result = Populator::wait_table_applied_for(
        table_id, barrier.required_change_id, Populate::QUERY_PROPAGATION_WAIT_SLICE_MS, barrier.buffer_generation);
    // Applied, or failed/detached propagation, which cannot still mutate these partitions.
    if (result == TablePropagationWaitResult::APPLIED || result == TablePropagationWaitResult::BROKEN ||
        result == TablePropagationWaitResult::GONE)
      break;
  }
  return SHANNON_SUCCESS;
}

// SECONDARY_UNLOAD PARTITION (...): detach only the named partitions.
int unload_named_partitions(THD *thd, Table_ref *table_list, table_id_t table_id) {
  auto *part_table = down_cast<Imcs::PartTable *>(Imcs::Imcs::instance()->get_rpd_parttable(table_id));
  if (part_table == nullptr) return SHANNON_SUCCESS;

  if (int error = drain_table_changes(thd, table_id)) return error;

  for_each_named_partition(table_list, table_list->table->part_info, [part_table](const char *name, uint part_id) {
    part_table->remove_partition(part_key_of(name, part_id));
  });
  return SHANNON_SUCCESS;
}
}  // namespace

ha_rapidpart::ha_rapidpart(handlerton *hton, TABLE_SHARE *table)
    : ha_rapid(hton, table), Partition_helper(this), m_thd(ha_thd()), m_share(nullptr) {}

int ha_rapidpart::open(const char *name, int mode, unsigned int test_if_locked, const dd::Table *table_def) {
  // part_info->partitions[] only holds top-level elements, while the part ids used everywhere in this handler
  // (find_partition(), collect_load_partitions(), PartTable keys) are leaf ids. Subpartitioning is unsupported.
  if (table != nullptr && table->part_info != nullptr && table->part_info->is_sub_partitioned())
    return fail_secondary("subpartitioned tables are not supported");

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

int ha_rapidpart::rnd_init_in_part(uint part_id, bool) {
  auto partition_ptr = find_partition(*m_cursor, part_id);
  m_current_part_empty = partition_ptr == nullptr || !partition_ptr->meta().active_rows();
  if (!m_current_part_empty) m_cursor->active_table(std::move(partition_ptr));
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::rnd_next_in_part(uint, uchar *buf) {
  if (m_current_part_empty || inited != handler::RND) return HA_ERR_END_OF_FILE;

  auto *reader_pool = ShannonBase::Imcs::Imcs::pool();
  const bool use_async =
      reader_pool != nullptr &&
      table_share->fields > static_cast<uint>(ShannonBase::shannon_rpd_engine_cfg.async_column_threshold);

  const int error = use_async
                        ? boost::asio::co_spawn(*reader_pool, m_cursor->next_async(buf), boost::asio::use_future).get()
                        : m_cursor->next(buf);

  // Both paths report "no more rows" as either code; the handler API wants END_OF_FILE.
  if (error == HA_ERR_KEY_NOT_FOUND) return HA_ERR_END_OF_FILE;
  if (error != ShannonBase::SHANNON_SUCCESS) return error;

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::rnd_end_in_part(uint, bool) { return ShannonBase::SHANNON_SUCCESS; }

int ha_rapidpart::rnd_end() {
  (void)Partition_helper::ph_rnd_end();
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

  auto partition_ptr = find_partition(*m_cursor, part_id);
  if (partition_ptr == nullptr) return HA_ERR_END_OF_FILE;  // nothing ever loaded for this partition.

  m_cursor->active_table(std::move(partition_ptr));
  m_cursor_part_id = part_id;
  return ShannonBase::SHANNON_SUCCESS;
}

bool ha_rapidpart::row_key_equals_saved(const uchar *buf, const PartIndexScanState &state) const {
  const KEY &key_info = table->key_info[active_index];
  if (state.key.size() != key_info.key_length) return false;
  std::vector<uchar> cur(key_info.key_length);
  key_copy(cur.data(), buf, &key_info, key_info.key_length);
  return std::memcmp(cur.data(), state.key.data(), cur.size()) == 0;
}

bool ha_rapidpart::try_resume_partition(uint part_id, uchar *buf, int *error) {
  auto &state = m_part_scan_state[part_id];
  if (!state.valid) return false;

  const bool reverse = is_reverse_read(state.find_flag);
  const uint key_len = static_cast<uint>(state.key.size());
  // The cursor's range bounds were dropped by the partition switch; re-arm them or a resumed scan runs past
  // the end of an equality/range scan.
  m_cursor->set_end_range(end_range);

  if (state.rowid == INVALID_ROW_ID) {
    // Resuming after a start-lookup miss: there is no "last returned row", so a key-only re-seek is exact.
    *error = m_cursor->index_read(buf, state.key.data(), key_len, state.find_flag);
  } else {
    // Re-seek to the FIRST entry of the run of equal keys (inclusive seek), then walk to the entry that was
    // returned last and step past it. AFTER/BEFORE_KEY would skip every remaining duplicate of that key.
    *error = m_cursor->index_read(buf, state.key.data(), key_len, reverse ? HA_READ_KEY_OR_PREV : HA_READ_KEY_OR_NEXT);
    while (*error == ShannonBase::SHANNON_SUCCESS && m_cursor->position(nullptr) != state.rowid) {
      // Left the run without meeting the saved rowid (entry no longer visible): this row is the next one.
      if (!row_key_equals_saved(buf, state)) goto done;
      *error = reverse ? m_cursor->index_prev(buf) : m_cursor->index_next(buf);
    }
    if (*error == ShannonBase::SHANNON_SUCCESS)
      *error = reverse ? m_cursor->index_prev(buf) : m_cursor->index_next(buf);
  }
done:
  if (*error == ShannonBase::SHANNON_SUCCESS)
    save_scan_position(part_id, buf, reverse);
  else
    state.valid = false;
  return true;
}

void ha_rapidpart::save_scan_position(uint part_id, const uchar *buf, bool reverse) {
  auto &state = m_part_scan_state[part_id];
  const KEY &key_info = table->key_info[active_index];
  state.key.resize(key_info.key_length);
  key_copy(state.key.data(), buf, &key_info, key_info.key_length);
  state.find_flag = scan_flag(reverse);
  state.rowid = m_cursor->position(nullptr);
  state.valid = true;
}

void ha_rapidpart::save_miss_position(uint part_id, const uchar *key, uint key_len, bool reverse) {
  auto &state = m_part_scan_state[part_id];
  if (key == nullptr || key_len == 0) {
    state.valid = false;
    return;
  }
  state.key.assign(key, key + key_len);
  state.find_flag = scan_flag(reverse);
  state.rowid = INVALID_ROW_ID;
  state.valid = true;
}

int ha_rapidpart::index_first_in_part(uint part_id, uchar *buf) {
  if (int error = switch_to_partition(part_id)) return error;

  const int error = m_cursor->index_read(buf, nullptr, 0, HA_READ_KEY_OR_NEXT);
  if (error == ShannonBase::SHANNON_SUCCESS) save_scan_position(part_id, buf, false);
  return error;
}

int ha_rapidpart::index_last_in_part(uint part_id, uchar *buf) {
  if (int error = switch_to_partition(part_id)) return error;

  const int error = m_cursor->index_read(buf, nullptr, 0, HA_READ_BEFORE_KEY);
  // The handler API does not allow this to return HA_ERR_KEY_NOT_FOUND (mirrors ha_rapid::index_last).
  if (error == HA_ERR_KEY_NOT_FOUND) return HA_ERR_END_OF_FILE;
  if (error == ShannonBase::SHANNON_SUCCESS) save_scan_position(part_id, buf, true);
  return error;
}

// Steps one row forward or backward in `part_id`, resuming from the saved position if the cursor was
// parked on another partition in the meantime.
int ha_rapidpart::step_in_part(uint part_id, uchar *buf, bool reverse) {
  const bool same_partition = (part_id == m_cursor_part_id);
  if (int error = switch_to_partition(part_id)) return error;

  int error;
  if (!same_partition && try_resume_partition(part_id, buf, &error)) return error;

  error = reverse ? m_cursor->index_prev(buf) : m_cursor->index_next(buf);
  if (error == ShannonBase::SHANNON_SUCCESS) save_scan_position(part_id, buf, reverse);
  return error;
}

int ha_rapidpart::index_prev_in_part(uint part_id, uchar *buf) { return step_in_part(part_id, buf, true); }

int ha_rapidpart::index_next_in_part(uint part_id, uchar *buf) { return step_in_part(part_id, buf, false); }

int ha_rapidpart::index_next_same_in_part(uint part_id, uchar *buf, const uchar *, uint) {
  return index_next_in_part(part_id, buf);
}

int ha_rapidpart::index_read_map_in_part(uint part_id, uchar *buf, const uchar *key, key_part_map keypart_map,
                                         ha_rkey_function find_flag) {
  if (int error = switch_to_partition(part_id)) return error;

  const uint key_len = calculate_key_len(table, active_index, keypart_map);
  const bool reverse = is_reverse_read(find_flag);

  const int error = m_cursor->index_read(buf, key, key_len, find_flag);
  if (error == ShannonBase::SHANNON_SUCCESS)
    save_scan_position(part_id, buf, reverse);
  else if (error == HA_ERR_KEY_NOT_FOUND)
    save_miss_position(part_id, key, key_len, reverse);
  return error;
}

int ha_rapidpart::index_read_last_map_in_part(uint part_id, uchar *buf, const uchar *key, key_part_map keypart_map) {
  return index_read_map_in_part(part_id, buf, key, keypart_map, HA_READ_PREFIX_LAST);
}

// True (after releasing the row lock) when the row just read lies beyond the end of the range.
bool ha_rapidpart::past_range_end() {
  if (compare_key(end_range) <= 0) return false;
  unlock_row();
  return true;
}

int ha_rapidpart::read_range_first_in_part(uint part_id, uchar *buf, const key_range *start_key,
                                           const key_range *end_key, bool eq_range_arg) {
  uchar *record = buf ? buf : table->record[0];

  eq_range = eq_range_arg;
  set_end_range(end_key, handler::RANGE_SCAN_ASC);
  range_key_part = table->key_info[active_index].key_part;

  const int error =
      start_key ? index_read_map_in_part(part_id, record, start_key->key, start_key->keypart_map, start_key->flag)
                : index_first_in_part(part_id, record);
  if (error) return (error == HA_ERR_KEY_NOT_FOUND) ? HA_ERR_END_OF_FILE : error;

  return past_range_end() ? HA_ERR_END_OF_FILE : ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::read_range_next_in_part(uint part_id, uchar *buf) {
  uchar *record = buf ? buf : table->record[0];

  /* We trust that index_next_same always gives a row in range. */
  if (eq_range) return index_next_same_in_part(part_id, record, end_range->key, end_range->length);

  const int error = index_next_in_part(part_id, record);
  if (error) return error;

  return past_range_end() ? HA_ERR_END_OF_FILE : ShannonBase::SHANNON_SUCCESS;
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

#ifndef NDEBUG
  // As in ha_rapid::load_table(): mysql_secondary_load_or_unload() holds
  // MDL_EXCLUSIVE on the table for the whole operation (sql/sql_table.cc), so
  // no concurrent DML, query or DDL can be touching it while the partitions
  // below are enumerated and copied.
  assert(m_thd != nullptr);
  assert(m_thd->mdl_context.owns_equal_or_stronger_lock(MDL_key::TABLE, table.s->db.str, table.s->table_name.str,
                                                        MDL_SHARED_READ));
#endif

  const char *db = table.s->db.str;
  const char *tbl = table.s->table_name.str;
  auto *mutable_table = const_cast<TABLE *>(&table);

  // A partitioned table without partition info cannot be loaded at all, and
  // every partition-enumerating path below dereferences it.
  if (table.part_info == nullptr) return fail_secondary("partitioned table has no partition info");

  // Loading specific partitions? (e.g. SECONDARY_LOAD PARTITION (p1)).
  Table_ref *table_list = statement_table_list(m_thd);
  const bool is_partition_load = table_list != nullptr && table_list->partition_names != nullptr;

  if (!is_partition_load && shannon_loaded_tables->get(db, tbl) != nullptr)
    return fail_secondary(std::string(db) + "." + tbl + " already loaded");

  if (auto unsupported = unsupported_column(table)) return fail_secondary(*unsupported);

  m_thd->set_sent_row_count(0);

  // Read data from InnoDB and load it into Rapid.
  ShannonBase::Rapid_load_context context;
  context.m_table = mutable_table;
  context.m_table_id = table.file->get_table_id();
  context.m_thd = m_thd;
  context.m_extra_info.m_keynr = active_index;
  context.m_schema_name = db;
  context.m_table_name = tbl;
  context.m_sch_tb_name = context.m_schema_name + "." + context.m_table_name;
  // Bulk load, not DML. Leaving m_oper at its PROPAGATION default sends every
  // row down Imcu::insert_row()'s DML branch: a WAL prepare/commit pair per row
  // for data being read out of InnoDB (which is already durable), and a journal
  // entry per row. With no SCN the entry is ACTIVE, so its txn id lands in
  // active_txns and nothing ever commits it -- has_uncommitted_changes() stays
  // true for the life of the process, the checkpoint gate refuses every
  // snapshot, the WAL is never truncated and fast recovery can never arm. Even
  // where an SCN is set the entries alone keep is_fully_visible() false, which
  // costs every scan the per-row visibility walk.
  context.m_extra_info.m_oper = ShannonBase::Rapid_context::extra_info_t::OperType::LOAD;

  context.m_trx = Transaction::get_or_create_trx(m_thd);
  if (context.m_trx == nullptr) return fail_secondary("cannot start a Rapid transaction for the load");
  context.m_trx->begin_stmt();
  context.m_extra_info.m_trxid = context.m_trx->get_id();
  context.m_extra_info.m_scn = TransactionCoordinator::instance().allocate_scn();  // see the comment on RpdTable load.

  collect_load_partitions(table, table_list, is_partition_load, context);

  Utils::Util::update_rpd_meta_info(&context, &table, Utils::Util::STAGE::BEGIN);
  if (Imcs::Imcs::instance()->load_parttable(&context, mutable_table)) {
    // load_parttable() already raised a specific error and cleaned up its own state; only add one if it did not.
    int error = HA_ERR_GENERIC;
    if (!m_thd->is_error()) error = fail_secondary("load of " + context.m_sch_tb_name + " into Rapid failed");
    context.m_trx->rollback_stmt();
    return error;
  }
  Utils::Util::update_rpd_meta_info(&context, &table, Utils::Util::STAGE::END);
  if (context.m_trx->commit()) return fail_secondary("cannot commit the Rapid load transaction");

  // For partition-level loads on an already-loaded table, the share and
  // shannon_loaded_tables entry already exist — don't replace them.
  if (is_partition_load && shannon_loaded_tables->get(db, tbl) != nullptr) return ShannonBase::SHANNON_SUCCESS;

  m_share = std::make_shared<RapidPartShare>(table);
  m_share->is_partitioned = true;
  m_share->m_tableid = context.m_table_id;

  shannon_loaded_tables->add(db, tbl, m_share);
  if (shannon_loaded_tables->get(db, tbl) == nullptr) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), db, tbl);
    return HA_ERR_KEY_NOT_FOUND;
  }

  // Start the population thread now that a table is loaded.
  ShannonBase::Populate::Populator::start();
  return ShannonBase::SHANNON_SUCCESS;
}

int ha_rapidpart::unload_table(const char *db_name, const char *table_name, bool error_if_not_loaded) {
  const auto share = shannon_loaded_tables->get(db_name, table_name);
  if (!share && error_if_not_loaded)
    return fail_secondary(std::string(db_name) + "." + table_name + " table is not loaded into rapid yet");

  const auto table_id = share ? share->m_tableid : 0;

  // SECONDARY_UNLOAD PARTITION (p0, p2): remove only the named partitions and leave the
  // shannon_loaded_tables entry intact, because other partitions may still be loaded and
  // later partition-level operations must still find the table.
  Table_ref *table_list = statement_table_list(m_thd);
  if (table_list != nullptr && table_list->partition_names != nullptr && table_list->table != nullptr &&
      table_list->table->part_info != nullptr)
    return unload_named_partitions(m_thd, table_list, table_id);

  // Full table unload.
  ShannonBase::Populate::Populator::unload(table_id);

  ShannonBase::Rapid_load_context context;
  context.m_table = nullptr;  // see ha_rapid::unload_table()
  context.m_thd = m_thd;
  context.m_extra_info.m_keynr = active_index;
  context.m_schema_name = db_name;
  context.m_table_name = table_name;
  Imcs::Imcs::instance()->unload_table(&context, table_id, false, true);

  // Erase the column meta info.
  {
    std::lock_guard<std::mutex> lock(ShannonBase::shannon_rpd_columns_mutex);
    std::erase_if(ShannonBase::shannon_rpd_columns_info, [&](const auto &col) {
      return std::strcmp(db_name, col.schema_name) == 0 && std::strcmp(table_name, col.table_name) == 0;
    });
  }

  shannon_loaded_tables->erase(db_name, table_name);
  ShannonBase::RpdMirror::Registry::mark_unloaded(db_name, table_name);

  if (!shannon_loaded_tables->size()) ShannonBase::Populate::Populator::shutdown();

  return ShannonBase::SHANNON_SUCCESS;
}
}  // namespace ShannonBase
