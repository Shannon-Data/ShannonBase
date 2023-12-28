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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
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
#include "sql/current_thd.h" //current_thd
#include "sql/debug_sync.h"
#include "sql/handler.h"
#include "sql/join_optimizer/access_path.h"
#include "sql/join_optimizer/make_join_hypergraph.h"
#include "sql/join_optimizer/walk_access_paths.h"
#include "sql/sql_class.h"
#include "sql/sql_const.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"
#include "template_utils.h"
#include "thr_lock.h"

#include "storage/innobase/include/trx0trx.h"
#include "storage/innobase/handler/ha_innodb.h"

#include "storage/rapid_engine/include/rapid_stats.h"
#include "storage/rapid_engine/imcs/imcs.h"
#include "storage/rapid_engine/populate/populate.h"
#include "storage/rapid_engine/compress/dictionary/dictionary.h"
#include "storage/rapid_engine/include/rapid_context.h"

/* clang-format off */
namespace dd {
class Table;
}

namespace {
// Map from (db_name, table_name) to the RapidShare with table state.
class LoadedTables {
  std::map<std::pair<std::string, std::string>, ShannonBase::RapidShare*> m_tables;
  std::mutex m_mutex;
 public:
  void add(const std::string &db, const std::string &table, ShannonBase::RapidShare* share) {
    std::lock_guard<std::mutex> guard(m_mutex);
    m_tables.insert({std::make_pair(db, table), share});
  }
  ShannonBase::RapidShare *get(const std::string &db, const std::string &table) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_tables.find(std::make_pair(db, table));
    return it == m_tables.end() ? nullptr : it->second;
  }
  void erase(const std::string &db, const std::string &table) {
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_tables.find(std::make_pair(db, table));
    auto ptr = it == m_tables.end() ? nullptr : it->second;
    if (ptr)
      delete it->second;
    m_tables.erase(std::make_pair(db, table));
  }
};

LoadedTables *loaded_tables{nullptr};
static struct handlerton* shannon_rapid_hton_ptr {nullptr};
ShannonBase::Imcs::Imcs* imcs_instance = ShannonBase::Imcs::Imcs::Get_instance();;
/**
  Execution context class for the Rapid engine. It allocates some data
  on the heap when it is constructed, and frees it when it is
  destructed, so that LeakSanitizer and Valgrind can detect if the
  server doesn't destroy the object when the query execution has
  completed.
*/
class Rapid_execution_context : public Secondary_engine_execution_context {
 public:
  Rapid_execution_context() : m_data(std::make_unique<char[]>(10)) {}
  /**
    Checks if the specified cost is the lowest cost seen so far for executing
    the given JOIN.
  */
  bool BestPlanSoFar(const JOIN &join, double cost) {
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

 private:
  std::unique_ptr<char[]> m_data;
  /// The JOIN currently being optimized.
  const JOIN *m_current_join{nullptr};
  /// The cost of the best plan seen so far for the current JOIN.
  double m_best_cost;
};

}  // namespace

