/* Copyright (c) 2018, 2023, Oracle and/or its affiliates.

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

   Copyright (c) 2023, Shannon Data AI and/or its affiliates.
*/

#include "storage/rapid_engine/handler/ha_shannon_rapid.h"

#include <stddef.h>
#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

#include "lex_string.h"
#include "my_alloc.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysql/plugin.h"
#include "mysqld_error.h"
#include "sql/current_thd.h"  //current_thd
#include "sql/debug_sync.h"
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/make_join_hypergraph.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/key.h"
#include "sql/opt_costmodel.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"
#include "template_utils.h"
#include "thr_lock.h"

#include "storage/innobase/handler/ha_innodb.h"
#include "storage/innobase/include/dict0dd.h"
#include "storage/innobase/include/lock0lock.h"
#include "storage/innobase/include/trx0trx.h"

#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/imcs/cu.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/include/rapid_context.h"
#include "storage/rapid_engine/include/rapid_stats.h"
#include "storage/rapid_engine/utils/utils.h"

#include "storage/rapid_engine/cost/cost.h"             //costestimator
#include "storage/rapid_engine/optimizer/optimizer.h"   //optimizer
#include "storage/rapid_engine/optimizer/rules/rule.h"  //Rule
#include "storage/rapid_engine/populate/populate.h"

namespace dd {
class Table;
}
/**
  this is used to disable the warings:
  'piecewise_construct' which is in '/usr/include/c++/11/bits/stl_pair.h:83:53'
*/
extern "C" const char* __asan_default_options() {
  return "detect_odr_violation=0";
}

namespace ShannonBase {
// Map from (db_name, table_name) to the RapidShare with table state.
void ShannonLoadedTables::add(const std::string &db, const std::string &table, ShannonBase::RapidShare* share) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_tables.insert({std::make_pair(db, table), share});
}

ShannonBase::RapidShare *ShannonLoadedTables::get(const std::string &db, const std::string &table) {
  std::lock_guard<std::mutex> guard(m_mutex);
  auto it = m_tables.find(std::make_pair(db, table));
  return it == m_tables.end() ? nullptr : it->second;
}

void ShannonLoadedTables::erase(const std::string &db, const std::string &table) {
  std::lock_guard<std::mutex> guard(m_mutex);
  auto it = m_tables.find(std::make_pair(db, table));
  auto ptr = it == m_tables.end() ? nullptr : it->second;
  if (ptr)
    delete it->second;
  m_tables.erase(std::make_pair(db, table));
}
void ShannonLoadedTables::table_infos(uint index, ulonglong&tid, std::string& schema, std::string& table ) {
  if (index > m_tables.size()) return;
  uint count = 0;
  for (auto& item : m_tables) {
    if (count == index) {
      schema= (item.first).first;
      table = (item.first).second;
      tid = (item.second)->m_tableid;
    }
    count ++;
  }
}  

ShannonLoadedTables *shannon_loaded_tables{nullptr};
handlerton* shannon_rapid_hton_ptr {nullptr};
static bool shannon_rpd_inited {false};
ShannonBase::Imcs::Imcs* imcs_instance = ShannonBase::Imcs::Imcs::get_instance();;

//global rpd meta infos
rpd_columns_container meta_rpd_columns_infos;
std::map<std::string, std::unique_ptr<Compress::Dictionary>> loaded_dictionaries;

Shannon_execution_context::Shannon_execution_context() : m_data(std::make_unique<char[]>(10)) {
}

bool Shannon_execution_context::BestPlanSoFar(const JOIN &join, double cost) {
    if (&join != m_current_join) {
      // No plan has been seen for this join. The current one is best so far.
      m_current_join = &join;
      m_best_cost = cost;
      return true;
    }

    // Check if the current plan is the best seen so far.
    const bool cheaper = cost < m_best_cost;
    m_best_cost = std::min(m_best_cost, cost);
    return cheaper;
}

ha_rapid::ha_rapid(handlerton *hton, TABLE_SHARE *table_share_arg)
    : handler(hton, table_share_arg), m_rpd_thd(nullptr), m_start_of_scan(false),
    m_share(nullptr), m_rpd_context(nullptr), m_imcs_reader(nullptr),
    m_primary_key(nullptr), m_key_type(MYSQL_TYPE_INVALID) {
  m_rpd_thd = ha_thd();
}
ha_rapid::~ha_rapid() {
}
const char *ha_rapid::table_type() const { return (rapid_hton_name); }

int ha_rapid::create(const char *, TABLE *, HA_CREATE_INFO *, dd::Table *) {
  DBUG_TRACE;
  return HA_ERR_WRONG_COMMAND;
}

int ha_rapid::open(const char *name, int mode, unsigned int test_if_locked, const dd::Table * table_def) {
  DBUG_TRACE;
  m_share = shannon_loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (m_share == nullptr) {
    // The table has not been loaded into the secondary storage engine yet.
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Table has not been loaded");
    return HA_ERR_GENERIC;
  }
  thr_lock_data_init(&m_share->m_lock, &m_lock, nullptr);
  return 0;
}

int ha_rapid::close () {
  DBUG_TRACE;
  return 0;
}

/* Assume that the read time is proportional to the scan time for all
  rows + at most one seek per range. */
//refined cost model should be impl in future. here, it's a coarse one.
double ha_rapid::scan_time() {
  //all memory operations.
  const Cost_model_table *const cost_model = table->cost_model();
  double blocks = (estimate_rows_upper_bound() * SHANNON_ROW_TOTAL_LEN) / UNIV_PAGE_SIZE;

  return cost_model->buffer_block_read_cost(blocks ? blocks : 1);
}