namespace ShannonBase {
//global rpd meta infos
rpd_columns_container meta_rpd_columns_infos;
std::map<std::string, std::unique_ptr<Compress::Dictionary>> loaded_dictionaries;
ha_rapid::ha_rapid(handlerton *hton, TABLE_SHARE *table_share_arg)
    : handler(hton, table_share_arg) {
}
int ha_rapid::create(const char *, TABLE *, HA_CREATE_INFO *, dd::Table *) {
  assert(false);
  return HA_ERR_WRONG_COMMAND;
}
int ha_rapid::open(const char *name, int mode, unsigned int test_if_locked, const dd::Table * table_def) {
  m_share = loaded_tables->get(table_share->db.str, table_share->table_name.str);
  if (m_share == nullptr) {
    // The table has not been loaded into the secondary storage engine yet.
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Table has not been loaded");
    return HA_ERR_GENERIC;
  }
  thr_lock_data_init(&m_share->m_lock, &m_lock, nullptr);
  return 0;
}
int ha_rapid::close () {
  m_start_of_scan = false;
  inited = NONE;
  return 0;
}
int ha_rapid::rnd_init(bool scan) {
  assert(inited == handler::NONE);
  assert(m_start_of_scan == false);
  inited = handler::RND;
  m_start_of_scan = true;
  //imcs do initialization. scan: random read or scan.
  return imcs_instance->Rnd_init(scan);
}
int ha_rapid::rnd_end() {
  m_start_of_scan = false;
  return imcs_instance->Rnd_end();
}
int ha_rapid::read_range_first(const key_range *start_key,
                                  const key_range *end_key, bool eq_range_arg,
                                  bool sorted) {
  return handler::read_range_first(start_key, end_key, eq_range_arg, sorted);
}

int ha_rapid::read_range_next() {
  return (handler::read_range_next());
}
int ha_rapid::rnd_next(unsigned char *buffer) {
  if (inited != RND) return HA_ERR_END_OF_FILE;
  if (!m_start_of_scan)
    m_start_of_scan = true;
  assert(imcs_instance);

  if (pushed_idx_cond){ //icp
    //TODO: evaluate condition item, and do condtion eval in scan.
  }
  ShannonBase::RapidContext context;
  context.m_current_db = table->s->db.str;
  context.m_current_table = table->s->table_name.str;
  context.m_trx = thd_to_trx (current_thd);
  trx_start_if_not_started(context.m_trx, false, UT_LOCATION_HERE);
  context.m_table = table;
  if (!srv_read_only_mode) {
    trx_assign_read_view(context.m_trx);
  }
  if (loaded_dictionaries.find(context.m_current_db) == loaded_dictionaries.end()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "Local dictionary error.");
    return HA_ERR_GENERIC;
  }
  context.m_local_dict = loaded_dictionaries[context.m_current_db].get();
  return imcs_instance->Read(&context, buffer);
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
  // Secondary engines do not support index access. Indexes are only used for
  // cost estimates.
  return HA_NO_INDEX_ACCESS;
}

Item *ha_rapid::idx_cond_push(uint keyno, Item *idx_cond)
{
  DBUG_TRACE;
  assert(keyno != MAX_KEY);
  assert(idx_cond != nullptr);

  pushed_idx_cond = idx_cond;
  pushed_idx_cond_keyno = keyno;
  in_range_check_pushed_down = true;
  /* We will evaluate the condition entirely */
  return nullptr;
}
unsigned long ha_rapid::index_flags(unsigned int idx, unsigned int part,
                                   bool all_parts) const {
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
  return ((HA_READ_RANGE | HA_KEY_SCAN_NOT_ROR) & primary_flags);
}

ha_rows ha_rapid::records_in_range(unsigned int index, key_range *min_key,
                                  key_range *max_key) {
  // Get the number of records in the range from the primary storage engine.
  return ha_get_primary_handler()->records_in_range(index, min_key, max_key);
}

THR_LOCK_DATA **ha_rapid::store_lock(THD *, THR_LOCK_DATA **to,
                                    thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && m_lock.type == TL_UNLOCK)
    m_lock.type = lock_type;
  *to++ = &m_lock;
  return to;
}