/** Calculate the time it takes to read a set of ranges through an index
 This enables us to optimise reads for clustered indexes.
 @return estimated time measured in disk seeks */
double ha_rapid::read_time(uint index, uint ranges, ha_rows rows) {
  ha_rows total_rows;

  if (index != table->s->primary_key) {
    /* Not clustered */
    return (handler::read_time(index, ranges, rows));
  }

  if (rows <= 2) {
    return ((double)rows);
  }

  /* Assume that the read time is proportional to the scan time for all
  rows + at most one seek per range. */

  double time_for_scan = scan_time();

  if ((total_rows = estimate_rows_upper_bound()) < rows) {
    return (time_for_scan);
  }

  return (ranges + (double)rows / (double)total_rows * time_for_scan);
}

int ha_rapid::read_range_first(const key_range *start_key,
                               const key_range *end_key, bool eq_range_arg,
                               bool sorted) {
  DBUG_TRACE;
  return handler::read_range_first(start_key, end_key, eq_range_arg, sorted);
}

int ha_rapid::read_range_next() {
  return (handler::read_range_next());
}

int ha_rapid::rapid_initialize() {
  ut_ad(shannon_rpd_inited == true);
  ut_ad(m_start_of_scan == false);
  m_start_of_scan = true;

  trx_t* trx = check_trx_exists (m_rpd_thd);
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (trx->isolation_level > TRX_ISO_READ_UNCOMMITTED) {
    trx_assign_read_view(trx);
  }

  //init rapidcontext
  m_rpd_context.reset(new ShannonBase::RapidContext());
  m_rpd_context->m_handler = this;
  m_rpd_context->m_table = table;
  m_rpd_context->m_current_db = table->s->db.str;
  m_rpd_context->m_current_table = table->s->table_name.str;
  m_rpd_context->m_trx = trx;
  if (loaded_dictionaries.find(m_rpd_context->m_current_db) == loaded_dictionaries.end()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Local dictionary error.");
    return HA_ERR_GENERIC;
  }
  m_rpd_context->m_local_dict = loaded_dictionaries[m_rpd_context->m_current_db].get();
  m_rpd_context->m_extra_info.m_keynr = active_index;
  m_imcs_reader.reset(new ImcsReader(table)) ;

  return m_imcs_reader->open();
}

int ha_rapid::rapid_deinitialize() {
  if (m_start_of_scan) {
    m_start_of_scan = false;
    trx_t* trx = thd_to_trx (m_rpd_thd);
    if (trx_is_started(trx)) {
      const dberr_t error [[maybe_unused]] = trx_commit_for_mysql(trx);
      ut_ad(DB_SUCCESS == error);
    }
    trx->will_lock = 0;

    m_imcs_reader->close();
    m_rpd_context.reset(nullptr);
    m_imcs_reader.reset(nullptr);
  }

  return 0;
}

int ha_rapid::rnd_init(bool scan) {
  DBUG_TRACE;
  inited = handler::RND;
  return rapid_initialize();
}

/** Ends a table scan.
 @return 0 or error number */
int ha_rapid::rnd_end() {
  DBUG_TRACE;
  inited = handler::NONE;
  return rapid_deinitialize();
}

/** Reads the next row in a table scan (also used to read the FIRST row
 in a table scan).
 @return 0, HA_ERR_END_OF_FILE, or error number */
int ha_rapid::rnd_next(unsigned char *buffer) {
  DBUG_TRACE;
  ut_ad (m_start_of_scan && inited == handler::RND);

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  auto err = m_imcs_reader->read(m_rpd_context.get(), buffer);
  return err;
}

int ha_rapid::info(unsigned int flags) {
  // Get the cardinality statistics from the primary storage engine.
  handler *primary = ha_get_primary_handler();
  int ret = primary->info(flags);
  if (ret == 0) {
    stats.records = primary->stats.records;
  }
  return ret;
}

handler::Table_flags ha_rapid::table_flags() const {
  ulong flags = HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
                HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN;
  return flags;
}

int ha_rapid::key_cmp(KEY_PART_INFO *key_part, const uchar *key, uint key_length) {
  uint store_length;

  for (const uchar *end = key + key_length; key < end;
       key += store_length, key_part++) {
    int cmp;
    const int res = (key_part->key_part_flag & HA_REVERSE_SORT) ? -1 : 1;
    store_length = key_part->store_length;
    if (key_part->null_bit) {
      /* This key part allows null values; NULL is lower than everything */
      const bool field_is_null = key_part->field->is_null();
      if (*key)  // If range key is null
      {
        /* the range is expecting a null value */
        if (!field_is_null) return res;  // Found key is > range
        /* null -- exact match, go to next key part */
        continue;
      } else if (field_is_null)
        return -res;  // NULL is less than any value
      key++;          // Skip null byte
      store_length--;
    }
    if ((cmp = key_part->field->key_cmp(key, key_part->length)) < 0)
      return -res;
    if (cmp > 0) return res;
  }
  return 0;  // Keys are equal
}

int ha_rapid::compare_key_icp(const key_range *range) {
  int cmp;
  if (!range) return 0;  // no max range
  cmp = key_cmp(range_key_part, range->key, range->length);
  if (!cmp) cmp = get_key_comp_result();
  if (get_range_scan_direction() == RANGE_SCAN_DESC) cmp = -cmp;
  return cmp;
}

Item* ha_rapid::get_cond_item(Item* cond, Field* key_field) {
  /**
   * In rapide, it's a column store, we only juse the first part of an index as we
   * index when we insert the data into rapid, therefore, if we use comb index,
   * such as we have an PK(cola, colb), and using cond as cond(cola, a) OP cond(colb,b).
   * Therefore, in rapid, only the first part of cond used in pushed down condition.
  */
  if (cond->type() == Item::COND_ITEM) {
    List_iterator<Item> li(*((Item_cond *)cond)->argument_list());
    Item *item;
    while ((item = li++)) {
      if (item->type() == Item::COND_ITEM) {
        //if ((Item_cond*)item)->functype() == Item_func::)
        get_cond_item(item, key_field);

      } else if (item->type() == Item::FUNC_ITEM) {
        Item_func *item_func = (Item_func *)item;
        const Item_func::Functype func_type = item_func->functype();
        ut_a(func_type != Item_func::TRIG_COND_FUNC || func_type != Item_func::DD_INTERNAL_FUNC);

        if (item_func->argument_count() > 0) {
        /* This is a function, apply condition recursively to arguments */
          Item **item_end =
              (item_func->arguments()) + item_func->argument_count();
          for (Item **child = item_func->arguments(); child != item_end; child++) {
            auto arg = *child;
            if (uses_index_fields_only(arg, table, 0, false))
              return arg;
            get_cond_item(arg, key_field);
          }
        }
      } else {
        switch (item->type()) {
          case Item::FIELD_ITEM: {
             Item_field* item_f = down_cast<Item_field*> (item);
             if (item_f->field->table == table && item_f->field == key_field) return item_f;
          } break;
          case  Item::REF_ITEM: {
            cond->real_item();
          }break;
          default: break;
        }
      }
    }
  }
  return nullptr;
}

Item *ha_rapid::idx_cond_push(uint keyno, Item *idx_cond)
{
  DBUG_TRACE;
  ut_ad(keyno != MAX_KEY);
  ut_ad(idx_cond != nullptr);

  pushed_idx_cond = idx_cond;
  pushed_idx_cond_keyno = keyno;
  in_range_check_pushed_down = true;

  /* We will evaluate the condition entirely */
  return nullptr;
}

unsigned long ha_rapid::index_flags(unsigned int idx, unsigned int part,
                                   bool all_parts) const {
  //here, we support the same index flag as primary engine.
  const handler *primary = ha_get_primary_handler();
  const unsigned long primary_flags =
      primary == nullptr ? 0 : primary->index_flags(idx, part, all_parts);

  if(pushed_idx_cond) {}
  // Inherit the following index flags from the primary handler, if they are
  // set:
  //
  // HA_READ_RANGE - to signal that ranges can be read from the index, so that
  // the optimizer can use the index to estimate the number of rows in a range.
  //
  // HA_KEY_SCAN_NOT_ROR - to signal if the index returns records in rowid
  // order. Used to disable use of the index in the range optimizer if it is not
  // in rowid order.

  return ((HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER |
           HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN |
           HA_READ_RANGE | HA_KEY_SCAN_NOT_ROR) & primary_flags);
}

uint ha_rapid::max_supported_keys() const {
  return (MAX_KEY);
}

uint ha_rapid::max_supported_key_length() const {
  return (MAX_KEY);
}

int ha_rapid::index_init(uint keynr, bool sorted) {
  DBUG_TRACE;
  active_index = keynr;

  m_primary_key = dynamic_cast<ha_innobase*>(ha_get_primary_handler())->innobase_get_index(keynr);
  bool index_usable {false};
  if (m_primary_key == nullptr) {
    index_usable = false;
    return 1;
  }

  index_usable = m_primary_key->is_usable(thd_to_trx(ha_thd()));
  if (!index_usable) {
    if (m_primary_key->is_corrupted()) {
      return m_primary_key->is_clustered() ?  HA_ERR_TABLE_CORRUPT : HA_ERR_INDEX_CORRUPT;
     }
  }

  auto ret = rapid_initialize();
  inited = handler::INDEX;

  return ret;
}

int ha_rapid::index_end() {
  DBUG_TRACE;

  active_index = MAX_KEY;

  in_range_check_pushed_down = false;
  inited = handler::NONE;

  return rapid_deinitialize();
}

int ha_rapid::records(ha_rows *num_rows) {
  DBUG_TRACE;

  ulint n_rows = 0; /* Record count in this view */

  if (!m_start_of_scan)
    rapid_initialize();

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  n_rows = m_imcs_reader->records(m_rpd_context.get());
  *num_rows = n_rows;
  rapid_deinitialize();
  return 0;
}

/** Positions an index cursor to the index specified in the handle. Fetches the
 row if any.
 @return 0, HA_ERR_KEY_NOT_FOUND, or error number */
int ha_rapid::index_read(uchar *buf, const uchar *key, uint key_len,
                 ha_rkey_function find_flag) {
  DBUG_TRACE;
  ut_ad (m_start_of_scan && inited == handler::INDEX);

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  auto err = m_imcs_reader->index_read(m_rpd_context.get(), buf,
                                       const_cast<uchar*>(key), key_len, find_flag);
  return err;
}

int ha_rapid::index_read_last(uchar *buf, const uchar *key, uint key_len) {
  return HA_ERR_KEY_NOT_FOUND;
}

/** Reads the next row from a cursor.
 @return 0, HA_ERR_END_OF_FILE, or error number */
int ha_rapid::index_next(uchar *buf) {
  ut_ad (m_start_of_scan && inited == handler::INDEX);

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  auto err = m_imcs_reader->index_next(m_rpd_context.get(), buf);
  return err;
}

/** Reads the next row matching to the key value given as the parameter.
 @return 0, HA_ERR_END_OF_FILE, or error number */
int ha_rapid::index_next_same(uchar *buf, const uchar *key, uint keylen) {
  ut_ad (m_start_of_scan && inited == handler::INDEX);

  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);
  auto err = m_imcs_reader->index_next_same(m_rpd_context.get(), buf,
                                      const_cast<uchar*>(key), keylen,
                                      HA_READ_KEY_EXACT);
  return err;
}