int ha_rapid::load_table(const TABLE &table_arg) {
  assert(table_arg.file != nullptr);
  THD* thd = current_thd;
  if (loaded_tables->get(table_arg.s->db.str, table_arg.s->table_name.str) != nullptr) {
    std::ostringstream err;
    err << table_arg.s->db.str<< "." <<table_arg.s->table_name.str << " already loaded";
    my_error(ER_SECONDARY_ENGINE_LOAD, MYF(0), err.str().c_str());
    return HA_ERR_GENERIC;
  }

  //in future, we will load the content strings from dictionary file, which makes it more flexible.
  //at rapid engine startup phase.
  std::string db_name(table_arg.s->db.str);
  if (loaded_dictionaries.find(db_name) == loaded_dictionaries.end())
     loaded_dictionaries.insert(std::make_pair(db_name, std::make_unique<Compress::Dictionary>()));
  // check if primary key is missing. rapid engine must has at least one PK. 
  if (table_arg.s->is_missing_primary_key()) {
    my_error(ER_REQUIRES_PRIMARY_KEY, MYF(0));
    return HA_ERR_GENERIC;
  } else {
    KEY* key  = table_arg.s->key_info;
    for (uint idx =0; idx < key->user_defined_key_parts; idx ++) {
       Field* key_field = (key->key_part + idx)->field;
       if (key_field->is_flag_set(NOT_SECONDARY_FLAG)) {////The key field MUST be set.
         my_error(ER_RAPID_DA_PRIMARY_KEY_CAN_NOT_HAVE_NOT_SECONDARY_FLAG, MYF(0),
                  table_arg.s->db.str, table_arg.s->table_name.str);
         return HA_ERR_GENERIC;
       }
       //only numeric data type allowed.
       switch (key_field->type()){
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_INT24:
          break;
        default:
           std::ostringstream err;
           err << table_arg.s->table_name.str << "." << key_field->field_name << " PK type not allowed";
           my_error(ER_SECONDARY_ENGINE_LOAD, MYF(0), table_arg.s->db.str,
           table_arg.s->table_name.str);
           return HA_ERR_GENERIC;
       }
    }
  }
  // Scan the primary table and read the records.
  if (table_arg.file->inited == NONE && table_arg.file->ha_rnd_init(true)) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), table_arg.s->db.str,
             table_arg.s->table_name.str);
    return HA_ERR_GENERIC;
  }
  //Start to write to IMCS. Do scan the primary table.
  int tmp {HA_ERR_GENERIC};
  ShannonBase::RapidContext context;
  context.m_current_db = table_arg.s->db.str;
  context.m_current_table = table_arg.s->table_name.str;
  context.m_table = const_cast<TABLE *>(&table_arg);
  while ((tmp = table_arg.file->ha_rnd_next(table_arg.record[0])) != HA_ERR_END_OF_FILE) {
   /*
      ha_rnd_next can return RECORD_DELETED for MyISAM when one thread is
      reading and another deleting without locks. Now, do full scan, but
      multi-thread scan will impl in future.
    */
    if (tmp == HA_ERR_KEY_NOT_FOUND) break;
    uint32 field_count = table_arg.s->fields;
    Field *field_ptr = nullptr;
    uint32 primary_key_idx [[maybe_unused]] = field_count;
    // Gets trx id
    field_ptr = *(table_arg.field + field_count); //ghost field.
    assert(field_ptr->type() == MYSQL_TYPE_DB_TRX_ID);
    context.m_extra_info.m_trxid = field_ptr->val_int();
    context.m_trx = thd_to_trx(current_thd);
    if (context.m_trx->state == TRX_STATE_NOT_STARTED) {
      assert (false);
    }
    context.m_extra_info.m_pk = 0;
    for (uint32 index = 0; index < field_count; index++) {
      field_ptr = *(table_arg.field + index);
      if (field_ptr->is_flag_set(PRI_KEY_FLAG))
         context.m_extra_info.m_pk += field_ptr->val_real();
    }
    for (uint32 index = 0; index < field_count; index++) {
      field_ptr = *(table_arg.field + index);
      // Skip columns marked as NOT SECONDARY. │
      if ((field_ptr)->is_flag_set(NOT_SECONDARY_FLAG)) continue;
#ifndef NDEBUG
      my_bitmap_map *old_map = 0;
      TABLE *table = const_cast<TABLE *>(&table_arg);
      if (table && table->file)
        old_map = dbug_tmp_use_all_columns(table, table->read_set);
#endif
#ifndef NDEBUG
      if (old_map) dbug_tmp_restore_column_map(table->read_set, old_map);
#endif
      if (imcs_instance->Write(&context, field_ptr)) {
        my_error(ER_SECONDARY_ENGINE_LOAD, MYF(0), table_arg.s->db.str,
                 table_arg.s->table_name.str);
        return HA_ERR_GENERIC;
      }
    }
    ha_statistic_increment(&System_status_var::ha_read_rnd_count);
    if (tmp == HA_ERR_RECORD_DELETED && !thd->killed) continue;
  }
  table_arg.file->ha_rnd_end();

  m_share = new RapidShare ();
  m_share->file = this;
  loaded_tables->add(table_arg.s->db.str, table_arg.s->table_name.str, m_share);
  if (loaded_tables->get(table_arg.s->db.str, table_arg.s->table_name.str) ==
      nullptr) {
    my_error(ER_NO_SUCH_TABLE, MYF(0), table_arg.s->db.str,
             table_arg.s->table_name.str);
    return HA_ERR_KEY_NOT_FOUND;
  }
  return 0;
}