/** Reads the previous row from a cursor, which must have previously been
 positioned using index_read.
 @return 0, HA_ERR_END_OF_FILE, or error number */
int ha_rapid::index_prev(uchar *buf) {
  return HA_ERR_END_OF_FILE;
}

/** Positions a cursor on the first record in an index and reads the
 corresponding row to buf.
 @return 0, HA_ERR_END_OF_FILE, or error code */
int ha_rapid::index_first(uchar *buf) {
  ha_statistic_increment(&System_status_var::ha_read_rnd_next_count);

  //m_rpd_context->m_extra_info.m_find_flag = HA_READ_AFTER_KEY;
  return m_imcs_reader->index_general(m_rpd_context.get(), buf);
}

/** Positions a cursor on the last record in an index and reads the
 corresponding row to buf.
 @return 0, HA_ERR_END_OF_FILE, or error code */
int ha_rapid::index_last(uchar *buf) {
  return HA_ERR_END_OF_FILE;
}

#if 0
int ha_rapid::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                          uint n_ranges, uint mode,
                          HANDLER_BUFFER *buf) {
  return 0;
}

int ha_rapid::multi_range_read_next(char **range_info) {
  return 0;
}

ha_rows ha_rapid::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                    void *seq_init_param, uint n_ranges,
                                    uint *bufsz, uint *flags,
                                    Cost_estimate *cost) {
  return 0;
}

ha_rows ha_rapid::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                              uint *bufsz, uint *flags,
                              Cost_estimate *cost) {
  return 0;
}
#endif
/**
Index Condition Pushdown interface implementation */

/** Shannon Rapid index push-down condition check
 @return ICP_NO_MATCH, ICP_MATCH, or ICP_OUT_OF_RANGE */
ICP_RESULT
shannon_rapid_index_cond(ha_rapid *h) /*!< in/out: pointer to ha_rapid */
{
  DBUG_TRACE;

  assert(h->pushed_idx_cond);
  assert(h->pushed_idx_cond_keyno != MAX_KEY);

  if (h->end_range && h->compare_key_icp(h->end_range) > 0) {
    /* caller should return HA_ERR_END_OF_FILE already */
    return ICP_OUT_OF_RANGE;
  }

  return h->pushed_idx_cond->val_int() ? ICP_MATCH : ICP_NO_MATCH;
}

ha_rows ha_rapid::estimate_rows_upper_bound() {
  DBUG_TRACE;

  /* Calculate a minimum length for a clustered index record and from
  that an upper bound for the number of rows. Since we only calculate
  new statistics in row0mysql.cc when a table has grown by a threshold
  factor, we must add a safety factor 2 in front of the formula below. */
  ShannonBase::Imcs::Imcs* imcs_instance = ShannonBase::Imcs::Imcs::get_instance();
  assert(imcs_instance);
  ha_rows estimate = imcs_instance->get_rows(table);

  return (ha_rows)estimate;
}

/** Estimates the number of index records in a range.
 @return estimated number of rows */
ha_rows ha_rapid::records_in_range(unsigned int index, key_range *min_key,
                                  key_range *max_key) {
  // Get the number of records in the range from the primary storage engine.
  DBUG_TRACE;
  /**
   * due to the rows stored in imcs by primary key order, therefore, it should be in ordered.
   * it's friendly to purge the unsatisfied chunk as fast as possible.
  */
  std::unique_ptr<ImcsReader> reader= std::make_unique<ImcsReader>(table);
  if (!reader.get()) return 0;

  reader->open();
  std::unique_ptr<RapidContext> rpd_context  = std::make_unique<RapidContext>();
  rpd_context->m_table = table;
  rpd_context->m_current_db = table->s->db.str;
  rpd_context->m_current_table = table->s->table_name.str;

  trx_t* trx = check_trx_exists (m_rpd_thd);
  TrxInInnoDB trx_in_innodb(trx);
  trx_start_if_not_started(trx, false, UT_LOCATION_HERE);
  if (trx->isolation_level > TRX_ISO_READ_UNCOMMITTED)
    trx_assign_read_view(trx);
  rpd_context->m_trx = trx;

  auto nums = reader->records_in_range(rpd_context.get(), index, min_key, max_key);
  reader->close();
  trx_commit(trx);

  return nums;
}

THR_LOCK_DATA **ha_rapid::store_lock(THD *, THR_LOCK_DATA **to,
                                    thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && m_lock.type == TL_UNLOCK)
    m_lock.type = lock_type;
  *to++ = &m_lock;
  return to;
}

int ha_rapid::load_table(const TABLE &table_arg) {
  DBUG_TRACE;
  ut_ad(table_arg.file != nullptr);
  THD* thd = m_rpd_thd;
  if (shannon_loaded_tables->get(table_arg.s->db.str, table_arg.s->table_name.str) != nullptr) {
    std::ostringstream err;
    err << table_arg.s->db.str << "." <<table_arg.s->table_name.str << " already loaded";
    my_error(ER_SECONDARY_ENGINE_LOAD, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  for (uint idx =0; idx < table_arg.s->fields; idx ++) {
    Field* key_field = *(table_arg.field + idx);
    if (!Utils::Util::is_support_type(key_field->type())) {
      std::ostringstream err;
      err << key_field->field_name << " type not allowed";
      my_error(ER_SECONDARY_ENGINE_LOAD, MYF(0), err.str().c_str());
      return HA_ERR_GENERIC;
    }
  }

  std::unique_ptr<ImcsReader> imcs_reader = std::make_unique<ImcsReader>(const_cast<TABLE*>(&table_arg));
  ut_a(imcs_reader.get());
  imcs_reader->open();
  /*** in future, we will load the content strings from dictionary file, which makes it more flexible.
  at rapid engine startup phase. and will make each loaded column use its own dictionary, so called
  local dictionary. the dictionary algo defined by column's comment text. */
  std::string db_name(table_arg.s->db.str);
  if (loaded_dictionaries.find(db_name) == loaded_dictionaries.end())
     loaded_dictionaries.insert(std::make_pair(db_name, std::make_unique<Compress::Dictionary>()));

  ShannonBase::RapidContext context;
  context.m_current_db = table_arg.s->db.str;
  context.m_current_table = table_arg.s->table_name.str;
  context.m_table = const_cast<TABLE *>(&table_arg);

  // check if primary key is missing. rapid engine must has at least one PK.
  if (table_arg.s->is_missing_primary_key()) {
    my_error(ER_REQUIRES_PRIMARY_KEY, MYF(0));
    return HA_ERR_GENERIC;
  }

  context.m_extra_info.m_keynr = 0;
  auto key = (table_arg.key_info + 0);
  for (uint keyid = 0; keyid < key->user_defined_key_parts; keyid++) {
    if (key->key_part[keyid].field->is_flag_set(NOT_SECONDARY_FLAG)) {
      my_error(ER_RAPID_DA_PRIMARY_KEY_CAN_NOT_HAVE_NOT_SECONDARY_FLAG, MYF(0), table_arg.s->db.str,
               table_arg.s->table_name.str);
      return HA_ERR_GENERIC;
    }
  }

  context.m_extra_info.m_key_buff = std::make_unique<uchar[]>(key->key_length);
  // Scan the primary table and read the records.
  if (table_arg.file->inited == NONE && table_arg.file->ha_rnd_init(true)) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), table_arg.s->db.str, table_arg.s->table_name.str);
    return HA_ERR_GENERIC;
  }
  //Start to write to IMCS. Do scan the primary table.
  m_rpd_thd->set_sent_row_count(0);
  int tmp {HA_ERR_GENERIC};

  while ((tmp = table_arg.file->ha_rnd_next(table_arg.record[0])) != HA_ERR_END_OF_FILE) {
   /*** ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is reading and another deleting
    without locks. Now, do full scan, but multi-thread scan will impl in future. */
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;

    auto offset {0};
    memset(context.m_extra_info.m_key_buff.get(), 0x0, key->key_length);
    for (uint key_partid = 0; key_partid < key->user_defined_key_parts; key_partid++) {
      memcpy(context.m_extra_info.m_key_buff.get() + offset, key->key_part[key_partid].field->field_ptr(),
             key->key_part[key_partid].store_length);
      offset += key->key_part[key_partid].store_length;
    }
    context.m_extra_info.m_key_len = offset;

    uint32 field_count = table_arg.s->fields;
    Field *field_ptr = nullptr;
    uint32 primary_key_idx [[maybe_unused]] = field_count;

    context.m_trx = thd_to_trx(m_rpd_thd);
    field_ptr = *(table_arg.field + field_count); //ghost field.
    if (field_ptr && field_ptr->type() == MYSQL_TYPE_DB_TRX_ID) {
      context.m_extra_info.m_trxid = field_ptr->val_int();
    }

    if (context.m_trx->state == TRX_STATE_NOT_STARTED) {
      assert (false);
    }
    //will used rowid as rapid pk.
    //if (imcs_instance->write_direct(&context, field_ptr)) {
    if (imcs_reader->write(&context, const_cast<TABLE*>(&table_arg)->record[0])) {
      table_arg.file->ha_rnd_end();
      imcs_instance->delete_all_direct(&context);
      my_error(ER_SECONDARY_ENGINE_LOAD, MYF(0), table_arg.s->db.str,
               table_arg.s->table_name.str);
      return HA_ERR_GENERIC;
    }
    ha_statistic_increment(&System_status_var::ha_read_rnd_count);
    m_rpd_thd->inc_sent_row_count(1);
    if (tmp == HA_ERR_RECORD_DELETED && !thd->killed) continue;
  }
  table_arg.file->ha_rnd_end();

  m_share = new RapidShare (table_arg);
  m_share->file = this;
  m_share->m_tableid = table_arg.s->table_map_id.id();
  shannon_loaded_tables->add(table_arg.s->db.str, table_arg.s->table_name.str, m_share);
  if (shannon_loaded_tables->get(table_arg.s->db.str, table_arg.s->table_name.str) ==
      nullptr) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), table_arg.s->db.str,
             table_arg.s->table_name.str);
    return HA_ERR_KEY_NOT_FOUND;
  }

  return 0;
}

int ha_rapid::unload_table(const char *db_name, const char *table_name,
                          bool error_if_not_loaded) {
  DBUG_TRACE;
  if (error_if_not_loaded &&
      shannon_loaded_tables->get(db_name, table_name) == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Table is not loaded on a secondary engine");
    return HA_ERR_GENERIC;
  }

  ShannonBase::Imcs::Imcs* imcs_instance = ShannonBase::Imcs::Imcs::get_instance();
  assert(imcs_instance);
  RapidContext context;
  context.m_current_db = std::string(db_name);
  context.m_current_table = std::string(table_name);

  if (auto ret = imcs_instance->delete_all_direct(&context)) {
    return ret;
  }
  shannon_loaded_tables->erase(db_name, table_name);
  m_imcs_reader.reset(nullptr);
  return 0;
}
}  // namespace ShannonBase