int ha_rapid::unload_table(const char *db_name, const char *table_name,
                          bool error_if_not_loaded) {
  if (error_if_not_loaded &&
      loaded_tables->get(db_name, table_name) == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Table is not loaded on a secondary engine");
    return HA_ERR_GENERIC;
  }
  
  ShannonBase::Imcs::Imcs* imcs_instance = ShannonBase::Imcs::Imcs::Get_instance();
  assert(imcs_instance);
  RapidContext context;
  context.m_current_db = std::string(db_name);
  context.m_current_table = std::string(table_name);
  //int ret{0};
  if (auto ret = imcs_instance->Delete_all(&context)) {
    return ret;
  }
  loaded_tables->erase(db_name, table_name);
  return 0;
}
}  // namespace ShannonBase 

static bool ShannonPrepareSecondaryEngine(THD *thd, LEX *lex) {
  DBUG_EXECUTE_IF("secondary_engine_rapid_prepare_error", {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "");
    return true;
  });

  auto context = new (thd->mem_root) Rapid_execution_context;
  if (context == nullptr) return true;
  lex->set_secondary_engine_execution_context(context);

  // Disable use of constant tables and evaluation of subqueries during
  // optimization.
  lex->add_statement_options(OPTION_NO_CONST_TABLES |
                             OPTION_NO_SUBQUERY_DURING_OPTIMIZATION);

  return false;
}