static bool ShannonPrepareSecondaryEngine(THD *thd, LEX *lex) {
  DBUG_EXECUTE_IF("secondary_engine_rapid_prepare_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
    return true;
  });

  auto context = new (thd->mem_root) ShannonBase::Shannon_execution_context();
  if (context == nullptr) return true;
  lex->set_secondary_engine_execution_context(context);

  // Disable use of constant tables and evaluation of subqueries during
  // optimization. OPTION_NO_CONST_TABLES
  lex->add_statement_options(OPTION_NO_SUBQUERY_DURING_OPTIMIZATION);

  return false;
}

static void ShannonAssertSupportedPath(const AccessPath *path) {
  switch (path->type) {
    // The only supported join type is hash join. Other join types are disabled
    // in handlerton::secondary_engine_flags.
    case AccessPath::NESTED_LOOP_JOIN: /* purecov: deadcode */
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
    case AccessPath::BKA_JOIN:
    case AccessPath::INDEX_SCAN:
    case AccessPath::REF:
    case AccessPath::REF_OR_NULL:
    case AccessPath::EQ_REF:
    case AccessPath::PUSHED_JOIN_REF:
    case AccessPath::INDEX_RANGE_SCAN:
    case AccessPath::INDEX_SKIP_SCAN:
    case AccessPath::GROUP_INDEX_SKIP_SCAN:
    case AccessPath::ROWID_INTERSECTION:
    case AccessPath::ROWID_UNION:
    case AccessPath::DYNAMIC_INDEX_RANGE_SCAN:
      assert(false); /* purecov: deadcode */
      break;
    default:
      break;
  }

  // This secondary storage engine does not yet store anything in the auxiliary
  // data member of AccessPath.
  assert(path->secondary_engine_data == nullptr);
}

static bool ShannonOptimizeSecondaryEngine(THD *thd [[maybe_unused]], LEX *lex) {
  // The context should have been set by PrepareSecondaryEngine.
  assert(lex->secondary_engine_execution_context() != nullptr);

  DBUG_EXECUTE_IF("secondary_engine_rapid_optimize_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
    return true;
  });

  DEBUG_SYNC(thd, "before_rapid_optimize");

  if (lex->using_hypergraph_optimizer) {
    WalkAccessPaths(lex->unit->root_access_path(), nullptr,
                    WalkAccessPathPolicy::ENTIRE_TREE,
                    [](AccessPath *path, const JOIN *) {
                      ShannonAssertSupportedPath(path);
                      return false;
                    });
  }
  /** Here, sql has been optimized with 'unit->optimize()'. hence, we just only use
  rapid optimizer to do some necessaries optimization to adjust the best plan.*/
#if 0
  std::unique_ptr<ShannonBase::OptimizeContext> optimze_context =
    std::make_unique<ShannonBase::OptimizeContext>();
  optimze_context->m_thd = thd;

  Query_expression* query_expr_second = lex->unit;
  std::shared_ptr<Query_expression> query_expr(query_expr_second);

  std::shared_ptr<ShannonBase::Optimizer::CostEstimator> cost_est;
  cost_est.reset(new ShannonBase::Optimizer::CostEstimator());
  std::unique_ptr<ShannonBase::Optimizer::Optimizer> optimizer =
    std::make_unique<ShannonBase::Optimizer::Optimizer> (query_expr, cost_est);

  auto ret = optimizer->optimize(optimze_context.get(), query_expr);
#endif
  return false;
}

//to estimate the cost used  secondary engine.
static bool ShannonCompareJoinCost(THD *thd, const JOIN &join, double optimizer_cost,
                            bool *use_best_so_far, bool *cheaper,
                            double *secondary_engine_cost) {
  *use_best_so_far = false;

  DBUG_EXECUTE_IF("secondary_engine_rapid_compare_cost_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
    return true;
  });

  DBUG_EXECUTE_IF("secondary_engine_rapid_choose_first_plan", {
    *use_best_so_far = true;
    *cheaper = true;
    *secondary_engine_cost = optimizer_cost;
  });

  // Just use the cost calculated by the optimizer by default.
  *secondary_engine_cost = optimizer_cost;

  // This debug flag makes the cost function prefer orders where a table with
  // the alias "X" is closer to the beginning.
  DBUG_EXECUTE_IF("secondary_engine_rapid_change_join_order", {
    double cost = join.tables;
    for (size_t i = 0; i < join.tables; ++i) {
      const Table_ref *ref = join.positions[i].table->table_ref;
      if (std::string(ref->alias) == "X") {
        cost += i;
      }
    }
    *secondary_engine_cost = cost;
  });

  // Check if the calculated cost is cheaper than the best cost seen so far.
  *cheaper = down_cast<ShannonBase::Shannon_execution_context *>(
                 thd->lex->secondary_engine_execution_context())
                 ->BestPlanSoFar(join, *secondary_engine_cost);

  return false;
}

static bool ShannonModifyAccessPathCost(THD *thd [[maybe_unused]],
                                 const JoinHypergraph &hypergraph
                                 [[maybe_unused]],
                                 AccessPath *path) {
  assert(!thd->is_error());
  assert(hypergraph.query_block()->join == hypergraph.join());
  ShannonAssertSupportedPath(path);
  return false;
}

static handler *ShannonCreateHandler(handlerton *hton, TABLE_SHARE *table_share, bool partitioned,
                       MEM_ROOT *mem_root) {
  assert(hton == ShannonBase::shannon_rapid_hton_ptr);
  if (partitioned) {
    //to do, partitioned rapid table will be supported in next.
  }
  return new (mem_root) ShannonBase::ha_rapid(hton, table_share);
}
static int ShannonStartConsistentSnapshot(handlerton* hton, THD* thd) {
  assert(hton == ShannonBase::shannon_rapid_hton_ptr);
  trx_t* trx = thd_to_trx(thd);
  assert(trx);
  if (!trx_is_started(trx)) { //maybe the some error in primary engine. it should be started.
                              //here,in case.
    trx_start_if_not_started_xa(trx, false, UT_LOCATION_HERE);
    if (trx->isolation_level == TRX_ISO_REPEATABLE_READ)
      trx_assign_read_view(trx);
  }
  return 0;
}
/** Frees a possible trx object associated with the current THD.
 @return 0 or error number */
static int ShannonCloseConnection(
    handlerton *hton, /*!< in: handlerton */
    THD *thd)         /*!< in: handle to the MySQL thread of the user
                      whose resources should be free'd */
{
  DBUG_TRACE;
  assert(hton == ShannonBase::shannon_rapid_hton_ptr);
  trx_t* trx = thd_to_trx (thd);
  if (trx && trx_is_started(trx)) {
     //const dberr_t error [[maybe_unused]] = trx_commit_for_mysql(trx);
     //ut_ad(DB_SUCCESS == error);
     trx_commit(trx);
     trx->will_lock = 0;
  }

  return 0;
}
/** Cancel any pending lock request associated with the current THD. */
static void ShannonKillConnection(
    handlerton *hton, /*!< in:  innobase handlerton */
    THD *thd){         /*!< in: handle to the MySQL thread being
                      killed */
  DBUG_TRACE;
  assert(hton == ShannonBase::shannon_rapid_hton_ptr);
}

/** Commits a transaction in an database or marks an SQL statement
 ended.
 @return 0 or deadlock error if the transaction was aborted by another
         higher priority transaction. */
static int ShannonCommit(handlerton *hton, /*!< in: handlerton */
                         THD *thd,         /*!< in: MySQL thread handle of the
                                             user for whom the transaction should
                                             be committed */
                         bool commit_trx) { /*!< in: true - commit transaction
                                             false - the current SQL statement
                                             ended */

  return 0;
}

/** Rolls back a transaction or the latest SQL statement.
 @return 0 or error number */
static int ShannonRollback(handlerton *hton, /*!< in: handlerton */
                           THD *thd, /*!< in: handle to the MySQL thread
                                       of the user whose transaction should
                                       be rolled back */
                            bool rollback_trx) { /*!< in: true - rollback entire
                                                transaction false - rollback the
                                                current statement only */
   return 0;
}

/** This function is used to prepare an X/Open XA distributed transaction.
 @return 0 or error number */
static int ShannonXAPrepare(handlerton *hton, /*!< in: handlerton */
                            THD *thd, /*!< in: handle to the MySQL thread of
                                         the user whose XA transaction should
                                         be prepared */
                            bool prepare_trx){ /*!< in: true - prepare
                                                 transaction false - the current
                                                 SQL statement ended */

  return 0;
}

/** This function is used to recover X/Open XA distributed transactions.
 @return number of prepared transactions stored in xid_list */
static int ShannonXAReconver (
    handlerton *hton,         /*!< in:  handlerton */
    XA_recover_txn *txn_list, /*!< in/out: prepared transactions */
    uint len,                 /*!< in: number of slots in xid_list */
    MEM_ROOT *mem_root) {      /*!< in: memory for table names */


  return 0;
}

/*********************************************************************
 *  Start to define the sys var for shannonbase rapid.
 *********************************************************************/
static void shannonbase_rapid_populate_buffer_size_update [[maybe_unused]](
    /*===========================*/
    THD *thd,                       /*!< in: thread handle */
    SYS_VAR *var [[maybe_unused]],  /*!< in: pointer to system variable */
    void *var_ptr [[maybe_unused]], /*!< out: where the formal string goes */
    const void *save) /*!< in: immediate result from check function */
{
  ulong in_val = *static_cast<const ulong *>(save);
  //set to in_val;
  if (in_val < ShannonBase::Populate::sys_population_buffer_sz) {
    in_val = ShannonBase::Populate::sys_population_buffer_sz;
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                        "population_buffer_size cannot be"
                        " set more than rapid_memory_size.");
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                        "Setting population_buffer_size to %llu",
                        ShannonBase::Populate::sys_population_buffer_sz);
  }

  ShannonBase::Populate::sys_population_buffer_sz = in_val;
}

static void rapid_memory_size_update [[maybe_unused]](
    /*===========================*/
    THD *thd,                       /*!< in: thread handle */
    SYS_VAR *var [[maybe_unused]],  /*!< in: pointer to system variable */
    void *var_ptr [[maybe_unused]], /*!< out: where the formal string goes */
    const void *save) /*!< in: immediate result from check function */
{
  ulong in_val = *static_cast<const ulong *>(save);

  if (in_val < ShannonBase::Imcs::rapid_memory_size) {
    in_val = ShannonBase::Imcs::rapid_memory_size;
    push_warning_printf(
        thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
        "rapid_memory_size cannot be set more than srv buffer.");
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                        "Setting rapid_memory_size to %lu",
                        ShannonBase::Imcs::rapid_memory_size);
  }

  ShannonBase::Imcs::rapid_memory_size = in_val;
}

/** Here we export shannonbase status variables to MySQL. */
static SHOW_VAR shannonbase_rapid_status_variables[] = {
    {"rapid_memory_size_max", (char*)&ShannonBase::Imcs::rapid_memory_size,
                          SHOW_LONG, SHOW_SCOPE_GLOBAL},

    {"rapid_populate_buffer_size_max", (char*)&ShannonBase::Populate::sys_population_buffer_sz,
                                  SHOW_LONG, SHOW_SCOPE_GLOBAL},

    {"rapid_chunk_size_max", (char*)&ShannonBase::Imcs::rapid_chunk_size,
                          SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}
};
    