static void ShannonAssertSupportedPath(const AccessPath *path) {
  switch (path->type) {
    // The only supported join type is hash join. Other join types are disabled
    // in handlerton::secondary_engine_flags.
    case AccessPath::NESTED_LOOP_JOIN: /* purecov: deadcode */
    case AccessPath::NESTED_LOOP_SEMIJOIN_WITH_DUPLICATE_REMOVAL:
    case AccessPath::BKA_JOIN:
    // Index access is disabled in ha_rapid::table_flags(), so we should see none
    // of these access types.
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

  return false;
}

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
  *cheaper = down_cast<Rapid_execution_context *>(
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

static handler *ShannonCreate(handlerton *hton, TABLE_SHARE *table_share, bool,
                       MEM_ROOT *mem_root) {
  assert(hton == shannon_rapid_hton_ptr);
  return new (mem_root) ShannonBase::ha_rapid(hton, table_share);
}
static int ShannonStartConsistentSnapshot(handlerton* hton, THD* thd) {
  assert(hton == shannon_rapid_hton_ptr);
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
/*********************************************************************
 *  Start to define the sys var for shannonbase rapid.
 *********************************************************************/
static void shannonbase_rapid_populate_buffer_size_update(
    /*===========================*/
    THD *thd,                       /*!< in: thread handle */
    SYS_VAR *var [[maybe_unused]],  /*!< in: pointer to system variable */
    void *var_ptr [[maybe_unused]], /*!< out: where the formal string goes */
    const void *save) /*!< in: immediate result from check function */
{
  ulong in_val = *static_cast<const ulong *>(save);
  //set to in_val;
  if (in_val < ShannonBase::Populate::population_buffer_size) {
    in_val = ShannonBase::Populate::population_buffer_size;
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                        "population_buffer_size cannot be"
                        " set more than rapid_memory_size.");
    push_warning_printf(thd, Sql_condition::SL_WARNING, ER_WRONG_ARGUMENTS,
                        "Setting population_buffer_size to %lu",
                        ShannonBase::Populate::population_buffer_size);
  }

  ShannonBase::Populate::population_buffer_size = in_val;
}

static void rapid_memory_size_update(
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
    {"rapid_memory_size", (char*)&ShannonBase::Imcs::rapid_memory_size,
                          SHOW_LONG, SHOW_SCOPE_GLOBAL},

    {"rapid_populate_buffer_size", (char*)&ShannonBase::Populate::population_buffer_size,
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
    rapid_memory_size, 
    ShannonBase::Imcs::rapid_memory_size,
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
    "Number of memory size that used for rapid engine, and it must "
    "not be oversize half of physical mem size.",
    nullptr, rapid_memory_size_update,
    ShannonBase::SHANNON_DEFAULT_MEMRORY_SIZE, 0,
    ShannonBase::SHANNON_MAX_MEMRORY_SIZE, 0);

static MYSQL_SYSVAR_ULONG(rapid_populate_buffer_size,
                           ShannonBase::Populate::population_buffer_size,
                          PLUGIN_VAR_RQCMDARG,
                          "Number of populate buffer size that must not be 10% "
                          "rapid_populate_buffer size.",
                          NULL, shannonbase_rapid_populate_buffer_size_update, 
                          ShannonBase::SHANNON_DEFAULT_POPULATION_BUFFER_SIZE, 0,
                          ShannonBase::SHANNON_MAX_POPULATION_BUFFER_SIZE,0);

//System variables of Shannonbase
static struct SYS_VAR *shannonbase_rapid_system_variables[] = {
    MYSQL_SYSVAR(rapid_memory_size),
    MYSQL_SYSVAR(rapid_populate_buffer_size),
    nullptr,
};

/** Here, end of, we export shannonbase status variables to MySQL. */
static int Shannonbase_Rapid_Init(MYSQL_PLUGIN p) {
  loaded_tables = new LoadedTables();

  handlerton *shannon_rapid_hton = static_cast<handlerton *>(p);
  shannon_rapid_hton_ptr = shannon_rapid_hton;
  shannon_rapid_hton->create = ShannonCreate;
  shannon_rapid_hton->state = SHOW_OPTION_YES;
  shannon_rapid_hton->flags = HTON_IS_SECONDARY_ENGINE;
  shannon_rapid_hton->db_type = DB_TYPE_UNKNOWN;
  shannon_rapid_hton->prepare_secondary_engine = ShannonPrepareSecondaryEngine;
  shannon_rapid_hton->optimize_secondary_engine = ShannonOptimizeSecondaryEngine;
  shannon_rapid_hton->compare_secondary_engine_cost = ShannonCompareJoinCost;
  shannon_rapid_hton->secondary_engine_flags =
      MakeSecondaryEngineFlags(SecondaryEngineFlag::SUPPORTS_HASH_JOIN);
  shannon_rapid_hton->secondary_engine_modify_access_path_cost = ShannonModifyAccessPathCost;
  shannon_rapid_hton->start_consistent_snapshot = ShannonStartConsistentSnapshot;
  //shannon_rapid_hton->commit = ;
  //shannon_rapid_hton->rollback = ;

  //MEM_ROOT* mem_root = current_thd->mem_root;
  imcs_instance = ShannonBase::Imcs::Imcs::Get_instance();
  if (!imcs_instance) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "Cannot get IMCS instance.");
    return 1;
  };
  return imcs_instance->Initialize();
}

static int Shannonbase_Rapid_Deinit(MYSQL_PLUGIN) {
  if (loaded_tables) {
    delete loaded_tables;
    loaded_tables = nullptr;
  }
  return imcs_instance->Deinitialize();
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
    Shannonbase_Rapid_Init,
    nullptr,
    Shannonbase_Rapid_Deinit,
    ShannonBase::SHANNON_RAPID_VERSION,
    shannonbase_rapid_status_variables_export,
    shannonbase_rapid_system_variables,
    nullptr,
    0,
} mysql_declare_plugin_end;