/** Callback function for accessing the Rapid variables from MySQL:  SHOW VARIABLES. */
static int show_shannonbase_rapid_vars(THD *, SHOW_VAR *var, char *) {
  //gets the latest variables of shannonbase rapid.
  var->type = SHOW_ARRAY;
  var->value = (char *)&shannonbase_rapid_status_variables;
  var->scope = SHOW_SCOPE_GLOBAL;

  return (0);
}
static SHOW_VAR shannonbase_rapid_status_variables_export[] = {
    {"ShannonBase Rapid", (char *)&show_shannonbase_rapid_vars, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {NullS, NullS, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

static MYSQL_SYSVAR_ULONG(
    rapid_memory_size_max,
    ShannonBase::Imcs::rapid_memory_size,
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
    "Number of memory size that used for rapid engine, and it must "
    "not be oversize half of physical mem size.",
    nullptr, nullptr,
    ShannonBase::SHANNON_MAX_MEMRORY_SIZE, 0,
    ShannonBase::SHANNON_MAX_MEMRORY_SIZE, 0);

static MYSQL_SYSVAR_ULONGLONG(rapid_populate_buffer_size_max,
                          ShannonBase::Populate::sys_population_buffer_sz,
                          PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                          "Number of populate buffer size that must not be 10% "
                          "rapid_populate_buffer size.",
                          NULL, nullptr,
                          ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE, 0,
                          ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE,0);

static MYSQL_SYSVAR_ULONG(
    rapid_chunk_size_max,
    ShannonBase::Imcs::rapid_chunk_size,
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
    "Number of chunk memory size that used for rapid engine.",
    nullptr, nullptr,
    ShannonBase::SHANNON_CHUNK_SIZE, 0,
    ShannonBase::SHANNON_CHUNK_SIZE, 0);

//System variables of Shannonbase
static struct SYS_VAR *shannonbase_rapid_system_variables[] = {
    MYSQL_SYSVAR(rapid_memory_size_max),
    MYSQL_SYSVAR(rapid_populate_buffer_size_max),
    MYSQL_SYSVAR(rapid_chunk_size_max),
    nullptr,
};

/** Here, end of, we export shannonbase status variables to MySQL. */
static int Shannonbase_Rapid_Init(MYSQL_PLUGIN p) {
  ShannonBase::shannon_loaded_tables = new ShannonBase::ShannonLoadedTables();

  handlerton *shannon_rapid_hton = static_cast<handlerton *>(p);
  ShannonBase::shannon_rapid_hton_ptr = shannon_rapid_hton;
  shannon_rapid_hton->create = ShannonCreateHandler;
  shannon_rapid_hton->state = SHOW_OPTION_YES;
  shannon_rapid_hton->flags = HTON_IS_SECONDARY_ENGINE;
  shannon_rapid_hton->db_type = DB_TYPE_RAPID;
  shannon_rapid_hton->prepare_secondary_engine = ShannonPrepareSecondaryEngine;
  shannon_rapid_hton->optimize_secondary_engine = ShannonOptimizeSecondaryEngine;
  shannon_rapid_hton->compare_secondary_engine_cost = ShannonCompareJoinCost;
  shannon_rapid_hton->secondary_engine_flags =
      MakeSecondaryEngineFlags(SecondaryEngineFlag::SUPPORTS_HASH_JOIN);
  shannon_rapid_hton->secondary_engine_modify_access_path_cost = ShannonModifyAccessPathCost;
  shannon_rapid_hton->start_consistent_snapshot = ShannonStartConsistentSnapshot;
  shannon_rapid_hton->close_connection = ShannonCloseConnection;
  shannon_rapid_hton->kill_connection = ShannonKillConnection;

  shannon_rapid_hton->commit = ShannonCommit;
  shannon_rapid_hton->rollback = ShannonRollback;
  shannon_rapid_hton->prepare = ShannonXAPrepare;
  shannon_rapid_hton->recover = ShannonXAReconver;

  //MEM_ROOT* mem_root = current_thd->mem_root;
  ShannonBase::imcs_instance = ShannonBase::Imcs::Imcs::get_instance();
  if (!ShannonBase::imcs_instance) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Cannot get IMCS instance.");
    return 1;
  };
  auto ret = ShannonBase::imcs_instance->initialize();
  if (!ret) ShannonBase::shannon_rpd_inited = true;

  //ShannonBase::Populate::sys_population_buffer.reset(new ShannonBase::Populate::Ringbuffer<byte>());
  return ret;
}

static int Shannonbase_Rapid_Deinit(MYSQL_PLUGIN) {
  if (ShannonBase::shannon_loaded_tables) {
    delete ShannonBase::shannon_loaded_tables;
    ShannonBase::shannon_loaded_tables = nullptr;
  }
  ShannonBase::shannon_rpd_inited = false;
  return ShannonBase::imcs_instance->deinitialize();
}

static st_mysql_storage_engine shannonbase_rapid_storage_engine{
    MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(shannon_rapid){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &shannonbase_rapid_storage_engine,
    "Rapid",
    PLUGIN_AUTHOR_SHANNON,
    "Shannon Rapid storage engine",
    PLUGIN_LICENSE_GPL,
    Shannonbase_Rapid_Init,   /* Plugin Init */
    nullptr,                  /* Plugin Check uninstall */
    Shannonbase_Rapid_Deinit, /* Plugin Deinit */
    ShannonBase::SHANNON_RAPID_VERSION,
    shannonbase_rapid_status_variables_export, /* status variables */
    shannonbase_rapid_system_variables,        /* system variables */
    nullptr,                                   /* reserved */
    0,                                         /* flags */
} mysql_declare_plugin_end;